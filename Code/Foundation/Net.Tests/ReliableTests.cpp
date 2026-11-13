// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net:reliable - the reliable-UDP transport over the lossy datagram simulator.
#include <doctest/doctest.h>
#include <initializer_list>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;

using namespace foundation::core;
namespace net = foundation::net;

namespace
{
    Array<byte> Bytes(std::initializer_list<u8> vals)
    {
        Array<byte> out;
        out.Resize(vals.size());
        usize i = 0;
        for (u8 v : vals)
        {
            out[i++] = static_cast<byte>(v);
        }
        return out;
    }

    // A client and server pair over one simulated network, with a step pump.
    struct Fixture
    {
        net::SimDatagramNetwork net;
        net::IDatagramSocket* sa;
        net::IDatagramSocket* sb;
        net::ReliableTransport client;
        net::ReliableTransport server;

        explicit Fixture(const net::SimConditions& sim, const net::ReliableConfig& cfg = {})
            : net(foundation::core::DefaultAllocator(), sim), sa(net.CreateSocket()), sb(net.CreateSocket()), client(*sa, cfg),
              server(*sb, cfg)
        {
        }

        void Step(f32 dt = 10.0f)
        {
            net.Advance(dt);
            client.Update(dt);
            server.Update(dt);
        }
        void Pump(int steps, f32 dt = 10.0f)
        {
            for (int i = 0; i < steps; ++i)
            {
                Step(dt);
            }
        }
    };

    // Drain the first bytes of all Received events from a transport (call while or after pumping).
    Array<u8> DrainReceived(net::INetTransport& t)
    {
        Array<u8> out;
        net::NetEvent ev;
        while (t.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Received && ev.payload.Size() > 0)
            {
                out.PushBack(static_cast<u8>(ev.payload[0]));
            }
        }
        return out;
    }
    bool AnyKind(net::INetTransport& t, net::NetEventKind kind)
    {
        net::NetEvent ev;
        bool found = false;
        while (t.Poll(ev))
        {
            if (ev.kind == kind)
            {
                found = true;
            }
        }
        return found;
    }
}

namespace
{
    // A socket that loses the first Data packet sent through it and passes everything else.
    class DropFirstDataSocket final : public net::IDatagramSocket
    {
    public:
        explicit DropFirstDataSocket(net::IDatagramSocket& inner) : m_inner(inner) {}
        void Send(const net::DatagramEndpoint& to, Span<const byte> data) override
        {
            net::BitReader reader(data);
            (void)reader.ReadU16(); // protocol
            const u8 type = reader.ReadU8();
            if (dropped == 0 && reader.Ok() && type == 2u) // Data
            {
                ++dropped;
                return;
            }
            m_inner.Send(to, data);
        }
        [[nodiscard]] bool Receive(net::DatagramEndpoint& from, Array<byte>& out) override { return m_inner.Receive(from, out); }
        [[nodiscard]] net::DatagramEndpoint LocalEndpoint() const override { return m_inner.LocalEndpoint(); }

        int dropped = 0;

    private:
        net::IDatagramSocket& m_inner;
    };

    // Counts the events of `kind` a transport has queued, draining it.
    int CountKind(net::INetTransport& t, net::NetEventKind kind)
    {
        int count = 0;
        net::NetEvent ev;
        while (t.Poll(ev))
        {
            count += ev.kind == kind ? 1 : 0;
        }
        return count;
    }
}

TEST_CASE("reliable: handshake connects both sides")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    CHECK(sp != net::kInvalidPeer);
    fx.Pump(20);
    CHECK(AnyKind(fx.client, net::NetEventKind::Connected));
    CHECK(AnyKind(fx.server, net::NetEventKind::Connected));
}

TEST_CASE("reliable: all reliable messages arrive in order despite 50% loss")
{
    net::SimConditions sim;
    sim.latencyMs = 30.0f;
    sim.jitterMs = 10.0f;
    sim.lossPct = 0.5f;
    sim.reorderPct = 0.3f;
    sim.seed = 123;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20); // establish
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 30; ++i)
    {
        Array<byte> m = Bytes({i});
        fx.client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    Array<u8> got;
    for (int s = 0; s < 400; ++s)
    {
        fx.Step();
        Array<u8> chunk = DrainReceived(fx.server);
        for (u8 b : chunk)
        {
            got.PushBack(b);
        }
        if (got.Size() >= 30u)
        {
            break;
        }
    }
    REQUIRE(got.Size() == 30u); // none lost
    for (u8 i = 0; i < 30; ++i)
    {
        CHECK(got[i] == i);
    } // strictly in order, no dups
}

