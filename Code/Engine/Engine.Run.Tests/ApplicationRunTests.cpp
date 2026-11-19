// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.run - the run bracket owns the script run state and time scale.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import engine.run;
import foundation.runtime;      // SubsystemManager/Subsystem
import foundation.scene;
import foundation.scene.resource;    // SceneDocument and the LoadScene round-trip
import foundation.content;           // ContentDatabase / Instance
import foundation.resource;          // ResourceManager
import foundation.vfs;               // NativeFileSystem
import foundation.script;
import foundation.script.facades; // RegisterScriptFacadeReflection (the Scene facade)
import foundation.script.resource; // BehaviorClass (the cooked Game class a run starts from)
import foundation.script.angelscript;
import foundation.script.luau;
import foundation.net;         // NetSession queries (IsServer/PeerCount)
import foundation.net.manager; // NetworkManager (the endpoint the instance owns)
import foundation.net.replication; // RegisterReplicationComponents (net scene managers)
import engine.net;                 // NetworkSceneSystem (the endpoint <-> scene wiring under test)
import foundation.input;       // ActionRuntime / IInputSourceProvider (per-instance input)
import foundation.shell;       // IKeyboard / KeyCode (a minimal fake device)
import foundation.http;        // HttpServer (a local server the run's client fetches from)
import foundation.http.script; // RegisterHttpScriptFacade (the run domain registers it in an app)
import foundation.run;         // IRun (an ApplicationRun is one)
import engine.behavior;          // BehaviorRunHost (one of the run's services)

using namespace foundation::core;
namespace script = foundation::script; // raw manager/context for the run facade battery
namespace scene = foundation::scene;
namespace messaging = foundation::messaging; // EventBus
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace input = foundation::input;
namespace shell = foundation::shell;
using foundation::vfs::NativeFileSystem;

