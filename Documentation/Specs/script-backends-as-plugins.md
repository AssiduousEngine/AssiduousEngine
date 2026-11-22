# Script backends as plugins

> APPROVED 2026-11-19 (proposed 2026-11-18; refreshed after
> [run-and-behavior-naming.md](run-and-behavior-naming.md) and [entity-events.md](entity-events.md)
> were built). Split on 2026-11-21 with no decision changed: the game logic that needs no language
> (the gameplay layer and native run, scene and entity behaviors) is now
> [native-behaviors.md](native-behaviors.md). Nothing here is built. Read on master at `0618e177f`
> (AngelScript, Luau, the engine layer, the plugin system) and on `dotnet` at `80a3fba85` (C#).
>
> The changes to how script calls are made are a separate proposal:
> [scripting-typed-calls.md](scripting-typed-calls.md). Neither depends on the other.

## Goal

Scripting plays an optional role. A language backend is a plugin: a project enables the
languages it uses, and a project that uses none carries no VM.

What a game with no language has for its logic is [native-behaviors.md](native-behaviors.md):
native run, scene and entity behaviors behind the same gameplay layer the languages implement.
This spec does not need it to be built, and it does not need this one. Until it is, a project with
no language has what NativeSample has today: native components updated by their managers, and no
run or scene behavior of its own.

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

The dialogue plugin is the first official plugin with all four parts, and the first plugin to
register a script facade (its domain's `registerScriptFacade`, removed in its Engine part's
unload). A facade is a reflected type, so it named no language there, as a language plugin's
facades will not need to.

**Every place non-test code names a backend today** (master):

- `Engine.DefaultApp/DefaultApplication.cppm:46-49` and `DefaultApplicationImpl.cpp:40-43`,
  `:282-285`: imports and `Register...Backend()` calls.
- `Pipeline.Registration/RegistrationImpl.cpp:61-66`, `:145-150`: imports and cook
  registration.
- `Tools/Tools.Editor/Main.cpp:113-117`, `:346-349`: cook registration for the script page.
- `Samples/GUISandbox/Main.cpp:63`.
- The root `CMakeLists.txt`: `OPTION_ENABLE_ANGELSCRIPT`, `OPTION_ENABLE_LUAU` (`:183-184`), the
  `SCRIPTLIB_*` aliases, and the fatal error when both are off (`:544-545`). On `dotnet`,
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

1. **The build no longer requires a backend.** The fatal error at `CMakeLists.txt:544` goes;
   the `OPTION_ENABLE_*` switches and `OPTION_HAS_*` defines are replaced by plugin enablement.
2. **A project with no language must work end to end.** To check by running: a project whose
   behaviors are native components (NativeSample is the starting point), with no scene behavior,
   no run behavior and no scripted GUI, through editor, play in editor, cook and export. Once
   [native-behaviors.md](native-behaviors.md) has native run and scene behaviors, the same check
   takes one of each.
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
   the plugin parts statically through `util_link_plugins` instead, as `Integration.Plugins` links
   the dialogue plugin. The conformance tests that run one script through every backend stay in
   `Code/Integration`.
8. **The script surface hash.** A cooked AngelScript product names the facade surface it was
   built against (plugins X7). A language plugin's own version joins the cook recipe, so
   updating a VM recooks its scripts.
9. **`provides` slots are not used.** Languages are not alternatives for one slot; several can
   be enabled. Using two in one run needs a context per language, which is in
   backlog-0.2 (`BehaviorRunHost::EnsureContext` refuses a second language today).

## What this does not make optional

The script core stays linked into every engine module that offers a facade. Making the core
itself optional would mean moving every facade registration out of its engine module, which is
a larger change with little gain: the core is small and has no third-party code.

## Order of work

Each step stands on its own and leaves the tree green.

1. **A project with no language runs.** Remove the build's requirement for a backend and take a
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

## Risks

- **Plugin order.** A language plugin must be loaded before the first context is asked for.
  The plugin host loads enabled plugins before a run starts; assert it.
- **A project opened without its language plugin.** Its script assets have no cook. The
  editor says which plugin the project needs, as it does for any missing plugin, and does not
  mark the scripts as broken.
- **Scope.** This changes what a project file holds and what an export contains, so it adds
  CHANGELOG lines and a section in `Documentation/Shipping/Scripting.md`.