TEST_CASE("reliable: duplication never double-delivers a reliable message")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.dupPct = 1.0f;
    sim.seed = 5;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 8; ++i)
    {
        Array<byte> m = Bytes({i});
        fx.client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    Array<u8> got;
    for (int s = 0; s < 200; ++s)
    {
        fx.Step();
        Array<u8> c = DrainReceived(fx.server);
        for (u8 b : c)
            got.PushBack(b);
        if (got.Size() >= 8u)
            break;
    }
    REQUIRE(got.Size() == 8u); // exactly once each despite 100% duplication
    for (u8 i = 0; i < 8; ++i)
    {
        CHECK(got[i] == i);
    }
}

TEST_CASE("reliable: unreliable messages can drop under loss (no resend)")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    sim.lossPct = 0.6f;
    sim.seed = 77;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    // The handshake's four legs (request, challenge, response, accept) take a few resends at 60% loss.
    bool connected = false;
    for (int step = 0; step < 1000 && !connected; ++step)
    {
        fx.Step();
        connected = AnyKind(fx.client, net::NetEventKind::Connected);
    }
    REQUIRE(connected);
    fx.Pump(20);
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 50; ++i)
    {
        Array<byte> m = Bytes({i});
        fx.client.Send(sp, 1, m.AsSpan(), net::Reliability::Unreliable);
        fx.Step();
    }
    fx.Pump(30);
    const usize delivered = DrainReceived(fx.server).Size();
    CHECK(delivered > 0u);
    CHECK(delivered < 50u); // lossy with no resend: some are gone for good
}

// More than a packet's worth due at once (a replication delta of a crowd) goes out in the same
// update, not one packet a tick: at one a tick, a big delta drains over several ticks while the next
// ones queue behind it, and the delay grows without bound.
TEST_CASE("reliable: a burst bigger than a packet goes out in one update")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 20; ++i)
    {
        Array<byte> m;
        m.Resize(1000); // one to a packet
        m[0] = static_cast<byte>(i);
        fx.client.Send(sp, 1, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    fx.Pump(3); // the latency and a step or two: far fewer updates than messages
    const Array<u8> got = DrainReceived(fx.server);
    REQUIRE(got.Size() == 20u);
    for (u8 i = 0; i < 20; ++i)
    {
        CHECK(got[i] == i);
    }
}

// Unreliable messages that do not fit the packet wait for the next one rather than being dropped.
TEST_CASE("reliable: unreliable messages beyond one packet are sent, not dropped")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 10; ++i)
    {
        Array<byte> m;
        m.Resize(600); // two do not fit one packet
        m[0] = static_cast<byte>(i);
        fx.client.Send(sp, 1, m.AsSpan(), net::Reliability::Unreliable);
    }
    fx.Pump(3);
    CHECK(DrainReceived(fx.server).Size() == 10u); // no loss in the sim: all of them
}

TEST_CASE("reliable: RTT is estimated from acks")
{
    net::SimConditions sim;
    sim.latencyMs = 40.0f; // ~80ms round trip
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(10);
    for (int i = 0; i < 5; ++i)
    {
        Array<byte> m = Bytes({1});
        fx.client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
        fx.Pump(10);
    }
    const net::TransportStats st = fx.client.Stats(sp);
    CHECK(st.rttMs > 40.0f); // at least one one-way; realistically ~80ms
    CHECK(st.rttMs < 200.0f);
}

TEST_CASE("reliable: a silent peer times out into Disconnected")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::ReliableConfig cfg;
    cfg.timeoutMs = 500.0f;
    Fixture fx(sim, cfg);
    fx.server.SetAccepting(true);
    (void)fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    CHECK(AnyKind(fx.client, net::NetEventKind::Connected));

    // Server goes dark: only tick the client and the network (the client hears nothing).
    bool disconnected = false;
    for (int s = 0; s < 100; ++s)
    {
        fx.net.Advance(10.0f);
        fx.client.Update(10.0f);
        if (AnyKind(fx.client, net::NetEventKind::Disconnected))
        {
            disconnected = true;
            break;
        }
    }
    CHECK(disconnected);
}