namespace
{
    // A minimal input source: one keyboard reporting a single held key, everything else absent.
    class OneKeyKeyboard final : public shell::IKeyboard
    {
    public:
        shell::KeyCode key{};
        bool down = false;
        [[nodiscard]] bool IsKeyDown(shell::KeyCode k) const override { return down && k == key; }
        [[nodiscard]] bool IsKeyPressed(shell::KeyCode k) const override
        {
            return down && k == key;
        }
        [[nodiscard]] bool IsKeyReleased(shell::KeyCode) const override { return false; }
        [[nodiscard]] shell::KeyModifiers Modifiers() const override
        {
            return shell::KeyModifiers::None;
        }
    };
    class OneKeySource final : public input::IInputSourceProvider
    {
    public:
        OneKeyKeyboard keyboard;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* Gamepad(i32) override { return nullptr; }
        [[nodiscard]] shell::ITouch* Touch() override { return nullptr; }
    };
    // A mouse that records the lock asked of it, for a run's end freeing it.
    class LockMouse final : public shell::IMouse
    {
    public:
        bool relative = false;
        bool cursorVisible = true;
        [[nodiscard]] f32 X() const override { return 0.0f; }
        [[nodiscard]] f32 Y() const override { return 0.0f; }
        [[nodiscard]] f32 GlobalX() const override { return 0.0f; }
        [[nodiscard]] f32 GlobalY() const override { return 0.0f; }
        [[nodiscard]] f32 DeltaX() const override { return 0.0f; }
        [[nodiscard]] f32 DeltaY() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollY() const override { return 0.0f; }
        [[nodiscard]] bool IsButtonDown(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonPressed(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return relative; }
        void SetRelativeMode(bool enabled) override { relative = enabled; }
        [[nodiscard]] bool CursorVisible() const override { return cursorVisible; }
        void SetCursorVisible(bool visible) override { cursorVisible = visible; }
        void SetCursor(shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };
    class MouseSource final : public input::IInputSourceProvider
    {
    public:
        OneKeyKeyboard keyboard;
        LockMouse mouse;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* Gamepad(i32) override { return nullptr; }
    };
    [[nodiscard]] input::InputMap MakeFireMap(shell::KeyCode key)
    {
        input::InputMap map;
        input::ActionSet set;
        set.name = String(u8"S");
        input::Action a;
        a.name = String(u8"fire");
        a.kind = input::ActionKind::Button;
        input::Binding b;
        b.source = input::BindingSource::Key;
        b.code = static_cast<u32>(key);
        a.bindings.PushBack(b);
        set.actions.PushBack(static_cast<input::Action&&>(a));
        map.sets.PushBack(static_cast<input::ActionSet&&>(set));
        return map;
    }
}

TEST_CASE("game-instance: instance time scale defaults to 1 and is settable; fresh instance idle")
{
    engine::runtime::ApplicationRun gi;
    CHECK(gi.InstanceTimeScale() == doctest::Approx(1.0f));
    gi.SetInstanceTimeScale(0.5f);
    CHECK(gi.InstanceTimeScale() == doctest::Approx(0.5f));
    CHECK_FALSE(gi.RunBehaviorRunning());
    CHECK(gi.GetScene() == nullptr);
    CHECK(gi.ScriptContext() == nullptr);
    CHECK_FALSE(
        gi.RunHost().IsActive()); // the instance owns its run host (idle until a run starts)

    // The instance owns a usable scene group (its SceneManager).
    CHECK(gi.Scenes().SceneCount() == 0u);
    scene::Scene* level = gi.Scenes().CreateScene(u8"L1");
    REQUIRE(level != nullptr);
    CHECK(gi.Scenes().SceneCount() == 1u);
    CHECK(gi.Scenes().CurrentScene() == level);
}

TEST_CASE("game-instance: an instance is its scenes' and scripts' run")
{
    // Run-scoped systems (audio) group a run's scenes and its script calls by one key: the
    // instance. Two instances are two runs.
    engine::runtime::ApplicationRun a;
    engine::runtime::ApplicationRun b;
    scene::Scene* level = a.Scenes().CreateScene(u8"L1");
    REQUIRE(level != nullptr);
    CHECK(level->Run() == &a);
    CHECK(a.RunHost().Binding().run == &a);
    CHECK(b.RunHost().Binding().run == &b);

    // A scene adopted into the run joins it.
    scene::SceneManager elsewhere{DefaultAllocator()};
    scene::Scene* adopted = elsewhere.CreateScene(u8"page");
    CHECK(adopted->Run() == nullptr);
    b.SetScene(adopted);
    CHECK(adopted->Run() == &b);
    CHECK(adopted->Events() == &b.Events()); // and its bus with it
    b.SetScene(nullptr);
}

// The run as a type: an ApplicationRun is its IRun, its scenes use the run's own bus, and what it
// owns is reached through the run.
TEST_CASE("game-instance: the instance is an IRun, its scenes on the run's bus, its services through the run")
{
    engine::runtime::ApplicationRun gi;
    foundation::run::IRun& run = gi;
    CHECK(run.Id() != 0);
    CHECK(run.State() == foundation::run::RunState::Idle);
    scene::Scene* level = gi.Scenes().CreateScene(u8"L1");
    REQUIRE(level != nullptr);
    CHECK(level->Events() == &run.Events()); // one bus: the run's

    CHECK(run.Get<engine::runtime::ApplicationRun>() == &gi);
    CHECK(run.Get<engine::runtime::NetworkController>() == &gi.Network());
    CHECK(run.Get<input::ActionRuntime>() == &gi.InputRuntime());
    CHECK(run.Get<engine::runtime::RunSave>() == &gi.Saves());
    CHECK(run.Get<engine::behavior::BehaviorRunHost>() == &gi.RunHost());
    CHECK(run.Get<foundation::http::HttpScriptBinding>() == &gi.Http());

    gi.Start();
    gi.SetPaused(true);
    CHECK(run.State() == foundation::run::RunState::Paused);
    gi.Stop();
    CHECK(run.State() == foundation::run::RunState::Idle);
}

TEST_CASE("game-instance: a run's end frees a pointer its game locked")
{
    // A run ends through StopRunBehavior (the player's and the editor's stop, and a restart), the path
    // that stops its pads too.
    engine::runtime::ApplicationRun gi;
    MouseSource source;
    gi.SetInputSource(&source);
    gi.SetInputMap(MakeFireMap(shell::KeyCode::H));
    gi.InputRuntime().SetPointerLock(true); // as Input.lockPointer asks it
    gi.DriveInput(0.016f, 1.0f);
    REQUIRE(source.mouse.relative);
    REQUIRE_FALSE(source.mouse.cursorVisible);

    gi.StopRunBehavior();
    CHECK_FALSE(source.mouse.relative);
    CHECK(source.mouse.cursorVisible);
    CHECK_FALSE(gi.InputRuntime().PointerLockApplied()); // the next run starts unlocked

    // A run that never locked leaves a pointer someone else holds alone (an editor viewport's).
    source.mouse.relative = true;
    gi.DriveInput(0.016f, 1.0f);
    gi.StopRunBehavior();
    CHECK(source.mouse.relative);
}

TEST_CASE("game-instance: each instance's input runtime reads ONLY its own source (per-instance "
          "isolation)")
{
    // Each ApplicationRun has its own ActionRuntime bound to its own source, so one tab's keys never
    // reach another tab's game (a shared runtime would let a key in the server tab drive the
    // client). Same map, same key, two sources: only the source with the key held fires.
    engine::runtime::ApplicationRun a;
    engine::runtime::ApplicationRun b;
    OneKeySource srcA;
    srcA.keyboard.key = shell::KeyCode::H;
    srcA.keyboard.down = true; // A holds H
    OneKeySource srcB;
    srcB.keyboard.key = shell::KeyCode::H;
    srcB.keyboard.down = false; // B does not
    a.SetInputSource(&srcA);
    a.SetInputMap(MakeFireMap(shell::KeyCode::H));
    b.SetInputSource(&srcB);
    b.SetInputMap(MakeFireMap(shell::KeyCode::H));

    a.DriveInput(0.016f, 1.0f);
    b.DriveInput(0.016f, 1.0f);

    CHECK(a.InputRuntime().IsDown(a.InputRuntime().Resolve(u8"fire")) == true); // A's source has it
    CHECK(b.InputRuntime().IsDown(b.InputRuntime().Resolve(u8"fire")) ==
          false); // B's does not (no cross-feed)

    // Flip which source holds the key: isolation holds the other way too.
    srcA.keyboard.down = false;
    srcB.keyboard.down = true;
    a.DriveInput(0.016f, 1.0f);
    b.DriveInput(0.016f, 1.0f);
    CHECK(a.InputRuntime().IsDown(a.InputRuntime().Resolve(u8"fire")) == false);
    CHECK(b.InputRuntime().IsDown(b.InputRuntime().Resolve(u8"fire")) == true);
}

TEST_CASE("game-instance: each instance owns an independent networked endpoint (server + client "
          "over UDP)")
{
    // The per-instance networking model: an ApplicationRun composes a NetworkController (the
    // INetworkController), which opens its own real UDP endpoint on StartServer/Connect. The
    // ApplicationRun forwards to it. Two instances are two isolated endpoints.
    engine::runtime::ApplicationRun server;
    engine::runtime::ApplicationRun client;
    CHECK(server.NetEndpoint() == nullptr); // offline until a role is entered

    REQUIRE(server.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(server.NetEndpoint() != nullptr);
    CHECK(server.NetEndpoint()->Session().IsServer());
    const u16 port = server.NetEndpoint()->BoundPort();
    CHECK(port != 0u);

    REQUIRE(client.Connect(u8"127.0.0.1", port));
    REQUIRE(client.NetEndpoint() != nullptr);
    CHECK(client.NetEndpoint()->Session().IsClient());
    CHECK(server.NetEndpoint() != client.NetEndpoint()); // independent endpoints

    for (int i = 0; i < 400 && server.NetEndpoint()->Session().PeerCount() == 0u; ++i)
    {
        // The transport pump is NetworkManager::UpdateTransport.
        server.NetEndpoint()->UpdateTransport(16.0f);
        client.NetEndpoint()->UpdateTransport(16.0f);
        SleepMilliseconds(1);
    }
    CHECK(server.NetEndpoint()->Session().PeerCount() == 1u);

    client.StopNetworking(); // disconnect drops the endpoint
    CHECK(client.NetEndpoint() == nullptr);
    CHECK(server.NetEndpoint() != nullptr); // the server is unaffected (isolation)
}

// A message's handler that stops the networking, as a script's Net.disconnect in a Net.on handler
// does: the endpoint is released at once (the run is offline) and freed after the pump returns.
TEST_CASE("game-instance: networking stopped from inside the pump is freed after it")
{
    engine::runtime::ApplicationRun server;
    engine::runtime::ApplicationRun client;
    REQUIRE(server.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(client.Connect(u8"127.0.0.1", server.NetEndpoint()->BoundPort()));
    bool offlineInHandler = false;
    server.NetEndpoint()->Rpc().On(u8"leave",
                                   [&server, &offlineInHandler](foundation::net::PeerId, foundation::net::BitReader&)
                                   {
                                       server.StopNetworking();
                                       offlineInHandler = server.NetEndpoint() == nullptr;
                                       server.StartServer(/*port=*/0, /*dedicated=*/true); // a new role, at once
                                   });
    // As the network subsystem pumps a run: its mutations deferred, flushed after.
    const auto pump = [](engine::runtime::ApplicationRun& gi)
    {
        if (foundation::net::NetworkManager* endpoint = gi.NetEndpoint())
        {
            {
                foundation::run::RunMutations::Scope scripts(gi.Mutations());
                endpoint->UpdateTransport(16.0f);
            }
            gi.Mutations().Flush();
        }
    };
    for (int i = 0; i < 400 && server.NetEndpoint()->Session().PeerCount() == 0u; ++i)
    {
        pump(server);
        pump(client);
        SleepMilliseconds(1);
    }
    REQUIRE(server.NetEndpoint()->Session().PeerCount() == 1u);
    foundation::net::NetworkManager* first = server.NetEndpoint();
    client.NetEndpoint()->Rpc().Call(client.NetEndpoint()->Session(), client.NetEndpoint()->Session().ServerPeer(), u8"leave");
    client.NetEndpoint()->Rpc().Call(client.NetEndpoint()->Session(), client.NetEndpoint()->Session().ServerPeer(), u8"leave");
    for (int i = 0; i < 400 && !offlineInHandler; ++i)
    {
        pump(client);
        pump(server); // the handler frees nothing under the pump
        SleepMilliseconds(1);
    }
    CHECK(offlineInHandler);
    REQUIRE(server.NetEndpoint() != nullptr);
    CHECK(server.NetEndpoint() != first); // the new role's endpoint
    CHECK(server.NetEndpoint()->Session().PeerCount() == 0u);
    CHECK(server.Mutations().PendingCount() == 0u); // the old one freed at the flush
}

// A ws:// join address: a native client joins the host's browser gateway, at the game port + 1 (the
// StartServer convention) since the address names none.
TEST_CASE("game-instance: a ws:// address joins the host's gateway at port + 1, natively")
{
    engine::runtime::ApplicationRun server;
    engine::runtime::ApplicationRun client;
    u16 port = 0;
    {
        // Two free ports in a row (the game's and its gateway's), searched among ordinary ports from
        // a start that differs run to run. Not beside an ephemeral port the system hands out: on
        // Windows the next one up is often taken or reserved.
        const u32 start = 20000u + static_cast<u32>(GetUnixTime() % 1000) * 30u;
        for (u32 candidate = start; candidate < start + 2000u && port == 0; candidate += 2)
        {
            foundation::net::TcpListener game(static_cast<u16>(candidate), true);
            foundation::net::TcpListener gateway(static_cast<u16>(candidate + 1), true);
            if (game.IsOpen() && gateway.IsOpen())
                port = static_cast<u16>(candidate);
        }
    }
    REQUIRE(port != 0);
    REQUIRE(server.StartServer(port, /*dedicated=*/true));
    REQUIRE(server.NetEndpoint() != nullptr);
    REQUIRE(server.NetEndpoint()->WebSocketBoundPort() == static_cast<u16>(port + 1));

    REQUIRE(client.Connect(u8"ws://127.0.0.1", port));
    REQUIRE(client.NetEndpoint() != nullptr);
    CHECK(client.NetEndpoint()->BoundPort() == 0u); // a WebSocket, not a UDP socket
    for (int i = 0; i < 2000 && server.NetEndpoint()->Session().PeerCount() == 0u; ++i)
    {
        server.NetEndpoint()->UpdateTransport(16.0f);
        client.NetEndpoint()->UpdateTransport(16.0f);
        SleepMilliseconds(1);
    }
    CHECK(server.NetEndpoint()->Session().PeerCount() == 1u);
}

TEST_CASE("network-controller: role lifecycle - start, stop, reconnect, and the online hook")
{
    // NetworkController, tested directly: it owns the endpoint and the INetworkController role. The
    // app-injected spawn resolver is applied to every endpoint the controller opens (a reconnect
    // too; it is not consumed), asserted through Replication().HasSpawnHandler(). The final
    // destruction with a live endpoint exercises the socket and session teardown path (ASAN).
    engine::runtime::NetworkController controller;
    controller.SetSpawnResolverFactory(
        []() -> foundation::net::StateReplication::SpawnHandler
        {
            return [](foundation::scene::Scene&, const Guid&,
                      foundation::net::NetworkId) -> foundation::scene::EntityHandle
            { return foundation::scene::EntityHandle::Invalid(); };
        });

    CHECK(controller.NetEndpoint() == nullptr); // offline until a role is entered

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(controller.NetEndpoint() != nullptr);
    CHECK(controller.NetEndpoint()->Session().IsServer());
    CHECK(controller.NetEndpoint()->BoundPort() != 0u);
    CHECK(controller.NetEndpoint()->Replication().HasSpawnHandler()); // resolver applied to the endpoint

    controller.StopNetworking();
    CHECK(controller.NetEndpoint() == nullptr);     // the endpoint drops

    // Reconnect: a fresh endpoint, and the resolver is applied again (the controller owns it).
    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(controller.NetEndpoint() != nullptr);
    CHECK(controller.NetEndpoint()->Replication().HasSpawnHandler());
    // controller destructs here holding a live endpoint: the socket and session teardown path.
}

TEST_CASE("network-controller: a fresh endpoint replicates the cached scene")
{
    // SetReplicatedScene cached before going online is applied to the endpoint at StartServer (a
    // server assigns NetworkIds from the scene), and a live change is forwarded to the running
    // endpoint too.
    engine::runtime::NetworkController controller;
    scene::SceneManager scenes{DefaultAllocator()};
    scene::Scene* level = scenes.CreateScene(u8"Level");
    REQUIRE(level != nullptr);
    controller.SetReplicatedScene(level); // cached while offline

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(controller.NetEndpoint() != nullptr); // came up; the cached scene was applied to it

    controller.SetReplicatedScene(nullptr); // live change forwarded to the running endpoint (clean)
    controller.StopNetworking();
    CHECK(controller.NetEndpoint() == nullptr);
}

TEST_CASE("network-controller: the replicated scene's NetworkSceneSystem drives the endpoint; "
          "stop detaches; reconnect re-wires (P2)")
{
    // The endpoint is wired into only the replicated scene's NetworkSceneSystem, at the
    // controller's edges. Endpoint dies before scene: StopNetworking detaches the system before the
    // endpoint dies. Stop, start and reconnect against a live scene re-wires. (ASAN covers the
    // teardown: the controller and scene destruct here holding a live endpoint.)
    foundation::net::RegisterReplicationComponents();
    engine::runtime::NetworkController controller;
    scene::Scene s{DefaultAllocator()};
    engine::net::AddNetworkSceneManagers(s); // installs the NetworkSceneSystem
    engine::net::NetworkSceneSystem* sys = s.GetSystem<engine::net::NetworkSceneSystem>();
    REQUIRE(sys != nullptr);

    controller.SetReplicatedScene(&s);
    CHECK(sys->Endpoint() == nullptr); // attached while offline: inert (no endpoint yet)

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    CHECK(sys->Endpoint() == controller.NetEndpoint()); // the scene's fixed lane now drives the endpoint

    controller.StopNetworking();
    CHECK(sys->Endpoint() == nullptr); // detached before the endpoint died (endpoint-dies-before-scene)
    CHECK(controller.NetEndpoint() == nullptr);

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true)); // reconnect on the live scene
    CHECK(sys->Endpoint() == controller.NetEndpoint()); // re-wired to the fresh endpoint
}

TEST_CASE("game-instance: destroying the replicated scene clears the endpoint's scene (no dangling)")
{
    // Scene dies before endpoint. DestroyScene of the current scene runs SetScene(nullptr) first,
    // so the endpoint's replicated-scene pointer and the controller's cache clear before the scene
    // is freed. A later transport pump or reconnect never touches dead memory (ASAN is the real
    // assertion here).
    engine::runtime::ApplicationRun gi;
    scene::Scene* level = gi.CreateScene(u8"Level");
    REQUIRE(level != nullptr);
    gi.SetScene(level);
    REQUIRE(gi.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(gi.NetEndpoint() != nullptr);

    gi.DestroyScene(level); // level == current -> SetScene(nullptr) clears the endpoint's scene + cache
    CHECK(gi.GetScene() == nullptr);     // controller cache cleared
    CHECK(gi.NetEndpoint() != nullptr);  // still online (the endpoint outlives the scene)

    gi.NetEndpoint()->UpdateTransport(16.0f); // transport pump must not touch the freed scene
    REQUIRE(gi.StartServer(/*port=*/0, /*dedicated=*/true));  // reconnect with no current scene is safe
    gi.StopNetworking();
}

TEST_CASE("game-instance: fallback path starts, ticks, and stops a Game script")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    const bool ok = gi.StartRunBehavior(u8"class Game : RunBehavior {\n"
                                   u8"  Game() {}\n"
                                   u8"  void onStart() {}\n"
                                   u8"  void onUpdate(double dt) {}\n"
                                   u8"  void onStop() {}\n"
                                   u8"}\n",
                                   u8"game.as", u8"Game");
    REQUIRE(ok);
    CHECK(gi.RunBehaviorRunning());
    CHECK(gi.ScriptContext() != nullptr);
    CHECK(gi.RunHost().IsActive()); // the game script runs on the instance's own run host

    gi.RunHost().Advance(0.016f); // advance the run host clock/GC (must not fault)
    gi.TickRunBehavior(0.016f, 1.0f); // must not fault
    CHECK(gi.RunBehaviorRunning());

    gi.StopRunBehavior();
    CHECK_FALSE(gi.RunBehaviorRunning());
}

TEST_CASE("game-instance: starts, ticks, and stops a LUAU Game orchestrator (the Game tier)")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    const bool ok = gi.StartRunBehavior(u8"Game = {}\n"
                                   u8"Game.__index = Game\n"
                                   u8"function Game.new() return setmetatable({}, Game) end\n"
                                   u8"function Game:onStart() end\n"
                                   u8"function Game:onUpdate(dt) end\n"
                                   u8"function Game:onStop() end\n",
                                   u8"game.luau", u8"Game"); // the .luau extension resolves the Luau backend
    REQUIRE(ok);
    CHECK(gi.RunBehaviorRunning());
    CHECK(gi.ScriptContext() != nullptr);
    CHECK(gi.RunHost().IsActive());

    gi.RunHost().Advance(0.016f); // advance the run host clock/GC (must not fault)
    gi.TickRunBehavior(0.016f, 1.0f); // update() (must not fault)
    CHECK(gi.RunBehaviorRunning());

