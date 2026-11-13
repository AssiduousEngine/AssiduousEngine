// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net - `foundation.net:reliable` partition.
///
/// Reliable UDP over the unreliable datagram layer. A `ReliableTransport` (an INetTransport)
/// manages per-remote connections over an IDatagramSocket and adds: a packet header (protocol id,
/// type, seq, and once anything has arrived ack and ackBits), ack-driven RTT, reliable messages
/// (resent on an RTT-scaled timer until an acked packet carried them, then delivered in order by
/// message id), unreliable pass-through (sequenced ones numbered per channel, a stale one dropped),
/// a connect and accept handshake that tells a new session from the same address apart by its
/// token, and keepalive and timeout. The model follows Glenn Fiedler's
/// reliable-UDP articles and suits a low message rate. A reliable message larger than one datagram
/// is fragmented and reassembled; an unreliable one that large is dropped. There is no congestion
/// control.
///
/// Tested headless against SimDatagramNetwork's loss and reordering, with no OS sockets.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"

export module foundation.net:reliable;

import foundation.core;
import :transport;
import :datagram;
import :wire;

using namespace foundation::core;

export namespace foundation::net
{

    // Tunables for a ReliableTransport (all times in ms).
    struct ReliableConfig
    {
        u16 protocolId = 0xDCE8;  // reject foreign or mismatched-version packets (bumped with the wire format)
        f32 keepAliveMs = 100.0f; // send an ack-only packet if idle this long (keeps acks flowing)
        f32 connectResendMs = 100.0f; // resend ConnectRequest until accepted
        f32 timeoutMs = 5000.0f;      // no packet received in this long => disconnect
        f32 minResendMs = 20.0f;   // reliable resend floor (a message resends no faster than this)
        f32 maxResendMs = 1000.0f; // reliable resend ceiling (caps the RTT-scaled backoff)
        // Before the first round trip is measured: a reliable message's first resend waits this, and
        // each one after twice the last, up to maxResendMs.
        f32 initialResendMs = 200.0f;
        u32 maxMessageBytes = 256u * 1024u; // largest message (reassembled); bigger is dropped
        u32 maxPacketBytes = 1200; // soft budget per datagram (< typical MTU); large reliable
                                   // messages fragment into this-sized pieces and reassemble
        u32 maxPacketsPerUpdate = 64; // most Data packets one update sends a connection (a burst cap)
        u32 maxPeers = 64; // live connections a server holds; a handshake past them is refused
        // Reliable messages queued to one peer and not yet acked. A peer that stays connected but
        // stops acking past this is disconnected, rather than the queue growing without end.
        u32 maxQueuedReliable = 16384;
    };

    enum class ConnectionState : u8
    {
        Connecting,
        Connected,
        Disconnected
    };

    class ReliableTransport final : public INetTransport
    {
    public:
        ReliableTransport(IDatagramSocket& socket, const ReliableConfig& config = {})
            : m_socket(&socket), m_cfg(config)
        {
        }

        // --- connection establishment (driven by the session layer or tests; not part of INetTransport) ---

        // Client: begin connecting to `remote`. Returns the local PeerId (a Connected event fires later).
        PeerId Connect(const DatagramEndpoint& remote)
        {
            Connection& c = OpenConnection(remote);
            c.state = ConnectionState::Connecting;
            c.token = NewToken();
            SendConnectRequest(c);
            return c.peer;
        }
        // Server: accept incoming ConnectRequests (default off; a listen server opts in).
        void SetAccepting(bool accepting) noexcept { m_accepting = accepting; }

        // --- INetTransport ---