TEST_CASE("reliable: a large message fragments + reassembles intact over loss")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.lossPct = 0.3f;
    sim.reorderPct = 0.2f;
    sim.seed = 31;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    {
        net::NetEvent ev;
        while (fx.server.Poll(ev))
        {
        }
    }

    // 5000 bytes make several fragments (FragPayloadLimit is about 1136).
    Array<byte> big;
    big.Resize(5000);
    for (usize i = 0; i < big.Size(); ++i)
    {
        big[i] = static_cast<byte>((i * 7u + 3u) & 0xFFu);
    }
    fx.client.Send(sp, 0, big.AsSpan(), net::Reliability::ReliableOrdered);

    Array<byte> got;
    for (int s = 0; s < 600; ++s)
    {
        fx.Step();
        net::NetEvent ev;
        while (fx.server.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Received)
            {
                got = static_cast<Array<byte>&&>(ev.payload);
            }
        }
        if (got.Size() == 5000u)
        {
            break;
        }
    }
    REQUIRE(got.Size() == 5000u); // reassembled whole
    bool exact = true;
    for (usize i = 0; i < got.Size(); ++i)
    {
        if (static_cast<u8>(got[i]) != static_cast<u8>((i * 7u + 3u) & 0xFFu))
        {
            exact = false;
            break;
        }
    }
    CHECK(exact); // byte-exact after reassembly
}

namespace
{
    // Decorator that counts datagrams sent (to show resend timing is not per tick).
    struct CountingSocket final : net::IDatagramSocket
    {
        net::IDatagramSocket* inner;
        int sends = 0;
        explicit CountingSocket(net::IDatagramSocket* i) : inner(i) {}
        void Send(const net::DatagramEndpoint& to, Span<const byte> data) override
        {
            ++sends;
            inner->Send(to, data);
        }
        bool Receive(net::DatagramEndpoint& from, Array<byte>& out) override
        {
            return inner->Receive(from, out);
        }
        net::DatagramEndpoint LocalEndpoint() const override { return inner->LocalEndpoint(); }
    };
}

TEST_CASE("reliable: resend is RTT-timed, not per-tick (no flooding for an unacked message)")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::ReliableConfig cfg;
    cfg.minResendMs = 50.0f;
    cfg.keepAliveMs = 100.0f;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sa = network.CreateSocket();
    net::IDatagramSocket* sb = network.CreateSocket();
    CountingSocket counting(sa);
    net::ReliableTransport client(counting, cfg);
    net::ReliableTransport server(*sb, cfg);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(sb->LocalEndpoint());
    for (int i = 0; i < 20; ++i)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
    }

    // Send a reliable message, then let the server go dark so it never acks. Over 200ms with a 50ms
    // resend floor, the client should resend about 4 times (plus about 2 keepalives), not about 20
    // (once per 10ms tick).
    Array<byte> m = Bytes({0x42});
    client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    const int before = counting.sends;
    for (int i = 0; i < 20; ++i)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
    } // 200ms, server dark
    const int sent = counting.sends - before;
    CHECK(sent >= 2);  // it does keep retrying
    CHECK(sent <= 10); // but RTT-timed (~4 resends and ~2 keepalives), not per tick (~20)
}

TEST_CASE("reliable: explicit Disconnect notifies the peer")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)AnyKind(fx.server, net::NetEventKind::Connected);
    fx.client.Disconnect(sp);
    fx.Pump(10);
    CHECK(AnyKind(fx.server, net::NetEventKind::Disconnected));
}

// A reliable message in a connection's very first Data packet is resent when that packet is lost:
// the far end, having received nothing, acks nothing. The resend waits longer than the far end's
// first keepalive, so an ack of a packet never received would retire it first.
TEST_CASE("reliable: a reliable message in the first packet is resent when that packet is lost")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::ReliableConfig cfg;
    cfg.keepAliveMs = 50.0f;
    cfg.minResendMs = 500.0f;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* serverSocket = network.CreateSocket();
    DropFirstDataSocket dropping(*network.CreateSocket());
    net::ReliableTransport client(dropping, cfg);
    net::ReliableTransport server(*serverSocket, cfg);
    server.SetAccepting(true);

    const net::PeerId sp = client.Connect(serverSocket->LocalEndpoint());
    // Queued while connecting, so it goes in the first Data packet, which is lost.
    const Array<byte> msg = Bytes({42});
    client.Send(sp, 0, msg.AsSpan(), net::Reliability::ReliableOrdered);
    Array<u8> got;
    for (int step = 0; step < 200; ++step)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
        for (u8 v : DrainReceived(server))
        {
            got.PushBack(v);
        }
    }
    CHECK(dropping.dropped == 1);
    REQUIRE(got.Size() == 1u); // resent, and delivered once
    CHECK(got[0] == 42);
}