    gi.StopRunBehavior();
    CHECK_FALSE(gi.RunBehaviorRunning());
}

// The run's HTTP: a game script's Http.get goes through the client the run makes on its first
// request, and the answer reaches the handler on the run's tick. Stopping the script drops what is
// still under way: its handler is never called.
TEST_CASE("game-instance: a game script fetches over the run's HTTP client; stopping drops what waits")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();
    foundation::http::RegisterHttpScriptFacade();
    foundation::http::HttpServer server(DefaultAllocator());
    REQUIRE(server.Start(foundation::http::HttpServerConfig{}));
    server.SetHandler(
        [](const foundation::http::HttpRequest& request) -> Optional<foundation::http::HttpResponse>
        {
            if (request.target.AsView() == StringView(u8"/slow"))
                return {}; // never answered
            return foundation::http::HttpResponse::Text(200, u8"text/plain", u8"best: 12.5");
        });

    engine::runtime::ApplicationRun gi;
    CHECK(gi.Http().Pending() == 0);
    const String source = Format(u8"Got = ''\n"
                                 u8"Late = 0\n"
                                 u8"Game = {{}}\n"
                                 u8"Game.__index = Game\n"
                                 u8"function Game.new() return setmetatable({{}}, Game) end\n"
                                 u8"function Game:onStart()\n"
                                 u8"  Http.get('http://127.0.0.1:{}/best', function(status, body, err) Got = status .. ' ' .. body end)\n"
                                 u8"  Http.get('http://127.0.0.1:{}/slow', function() Late = 1 end)\n"
                                 u8"end\n"
                                 u8"function Game:onUpdate(dt) end\n"
                                 u8"function Game:onStop() end\n",
                                 server.BoundPort(), server.BoundPort());
    REQUIRE(gi.StartRunBehavior(source.AsView(), u8"game.luau", u8"Game"));
    for (u32 i = 0; i < 5000 && gi.ScriptContext()->GetGlobal(u8"Got").Get<String>().IsEmpty(); ++i)
    {
        (void)server.Pump();
        gi.TickRunBehavior(0.016f, 1.0f);
        SleepMilliseconds(1);
    }
    CHECK(gi.ScriptContext()->GetGlobal(u8"Got").Get<String>() == StringView(u8"200 best: 12.5"));
    CHECK(gi.Http().Pending() == 1); // the slow one
    gi.StopRunBehavior();
    CHECK(gi.Http().Pending() == 0);
}

// The game script launches before any scene, so a `class Game` orchestrator's launch()/update() may
// call the Scene and run facades with no current scene. Two properties under test: the facades are
// null-scene-safe (no dereference of a null currentScene; the run must launch and tick without
// faulting), and the load facade binds in lowercase as `run`, not Game. A facade named Game is a
// hard AngelScript name conflict with the mandatory `Game` orchestrator class, so this test
// compiling at all guards that.
TEST_CASE("game-instance: Scene/run facades are null-scene-safe from a pre-scene "
          "orchestrator")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade(); // run facade (owned by this project)
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    CHECK_FALSE(gi.SceneReady()); // no scene yet: the orchestrator-first condition
    const bool ok = gi.StartRunBehavior(
        u8"class Game : RunBehavior {\n"
        u8"  Game() {}\n"
        u8"  void onStart() {\n"
        u8"    run::currentScene().find(\"nobody\");\n"
        u8"    run::currentScene().findByPath(\"a/b\");\n"
        u8"    run::sceneReady();\n"
        u8"    run::loadComplete(0);\n"
        u8"    run::loadProgress(0);\n"
        u8"  }\n"
        u8"  void onUpdate(double dt) {\n"
        u8"    run::currentScene().find(\"x\");\n"
        u8"    run::sceneReady();\n"
        u8"  }\n"
        u8"  void onStop() {}\n"
        u8"}\n",
        u8"game.as", u8"Game");
    REQUIRE(ok); // compiled (no Game-name clash) and launch() ran the pre-scene facade calls, no fault
    CHECK(gi.RunBehaviorRunning());
    gi.RunHost().Advance(0.016f);
    gi.TickRunBehavior(0.016f, 1.0f); // update() calls them again; still no fault
    CHECK(gi.RunBehaviorRunning());
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: Scene/run facades are null-scene-safe from a pre-scene "
          "orchestrator (AngelScript)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade(); // run facade (owned by this project)
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    // A facade named `Game` would fail here with "Name conflict. 'Game' is an extended data type";
    // binding the load facade in lowercase as `run` is what lets this `class Game` compile beside
    // it.
    const bool ok = gi.StartRunBehavior(
        u8"class Game : RunBehavior {\n"
        u8"  Game() {}\n"
        u8"  void onStart() {\n"
        u8"    run::currentScene().find(\"nobody\");\n"
        u8"    run::currentScene().findByPath(\"a/b\");\n"
        u8"    run::sceneReady();\n"
        u8"    run::loadComplete(0);\n"
        u8"    run::loadProgress(0);\n"
        u8"  }\n"
        u8"  void onUpdate(double dt) { run::currentScene().find(\"x\"); "
        u8"run::sceneReady(); }\n"
        u8"  void onStop() {}\n"
        u8"}\n",
        u8"game.as", u8"Game");
    REQUIRE(ok);
    CHECK(gi.RunBehaviorRunning());
    gi.RunHost().Advance(0.016f);
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(gi.RunBehaviorRunning());
    gi.StopRunBehavior();
}

// The run.* load facade routing, end to end on both backends (raw context, as the Net tests do): a
// fake RunBinding (standing in for the app's content-DB-backed load pointers) is installed as
// the run.runtime service, then a script starts an async load and polls the ticket to completion.
// Under test: the facade-to-binding routing and the ticket round-trip.
namespace
{
    // Fake load host: hands back a fixed ticket, reports complete on the second poll of that
    // ticket.
    struct RunFake
    {
        engine::runtime::RunBinding binding;
        int asyncCalls = 0;
        Guid requested;
        int completePolls = 0;
        i32 ticketSeen = -1;
        static constexpr i32 kTicket = 7;

        RunFake()
        {
            binding.loadSceneAsync = Function<i32(const Guid&)>{[this](const Guid& id) -> i32
                                                               {
                                                                   ++asyncCalls;
                                                                   requested = id;
                                                                   return kTicket;
                                                               }};
            binding.loadProgress = Function<f64(i32)>{
                [](i32 t) -> f64 { return t == kTicket ? 0.5 : -1.0; }};
            binding.loadComplete = Function<bool(i32)>{[this](i32 t) -> bool
                                                       {
                                                           ++completePolls;
                                                           ticketSeen = t;
                                                           return completePolls >= 2;
                                                       }};
        }
    };
}

