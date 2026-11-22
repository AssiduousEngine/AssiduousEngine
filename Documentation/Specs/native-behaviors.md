# Native behaviors

> APPROVED 2026-11-19 as part of the script backends spec (proposed 2026-11-18), split out on
> 2026-11-21 with no decision changed: [script-backends-as-plugins.md](script-backends-as-plugins.md)
> keeps the languages as plugins, this spec the game logic that needs no language. Nothing here is
> built. Read on master at `0618e177f`.
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
  asset, not the plugin, since a plugin is shared by many projects and writes into none: the
  behavior picker lists the registered native classes beside the project's assets, and picking one
  that has no asset yet makes it; File > New and the MCP tools make one too. The engines that name
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

## Order of work

Each step stands on its own and leaves the tree green.

0. **N0 Map `Engine.Behavior`.** Which parts are gameplay machinery and which are VM calls; write
   the seam (the class implementation interface, in place of `Instantiate` and `ScriptObject`) into
   this spec before any code moves, with where the cook harvests a native class's properties and
   handlers.
1. **N1 The seam.** `Engine.Behavior` calls classes through the class implementation interface,
   and the service lookup moves to the run's call scope; the three VMs implement it. No behavior
   change; the conformance suites prove it.
2. **N2 Native classes.** A plugin registers native run, scene and entity behaviors: the Native
   Behavior asset and its cook (properties from the marked fields, handlers from the `on` methods,
   the plugin's build stamp in its recipe), the picker's entry for a registered class with no asset
   yet, File > New and the MCP tools, `Fault(reason)`, and the current call in the crash report.
   NativeSample gains one of each kind, and the conformance suites gain a native column beside the
   languages. A project with no language enabled then has every kind:
   [script-backends-as-plugins.md](script-backends-as-plugins.md)'s scriptless project gains a
   native run behavior to check with.

None of these waits for the languages to become plugins: the seam comes first, and native classes
then work in a project that also uses a VM.

## Risks

- **The seam's cost.** Every handler call goes through one more interface. The typed call path
  ([scripting-typed-calls.md](scripting-typed-calls.md)) measures script calls against native ones;
  N1 is measured the same way before and after.
- **Scope.** A native behavior is something a game's author writes, so it adds CHANGELOG lines, a
  section in `Documentation/Shipping/Scripting.md` (or a native code guide beside it), and the
  inspector's handling of native classes.