// A client that restarts and connects again from the same address starts a new session: the
// server ends the old one and accepts the new, whose messages arrive though their ids start over.
TEST_CASE("reliable: a client restarting at the same address connects again")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId first = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    for (u8 i = 0; i < 3; ++i)
    {
        const Array<byte> msg = Bytes({i});
        fx.client.Send(first, 0, msg.AsSpan(), net::Reliability::ReliableOrdered);
    }
    fx.Pump(20);
    CHECK(DrainReceived(fx.server).Size() == 3u);

    // The client process restarts on the same socket address.
    net::ReliableTransport restarted(*fx.sa);
    const net::PeerId second = restarted.Connect(fx.sb->LocalEndpoint());
    for (int i = 0; i < 20; ++i)
    {
        fx.net.Advance(10.0f);
        restarted.Update(10.0f);
        fx.server.Update(10.0f);
    }
    CHECK(AnyKind(restarted, net::NetEventKind::Connected));
    net::NetEvent ev;
    int disconnected = 0;
    int connected = 0;
    while (fx.server.Poll(ev))
    {
        disconnected += ev.kind == net::NetEventKind::Disconnected ? 1 : 0;
        connected += ev.kind == net::NetEventKind::Connected ? 1 : 0;
    }
    CHECK(disconnected == 1); // the old session ends
    CHECK(connected == 1);    // the new one starts

    const Array<byte> msg = Bytes({7});
    restarted.Send(second, 0, msg.AsSpan(), net::Reliability::ReliableOrdered);
    for (int i = 0; i < 20; ++i)
    {
        fx.net.Advance(10.0f);
        restarted.Update(10.0f);
        fx.server.Update(10.0f);
    }
    const Array<u8> after = DrainReceived(fx.server);
    REQUIRE(after.Size() == 1u); // the new session's first message arrives
    CHECK(after[0] == 7);
}

// A client that disconnects and connects again to the same server gets a working session.
TEST_CASE("reliable: a client that disconnected connects again")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId first = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    const Array<byte> one = Bytes({1});
    fx.client.Send(first, 0, one.AsSpan(), net::Reliability::ReliableOrdered);
    fx.Pump(20);
    fx.client.Disconnect(first);
    fx.Pump(10);
    CHECK(CountKind(fx.server, net::NetEventKind::Disconnected) == 1);

    const net::PeerId second = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    CHECK(AnyKind(fx.client, net::NetEventKind::Connected));
    const Array<byte> nine = Bytes({9});
    fx.client.Send(second, 0, nine.AsSpan(), net::Reliability::ReliableOrdered);
    fx.Pump(20);
    const Array<u8> got = DrainReceived(fx.server);
    REQUIRE(got.Size() == 1u); // the new session's message arrives
    CHECK(got[0] == 9);
}

// Sequenced messages never arrive out of order: over a link that reorders, a stale one is dropped
// rather than delivered after a newer one.
TEST_CASE("reliable: sequenced messages never arrive out of order")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.lossPct = 0.1f;
    sim.reorderPct = 0.5f;
    sim.seed = 11;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)AnyKind(fx.server, net::NetEventKind::Connected);

    Array<u8> got;
    for (u32 i = 0; i < 200; ++i)
    {
        const Array<byte> msg = Bytes({static_cast<u8>(i)});
        fx.client.Send(sp, 3, msg.AsSpan(), net::Reliability::UnreliableSequenced);
        fx.Step();
        for (u8 v : DrainReceived(fx.server))
        {
            got.PushBack(v);
        }
    }
    fx.Pump(20);
    for (u8 v : DrainReceived(fx.server))
    {
        got.PushBack(v);
    }
    CHECK(got.Size() > 50u); // most arrive
    for (usize i = 1; i < got.Size(); ++i)
    {
        CHECK(got[i] > got[i - 1]); // each newer than the one before
    }
}

namespace
{
    // A handshake packet as the transport writes it: protocol, type, token, and a cookie or padding.
    Array<byte> Handshake(u8 type, u32 token, u64 cookie, bool padded = true)
    {
        net::BitWriter w;
        w.WriteU16(0xDCE8);
        w.WriteU8(type);
        w.WriteU32(token);
        if (padded)
        {
            w.WriteU64(cookie);
        }
        Array<byte> out;
        for (byte b : w.Data())
        {
            out.PushBack(b);
        }
        return out;
    }

    // The packets a socket received, by type.
    struct Received
    {
        u8 type = 0;
        u32 token = 0;
        u64 cookie = 0;
        usize size = 0;
    };
    Array<Received> ReceiveAll(net::IDatagramSocket& socket)
    {
        Array<Received> out;
        net::DatagramEndpoint from;
        Array<byte> data;
        while (socket.Receive(from, data))
        {
            net::BitReader r(data.AsSpan());
            (void)r.ReadU16();
            Received p;
            p.type = r.ReadU8();
            p.token = r.ReadU32();
            p.size = data.Size();
            if (p.type == 4u)
            {
                p.cookie = r.ReadU64();
            }
            out.PushBack(p);
        }
        return out;
    }
}