TEST_CASE("game-instance: run.loadSceneAsync -> ticket, polled to completion (Luau)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::CreateLuauScriptManager(DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> scriptContext = manager->CreateContext();

    RunFake fake;
    foundation::script::ScriptServices services; // a bare context: the test answers its lookups
    services.Add(fake.binding);
    scriptContext->SetServiceResolver(services.Resolver());

    // Start the load, then poll to completion; record the observable results in module globals.
    const Status status = scriptContext->Load(u8"t = run.loadSceneAsync(Guid.new(0xABC, 0xDEF))\n"
                                    u8"Poll1 = run.loadComplete(t)\n"
                                    u8"Prog = run.loadProgress(t)\n"
                                    u8"Poll2 = run.loadComplete(t)\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(fake.asyncCalls == 1);
    CHECK(fake.requested == Guid{0xABC, 0xDEF});
    CHECK(fake.ticketSeen == RunFake::kTicket); // the exact ticket round-tripped
    CHECK(scriptContext->GetGlobal(u8"Poll1").Get<bool>() == false); // first poll: not complete
    CHECK(scriptContext->GetGlobal(u8"Prog").Get<f64>() == doctest::Approx(0.5));
    CHECK(scriptContext->GetGlobal(u8"Poll2").Get<bool>() == true); // second poll: complete
}

TEST_CASE("game-instance: run.loadSceneAsync -> ticket, polled to completion (AngelScript)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> scriptContext = manager->CreateContext();

    RunFake fake;
    foundation::script::ScriptServices services; // a bare context: the test answers its lookups
    services.Add(fake.binding);
    scriptContext->SetServiceResolver(services.Resolver());

    const Status status = scriptContext->Load(u8"int t;\n"
                                    u8"bool Poll1; double Prog; bool Poll2;\n"
                                    u8"void main() {\n"
                                    u8"  t = run::loadSceneAsync(Guid(0xABC, 0xDEF));\n"
                                    u8"  Poll1 = run::loadComplete(t);\n"
                                    u8"  Prog = run::loadProgress(t);\n"
                                    u8"  Poll2 = run::loadComplete(t);\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(fake.asyncCalls == 1);
    CHECK(fake.requested == Guid{0xABC, 0xDEF});
    CHECK(fake.ticketSeen == RunFake::kTicket);
    CHECK(scriptContext->GetGlobal(u8"Poll1").Get<bool>() == false);
    CHECK(scriptContext->GetGlobal(u8"Prog").Get<f64>() == doctest::Approx(0.5));
    CHECK(scriptContext->GetGlobal(u8"Poll2").Get<bool>() == true);
}

// run.requestExit routing on both backends: a fake RunBinding whose requestExit records the
// code stands in for the app's host route (standalone stops the loop; the editor stops the Game
// tab). A small script calls run::requestExit(2) / run.requestExit(2), and the arity-0 overload
// routes code 0. Under test: the facade-to-binding routing and the arity family, not the host
// itself.
namespace
{
    struct ExitFake
    {
        engine::runtime::RunBinding binding;
        int calls = 0;
        i32 lastCode = -999;

        ExitFake()
        {
            binding.requestExit = Function<void(i32)>{[this](i32 code)
                                                      {
                                                          ++calls;
                                                          lastCode = code;
                                                      }};
        }
    };
}

TEST_CASE("game-instance: run.requestExit routes the exit code to the binding (AngelScript)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> scriptContext = manager->CreateContext();

    ExitFake fake;
    foundation::script::ScriptServices services; // a bare context: the test answers its lookups
    services.Add(fake.binding);
    scriptContext->SetServiceResolver(services.Resolver());

    // The 1-arg overload routes the code; the 0-arg overload routes code 0.
    const Status status = scriptContext->Load(u8"void main() {\n"
                                    u8"  run::requestExit(2);\n"
                                    u8"  run::requestExit();\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(fake.calls == 2);
    CHECK(fake.lastCode == 0); // the trailing no-arg call routed code 0
}

TEST_CASE("game-instance: run.requestExit routes the exit code to the binding (Luau)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::CreateLuauScriptManager(DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> scriptContext = manager->CreateContext();

    ExitFake fake;
    foundation::script::ScriptServices services; // a bare context: the test answers its lookups
    services.Add(fake.binding);
    scriptContext->SetServiceResolver(services.Resolver());

    const Status status = scriptContext->Load(u8"run.requestExit(2)\n"
                                    u8"Seen = 1\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(fake.calls == 1);
    CHECK(fake.lastCode == 2); // the exact code round-tripped through the facade

    // The arity-0 overload routes code 0.
    const Status status0 = scriptContext->Load(u8"run.requestExit()\n", u8"main0");
    REQUIRE(status0.IsOk());
    CHECK(fake.calls == 2);
    CHECK(fake.lastCode == 0);
}

TEST_CASE("game-instance: a debugger suspension in update is not a fault - the script survives")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    const bool ok = gi.StartRunBehavior(u8"class Game : RunBehavior {\n"               // 1
                                   u8"  void onStart() {}\n"         // 2
                                   u8"  void onUpdate(double dt) {\n" // 3
                                   u8"    int a = 1;\n"             // 4  <- breakpoint
                                   u8"    int b = a + 1;\n"         // 5
                                   u8"  }\n"
                                   u8"  void onStop() {}\n"
                                   u8"}\n",
                                   u8"game.as", u8"Game");
    REQUIRE(ok);
    REQUIRE(gi.RunBehaviorRunning());

    foundation::script::IScriptDebugger* debugger = nullptr;
    gi.RunHost().RequestDebugger(Function<void(foundation::script::IScriptDebugger&)>{
        [&debugger](foundation::script::IScriptDebugger& created)
        {
            created.SetBreakpoint(u8"game.as", 4);
            debugger = &created;
        }});
    REQUIRE(debugger != nullptr);

    struct BreakCounter final : foundation::script::IScriptDebuggerListener
    {
        int breaks = 0;
        void OnDebuggerStateChanged(foundation::script::ScriptDebuggerState state) override
        {
            if (state == foundation::script::ScriptDebuggerState::Breakpoint)
            {
                ++breaks;
            }
        }
    };
    BreakCounter counter;
    gi.RunHost().SetExternalDebugListener(&counter);

    // Hitting the breakpoint suspends update mid-call. The suspension surfaces as an error result,
    // and TickRunBehavior must not read it as a fault and kill the game script.
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(gi.RunHost().IsDebugPaused());
    CHECK(gi.RunBehaviorRunning()); // the script survives the pause
    CHECK(counter.breaks == 1);

    // Ticking while paused must not start a new update call (it would break again every frame and
    // flicker the locals): no new pause events, still paused, still alive.
    gi.TickRunBehavior(0.016f, 1.0f);
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(counter.breaks == 1);
    CHECK(gi.RunHost().IsDebugPaused());
    CHECK(gi.RunBehaviorRunning());

    // Continue completes the held call; the next tick hits the breakpoint again.
    debugger->Continue();
    CHECK_FALSE(gi.RunHost().IsDebugPaused());
    CHECK(gi.RunBehaviorRunning());
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(gi.RunHost().IsDebugPaused());
    CHECK(gi.RunBehaviorRunning());
    CHECK(counter.breaks == 2);

    // Removing the breakpoint while paused: Continue finishes the held call and the next ticks run
    // freely (the remove-during-pause flow, at the backend level).
    debugger->RemoveBreakpoint(u8"game.as", 4);
    debugger->Continue();
    gi.TickRunBehavior(0.016f, 1.0f);
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK_FALSE(gi.RunHost().IsDebugPaused());
    CHECK(gi.RunBehaviorRunning());
    CHECK(counter.breaks == 2);

    gi.RunHost().SetExternalDebugListener(nullptr);
    gi.StopRunBehavior();
    CHECK_FALSE(gi.RunBehaviorRunning());
}

TEST_CASE("game-instance: two instances own separate, isolated run-host contexts")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    const char8_t* src =
        u8"class Game { Game() {}\n void onStart() {}\n void onUpdate(double dt) {}\n void onStop() {}\n}\n";
    engine::runtime::ApplicationRun a;
    engine::runtime::ApplicationRun b;
    REQUIRE(a.StartRunBehavior(src, u8"game.as", u8"Game"));
    REQUIRE(b.StartRunBehavior(src, u8"game.as", u8"Game"));

    REQUIRE(a.RunHost().ScriptContext() != nullptr);
    REQUIRE(b.RunHost().ScriptContext() != nullptr);
    CHECK(a.RunHost().ScriptContext() !=
          b.RunHost().ScriptContext()); // distinct contexts = no shared script globals

    a.SetHeadless(true);
    CHECK(a.IsHeadless());
    CHECK_FALSE(b.IsHeadless());

    a.StopRunBehavior();
    b.StopRunBehavior();
    CHECK_FALSE(a.RunBehaviorRunning());
    CHECK_FALSE(b.RunBehaviorRunning());
}

TEST_CASE("game-instance: the run-scoped event bus - deferred delivery, order, cascade, isolation")
{
    RegisterCoreTypes();

    engine::runtime::ApplicationRun gi;
    messaging::EventBus& bus = gi.Events();

    // Deferred delivery, payload and subscription order (the native run bus, no script).
    Array<i32> order;
    i32 received = 0;
    (void)bus.Subscribe(StringHash(StringView(u8"Ping")),
                        [&](const Variant& p) { order.PushBack(1); received = p.Get<i32>(); });
    (void)bus.Subscribe(StringHash(StringView(u8"Ping")), [&](const Variant&) { order.PushBack(2); });
    bus.Publish(StringHash(StringView(u8"Ping")), Variant::From<i32>(42));
    CHECK(order.IsEmpty()); // nothing fires before Drain (delivery is deferred)
    gi.DrainRunEvents();
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == 1); // subscription order preserved
    CHECK(order[1] == 2);
    CHECK(received == 42); // payload delivered

    // Cascade: a handler's Publish is delivered within the same drain.
    bool cascaded = false;
    (void)bus.Subscribe(StringHash(StringView(u8"First")), [&](const Variant&)
                        { bus.Publish(StringHash(StringView(u8"Second")), Variant{}); });
    (void)bus.Subscribe(StringHash(StringView(u8"Second")), [&](const Variant&) { cascaded = true; });
    bus.Publish(StringHash(StringView(u8"First")), Variant{});
    gi.DrainRunEvents();
    CHECK(cascaded);

    // Two instances own independent buses (the multi-instance correctness check).
    engine::runtime::ApplicationRun other;
    i32 aHits = 0, bHits = 0;
    (void)gi.Events().Subscribe(StringHash(StringView(u8"Only")), [&](const Variant&) { ++aHits; });
    (void)other.Events().Subscribe(StringHash(StringView(u8"Only")),
                                      [&](const Variant&) { ++bHits; });
    gi.Events().Publish(StringHash(StringView(u8"Only")), Variant{});
    gi.DrainRunEvents();
    other.DrainRunEvents();
    CHECK(aHits == 1);
    CHECK(bHits == 0); // other's bus never saw gi's event
}

TEST_CASE("game-instance: the Game tier's on<Event> inbox harvests the run bus (AngelScript)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    const String handlers[] = {String(u8"onPing")}; // the cooked BehaviorClass's handler list
    const bool ok = gi.StartRunBehavior(u8"double Pinged = 0;\n"
                                   u8"class Game : RunBehavior {\n"
                                   u8"  Game() {}\n"
                                   u8"  void onStart() {}\n"
                                   u8"  void onUpdate(float dt) {}\n"
                                   u8"  void onStop() {}\n"
                                   u8"  void onPing(int x) { Pinged = x; }\n"
                                   u8"}\n",
                                   u8"game.as", u8"Game", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* scriptContext = gi.RunHost().ScriptContext();
    REQUIRE(scriptContext != nullptr);
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0)); // nothing fired yet

    // C++ publishes on the run bus -> onPing(42) fires on the Game class at the next drain.
    gi.Events().Publish(StringHash(StringView(u8"Ping")), Variant::From<i32>(42));
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0)); // deferred until Drain
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0)); // delivered with payload

    // An event with no matching on<Event> handler is ignored (only declared handlers subscribe).
    gi.Events().Publish(StringHash(StringView(u8"Unheard")), Variant::From<i32>(99));
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0)); // unchanged

    gi.StopRunBehavior();
}

// The cooked class carries its handlers: a game started from it (the player's and the Game tab's
// way) hears the run bus without the caller spelling the list out.
TEST_CASE("game-instance: a Game started from its cooked class hears the run bus through its handlers")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    foundation::script::BehaviorClass cooked;
    cooked.language = String(u8"angelscript");
    cooked.className = String(u8"Game");
    cooked.kind = foundation::script::BehaviorKind::Run;
    cooked.sourceName = String(u8"game.as");
    cooked.source = String(u8"double Reached = 0;\n"
                           u8"class Game : RunBehavior {\n"
                           u8"  Game() {}\n"
                           u8"  void onStart() {}\n"
                           u8"  void onUpdate(float dt) {}\n"
                           u8"  void onStop() {}\n"
                           u8"  void onGoalReached(int x) { Reached = x + 1; }\n"
                           u8"}\n");
    cooked.handlers.PushBack(String(u8"onGoalReached"));

    engine::runtime::ApplicationRun gi;
    REQUIRE(gi.StartRunBehavior(cooked));
    auto* scriptContext = gi.RunHost().ScriptContext();
    REQUIRE(scriptContext != nullptr);
    gi.Events().Publish(StringHash(StringView(u8"GoalReached")), Variant::From<i32>(0));
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Reached").Get<f64>() == doctest::Approx(1.0));
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: the Game tier's on<Event> inbox harvests the run bus (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    const String handlers[] = {String(u8"onPing")};
    const bool ok = gi.StartRunBehavior(u8"Pinged = 0\n"
                                   u8"Game = {}\n"
                                   u8"Game.__index = Game\n"
                                   u8"function Game.new() return setmetatable({}, Game) end\n"
                                   u8"function Game:onStart() end\n"
                                   u8"function Game:onUpdate(dt) end\n"
                                   u8"function Game:onStop() end\n"
                                   u8"function Game:onPing(x) Pinged = x end\n",
                                   u8"game.luau", u8"Game", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* scriptContext = gi.ScriptContext();
    REQUIRE(scriptContext != nullptr);
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0));

    // C++ publishes on the run bus -> Game:onPing(42) fires at drain (deferred).
    gi.Events().Publish(StringHash(StringView(u8"Ping")), Variant::From<i32>(42));
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0));

    // A non-subscribed event is ignored.
    gi.Events().Publish(StringHash(StringView(u8"Unheard")), Variant::From<i32>(99));
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0));

    gi.StopRunBehavior();
}

