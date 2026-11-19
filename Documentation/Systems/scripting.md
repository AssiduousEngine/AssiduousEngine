# Scripting

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[script-behaviors-p1]] / [[luau-backend-track]] / [[scripting-backend-neutrality]]

Backend-neutral, reflection-driven gameplay scripting. A language backend is a LIBRARY,
never the architecture: the neutral contract exposes the ENTIRE engine reflection surface
to any backend for free, and a backend is "done" only when the shared conformance battery
is green. Two backends are certified and independently switchable (see
[[script-backend-gating]]): **AngelScript** and **Luau** (the go-forward primary). Wren was
the neutrality-proving first backend and was RETIRED 2026-08-19 (1b220034) once it had
served that purpose - historical mentions in older specs refer to it.

## Architecture (the neutral contract - `foundation.script`)

- `IScriptManager` / `IScriptContext` - VM-agnostic; everything crosses as `Variant` +
  `TypeInfo`. `ScriptObject::Invoke(method, Span<Variant>)` calls a script method;
  `IScriptErrorHandler` receives a `ScriptError`: kind (compiler warnings have a kind of their
  own and never fail a cook), module, line, column, message, and for a runtime fault the script
  call stack, innermost first, which the log prints beneath the message. Numbers of every width
  cross through one conversion (`ScriptNumbers`) in both backends. A context's host services are found
  by type (`CurrentService<T>`): its run host answers with its runtime binding, then its run's
  services, then the application's (subsystems register theirs with their manager), so there are
  no process globals and no lists of what to install.
