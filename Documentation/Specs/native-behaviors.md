# Native behaviors

> APPROVED 2026-11-19 as part of the script backends spec (proposed 2026-11-18), split out on
> 2026-11-21 with no decision changed: [script-backends-as-plugins.md](script-backends-as-plugins.md)
> keeps the languages as plugins, this spec the game logic that needs no language. N0 to N2 are
> built (2026-11-22); NativeSample's own scene does not use its native behaviors yet. Read on master
> at `0618e177f`.
>
> The changes to how script calls are made are a separate proposal:
> [scripting-typed-calls.md](scripting-typed-calls.md). Neither depends on the other.

## Goal

Game logic keeps one shape whatever it is written in. A game has a run behavior, a scene behavior
per scene and entity behaviors, with the same lifecycle, events, properties and services in
AngelScript, Luau, C# and C++. In C++ they are native classes behind a gameplay layer the engine
owns, not code in a VM, so a game with no language enabled still has all three kinds.

## The three kinds today

A class the engine runs derives from a kind; the kind is recorded in its `BehaviorClass`
(`Script.Resource/ScriptResource.cppm`) with its language, class name, declared properties (with
defaults, which the inspector overrides per entity or per scene) and declared handlers. Everything
in that record is language-neutral. Only the code behind it (source, bytecode, an assembly) is a
VM's. Each place that takes a class takes only its kind.

| Kind | Declared by | Run by |
|---|---|---|
| `RunBehavior` | the project's `runBehaviorId` (`Engine.Project/ProjectModule.cppm`) | `PlayerApplication::LoadAndStartRunBehavior`, `ApplicationRun::StartRunBehavior`: `onStart`, `onUpdate(dt)`, `onStop`, `on<Event>` |
| `SceneBehavior` | `SceneBehaviorSettings.behaviorClass` on each scene (`Engine.Behavior/BehaviorSubsystem.cppm`) | `SceneBehaviorSystem`, one object per scene |
| `EntityBehavior` | `BehaviorComponent.behaviors` on entities (`Engine.Behavior/BehaviorComponents.cppm`) | `EntityBehaviorSystem`: `onEnable`, `onStart`, `onUpdate`, `onDisable`, `onDestroy`, `on<Event>`, messages, entity events |

Every class is made through one call, `BehaviorRunHost::Instantiate(class, arguments, kind)`
(`BehaviorSubsystem.cppm:441`), which returns a VM's `ScriptObject`; its handlers are called on
that object by name. That call and that object are where the seam goes.

## What assumes a VM

- `DefaultApplication` imports the backends under `#ifdef` and holds the `BehaviorSubsystem`.
- `ApplicationRun` owns the run's `BehaviorRunHost` and its run behavior's object, and registers
  the run's services (`run.Get<T>()`: its run binding, save, input, HTTP and networking).
- `BehaviorRunHost::EnsureContext` creates the VM context the first time a kind needs one. With no
  backend for the language it logs an error and the class does not run.
- Facades find the run's services through the *script context* a VM pushes: `CurrentService<T>()`
  reads the context `ScriptCallScope` sets (`Script/IScriptContext.cppm`). A native call has no
  context to push.