TEST_CASE("game-instance: run.events().emit publishes on the run bus, round-tripping to on<Event> (AS)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade(); // the run.* facade (bound as `run`)
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    const String handlers[] = {String(u8"onPing")};
    const bool ok = gi.StartRunBehavior(
        u8"double Pinged = 0;\n"
        u8"class Game : RunBehavior {\n"
        u8"  Game() {}\n"
        u8"  void onStart() { run::events().emit(\"Ping\", 7); }\n" // publish on the run bus from script
        u8"  void onUpdate(float dt) {}\n"
        u8"  void onStop() {}\n"
        u8"  void onPing(int x) { Pinged = x; }\n" // the Game's own inbox hears it
        u8"}\n",
        u8"game.as", u8"Game", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* scriptContext = gi.RunHost().ScriptContext();
    REQUIRE(scriptContext != nullptr);
    // launch() emitted, but the run bus delivers deferred: nothing fires until the drain.
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0));
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(7.0)); // script -> run bus -> script
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: run.setTimeScale routes from script to the run binding")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    f32 captured = -1.0f;
    gi.Binding().setTimeScale = Function<void(f32)>{[&captured](f32 s) { captured = s; }};
    const bool ok = gi.StartRunBehavior(u8"class Game : RunBehavior {\n"
                                   u8"  Game() {}\n"
                                   u8"  void onStart() { run::setTimeScale(0.5f); }\n" // pause/slow-mo API
                                   u8"  void onUpdate(float dt) {}\n"
                                   u8"  void onStop() {}\n"
                                   u8"}\n",
                                   u8"game.as", u8"Game", Span<const String>{});
    REQUIRE(ok);
    CHECK(captured == doctest::Approx(0.5f)); // launch() -> run::setTimeScale -> binding (synchronous)
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: the host's pause holds the Game class still: no update, the run's clock stopped")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    int updates = 0;
    // Each update reports itself through the binding (run::setTimeScale), so the test counts them.
    gi.Binding().setTimeScale = Function<void(f32)>{[&updates](f32) { ++updates; }};
    REQUIRE(gi.StartRunBehavior(u8"class Game : RunBehavior {\n"
                           u8"  Game() {}\n"
                           u8"  void onStart() {}\n"
                           u8"  void onUpdate(float dt) { run::setTimeScale(1.0f); }\n"
                           u8"  void onStop() {}\n"
                           u8"}\n",
                           u8"game.as", u8"Game", Span<const String>{}));
    gi.Start();
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(updates == 1);
    const f64 before = gi.RunTime();

    gi.SetPaused(true); // the editor's toolbar
    gi.TickRunBehavior(0.016f, 1.0f);
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(updates == 1);              // a level clock in update does not run on
    CHECK(gi.RunTime() == before);    // nor does the run's

    gi.SetPaused(false);
    gi.TickRunBehavior(0.016f, 1.0f);
    CHECK(updates == 2);
    CHECK(gi.RunTime() > before);
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: run.setTimeScale/timeScale route from script to the run binding (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    f32 captured = -1.0f;
    gi.Binding().setTimeScale = Function<void(f32)>{[&captured](f32 s) { captured = s; }};
    gi.Binding().timeScale = Function<f32()>{[&captured]() { return captured; }};
    const bool ok = gi.StartRunBehavior(u8"Scale = -1\n"
                                   u8"Game = {}\n"
                                   u8"Game.__index = Game\n"
                                   u8"function Game.new() return setmetatable({}, Game) end\n"
                                   u8"function Game:onStart()\n"
                                   u8"  run.setTimeScale(0.5)\n" // pause/slow-mo API
                                   u8"  Scale = run.timeScale()\n" // read it back through the binding
                                   u8"end\n"
                                   u8"function Game:onUpdate(dt) end\n"
                                   u8"function Game:onStop() end\n",
                                   u8"game.luau", u8"Game", Span<const String>{});
    REQUIRE(ok);
    CHECK(captured == doctest::Approx(0.5f)); // launch() -> run.setTimeScale -> binding (synchronous)
    auto* scriptContext = gi.ScriptContext();
    REQUIRE(scriptContext != nullptr);
    // The exact scale round-tripped back through run.timeScale() (getter reads the binding).
    CHECK(scriptContext->GetGlobal(u8"Scale").Get<f64>() == doctest::Approx(0.5));
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: run.setProfilerOverlay/profilerAvailable route to the app, a no-op unwired")
{
    // Every build binds both calls (a script compiles the same in Shipping); the app decides what
    // they do, and an app that wires nothing leaves them harmless.
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    const char8_t* source = u8"bool Available = true;\n"
                            u8"class Game : RunBehavior {\n"
                            u8"  Game() {}\n"
                            u8"  void onStart() { Available = run::profilerAvailable(); run::setProfilerOverlay(true); }\n"
                            u8"  void onUpdate(float dt) {}\n"
                            u8"  void onStop() { run::setProfilerOverlay(false); }\n"
                            u8"}\n";

    {
        engine::runtime::ApplicationRun unwired;
        REQUIRE(unwired.StartRunBehavior(source, u8"game.as", u8"Game", Span<const String>{}));
        REQUIRE(unwired.ScriptContext() != nullptr);
        CHECK_FALSE(unwired.ScriptContext()->GetGlobal(u8"Available").Get<bool>());
        unwired.StopRunBehavior();
    }

    engine::runtime::ApplicationRun gi;
    Array<bool> calls(DefaultAllocator());
    gi.Binding().setProfilerOverlay = Function<void(bool)>{[&calls](bool shown) { calls.PushBack(shown); }};
    gi.Binding().profilerAvailable = Function<bool()>{[]() { return true; }};
    REQUIRE(gi.StartRunBehavior(source, u8"game.as", u8"Game", Span<const String>{}));
    CHECK(gi.ScriptContext()->GetGlobal(u8"Available").Get<bool>());
    REQUIRE(calls.Size() == 1);
    CHECK(calls[0]);
    gi.StopRunBehavior(); // exit() hides it
    REQUIRE(calls.Size() == 2);
    CHECK_FALSE(calls[1]);
}

TEST_CASE("game-instance: run.setProfilerOverlay/profilerAvailable route to the app (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    Array<bool> calls(DefaultAllocator());
    gi.Binding().setProfilerOverlay = Function<void(bool)>{[&calls](bool shown) { calls.PushBack(shown); }};
    gi.Binding().profilerAvailable = Function<bool()>{[]() { return true; }};
    REQUIRE(gi.StartRunBehavior(u8"Available = false\n"
                           u8"Game = {}\n"
                           u8"Game.__index = Game\n"
                           u8"function Game.new() return setmetatable({}, Game) end\n"
                           u8"function Game:onStart()\n"
                           u8"  Available = run.profilerAvailable()\n"
                           u8"  run.setProfilerOverlay(true)\n"
                           u8"end\n"
                           u8"function Game:onUpdate(dt) end\n"
                           u8"function Game:onStop() end\n",
                           u8"game.luau", u8"Game", Span<const String>{}));
    REQUIRE(gi.ScriptContext() != nullptr);
    CHECK(gi.ScriptContext()->GetGlobal(u8"Available").Get<bool>());
    REQUIRE(calls.Size() == 1);
    CHECK(calls[0]);
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: ClearScenes resets the run-scoped group time scale to 1")
{
    engine::runtime::ApplicationRun gi;
    (void)gi.Scenes().CreateScene(u8"L1");

    // A game paused through run.setTimeScale(0) then stopped: the SceneManager survives a stop on
    // the persistent editor instance, so the run-scoped scale must not leak into the next Play.
    gi.Scenes().SetTimeScale(0.0f);
    CHECK(gi.Scenes().TimeScale() == doctest::Approx(0.0f));

    gi.ClearScenes();
    CHECK(gi.Scenes().SceneCount() == 0u);                    // the group is gone
    CHECK(gi.Scenes().TimeScale() == doctest::Approx(1.0f)); // the next run starts unfrozen
}

TEST_CASE("game-instance: run.events():emit publishes on the run bus, round-tripping to on<Event> (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    const String handlers[] = {String(u8"onPing")};
    const bool ok = gi.StartRunBehavior(
        u8"Pinged = 0\n"
        u8"Game = {}\n"
        u8"Game.__index = Game\n"
        u8"function Game.new() return setmetatable({}, Game) end\n"
        u8"function Game:onStart() run.events():emit(\"Ping\", 7) end\n" // publish on the run bus
        u8"function Game:onUpdate(dt) end\n"
        u8"function Game:onStop() end\n"
        u8"function Game:onPing(x) Pinged = x end\n",
        u8"game.luau", u8"Game", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* scriptContext = gi.ScriptContext();
    REQUIRE(scriptContext != nullptr);
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0)); // deferred until drain
    gi.DrainRunEvents();
    CHECK(scriptContext->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(7.0)); // script -> run bus -> script
    gi.StopRunBehavior();
}

TEST_CASE("game-instance: a missing Game class fails to start cleanly")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::ApplicationRun gi;
    const bool ok = gi.StartRunBehavior(u8"int X = 1;\n", u8"game.as", u8"Game");
    CHECK_FALSE(ok); // no `Game` class
    CHECK_FALSE(gi.RunBehaviorRunning());
}

namespace
{
    // A crate prefab (the crate and its lid) and a level placing it three times, as stored.
    content::Instance* AuthorPlacements(content::ContentDatabase& db, Guid& prefabId)
    {
        scene::Scene author(DefaultAllocator(), u8"crate");
        const scene::EntityHandle crate = author.CreateEntity(u8"Crate");
        author.SetParent(author.CreateEntity(u8"Lid"), crate);
        content::Instance* prefab = db.RootGroup()->CreateInstance(u8"crate", scene::PrefabDocument::StaticType());
        REQUIRE(prefab != nullptr);
        REQUIRE(scene::SavePrefab(author, *prefab).IsOk());
        prefabId = prefab->Id();
        scene::Scene authored(DefaultAllocator(), u8"level");
        for (i32 i = 0; i < 3; ++i)
        {
            UniquePtr<IStream> payload = prefab->ReadData(u8"scene");
            REQUIRE(payload);
            REQUIRE(scene::SpawnPrefab(authored, *payload, prefabId).IsAssigned());
        }
        content::Instance* level = db.RootGroup()->CreateInstance(u8"level", scene::SceneDocument::StaticType());
        REQUIRE(level != nullptr);
        REQUIRE(scene::SaveScene(authored, *level).IsOk());
        return level;
    }

    // The crate made a single entity on disk, as an edit the game has not reloaded.
    void FlattenPrefab(content::ContentDatabase& db, const Guid& prefabId)
    {
        scene::Scene author(DefaultAllocator(), u8"crate");
        (void)author.CreateEntity(u8"Flat");
        REQUIRE(scene::SavePrefab(author, *db.GetInstance(prefabId)).IsOk());
    }
}