TEST_CASE("reliable: a request alone makes no peer; it draws one challenge no bigger than itself")
{
    // A connection was made for any ConnectRequest, spoofed addresses included: a flood of them
    // filled the server with peers. Now a request is answered with a cookie and nothing is kept.
    net::SimConditions sim;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    net::IDatagramSocket* raw = fx.net.CreateSocket();
    const Array<byte> request = Handshake(0u, 1234u, 0u);
    raw->Send(fx.sb->LocalEndpoint(), request.AsSpan());
    raw->Send(fx.sb->LocalEndpoint(), Handshake(0u, 1234u, 0u, /*padded*/ false).AsSpan()); // too short to answer
    fx.Pump(5);
    CHECK(CountKind(fx.server, net::NetEventKind::Connected) == 0);
    const Array<Received> got = ReceiveAll(*raw);
    REQUIRE(got.Size() == 1u); // the padded one, once
    CHECK(got[0].type == 4u);  // a challenge
    CHECK(got[0].token == 1234u);
    CHECK(got[0].size <= request.Size());

    // A response with any other cookie makes no peer either; the challenge's does.
    raw->Send(fx.sb->LocalEndpoint(), Handshake(5u, 1234u, got[0].cookie ^ 1u).AsSpan());
    fx.Pump(5);
    CHECK(CountKind(fx.server, net::NetEventKind::Connected) == 0);
    CHECK(ReceiveAll(*raw).IsEmpty());
    raw->Send(fx.sb->LocalEndpoint(), Handshake(5u, 1234u, got[0].cookie).AsSpan());
    fx.Pump(5);
    CHECK(CountKind(fx.server, net::NetEventKind::Connected) == 1);
    const Array<Received> accepted = ReceiveAll(*raw);
    REQUIRE_FALSE(accepted.IsEmpty());
    CHECK(accepted[0].type == 1u); // ConnectAccept
}

TEST_CASE("reliable: a server at its peer limit refuses the next client, who stops")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::ReliableConfig cfg;
    cfg.maxPeers = 1;
    Fixture fx(sim, cfg);
    fx.server.SetAccepting(true);
    (void)fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    CHECK(AnyKind(fx.client, net::NetEventKind::Connected));

    net::IDatagramSocket* second = fx.net.CreateSocket();
    net::ReliableTransport late(*second, cfg);
    (void)late.Connect(fx.sb->LocalEndpoint());
    bool refused = false;
    bool connected = false;
    for (int step = 0; step < 40; ++step)
    {
        fx.Step();
        late.Update(10.0f);
        net::NetEvent ev;
        while (late.Poll(ev))
        {
            refused = refused || ev.kind == net::NetEventKind::Disconnected;
            connected = connected || ev.kind == net::NetEventKind::Connected;
        }
    }
    CHECK(refused);
    CHECK_FALSE(connected);
    CHECK(CountKind(fx.server, net::NetEventKind::Connected) == 1); // the first client only
}

namespace
{
    // Holds back the first Data packet that carries a message, for the test to replay.
    class HoldDataSocket final : public net::IDatagramSocket
    {
    public:
        explicit HoldDataSocket(net::IDatagramSocket& inner) : m_inner(inner) {}
        void Send(const net::DatagramEndpoint& to, Span<const byte> data) override
        {
            net::BitReader reader(data);
            (void)reader.ReadU16();
            const u8 type = reader.ReadU8();
            if (arm && held.IsEmpty() && type == 2u && data.Size() > 16u) // a Data packet with a message
            {
                for (byte b : data)
                {
                    held.PushBack(b);
                }
                heldTo = to;
                return;
            }
            m_inner.Send(to, data);
        }
        [[nodiscard]] bool Receive(net::DatagramEndpoint& from, Array<byte>& out) override { return m_inner.Receive(from, out); }
        [[nodiscard]] net::DatagramEndpoint LocalEndpoint() const override { return m_inner.LocalEndpoint(); }
        void SendRaw(Span<const byte> data) { m_inner.Send(heldTo, data); }

        bool arm = false;
        Array<byte> held;
        net::DatagramEndpoint heldTo;

    private:
        net::IDatagramSocket& m_inner;
    };
}