        void Send(PeerId peer, u8 channel, Span<const byte> data, Reliability reliability) override
        {
            Connection* c = FindByPeer(peer);
            if (c == nullptr || c->state == ConnectionState::Disconnected)
            {
                return;
            }
            if (data.Size() > m_cfg.maxMessageBytes)
            {
                LOG_WARNING(u8"Net", u8"message {} B exceeds maxMessageBytes {} - dropped",
                                     data.Size(), m_cfg.maxMessageBytes);
                return;
            }
            const usize limit = FragPayloadLimit();
            if (reliability != Reliability::ReliableOrdered)
            {
                // Unreliable messages are single-datagram: fragmenting fire-and-forget data makes no
                // sense, since a lost fragment orphans the rest. A too-big unreliable message is dropped.
                if (data.Size() > limit)
                {
                    LOG_WARNING(
                        u8"Net", u8"unreliable message {} B exceeds one datagram ({} B) - dropped",
                        data.Size(), limit);
                    return;
                }
                OutMessage m;
                m.channel = channel;
                m.reliable = false;
                if (reliability == Reliability::UnreliableSequenced)
                {
                    m.sequenced = true;
                    const u16* next = c->nextOutSequenced.Find(channel);
                    m.id = next != nullptr ? *next : u16{0};
                    c->nextOutSequenced.InsertOrAssign(channel, static_cast<u16>(m.id + 1u));
                }
                m.data.Resize(data.Size());
                if (data.Size() > 0)
                {
                    MemCopy(m.data.Data(), data.Data(), data.Size());
                }
                c->pendingUnreliable.PushBack(static_cast<OutMessage&&>(m));
                return;
            }
            // Reliable: one message when it fits, else fragment into consecutive-id pieces.
            const usize pieces = (data.Size() + limit - 1) / limit;
            if (c->unackedReliable.Size() + Max<usize>(pieces, 1) > m_cfg.maxQueuedReliable)
            {
                LOG_WARNING(u8"Net", u8"peer {}: {} reliable messages unacked, over maxQueuedReliable {}, disconnecting",
                            c->peer, c->unackedReliable.Size(), m_cfg.maxQueuedReliable);
                SendControl(*c, PacketType::Disconnect);
                c->state = ConnectionState::Disconnected;
                PushEvent(NetEventKind::Disconnected, c->peer, 0, {});
                return;
            }
            if (data.Size() <= limit)
            {
                OutMessage m;
                m.channel = channel;
                m.reliable = true;
                m.id = c->nextOutReliableId++;
                m.data.Resize(data.Size());
                if (data.Size() > 0)
                {
                    MemCopy(m.data.Data(), data.Data(), data.Size());
                }
                c->unackedReliable.PushBack(static_cast<OutMessage&&>(m));
            }
            else
            {
                const u32 fragCount = static_cast<u32>((data.Size() + limit - 1) / limit);
                const u32 group = c->nextFragGroup++;
                for (u32 i = 0; i < fragCount; ++i)
                {
                    const usize off = static_cast<usize>(i) * limit;
                    const usize len = Min(limit, data.Size() - off);
                    OutMessage m;
                    m.channel = channel;
                    m.reliable = true;
                    m.id = c->nextOutReliableId++;
                    m.fragmented = true;
                    m.fragGroup = group;
                    m.fragIndex = i;
                    m.fragCount = fragCount;
                    m.data.Resize(len);
                    MemCopy(m.data.Data(), data.Data() + off, len);
                    c->unackedReliable.PushBack(static_cast<OutMessage&&>(m));
                }
            }
        }

        void Disconnect(PeerId peer) override
        {
            Connection* c = FindByPeer(peer);
            if (c == nullptr || c->state == ConnectionState::Disconnected)
            {
                return;
            }
            SendControl(*c, PacketType::Disconnect);
            c->state = ConnectionState::Disconnected;
        }

        /// Says goodbye to every connection, connected or still connecting, as Disconnect does
        /// to one: the other ends hear it at once instead of waiting out their timeout.
        void DisconnectAll()
        {
            for (Connection& c : m_connections)
            {
                if (c.state != ConnectionState::Disconnected)
                {
                    SendControl(c, PacketType::Disconnect);
                    c.state = ConnectionState::Disconnected;
                }
            }
        }

        [[nodiscard]] bool Poll(NetEvent& out) override
        {
            if (m_eventHead >= m_events.Size())
            {
                return false;
            }
            out = static_cast<NetEvent&&>(m_events[m_eventHead++]);
            if (m_eventHead >= m_events.Size())
            {
                m_events.Clear();
                m_eventHead = 0;
            }
            return true;
        }

        void Update(f32 deltaMs) override
        {
            m_nowMs += static_cast<f64>(deltaMs);
            ReceiveAll();
            for (Connection& c : m_connections)
            {
                UpdateConnection(c);
            }
            RemoveDisconnected();
        }

        [[nodiscard]] TransportStats Stats(PeerId peer) const override
        {
            TransportStats s;
            if (const Connection* c = FindByPeer(peer))
            {
                s.rttMs = static_cast<f32>(c->rttMs); // Connection tracks rtt in f64
                s.sentBytes = c->sentBytes;
                s.recvBytes = c->recvBytes;
            }
            return s;
        }

    private:
        enum class PacketType : u8
        {
            ConnectRequest = 0,
            ConnectAccept = 1,
            Data = 2,
            Disconnect = 3,
            Challenge = 4,      // server: the cookie a ConnectResponse must echo
            ConnectResponse = 5 // client: the ConnectRequest again, with the challenge's cookie
        };
        // Reliable message ids in flight at once: the sender sends none past its oldest unacked id plus
        // this, and the receiver buffers none past its next expected id plus this. Every id below the
        // sender's oldest unacked one has reached the receiver, so a sender inside its window is inside
        // the receiver's. Both ends use it, so it is not a setting; it keeps ids well inside u16's
        // half range, where SeqGreater stops telling older from newer.
        static constexpr u16 kReliableWindow = 1024;
        // A handshake packet's size: protocol, type, token and a cookie. A ConnectRequest is padded
        // to it, so a challenge is never bigger than the request that drew it (no amplification
        // toward a spoofed address).
        static constexpr usize kHandshakeBytes = 2 + 1 + 4 + 8;
        // A cookie is good for the window it was made in and the one after.
        static constexpr f64 kCookieWindowMs = 10000.0;