A project with no scripts runs today: no run behavior, so the player loads the default scene, and
native components update through their managers (NativeSample). What it cannot have is a run or
scene behavior of its own: native code gets component managers (per entity, per scene system) and
application subsystems (per process), and no run lifecycle, no event inbox, no per-scene object and
no inspector-overridable properties on any of them. A plugin's per-run service is the nearest it
has today (the dialogue plugin's runner is one, made for every run by a `RunServiceContribution`),
but a service is the engine's, not the game's, and has no properties a scene or an entity
overrides.

## What is not about a VM

Most of `Engine.Behavior` (about 2,600 lines) is the run's gameplay machinery:

- the kinds and their lifecycles: enable and start edges, update intervals, entity suspension,
  per-behavior fault isolation;
- events: `on<Event>` handlers subscribe to the run's `EventBus` (`ScriptEventSubscriptions`);
- entity messages (`entity.send`, queued and delivered at a set point in the frame);
- entity events: contacts, triggers and animation events, which physics and animation post to the
  scene (`Scene::PostEntityEvent`) and `EntityBehaviorSystem` hears as a sink, queued with the
  messages. Native code hears them already, through the same sink interface;
- declared properties, their defaults and the authored overrides;
- the run's services, which facades reach through `CurrentService<T>()`;
- scene loads a run starts, the run's save, its networking.

The VM-specific part is narrower: creating a context, compiling or loading code, binding a class's
methods (`ScriptMethod`), calling them with arguments, budgets, coroutines and the debugger. Where
that line falls inside `BehaviorSubsystem.cppm` is not measured; it is the first thing to map (N0).

## The proposal

1. **A gameplay layer, language-neutral.** The three kinds are the engine's concepts, with their
   lifecycles, handlers, properties and services. `Engine.Behavior` keeps that machinery and asks a
   *class implementation* to make objects and call them, in place of `Instantiate` returning a
   `ScriptObject`. The data stays: project, scene and entity still name a class, with its
   overrides.
2. **A language is an implementation of the layer.** AngelScript, Luau and C# implement it through
   their VMs, as they do today (from their plugins, once
   [script-backends-as-plugins.md](script-backends-as-plugins.md) is built; this spec does not wait
   for it).
3. **Native classes are an implementation too.** A plugin's Engine part registers C++ classes
   deriving from `engine::RunBehavior`, `SceneBehavior` or `EntityBehavior` (the naming spec's
   decision): a reflected type with lifecycle methods, the reflected fields it marks as its
   properties, named in the project by a Native Behavior asset (see Decided). The layer calls them
   directly, with no context and no budget; only an event's or a message's queued values are
   converted to the handler's arguments, as they are for a script. This is the native backend worth
   having: native classes behind the gameplay layer, not a C++ VM behind `IScriptManager`.
4. **One API across languages.** The facades are C++ types that read the run's services. A native
   class calls the same facades inside the same call scope, so `RunFacade::loadScene`
   (`run.loadScene` in a script), `SaveFacade` and `scene.events.emit` read alike in C++ and in a
   script. For that, the service lookup moves from the script context to a neutral call scope the
   run owns, which a VM's call and a native call both push. A plugin's facade is no different: the
   dialogue plugin registers `Dialogue` through its domain's `registerScriptFacade` and removes it
   on unload, and a native class would call `DialogueFacade` the same way.
5. **No VM, nothing missing.** With no language enabled, the layer still runs native run, scene
   and entity behaviors. `DefaultApplication` and `ApplicationRun` hold the gameplay layer, not a
   VM; the contexts, budgets and cooks belong to the languages.

Component managers stay what they are: the way to update many entities as data. A native
`EntityBehavior` is the per-entity object form, for the logic a script would hold.

## Decided

- **The C++ lifecycle surface** (2026-11-19): reflected methods found by name, as a script's are,
  not virtual methods on the kind's base class. Nothing in C++ is discovered implicitly: a method
  exists to the engine only when the class's reflection registers it, and the registration gives it
  its script name apart from its C++ one. So the registration is the declaration, and no attribute
  marks a handler:

  ```cpp
  class Orb final : public engine::EntityBehavior { /* ... */ void OnCollected(Entity by); };

  REFLECT_MEMBERS(Orb, "rtti::game")
  {
      builder.Method<&Orb::OnCollected>("onCollected"); // a handler, by the name scripts use
  }
  ```

  The layer takes a native class's handlers from its reflected methods named `on<Name>`, as the
  cooks take a script class's: `onCollected` hears the `Collected` message, `onContactBegin` the
  `contactBegin` entity event, `onStart` and the rest the lifecycle. The C++ name keeps the
  engine's style (`OnCollected`). The arguments come from the reflected signature and are converted
  from the queued values as for a script call, an `EntityHandle` made the `Entity`. This keeps
  "declare `onOrbCollected` to hear OrbCollected" identical across languages, which virtuals
  cannot, since an event handler is declared by its name. A marker on the registration (as `.Pure()`
  marks a pure method) would be needed only for a method registered under an `on` name that is not
  a handler; helpers simply do not take `on` names.
- **Naming**: [run-and-behavior-naming.md](run-and-behavior-naming.md) is built (the kinds as base
  types, `Engine.Behavior` and its types, `ApplicationRun`, the `Facade` suffix, the data
  migrations). What it left here is the service lookup's move to a neutral call scope (proposal 4)
  and how a native class's handlers are found (above).

- **How a native class is named** (2026-11-21): by a thin asset per class, made by the project.
  Every reference to a class (an entity's behavior, a scene's, the project's run behavior) is a
  `Ref<BehaviorClass>`, a guid, and the overrides, the picker and export pruning build on it. A
  **Native Behavior** asset holds only the registered class's name and cooks into an ordinary
  `BehaviorClass` (language `native`), its properties and handlers from reflection; everything that
  names a class names it as it names a script's, with no second reference kind and no data format
  change. A rename or move of the asset keeps every reference (the guid); a renamed C++ class fails
  the asset's cook, naming the class, and is fixed by editing one field. The project makes the
  asset, not the plugin, since a plugin is shared by many projects and writes into none. File > New
  > Native Behavior and `asset_create` make one, filling its class from the asset's name (an asset
  named Spinner names the registered class Spinner; the class is the asset's field to set
  otherwise), and the behavior picker then offers it beside the scripts, since it cooks to a
  `BehaviorClass`; an MCP tool lists the registered native classes. The picker does not list
  classes that have no asset yet (user, 2026-11-22: File > New is enough). The engines that name
  native and script classes alike by type name (Flax, Lumina, Zero) need rename redirects for it;
  the rest keep two reference kinds (Godot, ez, Hyperion, traktor).
- **Properties** (2026-11-21): a native class's properties are the reflected fields it marks as
  such, `.PropAttribute("behaviorProperty", true)`; its other reflected fields (internal state it
  reflects for saving or debugging) are not shown or overridden. The cook harvests the marked
  fields into the `BehaviorProperty` list a script's cook writes: the name and its hash, the type
  (f32 Float, i32 Int, bool, String, Color, Float3 Vec3, an entity reference Entity, an asset
  reference Asset with its product type; any other type is a cook error naming the field), the
  default read from a default-constructed instance, `description` from the field's attribute, and
  `replicated` from a `replicated` attribute. The inspector reads the one list either way, and the
  overrides keep their format (keyed by the name's hash, as in every reference engine that unifies
  the two). At run time the layer writes the defaults, then the overrides, into the object's fields
  through reflection, where a script's go through its setters. A list cooked before its plugin was
  rebuilt would be stale, so the plugin's build stamp joins the Native Behavior asset's cook recipe
  (a rebuilt plugin recooks its classes; as decision 8 of the backends spec does for a VM), and the
  layer checks the cooked list against reflection when it makes an object, faulting it (below) on a
  mismatch.

  ```cpp
  REFLECT_MEMBERS(Orb, "rtti::game")
  {
      builder.Property<&Orb::worth>("worth").PropAttribute("behaviorProperty", true)
          .PropAttribute("description", String(u8"Points it gives when collected"));
      builder.Property<&Orb::spin>("spin"); // reflected, not a property
      builder.Method<&Orb::OnCollected>("onCollected");
  }
  ```
- **Reload** (2026-11-21): native classes reload with their plugin while editing, as they do today:
  the editor's `ReloadPlugins` stops every game run, snapshots the live scenes, reloads the plugins
  from fresh copies of their libraries and restores the scenes, and behaviors exist only while
  something runs (play in editor, a scene page's Simulate), so no behavior object outlives a reload.
  The reload also recooks the project's Native Behavior assets (the build stamp above). Reload
  during play is not offered, as Flax and Lumina do not offer it for their native classes. A later
  direction, not a step: carry live objects across a reload as Godot does (save each object's
  properties by their hashes, make the new object, write them back), which reflected properties make
  cheap.
- **Faults** (2026-11-21): a native crash is a crash, as a native component's is today; no reference
  engine isolates a native fault in process, and this build has no exceptions to catch one with.
  Three things make one easier to read and to avoid:
  1. **The crash names the behavior.** The layer keeps a thread-local current call (the class, the
     handler, the entity) around each native call, and the crash handler's `=== SIG` report prints
     it beside the backtrace.
  2. **A native class can fault itself.** `Fault(reason)` on the kind's base takes the path a
     script error takes: the behavior is disabled, the fault logged with its reason, and the
     inspector shows it. It is for a class that finds its own state broken.
  3. **The layer's own errors are faults.** A handler whose arguments cannot be converted, or a
     cooked property list that no longer matches the class, disables the behavior with a logged
     reason, never a crash.

  Surviving a game's crash in the editor needs the game in a process of its own (ez, traktor and
  Godot do this), which is a change to the editor for scripts as much as for native code; it is a
  direction, outside this spec.

## The seam (N0, mapped 2026-11-21)

Read on master at `805e7478d`: `Engine.Behavior` (`BehaviorSubsystem.cppm`, 2,056 lines;
`BehaviorComponents.cppm`; `BehaviorSubsystemImpl.cpp`), `Engine.Run`'s `ApplicationRun`, and
`Foundation/Script`'s `IScriptContext.cppm`.

### What each part of `Engine.Behavior` is

| Part | Gameplay layer or VM |
|---|---|
| `IsReservedHandler`, `EventNameForHandler`, `ScriptEventSubscriptions` (`on<Event>` handlers subscribed to a bus) | layer: works from `BehaviorClass::handlers`, names only |
| `BehaviorRunHost`: `FindService` (the runtime binding, the run's services, the application's), `Binding()` (time, random, resources, message routing), `Advance`'s clock | layer |
| `BehaviorRunHost`: `EnsureContext`, `EnsureContextForFile`, `EnsureClassLoaded` (a module per class, `behaviors#N`), the manager, `RunErrorSink`, the debugger and `DebugPauseTracker`, `Advance`'s and `CollectAllGarbage`'s collector steps, `Teardown`, the game-script hold | VM |
| `BehaviorRunHost::Instantiate` | the seam: checks the kind (layer), then loads and creates on the context (VM) |
| `ScriptPropertyValueToVariant`, `ApplyScriptProperties` | layer, but it sets a property by invoking the script's `<name>=` setter, which is a script convention |
| `EntityBehaviorSystem`: start and enable edges, intervals, suspension, the fault rule (`HandlerResult`), messages, entity events, bus events, hot reload by the bound product changing | layer; it reaches the VM only through `Instantiate`, the object's `Bind` / `Invoke`, `IsDebugPaused`, and the manager's `AdvanceCoroutines` / `CancelCoroutinesFor` |
| `SceneBehaviorSystem` | the same, for one object per scene |
| `ApplicationRun::StartRunBehavior` | VM throughout: it compiles the source or loads the assembly on the run host's context and creates the object there, not through `Instantiate` |

The object every call goes through, `foundation::script::ScriptObject`, is already language-neutral:
`Invoke` by name or by a `ScriptMethod` handle from `Bind(name, argCount)`, and `GetProperty`
(what `pie_run`'s probes read). Nothing in the layer depends on what is behind it.

Two things find services through the VM where nothing needs one:

- `CurrentService<T>()` reads the *context* a backend pushes (`ScriptCallScope`); every facade
  that reaches the run (`run`, `Save`, `Input`, `Time`, `Random`, `Audio`, a plugin's) goes through
  it. A native call has no context to push.
- The dialogue plugin's runner pushes `ScriptCallScope(the run's VM context)` around its staging
  only so the facades it calls find the run (`DialogueRun.cpp`). In a run with no VM that context
  is null, so the facades find no run and the staging's clips and sounds would do nothing (read
  from the code, not run). It is the first case of a
  plugin's native code calling facades; native behaviors make it the common one.

### The seam

1. **A service scope apart from the script context.** `foundation.script` gains a `ServiceScope`
   (a resolver pushed on a thread-local slot, defined out of line as the context slot is, for the
   shared-library build) and `CurrentService<T>()` reads it. `ScriptCallScope` pushes the context
   and its resolver both, so the backends change nothing. The layer pushes
   `ServiceScope(host.FindService)` around every native call, and the dialogue runner pushes it in
   place of the VM context. A facade cannot tell which kind of caller it serves.
2. **The VM moves out of `BehaviorRunHost`.** The VM half (the manager, the context, the class
   modules, the error sink, the debugger and the pause tracker, the collector steps, coroutines,
   teardown) becomes a `ScriptBehaviorClasses` the host holds, made on first use as the context is
   today. The host keeps the layer half (services, binding, clock), and
   `Instantiate(class, args, kind)` checks the kind and asks the implementation for
   `class.language`: `native` goes to `NativeBehaviorClasses`, any other language to
   `ScriptBehaviorClasses`. The per-scene systems ask the host for the script half (null with no
   VM) for coroutines and the debug pause, which they already treat as optional.
3. **The run behavior goes through `Instantiate`.** `StartRunBehavior(const BehaviorClass&)` calls
   `Instantiate(class, {}, Run)` as the entity and scene tiers do, so a native run behavior needs no
   path of its own. The raw-source overload stays, for the tests that compile a source with no cook.
4. **Properties go through the object.** `ScriptObject` gains `SetProperty(name, value)`; a VM
   object's calls its `<name>=` setter, as `ApplyScriptProperties` does today, and a native object's
   writes the reflected field. `ApplyScriptProperties` calls it, so the layer no longer spells a
   script convention.

N1 is these four, with no behavior change; the conformance suites and the dialogue staging tests
prove it.

### What N2 adds on the seam

- **The kinds as C++ bases**: `engine::RunBehavior`, `SceneBehavior` and `EntityBehavior`, each an
  `Object` the layer gives its `Entity` (an entity behavior) or its `Scene` (a scene behavior) on
  creation, as a script's constructor takes them; `Fault(reason)` on each.
- **`NativeBehaviorClasses`**: finds the class in the type registry by the qualified name its
  `BehaviorClass` records (`rtti::game::Orb`, by `FindByName`), checks its base chain for the kind,
  makes it through its reflected creator (`builder.Creator()`: an `Object` from an allocator, as a
  script's object takes its entity or scene after, through the base's `BindEntity` / `BindScene`),
  and wraps it in a native `BehaviorObject`. The bases carry `hiddenFromScripts`, which keeps them
  and every class derived from them out of the languages' surface (`RegisterReflectedTypes`): a
  script reaches a native behavior by its messages and events, and the bases' names are the
  scripts' own base types'. A plugin registers a class by its `StaticType()`. `Bind(name, argCount)` finds the reflected method by its script name and
  parameter count (a handle to its `MethodInfo`); `Invoke` pushes the service scope and the current
  call (for the crash report), and calls the method's typed entry where it has one, else `invoke`
  with the `Variant`s, an argument that does not convert being an error, so a fault; `GetProperty`
  and `SetProperty` read and write the marked fields. No separate registry: a plugin's reflection,
  registered at its load and removed at its unload, is the list of native classes.
- **The Native Behavior asset's cook** sits beside the script cook in `Pipeline/Script.Pipeline`:
  a builder for the new asset that finds the class as `NativeBehaviorClasses` does (the cook loads a
  project's Engine parts, so the reflection is there), harvests the marked fields and the `on`
  methods into a `BehaviorClassSource`, and puts the plugin's build stamp in the recipe.

### Decided (2026-11-21)

- **`ScriptObject` becomes `BehaviorObject`**, renamed in N1 with every use: after N2 it is also a
  native object, and the layer would otherwise call C++ objects script objects.
- **Native classes are found by reflection alone**: a plugin's reflected types, registered at its
  load and removed at its unload, are the list. No registry of their own.
- **`SetProperty` on the object** (seam 4): a native class never answers a script's `<name>=`
  setter names.

## Order of work

Each step stands on its own and leaves the tree green.

0. **N0 Map `Engine.Behavior`.** Which parts are gameplay machinery and which are VM calls; write
   the seam (the class implementation interface, in place of `Instantiate` and `ScriptObject`) into
   this spec before any code moves, with where the cook harvests a native class's properties and
   handlers. Done: The seam, above, and its decisions.
1. **N1 The seam.** The four parts of The seam, above, and the rename of `ScriptObject` to
   `BehaviorObject`. No behavior change; the conformance suites and the dialogue staging tests
   prove it. Done 2026-11-21: the service scope (`0e8d9776e`), the rename, with `ScriptMethod` made
   `BehaviorMethod` beside it (`dd67c7dd7`), `SetProperty` (`1196911c2`), `ScriptBehaviorClasses`
   (`a9133c625`), and the run behavior through `Instantiate`. The game script now loads as every
   behavior's class does (a class module, from its cooked bytecode where it has some), and the
   hosts start a run behavior with no source, since a native one has none.
2. **N2 Native classes.** A plugin registers native run, scene and entity behaviors: the Native
   Behavior asset and its cook (properties from the marked fields, handlers from the `on` methods,
   a hash of every native class's declaration in its recipe, so a changed class recooks), File >
   New and the MCP tools (no picker entry for a class with no asset: user, 2026-11-22),
   `Fault(reason)`, and the current call in the crash report. Built so far: the bases and native
   objects with their faults (`561191f44`), the crash note (`448b286ea`), the asset and its cook
   (`c8dd9b8f1`), the creator (`abeb06376`), NativeSample's three and `native_behavior_list`
   (`ac14be117`), and the behavior conformance test, one probe in AngelScript, Luau and C++ coming
   out the same (Integration.ScriptScene).
   NativeSample gains one of each kind, and the conformance suites gain a native column beside the
   languages. A project with no language enabled then has every kind:
   [script-backends-as-plugins.md](script-backends-as-plugins.md)'s scriptless project gains a
   native run behavior to check with.

None of these waits for the languages to become plugins: the seam comes first, and native classes
then work in a project that also uses a VM.

## Risks

- **The seam's cost.** Every handler call goes through one more interface. The typed call path
  ([scripting-typed-calls.md](scripting-typed-calls.md)) measures script calls against native ones;
  N1 is measured the same way before and after. Measured 2026-11-22 (`build/clang-reldbg`,
  Integration.ScriptScene's measures, three runs each, alternating with `f7238a93f`, the commit
  before the seam): AngelScript did not move, and Luau's calls into the engine were 7 to 11%
  slower (an entity call 122 to 136 ns, an engine call 82 to 90 ns; 5,000 movers 3.11 to 3.29 ms a
  frame). Luau opens a `ScriptCallScope` around every call into the engine, and the seam made it
  push a second thread_local slot through a second accessor in another library. With both slots
  in one thread_local, found once per scope, every measure is back within 4% of the commit before
  the seam, the ranges overlapping (an entity call 122 against 123 ns, 5,000 movers 3.12 against
  3.12 ms).
- **What a native behavior costs** (measured 2026-11-22, the same tree, three runs; the scene
  measures gained a native behavior column, the same work as the scripts' through the same
  subsystem and facades). The median frame, against a component manager's own loop and the
  languages:

  | Median frame | Component | Native behavior | AngelScript | Luau |
  |---|---|---|---|---|
  | 1,000 empty update handlers | 0.006 ms | 0.168 ms | 0.27 ms | 0.20 to 0.21 ms |
  | 100 movers | 0.004 ms | 0.022 to 0.023 ms | 0.057 to 0.059 ms | 0.061 ms |
  | 1,000 movers | 0.040 to 0.042 ms | 0.23 to 0.24 ms | 0.58 to 0.59 ms | 0.62 to 0.64 ms |
  | 5,000 movers | 0.21 ms | 1.18 to 1.24 ms | 2.96 to 3.04 ms | 3.15 to 3.20 ms |
  | 1,000 component fetches | 0.005 ms | 0.078 to 0.082 ms | 0.44 to 0.45 ms | 0.38 to 0.39 ms |

  A native behavior's handler costs about 165 ns a call against a component's 6 ns, near Luau's
  200: the dispatch, not the work, sets it (the reflected Invoke with Variant arguments, the
  service and crash note scopes, the subsystem's own walk), and a script's handler pays the same
  path before its VM. Where the 165 ns goes is not yet measured part by part. Its facade calls
  are cheap (a mover's work beside an empty handler adds about 70 ns; a component fetch through
  the facade's of() is 80 ns, a Variant made and a manager found, against the manager's own 5 ns).
  Nothing allocates per frame.
- **Scope.** A native behavior is something a game's author writes, so it adds CHANGELOG lines, a
  section in `Documentation/Shipping/Scripting.md` (or a native code guide beside it), and the
  inspector's handling of native classes.