TEST_CASE("game-instance: a load places its prefabs from the prefab the resources loaded")
{
    scene::RegisterSceneResourceTypes();
    (void)RemoveDirectoryRecursive(u8"scratch_gi_load_prefabs");
    (void)CreateDirectories(u8"scratch_gi_load_prefabs");
    {
        NativeFileSystem mount(u8"scratch_gi_load_prefabs", DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        Guid prefabId;
        content::Instance* level = AuthorPlacements(db, prefabId);
        resource::ResourceManager resources(DefaultAllocator(), db);
        scene::PrefabFactory prefabs(DefaultAllocator());
        resources.AddFactory(&prefabs);
        engine::runtime::ApplicationRun gi;

        // Never bound before: the load binds it for its three placements.
        scene::Scene* first = gi.LoadScene(*level, resources);
        REQUIRE(first != nullptr);
        CHECK(first->EntityCount() == 6u); // three crates, each with its lid
        CHECK(resources.HasHandle(prefabId)); // loaded by the manager

        // Changed on disk, not reloaded: both loads place the prefab the game has.
        FlattenPrefab(db, prefabId);
        scene::Scene* second = gi.LoadScene(*level, resources);
        REQUIRE(second != nullptr);
        CHECK(second->EntityCount() == 6u); // the loaded crate, not the file
        engine::runtime::SceneLoadHandle handle = gi.LoadSceneAsync(*level, resources);
        REQUIRE_FALSE(handle.Failed());
        resources.WaitAll();
        REQUIRE(handle.Scene() != nullptr);
        CHECK(handle.Scene()->EntityCount() == 6u); // the streaming load places it too

        // Reloaded, the next load places it as it now is.
        REQUIRE(resources.Reload(prefabId));
        scene::Scene* third = gi.LoadScene(*level, resources);
        REQUIRE(third != nullptr);
        CHECK(third->EntityCount() == 3u); // three flat crates
    }
    (void)RemoveDirectoryRecursive(u8"scratch_gi_load_prefabs");
}

TEST_CASE("game-instance: a load without the prefab factory places nothing of it")
{
    scene::RegisterSceneResourceTypes();
    (void)RemoveDirectoryRecursive(u8"scratch_gi_load_no_prefabs");
    (void)CreateDirectories(u8"scratch_gi_load_no_prefabs");
    {
        NativeFileSystem mount(u8"scratch_gi_load_no_prefabs", DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        Guid prefabId;
        content::Instance* level = AuthorPlacements(db, prefabId);
        resource::ResourceManager resources(DefaultAllocator(), db);
        engine::runtime::ApplicationRun gi;
        scene::Scene* loaded = gi.LoadScene(*level, resources);
        REQUIRE(loaded != nullptr); // the rest of the level loads
        CHECK(loaded->EntityCount() == 0u);
    }
    (void)RemoveDirectoryRecursive(u8"scratch_gi_load_no_prefabs");
}

TEST_CASE("game-instance: LoadScene / LoadSceneAsync own the scene load orchestration (task #123)")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();

    FileDelete(u8"scratch_gi_load_db/level.rasset");
    FileDelete(u8"scratch_gi_load_db/level.scene.bin");
    FileDelete(u8"scratch_gi_load_db/level.scene.data");
    RemoveDirectory(u8"scratch_gi_load_db");
    NativeFileSystem mount(u8"scratch_gi_load_db", DefaultAllocator());

    Guid sceneId;
    {
        // A tiny entities-only scene round-trips with no component managers.
        scene::Scene authored(DefaultAllocator(), u8"level");
        (void)authored.CreateEntity(u8"a");
        (void)authored.CreateEntity(u8"b");
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"level", scene::SceneDocument::StaticType());
        sceneId = inst->Id();
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
    }

    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    resource::ResourceManager resources(DefaultAllocator(), db);
    auto* sceneInst = db.GetInstance(sceneId);
    REQUIRE(sceneInst != nullptr);

    SUBCASE("sync LoadScene returns the active, resolved scene")
    {
        engine::runtime::ApplicationRun gi;
        scene::Scene* s =
            gi.LoadScene(*sceneInst, resources);
        REQUIRE(s != nullptr);
        CHECK(s->EntityCount() == 2u);
        CHECK(gi.Scenes().IsActive(s)); // the sync path activates immediately
    }

    SUBCASE("async LoadSceneAsync creates the scene INACTIVE until ActivateLoadedScene")
    {
        engine::runtime::ApplicationRun gi;
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*sceneInst, resources);
        REQUIRE(handle.Scene() != nullptr);
        CHECK_FALSE(handle.Failed());
        CHECK_FALSE(gi.Scenes().IsActive(handle.Scene())); // inactive while "loading"
        CHECK(handle.IsComplete());                        // no async resources: complete at once
        CHECK(handle.Progress() == doctest::Approx(1.0f));

        scene::Scene* activated = gi.ActivateLoadedScene(handle);
        REQUIRE(activated == handle.Scene());
        CHECK(gi.Scenes().IsActive(activated)); // now ticked and rendered
        CHECK(activated->EntityCount() == 2u);
    }

    SUBCASE("a load with no scene stream fails cleanly")
    {
        auto* empty =
            db.RootGroup()->CreateInstance(u8"empty", scene::SceneDocument::StaticType());
        engine::runtime::ApplicationRun gi;
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*empty, resources);
        CHECK(handle.Failed());
        CHECK(handle.IsComplete());
        CHECK(handle.Scene() == nullptr);
        CHECK(gi.LoadScene(*empty, resources) ==
              nullptr);
    }

    SUBCASE("a level load replaces the scene it lands over, on the pump and synchronously")
    {
        engine::runtime::ApplicationRun gi;
        gi.SetSceneActivationPolicy(Function<void(scene::Scene*)>{[](scene::Scene*) {}});

        engine::runtime::SceneLoadHandle first =
            gi.LoadSceneAsync(*sceneInst, resources);
        (void)gi.TrackScriptLoad(first);
        gi.PumpScriptLoads();
        scene::Scene* level1 = gi.GetScene();
        REQUIRE(level1 != nullptr);
        CHECK(gi.Scenes().SceneCount() == 1u);

        // The same level again, as a restart loads it: both exist while the new one loads, then
        // the new copy is the run's scene and the manager's current, and the old one is gone.
        engine::runtime::SceneLoadHandle second =
            gi.LoadSceneAsync(*sceneInst, resources);
        scene::Scene* level2 = second.Scene();
        (void)gi.TrackScriptLoad(second);
        CHECK(gi.Scenes().SceneCount() == 2u);
        gi.PumpScriptLoads();
        CHECK(gi.GetScene() == level2);
        CHECK(gi.Scenes().CurrentScene() == level2);
        CHECK(gi.Scenes().IsActive(level2));
        CHECK(gi.Scenes().SceneCount() == 1u);

        // The synchronous path (run.loadScene) loads at once and is adopted by the same pump.
        REQUIRE(gi.LoadSceneForScript(*sceneInst, resources));
        CHECK(gi.GetScene() == level2); // still the level until the pump
        CHECK(gi.Scenes().SceneCount() == 2u);
        gi.PumpScriptLoads();
        CHECK(gi.GetScene() != level2);
        CHECK(gi.Scenes().IsActive(gi.GetScene()));
        CHECK(gi.Scenes().SceneCount() == 1u);
    }

    SUBCASE("run.loadScene from a level's own fixed step leaves it whole until the pump")
    {
        engine::runtime::ApplicationRun gi;
        gi.SetSceneActivationPolicy(Function<void(scene::Scene*)>{[](scene::Scene*) {}});
        REQUIRE(gi.LoadSceneForScript(*sceneInst, resources));
        gi.PumpScriptLoads();
        scene::Scene* level = gi.GetScene();
        REQUIRE(level != nullptr);
        // Its fixed step loads the next level, as a behaviour's onFixedUpdate would.
        struct Loader final : scene::SceneSystem
        {
            engine::runtime::ApplicationRun* gi = nullptr;
            content::Instance* next = nullptr;
            resource::ResourceManager* resources = nullptr;
            int loads = 0;
            void OnFixedUpdate(f32) override
            {
                if (loads++ == 0)
                    CHECK(gi->LoadSceneForScript(*next, *resources));
            }
        };
        Loader* loader = level->AddSystem<Loader>();
        loader->gi = &gi;
        loader->next = sceneInst;
        loader->resources = &resources;
        scene::FrameTime time(1.0f / 30.0f);
        gi.Scenes().BeginFrame(time); // two fixed steps over the active list
        CHECK(loader->loads == 2);    // the level kept stepping, whole
        CHECK(gi.GetScene() == level);
        CHECK(gi.Scenes().IsActive(level));
        gi.PumpScriptLoads();
        CHECK(gi.GetScene() != level);
        CHECK(gi.Scenes().SceneCount() == 1u);
    }

    SUBCASE("script-load registry: ticket -> poll -> PumpScriptLoads activates + runs the policy")
    {
        engine::runtime::ApplicationRun gi;
        CHECK_FALSE(gi.SceneReady()); // no current scene at launch (orchestrator-first boot)

        // The app owns the render/sim policy; here a fake records which scene it activated.
        scene::Scene* policyRanOn = nullptr;
        gi.SetSceneActivationPolicy(
            Function<void(scene::Scene*)>{[&](scene::Scene* s) { policyRanOn = s; }});

        // The Game facade hands the in-flight handle to the instance and gets a ticket back.
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*sceneInst, resources);
        REQUIRE(handle.Scene() != nullptr);
        scene::Scene* pending = handle.Scene();
        const i32 ticket = gi.TrackScriptLoad(handle);
        CHECK(ticket == 1); // 1-based; 0 is reserved for "did not start"

        // Registered but not yet pumped: not complete, scene still inactive, policy not run.
        CHECK_FALSE(gi.ScriptLoadComplete(ticket));
        CHECK_FALSE(gi.ScriptLoadFailed(ticket));
        CHECK(gi.ScriptLoadProgress(ticket) == doctest::Approx(1.0f)); // no async resources pending
        CHECK_FALSE(gi.Scenes().IsActive(pending));
        CHECK(policyRanOn == nullptr);

        gi.PumpScriptLoads(); // resources already complete: activate, SetScene, policy
        CHECK(gi.ScriptLoadComplete(ticket));
        CHECK_FALSE(gi.ScriptLoadFailed(ticket));
        CHECK(gi.ScriptLoadProgress(ticket) == doctest::Approx(1.0f));
        CHECK(gi.Scenes().IsActive(pending));
        CHECK(policyRanOn == pending);   // the app policy ran on the freshly activated scene
        CHECK(gi.GetScene() == pending); // SetScene bookkeeping happened
        CHECK(gi.SceneReady());          // Game.sceneReady() now true

        gi.PumpScriptLoads(); // idempotent: an already-activated load is skipped
        CHECK(policyRanOn == pending);

        // An unknown/expired ticket never hangs a `while (!complete) yield` loop.
        CHECK(gi.ScriptLoadComplete(999));
        CHECK_FALSE(gi.ScriptLoadFailed(999));
        CHECK(gi.ScriptLoadProgress(999) == doctest::Approx(1.0f));
    }

    SUBCASE("script-load registry: a failed load reports terminal-complete + failed by ticket")
    {
        auto* empty =
            db.RootGroup()->CreateInstance(u8"empty2", scene::SceneDocument::StaticType());
        engine::runtime::ApplicationRun gi;
        const i32 ticket = gi.TrackScriptLoad(
            gi.LoadSceneAsync(*empty, resources));
        CHECK(ticket == 1);
        gi.PumpScriptLoads(); // a failed handle is skipped (never activated), stays terminal
        CHECK(gi.ScriptLoadComplete(ticket)); // terminal (failed counts as complete)
        CHECK(gi.ScriptLoadFailed(ticket));
        CHECK_FALSE(gi.SceneReady()); // nothing became current
    }

    SUBCASE("destroying a pending scene sweeps its tracked load (no dangling activation)")
    {
        // The tracked handle holds a raw Scene*. Destroying the pending scene before it activates
        // must drop the tracked load, or PumpScriptLoads would later activate freed memory.
        engine::runtime::ApplicationRun gi;
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*sceneInst, resources);
        REQUIRE(handle.Scene() != nullptr);
        scene::Scene* pending = handle.Scene();
        const i32 ticket = gi.TrackScriptLoad(handle);
        CHECK_FALSE(gi.ScriptLoadComplete(ticket)); // in flight (not yet activated)

        gi.DestroyScene(pending);   // sweeps the tracked entry before freeing the scene
        gi.PumpScriptLoads();       // must be a safe no-op (nothing to activate)
        CHECK(gi.ScriptLoadComplete(ticket));   // the ticket now reads terminal-safe via the fallback
        CHECK_FALSE(gi.ScriptLoadFailed(ticket));
        CHECK_FALSE(gi.SceneReady());           // nothing became current
    }

    SUBCASE("ClearScenes drops in-flight tracked loads + tears the group down (no dangling activation)")
    {
        // The editor Game tab's Stop tears the run's scenes down while keeping the instance. An
        // async load still in flight at that moment holds a raw Scene* in the tracked registry;
        // ClearScenes must drop it before Scenes().Clear() frees the scene, or the next
        // PumpScriptLoads (still driven every frame) would activate and Start() freed memory.
        engine::runtime::ApplicationRun gi;
        scene::Scene* live =
            gi.LoadScene(*sceneInst, resources);
        REQUIRE(live != nullptr);
        gi.SetScene(live);
        REQUIRE(gi.SceneReady());

        // A second load left in flight (inactive, tracked): the scene that would dangle.
        const i32 ticket = gi.TrackScriptLoad(
            gi.LoadSceneAsync(*sceneInst, resources));
        CHECK_FALSE(gi.ScriptLoadComplete(ticket)); // in flight
        CHECK(gi.Scenes().SceneCount() == 2u);      // the live scene and the pending inactive one

        bool policyRan = false;
        gi.SetSceneActivationPolicy(
            Function<void(scene::Scene*)>{[&](scene::Scene*) { policyRan = true; }});

        gi.ClearScenes(); // drop tracked loads, clear current, destroy every scene
        CHECK(gi.Scenes().SceneCount() == 0u);
        CHECK(gi.GetScene() == nullptr);
        CHECK_FALSE(gi.SceneReady());

        gi.PumpScriptLoads(); // the dropped ticket must not activate a freed scene
        CHECK_FALSE(policyRan);
        CHECK(gi.ScriptLoadComplete(ticket)); // terminal-safe via the unknown-ticket fallback
        CHECK(gi.GetScene() == nullptr);      // still no current scene
    }

    SUBCASE("script-load registry: a successful load is RETIRED on activation (bounded growth)")
    {
        // m_scriptLoads must not grow without bound. After activation the entry is dropped, so
        // destroying the (now active) scene again is a safe no-op and the ticket still reads
        // terminal-safe.
        engine::runtime::ApplicationRun gi;
        const i32 ticket = gi.TrackScriptLoad(
            gi.LoadSceneAsync(*sceneInst, resources));
        gi.PumpScriptLoads(); // activates and retires the entry
        REQUIRE(gi.SceneReady());
        scene::Scene* live = gi.GetScene();
        CHECK(gi.ScriptLoadComplete(ticket)); // retired -> fallback (complete)
        gi.DestroyScene(live);                // no tracked entry references it anymore: safe
        gi.PumpScriptLoads();                 // still a safe no-op
        CHECK(gi.ScriptLoadComplete(ticket));
    }

    FileDelete(u8"scratch_gi_load_db/level.rasset");
    FileDelete(u8"scratch_gi_load_db/level.scene.bin");
    FileDelete(u8"scratch_gi_load_db/level.scene.data");
    RemoveDirectory(u8"scratch_gi_load_db");
}

