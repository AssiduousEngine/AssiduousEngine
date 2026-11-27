# Scene composition & observation

> Status: SHIPPED (composition, observation and the `FrameTime` lane cutover)
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[game-instance-track]]

The scene layer of the engine is a "world model + systems" foundation (`foundation.scene`) driven by the
runtime tier (`foundation.runtime`, `engine.*` subsystems) through a single broker, `SceneSubsystem`
(`engine.scene`). Scene assembly is **declarative composition** (what systems a scene is made of, a
static blueprint built once from every engine domain's declaration) and scene lifecycle reactions
are **ordered observation** (who reacts as a scene comes and goes). The two are separate.

Related: [[game-instance]], [[runtime-host]], [[scripting]] (the one-context rule, now per instance).

## The pieces

**In `foundation.scene`** (runtime-free; `Code/Foundation/Scene/SceneComposition.cppm`, the
`:composition` partition, and `FrameTime.cppm`, the `:frame_time` partition):

- **`SceneModule`** - one domain's "how to build a scene": a stable id, an `install(Scene&)` function
  (its managers and settings systems), a `registerReflection()` function (its component
  reflection), and an optional `dependsOn` list for cross-domain construction order.
- **`SceneComposition`** - the blueprint `Build` makes from a module list: topologically sorted by
  `dependsOn`, each module copied BY VALUE with `dependsOn` dropped (build-time data only), so a
  composition is self-contained. A dependency cycle is broken with a `LOG_ERROR` naming the module.
  `Instantiate(Scene&)` installs every module in order, then every runtime contribution;
  `RegisterReflection()` registers every module's reflection; `ModuleCount()`.
- **`SceneModuleContributions`** - scene managers added at run time: a native plugin's component
  managers, keyed by the installed system's `TypeId`. Every `Instantiate` appends them, scenes
  already alive get them through the live-scene sink `SceneSubsystem` installs (with a hook that
  resolves records preserved while the manager was absent), and `Remove` takes them out of live
  scenes before the plugin unloads. Process-single (`Global()`).
- **`ISceneObserver` + `SceneLifecycleStage`** - `Composing -> SystemsReady -> Destroying`. An
  observer registers for a stage with an `Order()` (lower first). There are no Started/Stopped
  stages: `Scene::Start`/`Stop` are called directly on the scene, and domain logic has
  `SceneSystem::OnSceneStarted`/`Stopped`.
- **`FrameTime`** - the one value type carrying the time-scale chain (`rawDt`, `contextScale`,
  `groupScale`, `sceneScale`) plus the lane's configured `fixedStep`; `ContextDt()` and `SceneDt()`
  compose it in one place. The fixed-step accumulator is per scene (each `Scene` owns its
  `FixedStepper`).
- **`SceneRegistry`** - the pure, non-`Subsystem` state: the composition, the observer list, the
  registered `SceneManager*` list, the cross-scene sweeps (`ForEachManager`, `ForEachScene`) and the
  lane fan-out (`BeginFrame`/`Update` with a `FrameTime`). Testable with no `SubsystemManager`.
- **`SceneManager`** (`:manager`) holds only type-erased install/uninstall hooks
  (`SceneInstaller`/`SceneUninstaller`), so it never imports `:composition`.

**In the engine:**

- **`engine.domain`** (`Code/Engine/Engine.Domain`) - `DomainModule`: what one engine domain
  declares, once, in its own library: its scene facet (`installScene`, `registerReflection`), its
  script facade registrar, its resource modules, and `dependsOn`. Every facet is optional (input has
  no scene content; `run` is only a facade). `EngineComposition::Build` makes a composition from a
  list of them and answers for a facet on demand: `Scene()` (a `SceneComposition` built from every
  module with scene content), `ResourceModules()`, factory descriptions and `CreateFactories`,
  `RegisterReflection`, `RegisterScriptFacades`, `RegisterResourceTypes`.
- **`engine.composition`** (`Code/Engine/Engine.Composition`) - the composition root: the ONE list of
  domains (render, animation, physics, audio, script, net, spline, prefabs, input, run, and gui
  when the GUI is built; particles, terrain and navigation are official plugins that add their
  domains through `EnginePluginServices`), and the facets every consumer
  asks it for: `FullComposition()`, `FullSceneComposition()`, `AddAllSceneManagers(scene)`,
  `RegisterAllSceneComponentReflection()`, `RegisterAllResourceTypes()`,
  `RegisterAllScriptFacades()`, and the scene export support (`CollectSceneStreams`,
  `ScanSceneReferences`). The runtime (`DefaultApplication`), the editor, the headless tools and
  the samples compose from this root and nothing else. A plugin adds a domain with
  `AddPluginDomain` (refused if it has a scene installer: plugin scene managers go through
  `SceneModuleContributions`), through `EnginePluginServices`, which records what each plugin
  added so unload reverses it. The guard is `Engine.Composition.Tests`, which counts the domains,
  the scene modules and the factories, plus the `kSubsystemFacadeNameCount` tripwire.
- **`SceneSubsystem`** (`engine.scene`) - the thin adapter over a `SceneRegistry`: it owns the
  composition (`SetComposition`/`Composition()`), the observer broker
  (`RegisterObserver`/`UnregisterObserver`), and wires each registered manager with an installer
  (notify `Composing`, `Instantiate`, notify `SystemsReady`) and an uninstaller (notify
  `Destroying`). It is THE bridge for time: each frame it builds one `FrameTime` from the
  subsystem manager's plain floats (`TimeScale()`, `FixedTimeStep()`) and fans both lanes out with
  it; each `SceneManager` contributes its group term and each scene its own.
- **`DefaultApplication::Configure`** calls `SetComposition(engine::FullSceneComposition())` and
  registers its instances' managers. Subsystems that react to scenes are `ISceneObserver`s wiring
  per-scene state at `SystemsReady` / `Destroying` (Render, Animation, Physics, Audio, Script, GUI,
  and the plugins' Particles, Terrain, Vegetation and Navigation); `DefaultApplication` observes `SystemsReady` to point
  each scene's `PrefabSpawnSystem` at its resources.
- **Scenes and runs**: a `SceneManager` sets its run on every scene it creates before assembly
  (`SetSceneRun`), so systems binding at `SystemsReady` see it: `Scene::Run()` and
  `Scene::Events()` (the run's bus). The lanes run with the run's mutations deferred
  ([[game-instance]]).
- **Scene facades are scene properties**: each domain's script facade registration gives the script
  `Scene` its facade as a property (`foundation::script::ExtendScene`, over Core's
  `ExtendProperty`), so scripts read `scene.physics`, `scene.render`, `scene.audio` and so on.
- **Handles across scenes**: a scene takes a slot in the process's `SceneTable` as it is made and
  frees it as it goes. A `WeakSceneRef` (a slot and a generation) reads null once its scene is
  destroyed, so a script handle kept across a level load does nothing rather than reach freed
  memory.

Every scene carries the FULL system set regardless of which subsystems exist; absent subsystems leave
their systems unwired (inert pools / no-op ticks). Per-configuration compositions remain the designed
escape hatch when a consumer wants a subset.

## Why (the strains it removed)

The earlier design was correct at the layer level but strained at the integration level:

1. **Scene assembly was imperative, duplicated, and coupled to live subsystems.** Each subsystem
   registered itself as scene-aware and injected its managers into each new scene; a parallel
   headless list duplicated the manager set and was kept honest by a count tripwire. Two sources of
   truth that could drift.
2. **One interface conflated assembly and observation**, and baked in a two-pass ordering; ordering
   leaked into the unrelated `Subsystem::UpdateOrder()` axis.
3. **`SceneSubsystem` did three jobs** (the observer list, the manager list, the lane driver).
4. **The time model was implemented three times** (`SceneManager`'s two lanes and
   `ApplicationRun::TickRunBehavior`), with a context-level fixed lane the scene lane never used.
5. **Registration was raw-pointer + linear de-dup**, with a real dangling-pointer hazard.

## What stays (the kept invariants)

- **Dependency direction points down.** `ApplicationRun` (runtime tier) owns a `SceneManager`; the scene
  library never references the runtime tier. The scene lib stays runtime-free and context-free.
- **Per-scene systems as value-pool component managers.** Components are plain value data in sparse sets
  (`Component.cppm`); behavior lives on `SceneSystem`/`ComponentManager<T>`, not on components.
- **Type-indexed, RTTI-free composition.** `SubsystemManager::GetSubsystem<T>()` and
  `Scene::GetSystem<T>()` resolve through reflected `TypeOf<T>()` (`-fno-rtti`).
- **The multi-scene / multi-instance model.** `SceneManager` as a first-class *group*, `0..N ApplicationRun`
  (play-in-editor + in-editor headless dedicated server).
- **The time layering** host x context x group x scene, composed in one place (`FrameTime`).

## Layering rules

`foundation.scene` and `foundation.runtime` are strict SIBLINGS - zero imports in either direction
(both depend on `foundation.run`). The subsystem manager never sees `scene::FrameTime`;
`foundation.runtime` exports plain floats (`TimeScale`/`FixedTimeStep`). `FrameTime` is CONSTRUCTED
at the engine bridge (`SceneSubsystem`) and handed DOWN. No `import foundation.scene` may appear
under `Code/Foundation/Runtime`. Subsystems have no fixed phase: the step is configuration the scene
bridge reads; the host's own accumulator drives only the app-level `OnFixedUpdate` hook.

## Decision log

- **`SceneModule` as a function table, not a reflected type.** Its responsibilities are static
  construction-time data; a plain value type with function pointers keeps `:composition`
  dependency-light.
- **`dependsOn` as data, not `UpdateOrder`.** Cross-domain construction order is a property of the
  *composition*, not of a per-frame tick order.
- **`SceneRegistry` as a value type, not another `Subsystem`.** The pure state is testable without a
  subsystem manager; `SceneSubsystem` remains the only thing that *drives* a frame lane.
- **`FrameTime` over individual dt parameters.** One place to reason about determinism, pause,
  slow-mo, and fixed-step interpolation across scenes *and* scripts.
- **The composition copies modules by value.** A composition that retained caller pointers
  dereferenced dead stack objects once (a GCC-only segfault); a regression test builds from
  block-scoped modules, destroys them, then instantiates.
- **One declaration per domain, one root.** Each domain declares a `DomainModule` in its own library
  and `Engine.Composition` lists them once; consumers ask the root for a facet rather than
  collecting lists of their own, so no executable can drift from the domains it links.

## Deferred

- Rename primary module interface units to `Module.cppm` (`SceneModule.cppm` is the primary
  interface unit of `foundation.scene` and defines no type; the file name merely squats the type
  name). A mechanical pass; folder == target == module is a standing convention in
  `Process/CONVENTIONS.md`.

---

The engine-composition design and its history are in
`Documentation/Archive/engine-composition-history.md`.
