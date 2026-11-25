# ApplicationRun

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[game-instance-track]]

"A running game" is a first-class object. The standalone player and the editor's Game tab share ONE
run bracket, and the editor can host more than one simultaneously (multi-instance play-in-editor + an
in-editor headless dedicated server). Run ownership lives on `ApplicationRun`, with a `SceneManager`
owning each group of scenes, and an `ApplicationRun` is a **run**: it derives from `RunBase`.

Related: [[runtime-host]], [[scripting]] (the one-context rule, now per instance), [[networking]]
(headless PIE), [[editor-track]].

## Runs (`foundation.run`)

A run is one running game: a player's game, each editor Game tab's game, an in-editor dedicated
server, and an editor scene page's Simulate. `Code/Foundation/Run` (Core and Messaging only; the
runtime and the scenes depend on it, never the reverse) defines:

- **`IRun`**: its `RunId` (never reused), name, `RunState` (`Idle -> Running <-> Paused -> Stopping
  -> Idle`, then `Disposed`), its event bus (`Events()`), its mutation queue (`Mutations()`), and
  the services it holds (`Get<T>()` / `FindService`).
- **`RunBase`**: `IRun` implemented once. Services are added owned (`Add`, or `AddForSession` for
  one session) or borrowed (`Register`), and removed by type (`Remove`, or `RemoveService` by a
  type's id); a service implementing `IRunService` hears its run pause and stop, and is stepped with
  the run's frame (`OnRunUpdate`, through `UpdateServices`). `Start` / `SetPaused` / `Stop` /
  `Dispose` drive the sessions.
- **`RunServiceContributions`** (`engine.run`): the services a run gets from outside its own
  members, each a type and what installs one (`run.Add<T>()`), as a scene's managers come from its
  contributions. Every `ApplicationRun` installs them as it is made; one added while runs exist is
  installed into them at once, and one removed is taken out of every live run. A plugin's Engine
  part adds one through `EnginePluginServices::AddRunService`, reversed as it unloads. An
  `ApplicationRun` steps its services each frame with its gameplay time (its scene's time scale
  applied), before its behavior's update and whether or not a game script runs; not while paused.
- **`IRunManager` / `RunRegistry`**: the live runs, the focused one, and every run's lifecycle
  announced to `IRunListener`s (created, started, paused, stopping, destroyed, focus changed).
  `DefaultApplication` owns the `RunRegistry` and hands it to the subsystem manager
  (`SetRunManager`), so every subsystem finds the runs there: audio and the GUI subscribe to keep
  per-run state; input and networking visit the runs each frame.
- **`RunMutations`**: what a run defers while its scripts run. Whoever runs a run's scripts (the
  scene lanes, `TickRunBehavior`, the run bus drain) opens a `Scope`; an entity destroy, a component
  removal, a scene destroy or a networking stop asked for meanwhile is queued under its owner and
  done at `Flush`, between the frame's phases. An owner that goes away first cancels its work.

The editor's scene page has its own `SimulationRun` (`Editor.Scene/SceneSimulation.cppm`): a
`RunBase` holding only a script host, each Simulate one session of it.

## Two layers

- **Engine host** (`IApplication` / `DefaultApplication`) - "the engine is running": the subsystem
  manager + subsystems + resource infra + shell/graphics, configured ONCE. It owns the primary
  `ApplicationRun` (`PrimaryRun()`) and the extra ones (`CreateRun(headless)` /
  `ReleaseRun`), registers each with its `RunRegistry` (`Runs()`), and exposes
  `PrimaryScenes()` (a `SceneManager&`, so callers reach the primary group without naming
  `ApplicationRun`).
- **`ApplicationRun`** (`engine.run`, `Code/Engine/Engine.Run`) - "a game is running":
  0..N, owned by the host. Each instance OWNS its `ScriptRunHost` (its script context + `Game`
  object + error sink), its `SceneManager` (its scenes + current scene + group time scale), its
  `NetworkController` (`Network()`: the `INetworkController` + the endpoint it owns, null =
  offline; composed, not inherited), its `ActionRuntime` (`InputRuntime()`), its save
  (`Saves()`), its HTTP binding and TLS backend, a headless flag, and an instance time scale. Its
  run bus is `RunBase`'s `Events()`: set on every scene it creates (`SetSceneRun`), so
  `scene.events` and the run bus are ONE object, drained once per frame by the instance
  (`DrainRunEvents`). Each of these is registered as a run service, which is how facades and
  engine libraries below it reach them. It BORROWS everything shared (subsystems, resource manager
  / content DB, factories, reflection registries).

The dependency arrow points DOWN (runtime -> scene): `ApplicationRun` owns a `SceneManager`; the scene
lib never references `ApplicationRun` (a scene knows only its `IRun`).

## `SceneManager` (scene lib)

A group of scenes as a first-class scene-lib object - the multi-scene model (a run manages a SET of
scenes with a current one, not a single scene). It owns membership (`CreateScene`, `ActivateScene`
/ `DeactivateScene`, `DestroyScene`, `Clear`, and the current scene), the group time scale and the
run its scenes belong to (`SetSceneRun`), and it TICKS its own group (`BeginFrame` for the fixed
lane and `Update`, each taking a `FrameTime`), with the run's mutations deferred while it does. A
destroy asked for mid-tick waits for the end of the tick or the run's flush. Scene assembly +
lifecycle run through the DECLARATIVE composition layer ([[scene-composition]]): `CreateScene`
assembles through the installer `SceneSubsystem` wires onto each manager, and teardown notifies
`ISceneObserver`s at the `Destroying` stage.