TEST_CASE("game-instance: created scenes share the run bus - cross-scene delivery, single drain (no relay)")
{
    // Every scene the instance creates is in its run and so uses the run bus: scene.events and the
    // run bus are one object. A subscriber in scene A hears an emit from scene B (and the Game tier
    // would too) with no relay, delivered exactly once by the instance's drain; the scenes never
    // drain it.
    engine::runtime::ApplicationRun gi;
    scene::Scene* a = gi.CreateScene(u8"A");
    scene::Scene* b = gi.CreateScene(u8"B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->Events() == &gi.Events()); // both use the one run bus
    CHECK(b->Events() == &gi.Events());

    int hits = 0;
    (void)a->Events()->Subscribe(StringHash(u8"Ping"), [&](const Variant&) { ++hits; });
    b->Events()->Publish(StringHash(u8"Ping"), Variant{}); // emit from a different scene

    // Ticking the borrowing scenes must not deliver it (they do not drain a borrowed bus).
    a->Update(0.016f);
    b->Update(0.016f);
    CHECK(hits == 0);

    gi.DrainRunEvents(); // the instance drains the shared bus once
    CHECK(hits == 1);    // delivered exactly once, cross-scene, no relay
}

// A faulting game script is remembered with where it faulted and why, and the run's clock is the
// gameplay time the script moved by. A new start clears the fault; a clean stop is not one.
TEST_CASE("game-instance: a faulted game script says where and why, and the run clock counts "
          "gameplay time")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    const StringView faulting = u8"Game = {}\n"
                                u8"Game.__index = Game\n"
                                u8"function Game.new() return setmetatable({ n = 0 }, Game) end\n"
                                u8"function Game:onStart() end\n"
                                u8"function Game:onUpdate(dt)\n"
                                u8"  self.n = self.n + 1\n"
                                u8"  if self.n == 4 then error(\"boom\") end\n"
                                u8"end\n"
                                u8"function Game:onStop() end\n";
    REQUIRE(gi.StartRunBehavior(faulting, u8"game.luau", u8"Game"));
    CHECK(gi.RunBehaviorFault().IsEmpty());
    for (int i = 0; i < 4; ++i)
    {
        gi.TickRunBehavior(1.0f / 60.0f, 1.0f);
    }
    CHECK_FALSE(gi.RunBehaviorRunning());
    CHECK(gi.RunBehaviorFault().StartsWith(u8"faulted in onUpdate"));
    CHECK(gi.RunBehaviorFault().ContainsIgnoreCase(u8"boom"));
    CHECK(gi.RunTime() == doctest::Approx(4.0 / 60.0).epsilon(1e-5));
    gi.ResetRunClock();
    CHECK(gi.RunTime() == 0.0);

    // A new start clears it; a clean stop is not a fault.
    REQUIRE(gi.StartRunBehavior(faulting, u8"game.luau", u8"Game"));
    CHECK(gi.RunBehaviorFault().IsEmpty());
    gi.StopRunBehavior();
    CHECK(gi.RunBehaviorFault().IsEmpty());

    // A script with no Game class says it did not instantiate (a fresh instance: a run's host
    // keeps the one script context it made).
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::runtime::ApplicationRun other;
    CHECK_FALSE(other.StartRunBehavior(u8"class Other { Other() {} }\n", u8"game.as", u8"Game"));
    CHECK(other.RunBehaviorFault().ContainsIgnoreCase(u8"did not instantiate"));
}

// A project with no game script (a scene of behaviours alone) still runs, and a playtest times its
// run by this clock: it counts the frames either way.
TEST_CASE("game-instance: the run clock counts frames without a game script")
{
    RegisterCoreTypes();
    engine::runtime::ApplicationRun gi;
    CHECK_FALSE(gi.RunBehaviorRunning());
    for (int i = 0; i < 5; ++i)
    {
        gi.TickRunBehavior(1.0f / 60.0f, 1.0f);
    }
    CHECK(gi.RunTime() == doctest::Approx(5.0 / 60.0).epsilon(1e-5));
    gi.ResetRunClock();
    CHECK(gi.RunTime() == 0.0);
}

// A project with no game script runs its behaviors on the run's host all the same, and they need
// the run's services: a behavior reading Input on a context without the run's input runtime would
// read the shared runtime, which has no project map. Every context the run's host makes takes them.
TEST_CASE("game-instance: a context for behaviors gets the run's input, run and save services without a game script")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::runtime::ApplicationRun gi;
    CHECK_FALSE(gi.RunBehaviorRunning());
    foundation::script::IScriptContext* scriptContext = gi.RunHost().EnsureContextForFile(u8"behaviors.as");
    REQUIRE(scriptContext != nullptr);
    CHECK(scriptContext->FindService<foundation::input::ActionRuntime>() == &gi.InputRuntime());
    CHECK(scriptContext->FindService<engine::runtime::RunBinding>() != nullptr);
    CHECK(scriptContext->FindService<engine::runtime::RunSave>() == &gi.Saves());
    CHECK(scriptContext->FindService<foundation::http::HttpScriptBinding>() == &gi.Http());
    CHECK(scriptContext->FindService<foundation::net::NetScriptBinding>() != nullptr);
}

// The run clock is the host's unscaled delta, so it keeps going while the game pauses its scene
// (behind a menu) and a playtest's timeline goes on; and a playtest reads the running game script's
// properties.
TEST_CASE("game-instance: the run clock ignores time scale, and a game script's properties read")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::ApplicationRun gi;
    CHECK_FALSE(gi.GetScriptProperty(u8"score").HasValue()); // no script running
    const StringView game = u8"Game = {}\n"
                            u8"Game.__index = Game\n"
                            u8"function Game.new() return setmetatable({ score = 0, real = 0 }, Game) end\n"
                            u8"function Game:onStart() end\n"
                            u8"function Game:onUpdate(dt)\n"
                            u8"  self.score = self.score + 1\n"
                            u8"  self.real = self.real + run.realDeltaTime()\n"
                            u8"end\n"
                            u8"function Game:onStop() end\n";
    REQUIRE(gi.StartRunBehavior(game, u8"game.luau", u8"Game"));
    gi.SetInstanceTimeScale(0.0f); // the scene paused: the run clock still moves
    for (int i = 0; i < 3; ++i)
    {
        gi.TickRunBehavior(1.0f / 60.0f, 1.0f);
    }
    CHECK(gi.RunTime() == doctest::Approx(3.0 / 60.0).epsilon(1e-5));

    auto score = gi.GetScriptProperty(u8"score");
    REQUIRE(score.HasValue());
    CHECK(score.Value().Get<f64>() == doctest::Approx(3.0));
    CHECK_FALSE(gi.GetScriptProperty(u8"lives").HasValue());
    // A script reads the real clock too, for a pause menu to time by.
    CHECK(gi.GetScriptProperty(u8"real").Value().Get<f64>() == doctest::Approx(3.0 / 60.0).epsilon(1e-4));
    CHECK(gi.Binding().realSeconds == doctest::Approx(3.0 / 60.0).epsilon(1e-5));
    CHECK_FALSE(gi.GetScriptProperty(u8"update").HasValue()); // a method is not a property
    gi.StopRunBehavior();
    CHECK_FALSE(gi.GetScriptProperty(u8"score").HasValue());
}

// ---- save data: the values a game keeps between runs ----

namespace
{
    // A fresh scratch file under the build tree the tests run in.
    String FreshSavePath(StringView name)
    {
        (void)CreateDirectories(u8"scratch_save");
        String path = PathJoin(u8"scratch_save", name);
        (void)FileDelete(path.AsView());
        return path;
    }
}

TEST_CASE("game-instance: a run save writes what changed, reads it back, and survives a bad file")
{
    const String path = FreshSavePath(u8"run_save.xml");

    engine::runtime::RunSave save(DefaultAllocator());
    CHECK_FALSE(save.Flush()); // no file named: nowhere to write
    save.Open(path.AsView());  // absent: an empty save
    CHECK(save.IsOpen());
    CHECK(save.Values().Count() == 0u);
    CHECK(save.Flush()); // nothing changed: nothing to write, and that is fine
    CHECK_FALSE(FileExists(path.AsView()));

    save.Values().SetInt(u8"best.level2", 4210);
    save.Values().SetFloat(u8"time.level2", 41.5f);
    save.MarkChanged();
    REQUIRE(save.Flush());
    CHECK_FALSE(save.HasChanges());
    CHECK(FileExists(path.AsView()));

    engine::runtime::RunSave reread(DefaultAllocator());
    reread.Open(path.AsView());
    CHECK(reread.Values().GetInt(u8"best.level2", 0) == 4210);
    CHECK(reread.Values().GetFloat(u8"time.level2", 0.0f) == 41.5f);

    // A file that is not a save: an empty save, and the file is set aside, so the game's next
    // write never replaces what the player had.
    const char garbage[] = "not a save";
    const String aside = Format(u8"{}.unreadable", path.AsView());
    (void)FileDelete(aside.AsView());
    REQUIRE(WriteFile(path.AsView(), Span<const byte>{reinterpret_cast<const byte*>(garbage), sizeof(garbage) - 1})
                .IsOk());
    engine::runtime::RunSave damaged(DefaultAllocator());
    damaged.Open(path.AsView());
    CHECK(damaged.Values().Count() == 0u);
    damaged.Values().SetInt(u8"best.level1", 99);
    damaged.MarkChanged();
    CHECK(damaged.Flush());
    Result<Array<byte>> bytes = ReadFile(aside.AsView());
    REQUIRE(bytes.HasValue());
    CHECK(bytes.Value().Size() == sizeof(garbage) - 1);
    engine::runtime::RunSave after(DefaultAllocator());
    after.Open(path.AsView());
    CHECK(after.Values().GetInt(u8"best.level1", 0) == 99);
    (void)FileDelete(aside.AsView());
}

TEST_CASE("game-instance: a game keeps its values between runs through Save (AngelScript)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    engine::runtime::RegisterSaveScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    const String path = FreshSavePath(u8"game_as.xml");

    // Each run counts itself and keeps a best time; the second sees the first's values. The
    // first never calls flush: the run writes what changed as it stops.
    const StringView source = u8"class Game : RunBehavior {\n"
                              u8"  Game() {}\n"
                              u8"  void onStart() {\n"
                              u8"    int runs = Save::getInt(\"runs\", 0);\n"
                              u8"    Save::setInt(\"runs\", runs + 1);\n"
                              u8"    if (Save::getFloat(\"best\", 999.0f) > 41.5f) Save::setFloat(\"best\", 41.5f);\n"
                              u8"    Save::setBool(\"seen\", Save::has(\"runs\"));\n"
                              u8"    Save::setString(\"name\", \"Hopper\");\n"
                              u8"    Save::setInt(\"scratch\", 1);\n"
                              u8"    Save::remove(\"scratch\");\n"
                              u8"  }\n"
                              u8"  void onUpdate(double dt) {}\n"
                              u8"  void onStop() {}\n"
                              u8"}\n";
    for (i32 run = 1; run <= 2; ++run)
    {
        engine::runtime::ApplicationRun gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartRunBehavior(source, u8"game.as", u8"Game"));
        CHECK(gi.Saves().Values().GetInt(u8"runs", 0) == run);
        gi.StopRunBehavior();
    }

    engine::runtime::RunSave reread(DefaultAllocator());
    reread.Open(path.AsView());
    CHECK(reread.Values().GetInt(u8"runs", 0) == 2);
    CHECK(reread.Values().GetFloat(u8"best", 0.0f) == 41.5f);
    CHECK(reread.Values().GetBool(u8"seen", false));
    CHECK(reread.Values().GetText(u8"name", u8"") == u8"Hopper");
    CHECK_FALSE(reread.Values().Has(u8"scratch"));
}

