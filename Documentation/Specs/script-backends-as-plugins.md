# Script backends as plugins

> APPROVED 2026-11-19 (proposed 2026-11-18; refreshed after
> [run-and-behavior-naming.md](run-and-behavior-naming.md) and [entity-events.md](entity-events.md)
> were built). Nothing here is built. Read on master at
> `8a105845e` (AngelScript, Luau, the engine layer, the plugin system) and on `dotnet` at
> `80a3fba85` (C#).
>
> The changes to how script calls are made are a separate proposal:
> [scripting-typed-calls.md](scripting-typed-calls.md). Neither depends on the other.

## Goal

Scripting plays an optional role. A language backend is a plugin: a project enables the
languages it uses, and a project that uses none carries no VM.

Game logic keeps one shape whatever it is written in. A game has a run behavior, a scene
behavior per scene and entity behaviors, with the same lifecycle, events, properties and services
in AngelScript, Luau, C# and C++. Without a VM the three kinds still exist; their classes are
native (see "Game logic without a VM").

## Is it feasible

Yes. The plugin system's parts already match what a language is made of, and code outside a
backend names it in only a few places.

**The fit with the plugin parts** (`Specs/plugins.md`, decision 2):

| Part | What the language puts in it | Today's library |
|---|---|---|
| `<Lang>.Engine` | the VM and its `ScriptBackendRegistry` registration | `Foundation/Script.AngelScript`, `Script.Luau`, `Script.DotNet` |
| `<Lang>.Pipeline` | its `IScriptLanguageCook` (check, cook, new-file template) | `Pipeline/Script.AngelScript.Pipeline`, `Script.Luau.Pipeline`, `Script.DotNet.Pipeline` |
| `<Lang>.Tooling` | export checks (C#: publishing the assembly, the runtime to ship) | parts of `Script.DotNet.Pipeline` |
| `<Lang>.Editor` | anything the editor shows for that language alone | none yet |

**Every place non-test code names a backend today** (master):

- `Engine.DefaultApp/DefaultApplication.cppm:46-49` and `DefaultApplicationImpl.cpp:40-43`,
  `:282-285`: imports and `Register...Backend()` calls.
- `Pipeline.Registration/RegistrationImpl.cpp:60-65`, `:143-147`: imports and cook
  registration.
- `Tools/Tools.Editor/Main.cpp:113-117`, `:356-359`: cook registration for the script page.
- `Samples/GUISandbox/Main.cpp:63`.
- The root `CMakeLists.txt`: `OPTION_ENABLE_ANGELSCRIPT`, `OPTION_ENABLE_LUAU`, the
  `SCRIPTLIB_*` aliases, and the fatal error when both are off (`:541`). On `dotnet`,
  `OPTION_ENABLE_DOTNET` and its imports beside these.

All of these are registration at a composition root, guarded by `#ifdef`. That is what a plugin
part's `OnLoad` replaces. No engine or editor module depends on a backend's types.

## What moves and what stays

**Stays in the engine:** `Foundation/Script` (the interfaces, about 1,400 lines),
`Script.Resource`, `Script.Facades`, `Engine.Behavior`, `Pipeline/Script.Pipeline`, and every
engine module's facade registration. Facades are reflected types, so a plugin that adds a facade
does not depend on any language.

**Moves to plugins:** the three VMs, their third-party libraries, their cooks and their test
suites.

A project with no language enabled has the script core and no VM. `CreateScriptManagerFor...`
already returns null with a warning when no backend matches; that path becomes the normal one
for such a project and should be silent there.

## What has to be decided or built

1. **The build no longer requires a backend.** The fatal error at `CMakeLists.txt:541` goes;
   the `OPTION_ENABLE_*` switches and `OPTION_HAS_*` defines are replaced by plugin enablement.
2. **A scriptless project must work end to end.** To check by running: a project whose
   behaviors are native components (NativeSample is the starting point), with no scene behavior,
   no run behavior and no scripted GUI, through editor, play in editor, cook and export.
3. **Existing projects must keep working.** A project's plugin list gains the language plugins.
   A project file with no entry for them is migrated by the languages its script assets use; this
   is a data version bump on the project file.
4. **A backend cannot unload during a run.** The plugin host reverses registrations on unload;
   a live context, script object or delegate would outlive its VM's code. The backend part
   refuses unload while its manager has live contexts, and the editor ends runs first.
5. **Export templates.** Official plugins are compiled into the templates and dormant unless
   enabled (plugins decision 1), so a template still carries every VM's code. Making a VM absent
   from a shipped player needs either templates per language set or the per-project build. Size
   of each VM in a player: not measured.
6. **C# gains the most.** It has files to deploy (the runtime, or nothing under NativeAOT), an
   SDK needed at cook time, and platforms it does not support. The manifest's `platforms` and
   `deploy` lists are made for exactly this. Today that knowledge is spread through
   `Script.DotNet.Pipeline`, `cmake/DotNet.cmake` and the root build.
7. **Tests.** Thirteen suites and integration tests link backends through `SCRIPTLIB_*`. They link
   the plugin parts statically through `util_link_plugins` instead. The conformance tests that run
   one script through every backend stay in `Code/Integration`.
8. **The script surface hash.** A cooked AngelScript product names the facade surface it was
   built against (plugins X7). A language plugin's own version joins the cook recipe, so
   updating a VM recooks its scripts.
9. **`provides` slots are not used.** Languages are not alternatives for one slot; several can
   be enabled. Using two in one run needs a context per language, which is in
   backlog-0.2 (`BehaviorRunHost::EnsureContext` refuses a second language today).

## Game logic without a VM

Making the VMs optional raises a second question: what a game's logic is when no VM is loaded.
Today the answer is "component managers only", which is less than a script gets.

### The three kinds today

A class the engine runs derives from a kind; the kind is recorded in its `BehaviorClass`
(`Script.Resource/ScriptResource.cppm:371`) with its language, class name, declared properties
(with defaults, which the inspector overrides per entity or per scene) and declared handlers.
Everything in that record is language-neutral. Only the code behind it (source, bytecode, an
assembly) is a VM's. Each place that takes a class takes only its kind.

| Kind | Declared by | Run by |
|---|---|---|
| `RunBehavior` | the project's `runBehaviorId` (`Engine.Project/ProjectModule.cppm:171`) | `PlayerApplication::LoadAndStartRunBehavior`, `ApplicationRun::StartRunBehavior`: `onStart`, `onUpdate(dt)`, `onStop`, `on<Event>` |
| `SceneBehavior` | `SceneBehaviorSettings.behaviorClass` on each scene (`Engine.Behavior/BehaviorSubsystem.cppm:1491`) | `SceneBehaviorSystem`, one object per scene |
| `EntityBehavior` | `BehaviorComponent.behaviors` on entities (`Engine.Behavior/BehaviorComponents.cppm:140`) | `EntityBehaviorSystem`: `onEnable`, `onStart`, `onUpdate`, `onDisable`, `onDestroy`, `on<Event>`, messages, entity events |

Every class is made through one call, `BehaviorRunHost::Instantiate(class, arguments, kind)`
(`BehaviorSubsystem.cppm:441`), which returns a VM's `ScriptObject`; its handlers are called on
that object by name. That call and that object are where the seam goes.

### What assumes a VM

- `DefaultApplication` imports the backends under `#ifdef` and holds the `BehaviorSubsystem`.
- `ApplicationRun` owns the run's `BehaviorRunHost` and its run behavior's object, and registers
  the run's services (`run.Get<T>()`: its run binding, save, input, HTTP and networking).
- `BehaviorRunHost::EnsureContext` creates the VM context the first time a kind needs one. With no
  backend for the language it logs an error and the class does not run.
- Facades find the run's services through the *script context* a VM pushes: `CurrentService<T>()`
  reads the context `ScriptCallScope` sets (`Script/IScriptContext.cppm:303`). A native call has
  no context to push.

A project with no scripts runs today: no run behavior, so the player loads the default scene,
and native components update through their managers (NativeSample). What it cannot have is a run
or scene behavior of its own: native code gets component managers (per entity, per scene system)
and application subsystems (per process), and no run lifecycle, no event inbox, no per-scene object
and no inspector-overridable properties on any of them.

### What is not about a VM

Most of `Engine.Behavior` (about 2,500 lines) is the run's gameplay machinery:

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
methods (`ScriptMethod`), calling them with arguments, budgets, coroutines and the debugger.
Where that line falls inside `BehaviorSubsystem.cppm` is not measured; it is the first thing to map.

### The proposal

1. **A gameplay layer, language-neutral.** The three kinds are the engine's concepts, with their
   lifecycles, handlers, properties and services. `Engine.Behavior` keeps that machinery and asks
   a *class implementation* to make objects and call them, in place of `Instantiate` returning a
   `ScriptObject`. The data stays: project, scene and entity still name a class, with its
   overrides.
2. **A language is an implementation of the layer.** AngelScript, Luau and C# implement it
   through their VMs, as they do today, from their plugins.
3. **Native classes are an implementation too.** A plugin's Engine part registers C++ classes
   deriving from `engine::RunBehavior`, `SceneBehavior` or `EntityBehavior` (the naming spec's
   decision): a reflected type with lifecycle methods, its reflected fields as the class's
   properties. The layer calls them directly, with no argument conversion, no context and no
   budget. This is the native backend worth having: native classes behind the gameplay layer, not
   a C++ VM behind `IScriptManager`.
4. **One API across languages.** The facades are C++ types that read the run's services. A
   native class calls the same facades inside the same call scope, so `RunFacade::loadScene`
   (`run.loadScene` in a script), `SaveFacade` and `scene.events.emit` read alike in C++ and in a
   script. For that, the service lookup moves from the script context to a neutral call scope the
   run owns, which a VM's call and a native call both push.
5. **No VM, nothing missing.** With no language enabled, the layer still runs native run, scene
   and entity behaviors. `DefaultApplication` and `ApplicationRun` hold the gameplay layer, not a
   VM; the contexts, budgets and cooks live in the language plugins.

Component managers stay what they are: the way to update many entities as data. A native
`EntityBehavior` is the per-entity object form, for the logic a script would hold.

### Open questions

- **How a native class is named.** A behavior, a scene and the project name a `BehaviorClass`
  asset, which a cook makes from a source file. A native class has no file. Either the reference
  becomes "an asset or a registered class name", or a plugin contributes one asset per class it
  registers. The first is simpler; the second keeps one reference kind.
- **Properties.** A script's properties are harvested at cook time (`BehaviorProperty`). A
  native class's come from reflection at registration. The inspector reads one list either way;
  the overrides format must not change.
- **The C++ lifecycle surface.** Decided (2026-11-19): reflected methods found by name, as a
  script's are, not virtual methods on the kind's base class. Nothing in C++ is discovered
  implicitly: a method exists to the engine only when the class's reflection registers it, and the
  registration gives it its script name apart from its C++ one. So the registration is the
  declaration, and no attribute marks a handler:

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
  cannot, since an event handler is declared by its name. A marker on the registration (as
  `.Pure()` marks a pure method) would be needed only for a method registered under an `on` name
  that is not a handler; helpers simply do not take `on` names.
- **Reload.** Scripts reload during a run. A native class reloads only if its plugin does, which
  needs plugin reload in the editor (the editor reloads a project's plugins today; whether a live
  run survives it is not checked).
- **Faults.** A script fault disables one behavior. A native crash is a crash. The layer cannot
  isolate it; the rule is documented, not built.

## Naming

Done: [run-and-behavior-naming.md](run-and-behavior-naming.md) is built (the kinds as base types,
`Engine.Behavior` and its types, `ApplicationRun`, the `Facade` suffix, the data migrations). What
it left to this spec is the service lookup's move to a neutral call scope (proposal 4) and how a
native class's handlers are found (the open question above).

## What this does not make optional

The script core stays linked into every engine module that offers a facade. Making the core
itself optional would mean moving every facade registration out of its engine module, which is
a larger change with little gain: the core is small and has no third-party code.

## Order of work

Each step stands on its own and leaves the tree green.

0. **Map `Engine.Behavior`.** Which parts are gameplay machinery and which are VM calls; write the
   seam (the class implementation interface, in place of `Instantiate` and `ScriptObject`) into
   this spec before any code moves.
1. **A scriptless project runs.** Remove the build's requirement for a backend and take a
   project with no scripts through editor, play in editor, cook and export, with both backends
   switched off by the existing options. This proves the engine side before anything moves.
2. **Luau as a plugin**, the pattern for the others (the smallest backend): its Engine and
   Pipeline parts, the project file migration, the unload refusal, its suites linked through
   `util_link_plugins`.
3. **AngelScript as a plugin.**
4. **C# as a plugin**, with its deploy files and platforms in the manifest and its export
   checks in a Tooling part.
5. **Remove** `OPTION_ENABLE_*`, `OPTION_HAS_*` and the `SCRIPTLIB_*` aliases.
6. **Export templates**: decide between templates per language set and the per-project build,
   after measuring what each VM adds to a player.
7. **The gameplay layer's seam.** `Engine.Behavior` calls classes through the class implementation
   interface, and the service lookup moves to the run's call scope; the three VMs implement it. No
   behavior change; the conformance suites prove it.
8. **Native classes.** A plugin registers native run, scene and entity behaviors; NativeSample
   gains one of each, and the conformance suites gain a native column beside the languages.

Steps 7 and 8 do not wait for 2 to 6: the seam can come first, and native classes then work in a
project that also uses a VM.

## Risks

- **Plugin order.** A language plugin must be loaded before the first context is asked for.
  The plugin host loads enabled plugins before a run starts; assert it.
- **A project opened without its language plugin.** Its script assets have no cook. The
  editor says which plugin the project needs, as it does for any missing plugin, and does not
  mark the scripts as broken.
- **Scope.** This changes what a project file holds and what an export contains, so it adds
  CHANGELOG lines and a section in `Documentation/Shipping/Scripting.md`.