        struct OutMessage
        {
            u8 channel = 0;
            bool reliable = false;
            u16 id = 0;             // reliable: its message id; sequenced: its number on its channel
            bool sequenced = false; // unreliable, but a stale one is dropped at the far end
            // Fragmentation of a large reliable message: fragments get consecutive ids and a shared group,
            // so ordered delivery releases them consecutively and the receiver reassembles by group.
            bool fragmented = false;
            u32 fragGroup = 0;
            u32 fragIndex = 0;
            u32 fragCount = 1;
            f64 lastSentMs =
                -1.0e9; // RTT-based resend: only (re)sent once this plus the resend timeout elapses
            u32 sendCount = 0; // times sent; before an RTT sample, each resend waits twice the last
            Array<byte> data;
        };
        static constexpr usize kSentWindow = 256;
        struct SentPacket
        {
            bool used = false; // a packet has been recorded in this slot
            u16 seq = 0;
            f64 sendTimeMs = 0.0;
            bool acked = false;
            Array<u16> reliableIds;
        };
        struct BufferedIn
        {
            u16 id = 0;
            u8 channel = 0;
            bool fragmented = false;
            u32 fragGroup = 0;
            u32 fragIndex = 0;
            u32 fragCount = 1;
            Array<byte> data;
        };
        // Accumulates the fragments of one large reliable message (they arrive in order, so one at a time).
        struct Reassembly
        {
            bool active = false;
            u32 group = 0;
            u32 nextIndex = 0;
            u32 count = 0;
            u8 channel = 0;
            Array<byte> data;
        };

        struct Connection
        {
            PeerId peer = kInvalidPeer;
            DatagramEndpoint remote;
            ConnectionState state = ConnectionState::Connecting;
            // The session's token, which the client picks and the server echoes: a request with
            // another one from the same address is a new session (a client that restarted), not a
            // resend of this one's.
            u32 token = 0;
            // outgoing sequencing / ack tracking
            u16 localSeq = 0;
            u16 remoteSeq = 0;    // highest received Data seq
            u32 receivedBits = 0; // history of the 32 packets before remoteSeq
            bool anyReceived = false;
            // The sent Data packets kept for acks and RTT, by seq modulo the window: a slot holds the
            // newest packet with that remainder, so an ack finds its packet without a search.
            FixedArray<SentPacket, kSentWindow> sentWindow{};
            // reliable messages
            u16 nextOutReliableId = 0;
            u32 nextFragGroup = 0;
            Array<OutMessage> unackedReliable;   // resent (RTT-timed) until acked
            Array<OutMessage> pendingUnreliable; // sent once, then cleared
            u16 nextInReliableId = 0;            // next ordered id to release
            Array<BufferedIn> reorderBuffer;     // received reliable ids ahead of nextInReliableId
            Reassembly reassembly;               // in-progress fragment reassembly
            // sequenced unreliable messages, per channel
            HashMap<u8, u16> nextOutSequenced; // the next number each channel sends
            HashMap<u8, u16> lastInSequenced;  // the newest number each channel delivered
            // timing
            f64 rttMs = 0.0;
            bool rttSampled = false; // an ack has measured a round trip (a 0 ms one counts)
            f64 lastRecvMs = 0.0;
            f64 lastSendMs = -1000.0;
            f64 lastConnectSendMs = -1000.0;
            u64 cookie = 0;          // client: the server's challenge, echoed until accepted
            bool haveCookie = false;
            bool ackPending = false; // received data since our last send, so ack promptly
            u32 sentBytes = 0, recvBytes = 0;
        };

        // seq1 is strictly newer than seq2 under u16 wraparound.
        static bool SeqGreater(u16 s1, u16 s2) noexcept
        {
            return ((s1 > s2) && (static_cast<u32>(s1 - s2) <= 0x8000u)) ||
                   ((s1 < s2) && (static_cast<u32>(s2 - s1) > 0x8000u));
        }

        // The live connection to `remote`, or a new one. One that has disconnected is let go first,
        // so connecting again starts a session from nothing rather than carrying the old one's
        // sequences and message ids. A new connection may move the others in memory: a Connection
        // pointer or reference taken before is not used after.
        Connection& OpenConnection(const DatagramEndpoint& remote)
        {
            if (Connection* existing = FindByRemote(remote))
            {
                if (existing->state != ConnectionState::Disconnected)
                {
                    return *existing;
                }
                Forget(*existing);
            }
            Connection c;
            c.peer = m_nextPeer++;
            c.remote = remote;
            c.lastRecvMs = m_nowMs;
            m_connections.PushBack(static_cast<Connection&&>(c));
            const usize index = m_connections.Size() - 1;
            m_byRemote.InsertOrAssign(remote.value, index);
            m_byPeer.InsertOrAssign(m_connections[index].peer, index);
            return m_connections[index];
        }
        // Takes a connection out of the lookups, so its address is free for a new one. It stays in
        // the list (its peer finds nothing) until RemoveDisconnected drops it.
        void Forget(const Connection& c)
        {
            const usize index = static_cast<usize>(&c - m_connections.Data());
            if (const usize* byRemote = m_byRemote.Find(c.remote.value); byRemote != nullptr && *byRemote == index)
            {
                (void)m_byRemote.Remove(c.remote.value);
            }
            if (const usize* byPeer = m_byPeer.Find(c.peer); byPeer != nullptr && *byPeer == index)
            {
                (void)m_byPeer.Remove(c.peer);
            }
        }
        // Drops the connections that have disconnected: their Disconnected events are queued
        // already, and nothing more is said on the wire for them. The lookups are rebuilt, since
        // the others' places in the list move.
        void RemoveDisconnected()
        {
            bool removed = false;
            for (usize i = m_connections.Size(); i > 0; --i)
            {
                if (m_connections[i - 1].state == ConnectionState::Disconnected)
                {
                    m_connections.RemoveAt(i - 1);
                    removed = true;
                }
            }
            if (!removed)
            {
                return;
            }
            m_byRemote.Clear();
            m_byPeer.Clear();
            for (usize i = 0; i < m_connections.Size(); ++i)
            {
                m_byRemote.InsertOrAssign(m_connections[i].remote.value, i);
                m_byPeer.InsertOrAssign(m_connections[i].peer, i);
            }
        }
        // The live connections by peer and by remote, through the lookups: a received packet and a
        // send find theirs without a search, so the cost per connection does not grow with their number.
        [[nodiscard]] Connection* FindByPeer(PeerId peer)
        {
            const usize* index = m_byPeer.Find(peer);
            return index != nullptr ? &m_connections[*index] : nullptr;
        }
        [[nodiscard]] const Connection* FindByPeer(PeerId peer) const
        {
            const usize* index = m_byPeer.Find(peer);
            return index != nullptr ? &m_connections[*index] : nullptr;
        }
        [[nodiscard]] Connection* FindByRemote(const DatagramEndpoint& r)
        {
            const usize* index = m_byRemote.Find(r.value);
            return index != nullptr ? &m_connections[*index] : nullptr;
        }
        // A session token: nonzero, and, drawn from the system's entropy, unlikely to repeat across
        // a client's restarts.
        [[nodiscard]] u32 NewToken() noexcept
        {
            const u32 token = m_random.NextU32();
            return token != 0 ? token : 1u;
        }