TEST_CASE("reliable: a data packet of another session at the peer's address is dropped")
{
    // Data packets carried no session token: one from an earlier session at the same address (or
    // forged with it) was read as this session's.
    net::SimDatagramNetwork network(DefaultAllocator(), net::SimConditions{});
    net::IDatagramSocket* clientRaw = network.CreateSocket();
    net::IDatagramSocket* serverSocket = network.CreateSocket();
    HoldDataSocket clientSocket(*clientRaw);
    net::ReliableTransport client(clientSocket);
    net::ReliableTransport server(*serverSocket);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(serverSocket->LocalEndpoint());
    const auto step = [&]
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
    };
    for (int i = 0; i < 20; ++i)
        step();
    REQUIRE(AnyKind(client, net::NetEventKind::Connected));
    (void)DrainReceived(server);

    clientSocket.arm = true;
    Array<byte> m = Bytes({42});
    client.Send(sp, 1, m.AsSpan(), net::Reliability::Unreliable);
    step();
    REQUIRE_FALSE(clientSocket.held.IsEmpty());
    Array<byte> forged = clientSocket.held;
    forged[3] = static_cast<byte>(static_cast<u8>(forged[3]) ^ 0xFFu); // the token's first byte
    clientSocket.SendRaw(forged.AsSpan());
    step();
    step();
    CHECK(DrainReceived(server).IsEmpty());
    clientSocket.SendRaw(clientSocket.held.AsSpan()); // the same packet with its own token
    step();
    step();
    const Array<u8> delivered = DrainReceived(server);
    REQUIRE(delivered.Size() == 1u);
    CHECK(delivered[0] == 42u);
}

namespace
{
    // Counts the Data packets bigger than a bare keepalive: the ones carrying a message.
    class MessagePacketCounter final : public net::IDatagramSocket
    {
    public:
        explicit MessagePacketCounter(net::IDatagramSocket& inner) : m_inner(inner) {}
        void Send(const net::DatagramEndpoint& to, Span<const byte> data) override
        {
            net::BitReader reader(data);
            (void)reader.ReadU16();
            // A keepalive from a client that has received nothing carries no ack fields: 11 bytes.
            if (reader.ReadU8() == 2u && data.Size() > 11u)
            {
                ++messagePackets;
            }
            m_inner.Send(to, data);
        }
        [[nodiscard]] bool Receive(net::DatagramEndpoint& from, Array<byte>& out) override { return m_inner.Receive(from, out); }
        [[nodiscard]] net::DatagramEndpoint LocalEndpoint() const override { return m_inner.LocalEndpoint(); }
        int messagePackets = 0;

    private:
        net::IDatagramSocket& m_inner;
    };
}

TEST_CASE("reliable: before a round trip is measured, resends back off")
{
    // With no RTT sample yet the resend timeout was the 20 ms floor: a message sent the moment a
    // connection opened, to a peer that went quiet, was resent fifty times a second.
    net::SimDatagramNetwork network(DefaultAllocator(), net::SimConditions{});
    net::IDatagramSocket* clientRaw = network.CreateSocket();
    net::IDatagramSocket* serverSocket = network.CreateSocket();
    MessagePacketCounter clientSocket(*clientRaw);
    net::ReliableConfig cfg; // initialResendMs 200, maxResendMs 1000
    net::ReliableTransport client(clientSocket, cfg);
    net::ReliableTransport server(*serverSocket, cfg);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(serverSocket->LocalEndpoint());
    bool connected = false;
    for (int step = 0; step < 50 && !connected; ++step)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
        connected = AnyKind(client, net::NetEventKind::Connected);
    }
    REQUIRE(connected);

    Array<byte> m = Bytes({7});
    client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    for (int step = 0; step < 200; ++step) // 2 s, the server silent
    {
        network.Advance(10.0f);
        client.Update(10.0f);
    }
    // Sent at 0 and resent after 200, 400, 800 and 1000 ms (the ceiling): at 0, 200, 600 and 1400.
    MESSAGE("message packets in 2 s: ", clientSocket.messagePackets);
    CHECK(clientSocket.messagePackets >= 3);
    CHECK(clientSocket.messagePackets <= 5);
}

namespace
{
    // The reliable message ids in a Data packet, read as the transport writes them.
    void ReliableIdsIn(Span<const byte> data, Array<u16>& out)
    {
        net::BitReader r(data);
        (void)r.ReadU16();
        if (r.ReadU8() != 2u)
            return;
        (void)r.ReadU32(); // token
        (void)r.ReadU16(); // seq
        if (r.ReadBool())
        {
            (void)r.ReadU16();
            (void)r.ReadU32();
        }
        const u32 count = r.ReadVarU32();
        for (u32 i = 0; i < count && r.Ok(); ++i)
        {
            (void)r.ReadU8();
            const bool reliable = r.ReadBool();
            u16 id = 0;
            if (!reliable)
            {
                if (r.ReadBool())
                    (void)r.ReadU16();
            }
            else
            {
                id = r.ReadU16();
                if (r.ReadBool())
                {
                    (void)r.ReadVarU32();
                    (void)r.ReadVarU32();
                    (void)r.ReadVarU32();
                }
            }
            const u32 len = r.ReadVarU32();
            for (u32 b = 0; b < len; ++b)
                (void)r.ReadU8();
            if (reliable && r.Ok())
                out.PushBack(id);
        }
    }