- **Values and identity**: a reflected type whose bytes copy it (`TypeInfo` records trivially
  copyable and destructible) crosses as a plain VALUE: the math types (Float3, Quaternion,
  Color, ...), `Guid`, the script `Entity` and the scene's facades. A property read is a copy (as a
  C# struct), a value's stored fields are real members, AngelScript refuses a field write through a
  property read, and a container element is reached by reference. A type that stands for one live
  thing declares it in its reflection with `HasIdentity` (a component's script methods do), and
  stays a handle to the live object.
- **Budgets** (`ScriptBudget`, `IScriptManager::SetBudget`): seconds per outermost call
  (`callSeconds`, default 5), heap bytes (`heapBytes`, 512 MiB) and the most a frame's garbage
  step takes (`collectSeconds`). A call past its time or memory fails with an error, that call
  only. Luau stops it from the VM interrupt and refuses heap growth in its allocator; AngelScript
  stops it from a line callback, counts and caps its process-wide allocations, and fails
  recursion past 20000 nested calls. The run steps the collector each frame within
  `collectSeconds` (`StepGarbage`) and collects in full where a pause is expected (a scene stopped
  or unloaded).
- **Check** (`IScriptManager::Check`): compiles a source against the registered surface without
  running any of it (no global initializers, no `main`, no chunk top level) and reports errors
  with line and column; the editor's as-you-type checks use it.
- `RegisterReflectedTypes(manager)` emits the whole reflection registry into a backend in
  one call. Facades are ordinary reflected classes, so a new backend inherits the full
  engine API from reflection.
- **Backend registry** (`ScriptBackendRegistry`): `{languageId, displayName, fileExtensions,
  factory}`; resolves a backend by language id or file extension. No consumer names a
  concrete backend type. `CreateScriptManagerForLanguage(id)` is the resolve entry point.
- **Conformance battery** (`Code/Foundation/Script.Tests/BackendConformance.h`): a shared
  doctest battery every registered backend must pass - reflected-type emission (incl.
  statics on constructible classes), Variant round-trips, per-context services +
  `CurrentScriptContext` nesting, error propagation, GC hook, and the optional-capability
  sections. A backend is certified when the battery is green, not "hopefully".
- **Capability flags** (`ScriptCapabilities`, reported by `IScriptManager::Capabilities()`):
  Coroutines, Delegates, Bytecode, Debugger, Profiler. The MANAGER is the single source of truth -
  consumers with a backend in hand ask it. AngelScript + Luau: Coroutines, Delegates, Bytecode
  and Debugger (no backend implements Profiler).
- **Two-phase type registration** (the AngelScript rule): `RegisterType` COLLECTS;
  `IScriptManager::FinalizeTypes()` (called by `RegisterReflectedTypes`, and defensively
  before the first `CreateContext`) does declare-all-types-then-bind-all-members. A backend
  may defer all emission to finalize (AngelScript needs it; Luau finalizes lazily).
  Facade authors write against the contract, not a backend quirk (`builder.Constructor()`
  is part of the documented contract).

## Tiers + the run host (`engine.script`)

Three tiers, all built. The run host (`ScriptRunHost`) resolves the backend from
`BehaviorClass.language` - the whole stack is backend-neutral. A run (a `ApplicationRun`, or a scene
page's Simulate, `SimulationRun`) registers its run host as a service, and the script subsystem
runs a scene's behaviours on its run's host (the default host only for a scene outside every
run). The host keeps its scripts' clock.

- **Game** (project orchestrator, the scripted `IApplication` counterpart): a `Game` class
  driven `new()` / `onStart()` / `onUpdate(dt)` / `onStop()`, coordinating ABOVE scenes.
- **Level** (per-scene): a `Level` class with `onStart`/`onUpdate`/`onFixedUpdate`/`onStop`,
  bound to a value-facade Scene.
- **Behavior** (per-entity): scripts ticked under simulation. This is the main surface.

**One gameplay context per run** (locked): a run owns ONE `IScriptContext` hosting the game
script + all behavior instances (shared module space). The FIRST-loaded behavior locks the
run's language; a behavior in a different language is refused and disabled
(`EnsureContext`/`WarnLanguageMismatchOnce`) - mixing languages in one run is by design not
supported. Stop tears the context down (isolation is between RUNS, the PIE rule).

Each behaviour class loads alone, in its own module, and a class that fails to load is not kept.
AngelScript discards a replaced class's module once a newer one declares all its classes and no
instance, coroutine, delegate or running call uses its code; Luau's replaced class table is
garbage collected. AngelScript calls reuse their contexts: a pool behind the engine's
`RequestContext` / `ReturnContext` keeps up to eight idle ones. Script handles to a scene
(entities, the scene, its facades, ray hits) hold a `WeakSceneRef`, so a handle kept across a
level load reads null rather than a freed scene.

## Entity behaviors

- **`ScriptComponent`** (serializable, value-pool, one per entity): an ordered array of
  `Behavior{ script: Ref<BehaviorClass>, enabled, updateInterval, overrides: [{nameHash, Variant}] }`
  plus a runtime `ScriptObject` instance. Execution order = array order; cross-entity order is
  unspecified. Prefab members work via the component-granular delta machinery (overrides
  live in the payload).
- **Authoring idiom** is shape-identical across backends, spelled in each language's OOP: a
  constructor takes the owner (`new(entity)` / Level `new(scene)` / Game `new()`), then
  handlers dispatch by presence (`onStart`/`onUpdate(dt)`/`onFixedUpdate(dt)`/`onEnable`/
  `onDisable`/`onDestroy`, plus event handlers). Ticking is simulation-gated
  (`OnlyWhenSimulating`); optional per-behavior `updateInterval` throttles.
- **Properties**: declared per language (AngelScript typed member fields + `[metadata]`;
  Luau instance fields walked from the constructed table). A property may be marked
  `replicated` (AngelScript's metadata word, a Luau class's `replicated` list) and then
  replicates on networked entities (networking.md).
  The **cook harvests** them (compile the class in the cook VM, construct + walk / read the
  typed members), so the editor renders the inspector with NO VM. Editor values are stored
  as name-HASHED override blobs (rename-safe) and re-applied after hot reload.
- **Hot reload** (edit during play/simulate): resource reload -> per-instance re-instantiate
  -> re-apply hashed overrides; old-instance coroutines stop. (Live-value migration is a
  deferred follow-up - see [[Archive/scripting-followups-history]].)

## Events into scripts

- **Physics**: `onContactBegin/End(other, point, normal, speed)`, `onTriggerEnter/Exit`.
  Script does NOT depend on physics - `engine.integration`'s `ScriptPhysicsContactBridge`
  (`DefaultApplication` owns it) registers as physics' `IContactListener`, resolves an
  entity-packed body user word to the exact entity and dispatches.
- **entity.send**: `entity.send("heal", amount)` invokes `onHeal(amount)` on every behavior
  of the target that declares it. Delivery is DEFERRED (queued + drained at the tick's top
  level - re-entrant VM calls are unsafe). Payload is a single `Variant`, not N overloads
  (see [[overloaded-name-contract]]).
- **Event bus**: `emit(name, payload)` -> `on<Event>` (NATIVE name-keyed bus, C++-first;
  `foundation.messaging`). ONE bus per run (messaging.md): the run owns it (`IRun::Events()`,
  `run.events()`), and a scene's bus is its run's (`Scene::Events()`), so `scene.events` and the run
  bus are the SAME object - a behavior's emit reaches the Game tier with NO relay. A scene outside
  every run (headless scratch: transcode, validate) has a NULL bus and never emits. The run drains
  it once per frame; scenes never drain it.
- **Delegates**: `IScriptDelegate` wraps a script function as a native callback (certified
  both typed backends; the `DelegateSignal` facade uses it), with a Detach-on-teardown
  protocol.
- **Coroutines** (all backends): `startCoroutine` / `waitSeconds` / `waitUntil`, resumed by
  `AdvanceCoroutines` and cancelled per-instance on destroy/disable (and on entity
  deactivation - [[entity-active-state]]). AngelScript + Luau use pooled resumable
  contexts/threads.

## Engine API surface (facades)

The reflection registry exposes every reflected type. On top, a curated gameplay facade set
(`foundation.script.facades` + per-subsystem facades, kept OUT-OF-TREE per [[facade-pattern]]):
`entity` (transform/name/destroy/findChild/components), `scene.find`, the scene's own systems as
properties of the scene (`scene.prefabs.spawn(prefab, position)` over its `PrefabSpawnSystem`, see
[[text-scenes]]; `scene.physics.rayCast`, `scene.animation`, `scene.audio`, `scene.render`,
`scene.particles`, `scene.splines`, `scene.scripts`, `scene.debugDraw`, `scene.environment`,
`scene.postProcess`), `scene.events`, `Input`, `Audio`, `gui` (the Foundation GUI's script root),
`Net`, `Http`, `Save`, `run`, `RenderQuality`, `Log`, `Time`, `Random`, and each component's
`X.of(entity)` / `X.addTo` / `X.removeFrom`. The script `Scene`
lives in Foundation (`Script.Facades`) and those systems in the engine domains above it, so each
domain adds its property at registration with `ExtendScene<&X::ForScene>("name")` (Core's
`ExtendProperty`): the backends see an ordinary read-only property. Rule:
a facade lands only WITH a sample that uses it (no API-first surface). Write facades with
natural C++ types (i32/i64), not f64 ([[script-facade-numerics]]). `Engine.Composition`
(`RegisterAllScriptFacades`, each domain's facade registrar declared on its DomainModule) is
the composition root for the complete bound surface (engine-composition.md).

## Cook + runtime resources

- **`BehaviorClass`** (cooked product, `foundation.script.resource`): source text + harvested
  metadata (className, properties, declared handler set, `sourceName`, version) + the sources
  of the files it includes + optional cooked **bytecode**. The class a script plays is the one its
  asset records (`className`), else the one named like its file, else its only class
  (`ChooseScriptClass`); anything else fails the cook.
- **`#include`** (AngelScript): resolved against the including file, inside the project's
  sources; the builder declares every included file as a cook input (editing one recooks its
  includers), and the record carries the included sources, which a class compiled from source
  loads beside its own as sections named by path (so errors and breakpoints name the file).
  Luau has no include directive.
- **Per-language cook** (`IScriptLanguageCook` / `ScriptLanguageCookRegistry`): compile-check
  (surfaces file/line cook errors), harvest properties/handlers, emit tier starters. The
  cook fingerprint includes the vendored Luau version (Luau bytecode is not stable across
  versions) and AngelScript's binding version, and a script product's recipe carries a hash of
  the script surface it compiles against (`EnvironmentHash`, `ScriptSurfaceHash`), so a change to
  the bound facades recooks every script. AngelScript global initializers run on the game's load,
  under the budget, never in the cook.
- **Bytecode consumed at runtime** (Luau + AngelScript): the run host LOADS the cooked
  bytecode instead of recompiling source. Luau loads each class's blob as its own chunk;
  AngelScript LoadByteCode's each class into its own module (`CreateInstance`/`FindFunction`
  search all owned modules newest-first). Debug info + section names are preserved, so
  breakpoints line up on bytecode-loaded classes.

## Debugger

In-process step debuggers for **AngelScript and Luau** (breakpoints, step into/over, call
stack, locals, capture-object expand), driven through the run host with game-pause
(`IsDebugPaused` gates the tick) and editor UI (breakpoint gutter + call-stack/locals panel).
Suspension-based, non-blocking. Full detail: [[Systems/script-debugger]].

## Editor

- **ScriptPage** (`Editor.Script`, on the GUI code pages' base): edit source -> a check a
  moment after each edit (compiled, never run) into the problems list -> Save, recook, hot
  reload. Completion and hover (`ScriptLanguageService`) and the API browser tree
  (`BuildScriptApiTree`) all read one `ScriptApiSurface`, built from `DescribeBoundApi()`; while a
  Game run is paused in the script, hover shows a name's value. The gutter's breakpoints are shared
  with the Game tabs.
- **Language definitions** are data (`Data/Editor/Code/angelscript.xml`, `luau.xml`), read by
  `GUI.Code`'s code editor; no per-language editor module.
- **New-Asset starters**: one creator per registered backend per tier (Behavior/Level/Game),
  seeded from the cook's tier template - e.g. "Luau Behavior". A backend appears only when its
  COOK is registered (the cook registration also registers the backend).

## MCP / tooling

`IScriptManager::DescribeBoundApi()` reports the accurate, backend-reported bound surface
(reflection-vs-backend diff conformance check keeps it honest, read-only properties marked); the
MCP `script_api` tool reports it per language. `IScriptManager::Declarations()` writes the same
surface as a declarations file for an external editor (AngelScript's `as.predefined`, Luau's
`engine.d.luau`), and the `script_declarations` tool writes them into a folder. The editor's
`script_create` and `script_validate` tools make and check a script. See [[mcp-track]].

---

Design lineage + resolved open questions: `Documentation/Archive/scripting-design-history.md`.
Deferred follow-ups: `Documentation/Archive/scripting-followups-history.md` (open items: week-2026-09-05).