        void PushEvent(NetEventKind kind, PeerId peer, u8 channel, Array<byte>&& payload)
        {
            NetEvent ev;
            ev.kind = kind;
            ev.peer = peer;
            ev.channel = channel;
            ev.payload = static_cast<Array<byte>&&>(payload);
            m_events.PushBack(static_cast<NetEvent&&>(ev));
        }

        void RawSend(Connection& c, BitWriter& w)
        {
            Span<const byte> bytes = w.Data();
            c.sentBytes += static_cast<u32>(bytes.Size());
            m_socket->Send(c.remote, bytes);
            c.lastSendMs = m_nowMs;
        }

        // Client: the request, or once challenged, the response with the server's cookie.
        void SendConnectRequest(Connection& c)
        {
            BitWriter w;
            w.WriteU16(m_cfg.protocolId);
            w.WriteU8(static_cast<u8>(c.haveCookie ? PacketType::ConnectResponse : PacketType::ConnectRequest));
            w.WriteU32(c.token);
            w.WriteU64(c.haveCookie ? c.cookie : 0u); // the request's is padding (kHandshakeBytes)
            RawSend(c, w);
            c.lastConnectSendMs = m_nowMs;
        }
        // Server: a packet to an address it holds no connection for (a challenge, a refusal).
        void SendStateless(const DatagramEndpoint& to, PacketType type, u32 token, const u64* cookie = nullptr)
        {
            BitWriter w;
            w.WriteU16(m_cfg.protocolId);
            w.WriteU8(static_cast<u8>(type));
            w.WriteU32(token);
            if (cookie != nullptr)
            {
                w.WriteU64(*cookie);
            }
            m_socket->Send(to, w.Data());
        }
        // The cookie for a handshake from `remote` with `token` in cookie window `window`: a hash keyed
        // by this transport's secret, so only an address that received the challenge can answer it.
        [[nodiscard]] u64 Cookie(const DatagramEndpoint& remote, u32 token, u64 window) const noexcept
        {
            u8 input[8 + 8 + 4 + 8];
            MemCopy(input, &m_cookieSecret, 8);
            MemCopy(input + 8, &remote.value, 8);
            MemCopy(input + 16, &token, 4);
            MemCopy(input + 20, &window, 8);
            const Sha256Digest digest = Sha256::Of(Span<const byte>(reinterpret_cast<const byte*>(input), sizeof(input)));
            u64 cookie = 0;
            MemCopy(&cookie, digest.bytes, 8);
            return cookie;
        }
        [[nodiscard]] u64 CookieWindow() const noexcept { return static_cast<u64>(m_nowMs / kCookieWindowMs); }
        [[nodiscard]] bool CookieValid(const DatagramEndpoint& remote, u32 token, u64 cookie) const noexcept
        {
            const u64 window = CookieWindow();
            return cookie == Cookie(remote, token, window) || (window > 0 && cookie == Cookie(remote, token, window - 1));
        }
        [[nodiscard]] usize LiveConnections() const noexcept
        {
            usize live = 0;
            for (const Connection& c : m_connections)
            {
                live += c.state != ConnectionState::Disconnected ? 1u : 0u;
            }
            return live;
        }
        // A control packet, carrying the session's token so the far end can tell this session from
        // an earlier one at the same address.
        void SendControl(Connection& c, PacketType type)
        {
            BitWriter w;
            w.WriteU16(m_cfg.protocolId);
            w.WriteU8(static_cast<u8>(type));
            w.WriteU32(c.token);
            RawSend(c, w);
        }