`SceneSubsystem` is a thin adapter over the pure `SceneRegistry` value type: the composition, the
staged `ISceneObserver` list, the registered `SceneManager*` list it ticks (building ONE
`scene::FrameTime` per frame from the subsystem manager's plain floats - the runtime layer never
sees the type), and the read-only sweeps (`ForEachManager`, registry-wide `ForEachScene` for
prefab-rebuild / export scans). It owns no scenes. Every owner of scenes creates its own plain
`SceneManager` and registers it (`scenes->RegisterManager(&mgr)` wires the install/uninstall
hooks): a `ApplicationRun` owns one; each editor scene page owns one, as do the preview viewport and
the thumbnail stage.

## Time model

```
effective dt a scene sees = host dt x context (app-wide) x group/instance x scene
```
The group/instance term is the manager's; the editor's editing managers use 1.0. The fixed-step
accumulator is per scene (each `Scene` owns its `FixedStepper`, seeded from the lane's configured
step). At N=1 in the player, context and instance collapse.

## Run-host binding

Run host and scene manager are PAIRED through the run. `ScriptSubsystem` keeps ONE default run host
(`RunHost()`) for scenes outside every run; a run registers its own `ScriptRunHost` as a service.
At `SystemsReady`, `ScriptSubsystem::RunHostFor(scene)` binds a scene's `ScriptSceneSystem` (and
its Level-tier `SceneScriptSystem`) to its run's host, else the default one; a scene's run is set
before its systems assemble. Each owner drives its own host (binding clock + GC + teardown).
Routing is shared logic on any host: `ScriptSubsystem::ConfigureRunHost(host)` installs the
application services, resource swaps and the message route (`scene.spawn` needs no host hook: it
reaches the scene's `PrefabSpawnSystem`); `MaybeTeardownRunHost(host)` tears a host down when
nothing pins it (no game-script hold, none of its scenes simulating, no live behaviors), and
otherwise collects its garbage in full. The game-script hold is a per-host flag; the game script
talks to the instance's host directly.

So: one gameplay context PER RUN - all of an instance's scenes' behaviors AND its game script
share its one run host, and the brain persists across level transitions (MainMenu -> Level1 -> Level2
is one game, one context).

## The `run` facade

`run` (the `Run` facade, backed by the run's `RunBinding` service, which the app fills per
instance) gives scripts the run tier: `run.events()` publishes onto the run bus;
`run.loadSceneAsync(id)` starts an async level load and returns a ticket (`loadComplete(t)`,
`loadProgress(t)`, `loadFailed(t)` polled from a coroutine); `loadScene(id)` is a sync convenience;
`sceneReady` and `currentScene` report the instance's scene; it also carries the run's time scale,
real time and an exit request. Unwired, every call is a safe no-op. The name is lowercase because
a facade named `Game` would collide with the mandatory `Game` orchestrator class.

## Save data

Each instance owns its run's saves (`RunSaves`, the `:save` partition): the values the game keeps
between runs, in stores, each a `SaveFile` (its file, its values, when it was last saved, and
whether it changed since the last write). The host names the saves folder with
`SetSavesFolder(folder)`, which reads its global store: the player uses
`<user data>/<project>.saves/`, the editor's Game tab `<project>/Editor/<project>.saves/` (ignored
per-user state, so testing never touches a player's saves); both name it with
`engine::project::SavesFolder`. The folder holds the global store (`global.save.xml`: the values
every playthrough shares, and the player's bindings) and the game's slots (`slot-<name>.save.xml`,
one playthrough's values each): `OpenSlot` (one open for writing, as it plays or at its save
points), `ReadSlot` (a read-only snapshot for a menu), `Slots` (the one saved last first, by the time
inside each), `CopySlot`, `DeleteSlot` and `SlotTime`; a store is held by a `SaveStoreHandle`, which
reaches nothing once its store is gone. An unreadable file is set aside as `<path>.unreadable` before
anything writes to it. The run's contexts reach the stores through the `Saves` facade, which hands
out a `SaveStore` (the script's `SaveStoreHandle`: `Saves::global()`, `openSlot`, `readSlot`) with
the value calls (`getInt(key, fallback)`, `setInt`, the same for float, bool and string, a float list,
plus `has`, `remove`, `clear` and `flush`); a game hands a store on to what keeps state for it (the
dialogue plugin's `Dialogue.useSave`). `StopRunBehavior`
writes whatever changed; `flush` writes at once; and when the app goes to the background
(`DefaultApplication::OnEnterBackground`, a phone's home button) every instance's saves are written,
since the platform may end the app there without asking. Writes are whole and atomic
(`WriteFileAtomic`), so a crash never leaves a torn save. Specs:
`Documentation/Archive/save-data-history.md`, `Documentation/Archive/save-slots-history.md`.

## Standalone vs editor + headless

- **Player**: exactly one instance, created + started at launch: the N=1 case.
- **Editor**: 0..N instances (Play uses the primary; Play New Instance opens another Game tab on a
  fresh instance). A scene page is NOT a `ApplicationRun`: its Simulate runs on its own
  `SimulationRun` (snapshot->simulate->restore, not fresh-from-cooked->teardown) with no input map,
  save, network or `run` facade.
- **Headless instances** (simulate, don't render, no listener) are the in-editor dedicated server for
  the networking track.

## Deferred

Player multi-scene compositing (N instances in the PLAYER hit the single-scene render path; the editor
is unaffected via per-viewport RTs), and the one-shared-editor-manager alternative to per-page managers:
`Documentation/Archive/game-instance-followups-history.md`.

---

The design history (the retrofit diagnosis, the original ownership audit, what blocked multiple
instances, the phased plan, the Zero `GameSession` precedent, and the run-host-ownership
investigation) is in `Documentation/Archive/game-instance-design-history.md`; the run type's is in
`Documentation/Archive/run-context-history.md`.