    class ReliableIdRecorder final : public net::IDatagramSocket
    {
    public:
        explicit ReliableIdRecorder(net::IDatagramSocket& inner) : m_inner(inner) {}
        void Send(const net::DatagramEndpoint& to, Span<const byte> data) override
        {
            ReliableIdsIn(data, ids);
            m_inner.Send(to, data);
        }
        [[nodiscard]] bool Receive(net::DatagramEndpoint& from, Array<byte>& out) override { return m_inner.Receive(from, out); }
        [[nodiscard]] net::DatagramEndpoint LocalEndpoint() const override { return m_inner.LocalEndpoint(); }
        Array<u16> ids;

    private:
        net::IDatagramSocket& m_inner;
    };

    // A connection made by hand: the handshake through a raw socket, with token `token`.
    bool RawConnect(net::SimDatagramNetwork& network, net::IDatagramSocket& raw, net::ReliableTransport& server,
                    const net::DatagramEndpoint& serverEndpoint, u32 token)
    {
        raw.Send(serverEndpoint, Handshake(0u, token, 0u).AsSpan());
        network.Advance(10.0f);
        server.Update(10.0f);
        network.Advance(10.0f);
        const Array<Received> challenge = ReceiveAll(raw);
        if (challenge.IsEmpty() || challenge[0].type != 4u)
            return false;
        raw.Send(serverEndpoint, Handshake(5u, token, challenge[0].cookie).AsSpan());
        network.Advance(10.0f);
        server.Update(10.0f);
        network.Advance(10.0f);
        (void)ReceiveAll(raw);
        return true;
    }

    // One Data packet carrying one reliable message of one byte.
    Array<byte> ReliableDataPacket(u32 token, u16 seq, u16 id, u8 value)
    {
        net::BitWriter w;
        w.WriteU16(0xDCE8);
        w.WriteU8(2u);
        w.WriteU32(token);
        w.WriteU16(seq);
        w.WriteBool(false); // no ack
        w.WriteVarU32(1u);
        w.WriteU8(0u);      // channel
        w.WriteBool(true);  // reliable
        w.WriteU16(id);
        w.WriteBool(false); // not fragmented
        w.WriteVarU32(1u);
        w.WriteU8(value);
        Array<byte> out;
        for (byte b : w.Data())
            out.PushBack(b);
        return out;
    }
}

TEST_CASE("reliable: a peer that stops acking past the queue cap is disconnected")
{
    // The unacked queue grew without end while a peer stayed connected and acked nothing.
    net::SimDatagramNetwork network(DefaultAllocator(), net::SimConditions{});
    net::IDatagramSocket* cs = network.CreateSocket();
    net::IDatagramSocket* ss = network.CreateSocket();
    net::ReliableConfig cfg;
    cfg.maxQueuedReliable = 10;
    net::ReliableTransport client(*cs, cfg);
    net::ReliableTransport server(*ss, cfg);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(ss->LocalEndpoint());
    for (int i = 0; i < 10; ++i)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
    }
    REQUIRE(AnyKind(client, net::NetEventKind::Connected));
    Array<byte> m = Bytes({1});
    for (int i = 0; i < 10; ++i) // the server is silent from here: nothing is acked
    {
        client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    CHECK(CountKind(client, net::NetEventKind::Disconnected) == 0);
    client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered); // the eleventh
    CHECK(CountKind(client, net::NetEventKind::Disconnected) == 1);
}

TEST_CASE("reliable: with nothing acked, no message past the window goes out")
{
    // The sender sent every queued message: ids could run past the receiver's buffer and, past
    // 32768 in flight, past where u16 ids still tell older from newer.
    net::SimDatagramNetwork network(DefaultAllocator(), net::SimConditions{});
    net::IDatagramSocket* cs = network.CreateSocket();
    net::IDatagramSocket* ss = network.CreateSocket();
    ReliableIdRecorder recorder(*cs);
    net::ReliableTransport client(recorder);
    net::ReliableTransport server(*ss);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(ss->LocalEndpoint());
    for (int i = 0; i < 10; ++i)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
    }
    REQUIRE(AnyKind(client, net::NetEventKind::Connected));
    Array<byte> m = Bytes({1});
    for (int i = 0; i < 3000; ++i)
    {
        client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    for (int i = 0; i < 300; ++i) // 3 s, the server silent
    {
        network.Advance(10.0f);
        client.Update(10.0f);
    }
    REQUIRE_FALSE(recorder.ids.IsEmpty());
    u16 highest = 0;
    for (u16 id : recorder.ids)
        highest = Max(highest, id);
    CHECK(highest == 1023u); // ids 0 to 1023, the window from the oldest unacked
}