        [[nodiscard]] usize FragPayloadLimit() const noexcept
        {
            // A fragment must fit one datagram with the packet + message headers. 64 B is generous
            // headroom (packet header ~13 + message header with frag fields ~24).
            return (m_cfg.maxPacketBytes > 64u) ? (m_cfg.maxPacketBytes - 64u) : 1u;
        }
        // How long to wait before resending an unacked reliable message: RTT-scaled once a round trip
        // is measured; before that, initialResendMs doubled per resend (the floor alone resent every
        // message every 20 ms on a link whose acks had not come back yet). Clamped either way.
        [[nodiscard]] f64 ResendTimeout(const Connection& c, const OutMessage& m) const noexcept
        {
            f64 timeout = c.rttMs * 1.5;
            if (!c.rttSampled)
            {
                timeout = static_cast<f64>(m_cfg.initialResendMs);
                for (u32 i = 1; i < m.sendCount && timeout < static_cast<f64>(m_cfg.maxResendMs); ++i)
                {
                    timeout *= 2.0;
                }
            }
            return Clamp(timeout, static_cast<f64>(m_cfg.minResendMs), static_cast<f64>(m_cfg.maxResendMs));
        }
        // Whether message `m` is inside the send window (unackedReliable is in id order, oldest first).
        [[nodiscard]] static bool InSendWindow(const Connection& c, const OutMessage& m) noexcept
        {
            return static_cast<u16>(m.id - c.unackedReliable[0].id) < kReliableWindow;
        }
        [[nodiscard]] bool AnyReliableDue(const Connection& c) const noexcept
        {
            for (const OutMessage& m : c.unackedReliable)
            {
                if (!InSendWindow(c, m))
                {
                    break;
                }
                if (m_nowMs - m.lastSentMs >= ResendTimeout(c, m))
                {
                    return true;
                }
            }
            return false;
        }

        // Build and send one Data packet: the header and as many due reliable, then pending unreliable, messages
        // as fit the byte budget. Records the sent packet for ack/RTT and stamps each reliable's resend time.
        void SendDataPacket(Connection& c)
        {
            BitWriter w;
            w.WriteU16(m_cfg.protocolId);
            w.WriteU8(static_cast<u8>(PacketType::Data));
            w.WriteU32(c.token); // the session's: a packet of another session at this address is dropped
            const u16 seq = c.localSeq++;
            w.WriteU16(seq);
            // Nothing received yet, nothing acked: an ack of the default seq would retire the far
            // end's first packet, lost or not, and with it the reliable messages it carried.
            w.WriteBool(c.anyReceived);
            if (c.anyReceived)
            {
                w.WriteU16(c.remoteSeq);
                w.WriteU32(c.receivedBits);
            }

            SentPacket rec;
            rec.seq = seq;
            rec.sendTimeMs = m_nowMs;
            Array<OutMessage*> chosen;
            usize budget = m_cfg.maxPacketBytes;
            for (OutMessage& m : c.unackedReliable)
            {
                if (!InSendWindow(c, m))
                {
                    break; // waits for the window to move
                }
                if (m_nowMs - m.lastSentMs < ResendTimeout(c, m))
                {
                    continue;
                } // not due yet (RTT-timed resend)
                if (m.data.Size() + 24u > budget)
                {
                    break;
                }
                chosen.PushBack(&m);
                budget -= (m.data.Size() + 24u);
                rec.reliableIds.PushBack(m.id);
                m.lastSentMs = m_nowMs;
                ++m.sendCount;
            }
            usize unreliableTaken = 0;
            for (OutMessage& m : c.pendingUnreliable)
            {
                if (m.data.Size() + 12u > budget)
                {
                    break;
                }
                chosen.PushBack(&m);
                budget -= (m.data.Size() + 12u);
                ++unreliableTaken;
            }
            // One that fits no packet at all (unreliable messages are not fragmented) goes, rather
            // than holding the queue up for ever.
            if (unreliableTaken == 0 && !c.pendingUnreliable.IsEmpty() &&
                c.pendingUnreliable[0].data.Size() + 12u > m_cfg.maxPacketBytes)
            {
                unreliableTaken = 1;
            }
            w.WriteVarU32(static_cast<u32>(chosen.Size()));
            for (OutMessage* m : chosen)
            {
                w.WriteU8(m->channel);
                w.WriteBool(m->reliable);
                if (!m->reliable)
                {
                    w.WriteBool(m->sequenced);
                    if (m->sequenced)
                    {
                        w.WriteU16(m->id);
                    }
                }
                else
                {
                    w.WriteU16(m->id);
                    w.WriteBool(m->fragmented);
                    if (m->fragmented)
                    {
                        w.WriteVarU32(m->fragGroup);
                        w.WriteVarU32(m->fragIndex);
                        w.WriteVarU32(m->fragCount);
                    }
                }
                w.WriteVarU32(static_cast<u32>(m->data.Size()));
                w.WriteBytes(Span<const byte>(m->data.Data(), m->data.Size()));
            }
            RecordSent(c, static_cast<SentPacket&&>(rec));
            RawSend(c, w);
            // Unreliable: sent once. Only those this packet carried leave the queue (they were
            // written above, so it is safe to drop them now); the rest go in the next packet.
            for (usize i = 0; i < unreliableTaken; ++i)
            {
                c.pendingUnreliable.RemoveAt(0);
            }
        }

        void RecordSent(Connection& c, SentPacket&& rec)
        {
            rec.used = true;
            c.sentWindow[rec.seq % kSentWindow] = static_cast<SentPacket&&>(rec);
        }

