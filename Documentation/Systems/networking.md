# Networking

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[networking-track]] / [[game-instance-track]]

Reflection-driven, server-authoritative networking, shipped end to end and wired into
`DefaultApplication` per game instance. A game sets a role, marks component fields `Replicated`,
and gets authoritative, fog-of-war-filtered, interpolated replication with prefab net-spawn +
late-join - no hand-written per-component net code. First target is real-time strategy; the
`IReplicationModel` seam keeps other genres open.

## Modules

- **`foundation.net`** - transport: reliable-UDP + TCP + loopback/sim + the wire codec. Raw
  sockets live in `Core/System` (the platform backend pattern - `System.cppm` + per-OS dirs),
  not here; `foundation.net` is the reliability/session layer above them. Hosts are a
  dotted-quad literal OR a DNS name everywhere a host is taken (`ResolveEndpoint`,
  `TcpSocket::Connect`, `Net.connect`, `HttpFetch`): `Core::ResolveHostIPv4` parses a literal
  without a lookup and otherwise blocks on the platform resolver (getaddrinfo, first A
  record) - resolve at the connect edge, never per frame. IPv4 only; IPv6 is deferred.
  `ReliableTransport` (`ReliableConfig`) has a connect handshake with a server cookie
  (`Challenge` / `ConnectResponse`) and a client-picked session token on every data packet,
  backs resends off until the round trip is measured, holds reliable messages to a window and
  caps the unacked queue (`maxQueuedReliable`, past which the peer is dropped), the peers a server
  holds (`maxPeers`) and a reassembled message (`maxMessageBytes`). No congestion control.
- **`foundation.net.tls`** - the TLS seam: `ITlsBackend` makes `ITlsSession`s (client or
  server, `TlsClientConfig` / `TlsServerConfig`, TLS 1.2 and 1.3, ALPN), and `INetByteStream`
  carries a connection's plaintext as `PlainStream` (http://, ws://) or `TlsStream` (https://,
  wss://) over a `TcpSocket`. A client checks the certificate against a `TlsTrust`: the system's
  roots (the default), a game's own PEM roots, or none (tests only). `LoadSystemRoots` reads the
  distribution's bundle on Linux and SteamOS, the ROOT store on Windows, and on Android the
  system's certificate folder (`/apex/com.android.conscrypt/cacerts`, then
  `/system/etc/security/cacerts`) through `ReadCertificateFolder`, which joins a folder of PEM
  files into one bundle; a web build has none (the browser does TLS). The Android player's
  manifest asks for the `INTERNET` permission, without which every socket is refused.
- **`foundation.net.tls.mbedtls`** - the first TLS backend, over Mbed TLS (built thread-safe):
  `CreateMbedTlsBackend`, plus `CreateSelfSignedCertificate` for tests and local hosts. The
  `GameInstance` makes one on first use (`TlsBackend()`, none in a web build).
- **`foundation.net.websocket`** - WebSocket for browser players, who have no UDP. The
  `WebSocketServerGateway` accepts connections, upgrades them through `foundation.http`, and speaks
  RFC 6455 binary frames (no fragments, no text frames, a 1 MiB frame cap, deadlines for the
  upgrade and for a quiet client); with a `WebSocketTls` (backend, certificate chain, key) it
  speaks wss. `WebSocketHybridSocket` wraps the host's UDP socket and the gateway as one
  `IDatagramSocket`, so native and browser peers join the same session (WebSocket endpoints are
  synthetic, bit 63 set). `WebSocketClientSocket` is the client: the browser's WebSocket in a web
  build, natively this module's own client over a plain or TLS stream, so a desktop, Deck or
  Android client can join a host that only speaks wss. `ParseWebSocketAddress` reads a
  `ws://` / `wss://` join address.
- **`foundation.http`** - the sibling HTTP/1.1 layer over the TCP wrappers: incremental message
  parser, a pump-model `HttpServer` (one request per connection, `Connection: close`) with
  Server-Sent Events streams (`SseStream`, ref-counted, held past the response) and deferred
  answers (a handler answers "not yet" and is called again every Pump), and a blocking
  `HttpFetch` client (a host name or dotted quad, no TLS - local tooling and tests). Consumers: the
  MCP streamable-HTTP host, the WebSocket upgrade path, loopback tooling/tests.
- **`foundation.http.client`** - the game-facing HTTP client, `IHttpClient`: requests go out
  and their responses come back by callback inside the caller's `Pump`. The native client
  (`CreateNativeHttpClient`) runs each request on a worker over a `PlainStream` or a `TlsStream`,
  with redirects (never https to http, credentials dropped across origins), a timeout over the
  whole request, cancel, a response size cap and streamed responses for downloads. The browser
  client (`CreateBrowserHttpClient`) is the browser's fetch. `CreateHttpClient` picks the
  platform's. No keep-alive, HTTP/2, cookies or proxies.