TEST_CASE("reliable: a message past the receive window is not buffered")
{
    // Early arrivals were buffered at any distance ahead: a peer could park 32767 messages each.
    net::SimDatagramNetwork network(DefaultAllocator(), net::SimConditions{});
    net::IDatagramSocket* raw = network.CreateSocket();
    net::IDatagramSocket* ss = network.CreateSocket();
    net::ReliableTransport server(*ss);
    server.SetAccepting(true);
    REQUIRE(RawConnect(network, *raw, server, ss->LocalEndpoint(), 99u));
    REQUIRE(CountKind(server, net::NetEventKind::Connected) == 1);

    u16 seq = 0;
    const auto deliver = [&](u16 id)
    {
        raw->Send(ss->LocalEndpoint(), ReliableDataPacket(99u, seq++, id, static_cast<u8>(id)).AsSpan());
        network.Advance(10.0f);
        server.Update(10.0f);
    };
    deliver(1029); // 1029 ahead of the next expected id, 0: past the window of 1024
    for (u16 id = 0; id < 1029; ++id)
    {
        deliver(id);
    }
    CHECK(CountKind(server, net::NetEventKind::Received) == 1029); // 0 to 1028; 1029 was never kept
    deliver(1029); // sent again once inside the window, it goes through
    CHECK(CountKind(server, net::NetEventKind::Received) == 1);
}

namespace
{
    // One Data packet carrying one fragment of `bytes` bytes.
    Array<byte> FragmentPacket(u32 token, u16 seq, u16 id, u32 group, u32 index, u32 count, u32 bytes)
    {
        net::BitWriter w;
        w.WriteU16(0xDCE8);
        w.WriteU8(2u);
        w.WriteU32(token);
        w.WriteU16(seq);
        w.WriteBool(false);
        w.WriteVarU32(1u);
        w.WriteU8(0u);
        w.WriteBool(true);
        w.WriteU16(id);
        w.WriteBool(true); // fragmented
        w.WriteVarU32(group);
        w.WriteVarU32(index);
        w.WriteVarU32(count);
        w.WriteVarU32(bytes);
        for (u32 b = 0; b < bytes; ++b)
            w.WriteU8(static_cast<u8>(b));
        Array<byte> out;
        for (byte b : w.Data())
            out.PushBack(b);
        return out;
    }
}

TEST_CASE("reliable: a fragmented message over maxMessageBytes is not reassembled")
{
    // The limit was checked when sending only: a peer's fragments were appended without end.
    net::SimDatagramNetwork network(DefaultAllocator(), net::SimConditions{});
    net::IDatagramSocket* raw = network.CreateSocket();
    net::IDatagramSocket* ss = network.CreateSocket();
    net::ReliableConfig cfg;
    cfg.maxMessageBytes = 4096;
    net::ReliableTransport server(*ss, cfg);
    server.SetAccepting(true);
    REQUIRE(RawConnect(network, *raw, server, ss->LocalEndpoint(), 7u));
    REQUIRE(CountKind(server, net::NetEventKind::Connected) == 1);

    u16 seq = 0;
    u16 id = 0;
    const auto send = [&](u32 group, u32 index, u32 count, u32 bytes)
    {
        raw->Send(ss->LocalEndpoint(), FragmentPacket(7u, seq++, id++, group, index, count, bytes).AsSpan());
        network.Advance(10.0f);
        server.Update(10.0f);
    };
    for (u32 i = 0; i < 6; ++i) // six fragments of 1000 bytes: 6000, over 4096
        send(1u, i, 6u, 1000u);
    send(2u, 0u, 100000u, 10u); // a first fragment claiming more pieces than 4096 bytes can hold
    CHECK(CountKind(server, net::NetEventKind::Received) == 0);

    for (u32 i = 0; i < 3; ++i) // then one that fits: delivered whole
        send(3u, i, 3u, 1000u);
    net::NetEvent ev;
    usize size = 0;
    int received = 0;
    while (server.Poll(ev))
    {
        if (ev.kind == net::NetEventKind::Received)
        {
            ++received;
            size = ev.payload.Size();
        }
    }
    CHECK(received == 1);
    CHECK(size == 3000u);
}