        void UpdateConnection(Connection& c)
        {
            if (c.state == ConnectionState::Disconnected)
            {
                return;
            }
            if (m_nowMs - c.lastRecvMs > static_cast<f64>(m_cfg.timeoutMs))
            {
                c.state = ConnectionState::Disconnected;
                PushEvent(NetEventKind::Disconnected, c.peer, 0, {});
                return;
            }
            if (c.state == ConnectionState::Connecting)
            {
                if (m_nowMs - c.lastConnectSendMs >= static_cast<f64>(m_cfg.connectResendMs))
                {
                    SendConnectRequest(c);
                }
                return;
            }
            // Connected: send a Data packet when a reliable message is due for (re)send, an unreliable is
            // queued, an ack is owed, or a keepalive is due. The RTT-timed resend avoids per-tick flooding.
            // Send as many packets as the due work fills (a delta bigger than a packet, a burst), up to a
            // cap: at one packet a tick a big delta would drain over several ticks while the next ones
            // queued behind it, and the lag would grow without end. Each reliable sent is stamped, so the
            // loop ends.
            const bool keepAliveDue =
                (m_nowMs - c.lastSendMs) >= static_cast<f64>(m_cfg.keepAliveMs);
            if (AnyReliableDue(c) || !c.pendingUnreliable.IsEmpty() || c.ackPending || keepAliveDue)
            {
                c.ackPending = false;
                SendDataPacket(c);
                for (u32 more = 1; more < m_cfg.maxPacketsPerUpdate &&
                                   (AnyReliableDue(c) || !c.pendingUnreliable.IsEmpty());
                     ++more)
                {
                    SendDataPacket(c);
                }
            }
        }

        void ReceiveAll()
        {
            DatagramEndpoint from;
            Array<byte> data;
            while (m_socket->Receive(from, data))
            {
                HandlePacket(from, data.AsSpan());
            }
        }

        void HandlePacket(const DatagramEndpoint& from, Span<const byte> bytes)
        {
            BitReader r(bytes);
            const u16 protocol = r.ReadU16();
            const u8 typeRaw = r.ReadU8();
            if (!r.Ok() || protocol != m_cfg.protocolId || typeRaw > static_cast<u8>(PacketType::ConnectResponse))
            {
                return;
            } // foreign, corrupt, or a type this version does not know
            const PacketType type = static_cast<PacketType>(typeRaw);

            Connection* c = FindByRemote(from);
            const u32 token = r.ReadU32();
            if (!r.Ok())
            {
                return;
            }
            switch (type)
            {
            case PacketType::ConnectRequest:
            {
                // Nothing is kept for a request: it is answered with a challenge, which only the
                // address that sent it receives, so a spoofed request costs one packet and no peer.
                if (!m_accepting || bytes.Size() < kHandshakeBytes)
                {
                    return;
                }
                if (c != nullptr && c->token == token && c->state != ConnectionState::Disconnected)
                {
                    c->lastRecvMs = m_nowMs;
                    SendControl(*c, PacketType::ConnectAccept); // re-accept: our accept was lost
                    return;
                }
                const u64 cookie = Cookie(from, token, CookieWindow());
                SendStateless(from, PacketType::Challenge, token, &cookie);
                return;
            }
            case PacketType::Challenge:
            {
                const u64 cookie = r.ReadU64();
                if (!r.Ok() || c == nullptr || c->token != token || c->state != ConnectionState::Connecting)
                {
                    return;
                }
                c->cookie = cookie;
                c->haveCookie = true;
                SendConnectRequest(*c); // the response, now
                return;
            }
            case PacketType::ConnectResponse:
            {
                const u64 cookie = r.ReadU64();
                if (!m_accepting || !r.Ok() || !CookieValid(from, token, cookie))
                {
                    return;
                }
                if (c != nullptr && c->token == token && c->state != ConnectionState::Disconnected)
                {
                    c->lastRecvMs = m_nowMs;
                    SendControl(*c, PacketType::ConnectAccept); // re-accept: our accept was lost
                    return;
                }
                const bool replaces = c != nullptr && c->state != ConnectionState::Disconnected;
                if (!replaces && LiveConnections() >= m_cfg.maxPeers)
                {
                    SendStateless(from, PacketType::Disconnect, token); // full: the client stops trying
                    return;
                }
                if (replaces)
                {
                    // Another session from the same address (the client restarted, or connected
                    // again): the old one is over, and its state must not carry into the new one.
                    c->state = ConnectionState::Disconnected;
                    PushEvent(NetEventKind::Disconnected, c->peer, 0, {});
                }
                Connection& nc = OpenConnection(from); // `c` is not used from here on
                nc.state = ConnectionState::Connected;
                nc.token = token;
                nc.lastRecvMs = m_nowMs;
                SendControl(nc, PacketType::ConnectAccept);
                PushEvent(NetEventKind::Connected, nc.peer, 0, {});
                return;
            }
            case PacketType::ConnectAccept:
            {
                // An accept for an earlier session (a late one, or a resend) is not this one's.
                if (c == nullptr || c->token != token)
                {
                    return;
                }
                c->lastRecvMs = m_nowMs;
                if (c->state == ConnectionState::Connecting)
                {
                    c->state = ConnectionState::Connected;
                    PushEvent(NetEventKind::Connected, c->peer, 0, {});
                }
                return;
            }
            case PacketType::Disconnect:
            {
                if (c != nullptr && c->token == token && c->state != ConnectionState::Disconnected)
                {
                    c->state = ConnectionState::Disconnected;
                    PushEvent(NetEventKind::Disconnected, c->peer, 0, {});
                }
                return;
            }
            case PacketType::Data:
                break;
            }
            if (c == nullptr || c->state == ConnectionState::Disconnected || c->token != token)
            {
                return;
            }
            c->recvBytes += static_cast<u32>(bytes.Size());
            c->lastRecvMs = m_nowMs;
            c->ackPending = true; // owe the sender an ack for this Data packet

            const u16 seq = r.ReadU16();
            const bool hasAck = r.ReadBool();
            u16 ack = 0;
            u32 ackBits = 0;
            if (hasAck)
            {
                ack = r.ReadU16();
                ackBits = r.ReadU32();
            }
            if (!r.Ok())
            {
                return;
            }
            if (hasAck)
            {
                ProcessAcks(*c, ack, ackBits);
            }
            UpdateReceivedhistory(*c, seq);

            const u32 count = r.ReadVarU32();
            for (u32 i = 0; i < count && r.Ok(); ++i)
            {
                const u8 channel = r.ReadU8();
                const bool reliable = r.ReadBool();
                u16 id = 0;
                bool sequenced = false;
                bool fragmented = false;
                u32 fragGroup = 0, fragIndex = 0, fragCount = 1;
                if (!reliable)
                {
                    sequenced = r.ReadBool();
                    if (sequenced)
                    {
                        id = r.ReadU16();
                    }
                }
                else
                {
                    id = r.ReadU16();
                    fragmented = r.ReadBool();
                    if (fragmented)
                    {
                        fragGroup = r.ReadVarU32();
                        fragIndex = r.ReadVarU32();
                        fragCount = r.ReadVarU32();
                    }
                }
                const u32 len = r.ReadVarU32();
                if (!r.Ok() || len > m_cfg.maxPacketBytes)
                {
                    return;
                } // a single message fits one datagram
                Array<byte> payload;
                payload.Resize(len);
                if (len > 0)
                {
                    r.ReadBytes(Span<byte>(payload.Data(), len));
                }
                if (!r.Ok())
                {
                    return;
                }
                if (reliable)
                {
                    DeliverReliable(*c, id, channel, fragmented, fragGroup, fragIndex, fragCount,
                                    static_cast<Array<byte>&&>(payload));
                }
                else if (!sequenced || TakeSequenced(*c, channel, id))
                {
                    PushEvent(NetEventKind::Received, c->peer, channel,
                              static_cast<Array<byte>&&>(payload));
                }
            }
        }