TEST_CASE("game-instance: a game keeps a list of numbers through Save (AngelScript and Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    engine::runtime::RegisterSaveScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    foundation::script::RegisterLuauScriptBackend();
    const String path = FreshSavePath(u8"game_floats.xml");

    // AngelScript records a run; it outlives the run, and reads back whole.
    {
        engine::runtime::ApplicationRun gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartRunBehavior(u8"class Game : RunBehavior {\n"
                               u8"  Game() {}\n"
                               u8"  void onStart() {\n"
                               u8"    array<float> samples = {0.5f, 1.25f, -3.0f};\n"
                               u8"    Save::setFloats(\"ghost\", samples);\n"
                               u8"    array<float>@ back = Save::getFloats(\"ghost\");\n"
                               u8"    Save::setInt(\"length\", back.length());\n"
                               u8"    Save::setInt(\"missing\", Save::getFloats(\"none\").length());\n"
                               u8"  }\n"
                               u8"  void onUpdate(double dt) {}\n"
                               u8"  void onStop() {}\n"
                               u8"}\n",
                               u8"game.as", u8"Game"));
        CHECK(gi.Saves().Values().GetInt(u8"length", -1) == 3);
        CHECK(gi.Saves().Values().GetInt(u8"missing", -1) == 0);
        gi.StopRunBehavior();
    }
    engine::runtime::RunSave reread(DefaultAllocator());
    reread.Open(path.AsView());
    const Array<f32> ghost = reread.Values().GetFloats(u8"ghost");
    REQUIRE(ghost.Size() == 3u);
    CHECK(ghost[0] == 0.5f);
    CHECK(ghost[1] == 1.25f);
    CHECK(ghost[2] == -3.0f);

    // Luau reads the same list as a table and writes its own.
    {
        engine::runtime::ApplicationRun gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartRunBehavior(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:onStart()\n"
                               u8"  local g = Save.getFloats('ghost')\n"
                               u8"  Save.setFloat('sum', g[1] + g[2] + g[3])\n"
                               u8"  Save.setFloats('ghost', {7, 8})\n"
                               u8"end\n"
                               u8"function Game:onUpdate(dt) end\n"
                               u8"function Game:onStop() end\n",
                               u8"game.luau", u8"Game"));
        CHECK(gi.Saves().Values().GetFloat(u8"sum", 0.0f) == doctest::Approx(-1.25f));
        REQUIRE(gi.Saves().Values().GetFloats(u8"ghost").Size() == 2u);
        CHECK(gi.Saves().Values().GetFloats(u8"ghost")[1] == 8.0f);
        gi.StopRunBehavior();
    }
}

TEST_CASE("game-instance: Save flushes on request and clears (Luau); a run with no file writes nowhere")
{
    RegisterCoreTypes();
    engine::runtime::RegisterSaveScriptFacade();
    foundation::script::RegisterLuauScriptBackend();
    const String path = FreshSavePath(u8"game_luau.xml");

    {
        engine::runtime::ApplicationRun gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartRunBehavior(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:onStart()\n"
                               u8"  Save.setInt('coins', 37)\n"
                               u8"  self.flushed = Save.flush()\n"
                               u8"end\n"
                               u8"function Game:onUpdate(dt) end\n"
                               u8"function Game:onStop() end\n",
                               u8"game.luau", u8"Game"));
        // Written at the flush, while the run is still going.
        engine::runtime::RunSave reread(DefaultAllocator());
        reread.Open(path.AsView());
        CHECK(reread.Values().GetInt(u8"coins", 0) == 37);
        gi.StopRunBehavior();
    }
    {
        engine::runtime::ApplicationRun gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartRunBehavior(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:onStart() Save.clear() end\n"
                               u8"function Game:onUpdate(dt) end\n"
                               u8"function Game:onStop() end\n",
                               u8"game.luau", u8"Game"));
        gi.StopRunBehavior();
        engine::runtime::RunSave reread(DefaultAllocator());
        reread.Open(path.AsView());
        CHECK(reread.Values().Count() == 0u);
    }
    {
        // No file named: the game's values last the run, its reads see them, nothing is written.
        engine::runtime::ApplicationRun gi;
        REQUIRE(gi.StartRunBehavior(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:onStart() Save.setInt('coins', 5) end\n"
                               u8"function Game:onUpdate(dt) end\n"
                               u8"function Game:onStop() end\n",
                               u8"game.luau", u8"Game"));
        CHECK(gi.Saves().Values().GetInt(u8"coins", 0) == 5);
        gi.StopRunBehavior();
    }
}

namespace
{
    struct AppOnlyService
    {
        int value = 0;
    };
}

// A run's script finds a service through its run host: the host's runtime binding, then its run's
// services, then the application's. A run's own copy of a type hides the application's for that
// run's scripts (each run reads its own input), and what only the application offers is still found.
TEST_CASE("game-instance: script services resolve the run first, then the application")
{
    engine::runtime::ApplicationRun gi;
    foundation::input::ActionRuntime appInput;
    AppOnlyService appOnly;
    foundation::script::ScriptServices app;
    app.Add(appInput);
    app.Add(appOnly);
    gi.RunHost().SetApplicationServices(Function<void*(TypeId)>{[&app](TypeId type) { return app.Find(type); }});

    engine::behavior::BehaviorRunHost& host = gi.RunHost();
    CHECK(host.FindService(TypeOf<foundation::input::ActionRuntime>().id) == &gi.InputRuntime()); // the run's
    CHECK(host.FindService(TypeOf<AppOnlyService>().id) == &appOnly);                           // the app's
    CHECK(host.FindService(TypeOf<foundation::script::ScriptRuntimeBinding>().id) == &host.Binding());
    CHECK(host.FindService(TypeOf<double>().id) == nullptr);
}

namespace
{
    // A stand-in language whose classes are built ahead into an assembly: its context has no
    // compiler (Load is refused) and loads a module from bytes alone. What it saw is kept here.
    struct AssembledSeen
    {
        Array<byte> loaded;
        String moduleName;
        Array<String> calls;
        bool compiled = false;
    };
    AssembledSeen g_assembled;

    class AssembledGame final : public foundation::script::ScriptObject
    {
    public:
        [[nodiscard]] Result<Variant> Invoke(StringView method, Span<Variant>) override
        {
            g_assembled.calls.PushBack(String(method));
            return Variant{};
        }
        [[nodiscard]] foundation::script::ScriptMethod Bind(StringView method, usize) override
        {
            m_bound.PushBack(String(method));
            return foundation::script::ScriptMethod{m_bound.Size()};
        }
        [[nodiscard]] Result<Variant> Invoke(foundation::script::ScriptMethod method, Span<Variant> args) override
        {
            if (method.id == 0 || method.id > m_bound.Size())
                return Err(ErrorCode::NotFound);
            return Invoke(m_bound[static_cast<usize>(method.id - 1)].AsView(), args);
        }
        [[nodiscard]] Result<Variant> GetProperty(StringView) override { return Err(ErrorCode::NotFound); }

    private:
        Array<String> m_bound;
    };

    class AssembledContext final : public foundation::script::IScriptContext
    {
    public:
        void SetErrorHandler(foundation::script::IScriptErrorHandler*) override {}
        Status Load(StringView, StringView, Span<const foundation::script::ScriptSection>) override
        {
            g_assembled.compiled = true;
            return Status{ErrorCode::NotSupported};
        }
        Status LoadBehaviorModule(Span<const foundation::script::BehaviorModuleClass> classes, StringView moduleName) override
        {
            g_assembled.moduleName = String(moduleName);
            for (const foundation::script::BehaviorModuleClass& entry : classes)
                for (byte b : entry.bytecode)
                    g_assembled.loaded.PushBack(b);
            return Status{};
        }
        void SetGlobal(StringView, const Variant&) override {}
        Variant GetGlobal(StringView) override { return Variant{}; }
        bool HasFunction(StringView) const override { return false; }
        Result<Variant> Call(StringView, Span<Variant>) override { return Err(ErrorCode::NotSupported); }
        RefPtr<foundation::script::ScriptObject> CreateInstance(StringView className, Span<Variant>) override
        {
            if (className != u8"Game" || g_assembled.loaded.IsEmpty())
                return nullptr;
            return RefPtr<foundation::script::ScriptObject>(MakeRef<AssembledGame>(DefaultAllocator()));
        }
    };

    class AssembledManager final : public foundation::script::IScriptManager
    {
    public:
        void RegisterType(const TypeInfo&) override {}
        RefPtr<foundation::script::IScriptContext> CreateContext() override
        {
            return RefPtr<foundation::script::IScriptContext>(MakeRef<AssembledContext>(DefaultAllocator()));
        }
        bool Check(StringView, StringView, foundation::script::IScriptErrorHandler&) override { return true; }
        void SetBudget(const foundation::script::ScriptBudget& budget) override { m_budget = budget; }
        [[nodiscard]] foundation::script::ScriptBudget Budget() const override { return m_budget; }

    private:
        foundation::script::ScriptBudget m_budget;
    };
}

// A Game class built into an assembly starts from the assembly, loaded as a behaviour module, not
// from its source: its language has no compiler at run time (C# is one). It is then made by name
// and launched, updated and exited as any Game is.
TEST_CASE("game-instance: a Game built into an assembly starts from it, not from its source")
{
    RegisterCoreTypes();
    foundation::script::ScriptBackendDesc backend;
    backend.languageId = String(u8"assembled");
    backend.displayName = String(u8"Assembled");
    backend.fileExtensions.PushBack(String(u8"asm"));
    backend.create = [](IAllocator& allocator) { return RefPtr<foundation::script::IScriptManager>(MakeRef<AssembledManager>(allocator)); };
    foundation::script::ScriptBackendRegistry::Get().Register(Move(backend));
    g_assembled = AssembledSeen{};

    foundation::script::BehaviorClass cooked;
    cooked.language = String(u8"assembled");
    cooked.className = String(u8"Game");
    cooked.kind = foundation::script::BehaviorKind::Run;
    cooked.sourceName = String(u8"Game.asm");
    cooked.source = String(u8"(the source, never compiled)");
    cooked.assembly = MakeRef<foundation::script::ScriptAssembly>(DefaultAllocator());
    cooked.assembly->language = String(u8"assembled");
    cooked.assembly->image.PushBack(byte{0x4D});
    cooked.assembly->image.PushBack(byte{0x5A});
    {
        engine::runtime::ApplicationRun gi;
        REQUIRE(gi.StartRunBehavior(cooked));
        CHECK_FALSE(g_assembled.compiled);
        CHECK(g_assembled.moduleName == StringView(u8"game"));
        REQUIRE(g_assembled.loaded.Size() == 2u);
        CHECK(g_assembled.loaded[0] == byte{0x4D});
        gi.TickRunBehavior(0.1f, 1.0f);
        gi.StopRunBehavior();
    }
    REQUIRE(g_assembled.calls.Size() >= 3u);
    CHECK(g_assembled.calls[0] == StringView(u8"onStart"));
    CHECK(g_assembled.calls[1] == StringView(u8"onUpdate"));
    CHECK(g_assembled.calls.Back() == StringView(u8"onStop"));
}

// The run's behavior is a RunBehavior: a class of another kind, or a script that declares none, is
// refused before anything compiles, and the fault says what it is and what it needs.
TEST_CASE("game-instance: a cooked class that is not a RunBehavior is refused as the run's behavior")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    foundation::script::BehaviorClass cooked;
    cooked.language = String(u8"angelscript");
    cooked.sourceName = String(u8"mover.as");
    cooked.className = String(u8"Mover");
    cooked.kind = foundation::script::BehaviorKind::Entity;
    cooked.source = String(u8"class Mover : EntityBehavior { Mover() {} }\n");

    engine::runtime::ApplicationRun gi;
    CHECK_FALSE(gi.StartRunBehavior(cooked));
    CHECK_FALSE(gi.RunBehaviorRunning());
    CHECK(gi.RunBehaviorFault().ContainsIgnoreCase(u8"derives from EntityBehavior"));

    cooked.className = String{};
    cooked.kind = foundation::script::BehaviorKind::None;
    CHECK_FALSE(gi.StartRunBehavior(cooked));
    CHECK(gi.RunBehaviorFault().ContainsIgnoreCase(u8"declares no behavior class"));
}