- **`foundation.http.script`** - the `Http` script facade: `Http.get` / `post` / `send` /
  `cancel`, the handler hearing `(status, body, error)` (status 0 and a reason when no response
  came). The run holds an `HttpScriptBinding` as a service; its client is made on the first
  request and pumped each frame. Registered by the run domain (`RegisterHttpScriptFacade`).
- **`foundation.net.manager`** - `NetworkManager` (a session endpoint) + `INetworkController` +
  the `Net` script-facade binding. A GameInstance owns its OWN networking and goes online at
  RUNTIME via the facade (no app-owned socket). The endpoint + role lifecycle + `INetworkController`
  impl live on `engine::runtime::NetworkController` (`engine.gameinstance`), which the GameInstance
  COMPOSES and registers with its run as the `INetworkController` and `NetScriptBinding` services.
  `NetworkManager::HostServer` takes an optional WebSocket port (and `WebSocketTls`);
  `JoinServer` takes a bare host (UDP natively, a WebSocket in a browser) or a `ws://` / `wss://`
  address (the gateway, natively too, wss checked through the run's TLS backend).
- **`foundation.net.replication`** - `StateReplication` + the `IReplicationModel` seam.
- **`engine.net`** - the net scene-integration: `NetworkSubsystem` (registers the reflected components)
  plus `NetworkSceneSystem` and the engine-level `AddNetworkSceneManagers` installer (installed
  through the net domain's `installScene`, `NetDomainImpl.cpp`) that adds the net component managers
  + the scene system. `NetworkSceneSystem::OnFixedUpdate` drives the endpoint's `UpdateReplication`
  on the per-scene FIXED lane (deterministic, physics-lockstep); the per-instance
  `NetworkController` points it at the live endpoint only for the endpoint's current replicated scene.
  The subsystem also OWNS the per-frame TRANSPORT pump: `NetworkSubsystem::PostUpdate` visits every
  run of the run manager, takes its `INetworkController` service, and drives the live endpoint's
  `UpdateTransport` (socket recv/send) on the Context lane, holding the run's mutations so a message
  handler that stops networking or replaces the scene takes effect after the pump.
  `NetworkManager::Update` is split into `UpdateTransport` (per-frame) and `UpdateReplication`
  (per-scene fixed lane).

## Ports

A host started with a fixed port (`NetworkController::StartServer`) listens on UDP `port` and
opens a WebSocket gateway on `port + 1` (plain ws://); a host on an OS-assigned port (tests,
tools) is UDP only. `NetworkController::Connect` sends a browser, or a `ws://` / `wss://` address
anywhere, to `port + 1` unless the address names its own port. A certificate for the gateway
itself is native code only (`WebSocketTls` to `HostServer`); a TLS reverse proxy in front of the
gateway is the usual way to serve browser players from an https page
(`Documentation/Shipping/Hosting.md`).

## Roles + startup

`NetworkStartup` carries the role (`NetworkRole` None/Server/Client) + preset; the app's role-preset
startup is a thin call into the instance's controller. Per-instance: each `GameInstance` has its own
endpoint and goes online at runtime via the `Net` facade (`Net.startServer` / `Net.connect`), so a
single process can host multiple instances. The prefab net-spawn resolver is injected once as a factory
onto each instance's `NetworkController` (`Network().SetSpawnResolverFactory`); the controller makes one per endpoint and installs it, so reconnect keeps it. The app provides only
the content-DB-backed factory (`MakeSpawnResolver`); it no longer wires the endpoint itself.

## Replication (`StateReplication`)

Mark component fields `Replicated` (a reflection attribute) and the model does the rest:
- the server builds a snapshot and a PER-PEER delta (reflection-driven field codec);
- **prefab net-spawn** replicates spawned entities; a joining client gets a full-snapshot
  **late-join** sync (first-class for this target - reconnect brings full state);
- **a joiner may wait for its map** (`NetworkManager::SetWaitForReady`, `Net.waitForReady(true)` on
  the host): a client that loads the host's map after connecting hears no state, only RPCs (the
  host's welcome naming the map), until it says it is ready (`SendReady`, `Net.ready()`, a byte on
  the replication channel, the one thing a client sends there); the host then forgets its
  baseline, so its first delta is the full snapshot. Off by default: state flows at once;
- clients **interpolate** replicated state for smoothness;
- **per-peer relevancy / fog-of-war** filters what each client sees - interest management is a
  SECURITY requirement here (a client never receives what it should not see), not just bandwidth.
  A game's own hook (`SetRelevance`) decides; without one, a built-in distance relevance can be
  turned on (`SetRelevanceRadius`, `Net.setRelevanceRadius(r)` on the host, kept in the run's
  binding for every endpoint it opens): a player is sent what lies within `r` of the first entity
  it owns, its own always, and everything when it owns none. Off by default (0): all goes to all.

No client prediction / reconciliation / lag compensation and no lockstep determinism are needed
for the current target (server-authoritative), which is what shipped.

## The model seam (`IReplicationModel`)

`StateReplication` is one model behind the `IReplicationModel` seam - the escape hatch that keeps
the design multi-genre without disturbing what shipped. A twitch-shooter unreliable-snapshot+ack
model, or a large-scale-RTS lockstep `CommandReplication`, can slot BESIDE it. Target-specific
choices (e.g. the reliable-ordered delta baseline) are POLICY of the current send path, not baked
into the wire format or the seam. A future lockstep `CommandReplication` would re-raise the
determinism caveat (lockstep needs bit-exact simulation - not promised by state replication).

## Script facade

The `Net` facade (registered by `RegisterNetScriptFacade` through the net domain, resolved per
script through the run's `NetScriptBinding` service, `CurrentService`): `Net.isServer()` /
`isClient()` / `peerCount()` / `networkTick()` / `networkTimeMs()`, `startServer(port)` /
`startDedicatedServer(port)` / `connect(host, port)` / `disconnect()`, `Net.canHost()` (false in a browser, which cannot listen, so a
title offers Host only where it works; `NetworkManager::CanHost`), plus SEND-side RPC (`Net.rpc(...)` / `rpcNumber(...)` /
`rpcText(...)`), and the RECEIVE side, `Net.on(name, fn)`: the
handler is called with the sender's peer id, then the number or the text, if any. The sends write
a one-byte kind (`ScriptRpcKind`) after the RPC id so the receiver knows what follows. Handlers
live in the instance's `NetScriptBinding`, so a script registers them before it hosts or joins and
every endpoint the controller opens gets them (`InstallScriptRpcHandlers`); AngelScript gets the
handler types `NetHandler`, `NetNumberHandler` and `NetTextHandler` through the script layer's
delegate shapes (`RegisterDelegateShape`). `Net.spawn(prefab, x, y, z)`, host only: spawns the prefab in the replicated scene through the scene's own
`PrefabSpawnSystem` (as `scene.prefabs.spawn`) and assigns its NetworkId with the prefab, so each client's
spawn handler builds its copy and a late joiner gets it in the snapshot; null on a client.
Players: a script names a player by an id, 0 the host and a client
the PeerId the host gave it, which the session's `Welcome` control message (sent reliably as the
host accepts) tells the client (`NetSession::LocalPeer`). `NetworkComponent.owner` is that id,
set by `Net.spawnFor(player, prefab, x, y, z)` (`spawn` is the host's) and sent in the spawn
record after the prefab, so every peer knows whose an entity is (`Net.isMine(entity)`,
`NetworkComponent.of(e).owner`). `Net.localPlayer()`; `Net.rpcTo` / `rpcNumberTo` / `rpcTextTo`,
host to one player; `Net.onJoin(fn(player))` / `Net.onLeave(fn(player))` through
`NetworkManager::SetPlayerEvents` (the host hears clients come and go; a client hears its own
join once welcomed, and the host leave as 0). On a client a `Net.on` handler's sender is 0, the
host. Orders are RPCs the host checks itself (it knows the sender); who leaves and what happens to
their entities is the game's call in its `onLeave`.
Script behaviour properties replicate when marked:
`ScriptPropertyDesc::replicated`, from AngelScript's `replicated` metadata word or a Luau class's
`replicated` list (the cook refuses an unknown word or name, and an entity or asset property).
Replication reaches them through `scene::IReplicatedStateSource`, a component manager's own state
beside reflected `net.replicated` fields: `ScriptComponentManager` writes each networked entity's
replicated properties (live from the script object on the host), and a client writes them into
its behaviour (an override, plus the setter when the instance runs). The blob is compared with each
peer's last, so only a change is sent.
`NetworkComponent.of(entity).authority` exposes
ownership to script. Certified on AngelScript + Luau (see `Integration.ScriptFacades`; Wren was
retired 2026-08-19).

## Deferred

- The script command layer the plan proposed (receive RPCs, spawn, players and owners) is built
  (above); `Documentation/Plans/networking-commands.md` is the original plan.
- Replication refinements, congestion control, IPv6, NAT traversal, matchmaking and services:
  `Documentation/Archive/networking-followups-history.md` keeps the list.
- A dedicated-server launch of the player with its own TLS files is planned
  (`Documentation/Shipping/Hosting.md`).
- **Unification with the legacy networking stack**: `Documentation/Plans/net-unification.md`.

---

Design rationale (roll-your-own transport + the escape hatch, sockets-in-Core, HTTP-as-sibling,
the genre-tuning analysis, the Traktor references) is in
`Documentation/Archive/networking-design-history.md`.