        // Whether a sequenced message numbered `id` is newer than the last delivered on its channel,
        // noting it if so. A stale one (reordered behind a newer one, or duplicated) is dropped.
        [[nodiscard]] bool TakeSequenced(Connection& c, u8 channel, u16 id) noexcept
        {
            if (const u16* last = c.lastInSequenced.Find(channel); last != nullptr && !SeqGreater(id, *last))
            {
                return false;
            }
            c.lastInSequenced.InsertOrAssign(channel, id);
            return true;
        }

        // Mark sent packets acked (by ack and the 32-bit history), ack their reliable messages, sample RTT.
        void ProcessAcks(Connection& c, u16 ack, u32 ackBits)
        {
            AckOne(c, ack);
            for (u32 bit = 0; bit < 32; ++bit)
            {
                if (ackBits & (1u << bit))
                {
                    AckOne(c, static_cast<u16>(ack - (bit + 1)));
                }
            }
        }
        void AckOne(Connection& c, u16 seq)
        {
            SentPacket& p = c.sentWindow[seq % kSentWindow];
            if (!p.used || p.seq != seq || p.acked)
            {
                return;
            }
            p.acked = true;
            const f64 sample = m_nowMs - p.sendTimeMs;
            c.rttMs = !c.rttSampled ? sample : (c.rttMs * 0.9 + sample * 0.1);
            c.rttSampled = true;
            for (u16 mid : p.reliableIds)
            {
                AckReliableMessage(c, mid);
            }
        }
        void AckReliableMessage(Connection& c, u16 id)
        {
            for (usize i = 0; i < c.unackedReliable.Size(); ++i)
            {
                if (c.unackedReliable[i].id == id)
                {
                    c.unackedReliable.RemoveAt(i);
                    return;
                }
            }
        }

        void UpdateReceivedhistory(Connection& c, u16 seq)
        {
            if (!c.anyReceived)
            {
                c.anyReceived = true;
                c.remoteSeq = seq;
                c.receivedBits = 0;
                return;
            }
            if (SeqGreater(seq, c.remoteSeq))
            {
                const u16 shift = static_cast<u16>(seq - c.remoteSeq);
                c.receivedBits =
                    (shift >= 32) ? 0u : ((c.receivedBits << shift) | (1u << (shift - 1)));
                c.remoteSeq = seq;
            }
            else
            {
                const u16 diff = static_cast<u16>(c.remoteSeq - seq);
                if (diff >= 1 && diff <= 32)
                {
                    c.receivedBits |= (1u << (diff - 1));
                }
            }
        }

        // Ordered reliable delivery: release in id order, buffering ids that arrive early, dropping dups.
        // A released message is either delivered directly or fed to the fragment reassembler.
        void DeliverReliable(Connection& c, u16 id, u8 channel, bool fragmented, u32 fragGroup,
                             u32 fragIndex, u32 fragCount, Array<byte>&& payload)
        {
            if (SeqGreater(c.nextInReliableId, id) ||
                id == static_cast<u16>(c.nextInReliableId - 1))
            {
                return; // already delivered (id < nextIn): a duplicate resend
            }
            if (id == c.nextInReliableId)
            {
                ReleaseMessage(c, channel, fragmented, fragGroup, fragIndex, fragCount,
                               static_cast<Array<byte>&&>(payload));
                c.nextInReliableId++;
                // release any buffered consecutive ids
                for (;;)
                {
                    bool released = false;
                    for (usize i = 0; i < c.reorderBuffer.Size(); ++i)
                    {
                        if (c.reorderBuffer[i].id == c.nextInReliableId)
                        {
                            BufferedIn b = static_cast<BufferedIn&&>(c.reorderBuffer[i]);
                            c.reorderBuffer.RemoveAt(i);
                            ReleaseMessage(c, b.channel, b.fragmented, b.fragGroup, b.fragIndex,
                                           b.fragCount, static_cast<Array<byte>&&>(b.data));
                            c.nextInReliableId++;
                            released = true;
                            break;
                        }
                    }
                    if (!released)
                    {
                        break;
                    }
                }
            }
            else
            {
                // Early arrival: buffer unless already buffered, or past the window, which a sender
                // keeping to it never sends (the buffer is bounded by the window, not by the peer).
                if (static_cast<u16>(id - c.nextInReliableId) >= kReliableWindow)
                {
                    return;
                }
                for (const BufferedIn& b : c.reorderBuffer)
                {
                    if (b.id == id)
                    {
                        return;
                    }
                }
                BufferedIn b;
                b.id = id;
                b.channel = channel;
                b.fragmented = fragmented;
                b.fragGroup = fragGroup;
                b.fragIndex = fragIndex;
                b.fragCount = fragCount;
                b.data = static_cast<Array<byte>&&>(payload);
                c.reorderBuffer.PushBack(static_cast<BufferedIn&&>(b));
            }
        }

        void LogOversizedMessage(const Connection& c)
        {
            if (!m_oversizedLogged)
            {
                LOG_WARNING(u8"Net", u8"peer {}: fragmented message over maxMessageBytes {}, dropped", c.peer,
                            m_cfg.maxMessageBytes);
                m_oversizedLogged = true;
            }
        }

        // A reliable message, released in order. A whole message is delivered as is; a fragment is appended to
        // the reassembly buffer (fragments arrive consecutively since delivery is ordered); deliver the
        // whole when the last fragment lands.
        void ReleaseMessage(Connection& c, u8 channel, bool fragmented, u32 fragGroup,
                            u32 fragIndex, u32 fragCount, Array<byte>&& payload)
        {
            if (!fragmented)
            {
                PushEvent(NetEventKind::Received, c.peer, channel,
                          static_cast<Array<byte>&&>(payload));
                return;
            }
            Reassembly& ra = c.reassembly;
            if (fragIndex == 0)
            { // first fragment starts a fresh reassembly
                // Every fragment carries at least a byte, so more fragments than maxMessageBytes
                // cannot make a message this end takes.
                if (fragCount == 0 || fragCount > m_cfg.maxMessageBytes)
                {
                    ra.active = false;
                    LogOversizedMessage(c);
                    return;
                }
                ra.active = true;
                ra.group = fragGroup;
                ra.nextIndex = 0;
                ra.count = fragCount;
                ra.channel = channel;
                ra.data.Clear();
            }
            if (!ra.active || ra.group != fragGroup || ra.nextIndex != fragIndex)
            {
                ra.active =
                    false; // out-of-sequence fragment (shouldn't happen under ordered delivery): drop
                return;
            }
            const usize base = ra.data.Size();
            if (base + payload.Size() > m_cfg.maxMessageBytes)
            {
                // Bigger than a message may be: the sender's limit is this end's, so this is a
                // peer not keeping to it. What it sent so far is let go, and the rest is dropped.
                ra.active = false;
                ra.data.Clear();
                LogOversizedMessage(c);
                return;
            }
            ra.data.Resize(base + payload.Size());
            if (payload.Size() > 0)
            {
                MemCopy(ra.data.Data() + base, payload.Data(), payload.Size());
            }
            ra.nextIndex++;
            if (ra.nextIndex >= ra.count)
            {
                PushEvent(NetEventKind::Received, c.peer, ra.channel,
                          static_cast<Array<byte>&&>(ra.data));
                ra.active = false;
                ra.data.Clear();
            }
        }

        IDatagramSocket* m_socket;
        ReliableConfig m_cfg;
        bool m_accepting = false;
        f64 m_nowMs = 0.0;
        PeerId m_nextPeer = 1;
        Array<Connection> m_connections;
        HashMap<u64, usize> m_byRemote;  // a live connection's index by its remote endpoint
        HashMap<PeerId, usize> m_byPeer; // and by its peer
        bool m_oversizedLogged = false; // the first oversized reassembly is logged
        Random m_random = Random::FromSystemEntropy(); // session tokens
        // Keys the handshake cookies; drawn once, so a cookie made by another transport, or before a
        // restart, is not this one's.
        u64 m_cookieSecret = (static_cast<u64>(m_random.NextU32()) << 32) | m_random.NextU32();
        Array<NetEvent> m_events;
        usize m_eventHead = 0;
    };

}
