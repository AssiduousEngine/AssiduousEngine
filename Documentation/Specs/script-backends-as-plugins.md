# Script backends as plugins

> APPROVED 2026-11-19; refreshed and APPROVED again 2026-11-26. The refresh re-read the code on
> `systems-plugins` at `42ec3ebfc`, where C# is on the branch and particles, terrain and navigation
> are already plugins. It fits the spec to the `<Name>.Foundation` library
> ([systems-as-plugins.md](systems-as-plugins.md), G8), lists what the first reading missed and
> records the user's decisions of 2026-11-26. Decision 5 (templates per language set) waits for the
> measurements in the last step. Nothing here is built.
>
> Game logic that needs no language is [native-behaviors.md](native-behaviors.md) (built). The
> changes to how script calls are made are [scripting-typed-calls.md](scripting-typed-calls.md).
> Neither depends on this spec.

## Goal

Scripting is optional. Each language backend is a plugin. A project enables the languages it
uses, and a project that uses none has no VM.

A game with no language gets its logic from native run, scene and entity behaviors
([native-behaviors.md](native-behaviors.md)), behind the same gameplay layer the languages
implement.

## The plugins

Three official plugins: `angelscript`, `luau` and `csharp`. Each has the shape the other official
plugins have, with its VM in the Foundation library, because its cook compiles with the VM and a
Pipeline part links only its Foundation library (user ruling for G8):

| Library | What it holds | Today's library |
|---|---|---|
| `<Lang>.Foundation` | the VM, its third-party library, its `IScriptManager` | `Foundation/Script.AngelScript`, `Script.Luau`, `Script.DotNet` |
| `<Lang>.Engine` | registers the backend with `ScriptBackendRegistry` on load, removes it on unload | the `Register...Backend()` calls in `DefaultApplicationImpl.cpp` |
| `<Lang>.Pipeline` | its `IScriptLanguageCook` (check, cook, new-file template), registered on load | `Pipeline/Script.AngelScript.Pipeline`, `Script.Luau.Pipeline`, `Script.DotNet.Pipeline` |
| `<Lang>.Tooling` | C# only: the export's checks and the project player build (below) | parts of `Script.DotNet.Pipeline` and `ExportImpl.cpp` |
| `<Lang>.Editor` | the code editor's language definition, if it moves (decision 4) | `Data/Editor/Code/<lang>.xml` |

**Stays in the engine:**
- `Foundation/Script`, `Script.Resource` and `Script.Facades`.
- `Engine.Behavior`, `Pipeline/Script.Pipeline` and `Editor.Script` (the script page and its
  completion, which read a language's API surface and keywords through interfaces).
- Every module's facade registration. Facades are reflected types and name no language, as the
  dialogue, particles and navigation plugins' facades show.

## Every place outside a backend that names one (at `42ec3ebfc`)

**Registration at a composition root, under `#ifdef OPTION_HAS_*`.** A plugin part's load replaces
each of these:
- `Engine.DefaultApp`: `DefaultApplication.cppm:45-50` (imports) and `DefaultApplicationImpl.cpp:35-43`
  (imports) and `:276-284` (the three backends' registration).
- `Pipeline.Registration/RegistrationImpl.cpp`:
  - `:52-62`: imports.
  - `:130-137`: AngelScript and Luau register their backends as well as their cooks, since the cook
    compiles with the VM.
  - `:179-182`: the C# cook.
- `Tools.Editor/Main.cpp:101-107` and `:329-334`: the script page's cooks.
- `Tools.DotNetBindings/Main.cpp:13`: the C# bindings generator imports the C# backend directly.
  It becomes a tool of the `csharp` plugin.
- Two samples use the backends directly:
  - `GUISandbox/Main.cpp:62` and `:764` (AngelScript).
  - `ScriptPlayground/main.cpp` (AngelScript and Luau, chosen by name).

**The build:**
- The root `CMakeLists.txt`:
  - `OPTION_ENABLE_ANGELSCRIPT`, `OPTION_ENABLE_LUAU` and `OPTION_ENABLE_DOTNET` (`:187-197`).
  - The `OPTION_HAS_*` defines (`:556-557`).
  - The `SCRIPTLIB_*` aliases (`:565-589`).
  - The fatal error when both AngelScript and Luau are off (`:545-546`).
- 19 CMake folders link through `SCRIPTLIB_*`, most of them test suites.

**Missed by the first reading:**
- **A default language.** A script with no language recorded is AngelScript in four places:
  - `Script.Pipeline/ScriptAsset.cppm:1119` and `:1479`;
  - `Engine.Behavior/BehaviorSubsystem.cppm:436`;
  - `Editor.Project/ExportImpl.cpp:1168`.

  This is what old data means, not a dependency: it can stay, but it should be one named constant.
- **A hard-coded language list.** `Editor.Mcp/ScriptValidate.cppm:42-48` lists `angelscript` and
  `luau` for `script_validate` and `script_create`, and leaves out C#. It should come from the cooks
  that are registered (`ScriptLanguageCookRegistry`). Then a project sees the languages it enabled,
  C# included.
- **C# builds the game's own player.**
  - `ExportImpl.cpp:1230` (`ChoosePlayer`) decides that from the project's C# folder, before any
    plugin is asked.
  - The root build links the game's C# into that player through `dotnet_link_compiled_runtime`
    (`CMakeLists.txt:1038` and `:1065`, defined in `cmake/DotNet.cmake`), outside
    `util_link_plugins`.
  - Whether a template carries the VM does not arise for C#; the `csharp` plugin owns this path.
- **The code editor's languages** are data: `Data/Editor/Code/angelscript.xml`, `luau.xml`,
  `csharp.xml` (decision 4).

**Done since the first reading:**
- A run with only native behaviors never starts a VM (`ScriptBehaviorClasses`; NativeSample runs so).
- What is not done: the build still requires AngelScript or Luau.
- One language per run still holds. `ScriptBehaviorClasses::EnsureContext`
  (`BehaviorSubsystem.cppm:337`) warns and refuses a second language. Several languages in one
  run stay in backlog-0.2.

## What has to be built

1. **The build no longer requires a backend.**
   - The fatal error goes.
   - The `OPTION_ENABLE_*` switches, the `OPTION_HAS_*` defines and the `SCRIPTLIB_*` aliases give
     way to the plugins and `util_link_plugins`.
   - The suites link the language plugins as `Integration.Plugins` links dialogue.
   - The tests that run one script through every backend stay in `Code/Integration`.
2. **A project with no language works end to end.** Check it by running a project whose logic is
   native behaviors through editor, play in editor, cook and export.
3. **Existing projects keep working.** A project written before the move enables the languages its
   scripts use, and no other (decision 1). This is a project data version bump. Unlike particles,
   terrain and navigation, which `ProjectSettings::kFormerlyBuiltIn` enables from the settings
   alone, it needs the project's script assets. So it runs where the project is opened with its
   content (`EditorProject`, and Tools.Mcp's open), and the project is saved with the result.
4. **A run outlives its backend's unload.** The official languages are linked into every host, so
   their code stays mapped after their plugin unloads (the editor's plugin reload keeps libraries
   mapped too). Unloading takes the backend out of the registry: a manager made before keeps
   running its contexts, and only a new run finds no language. No refusal is needed. A language
   shipped in a project's own package, opened at run time, would need one, and waits for such a
   language.
5. **The C# player.**
   - The `csharp` plugin answers whether a project needs its own player, through a hook the export
     asks before it chooses (decision 2).
   - Its build owns `dotnet_link_compiled_runtime`.
   - Its manifest's `platforms` leaves out Android, so the export's existing check refuses C# there.
     That replaces the special case in `ChoosePlayer`.
6. **The cook recipe.** A language plugin's version joins its cook recipe, so updating a VM
   recooks its scripts. A cooked AngelScript product already names the facade surface it was built
   against.
7. **`provides` slots are not used.** Languages are not alternatives for one slot.

## Decisions to make

1. **Which languages a project written before enables: those it uses (user, 2026-11-26).**
   - Each script asset's language enables its plugin (the four places that read a missing language
     as AngelScript read it so here too), and a C# folder enables `csharp`.
   - A project with no scripts enables none.
   - Enabling a language for every project would carry a VM a game never runs, and enabling C# where
     it is unused would refuse every Android export, since the `csharp` manifest does not list
     Android.
   - The samples, by their scripts:
     - `angelscript`: Sky Hopper, PaperKid, Snowline, Lamplight, Shader Gallery, Masquerade and
       Millbrook;
     - `csharp`: Holdout;
     - none: NativeSample.

     No sample uses Luau. Sky Hopper's `Code/engine.d.luau` is the declarations file the editor
     generates for code tooling, not a script.
2. **How C# tells the export it needs its own player: a hook asked by `ChoosePlayer` (user,
   2026-11-26).** It sits beside the export hooks (`Editor.Project/ExportHooks.cppm`), whose hooks
   run after staging, too late for this choice.
3. **Names: as the particles, terrain and navigation renames did (user, 2026-11-26).**
   - The plugin id is the language id the backend registers: `angelscript`, `luau`, `csharp`.
   - Its folder is `Code/Plugins/<Name>` with `<Name>.Foundation`, `.Engine`, `.Pipeline` and the
     rest beside it (`AngelScript`, `Luau`, `CSharp`).
   - One module per library, named for the plugin and the library: `angelscript.foundation`,
     `angelscript.engine`, `angelscript.pipeline`, and so on. Today's modules
     (`foundation.script.angelscript`, `script.angelscript.pipeline` and the like) become partitions
     of them where they hold more than one unit.
   - One namespace per plugin, its id: `angelscript`, `luau` and `csharp` replace
     `foundation::script::angelscript`, `foundation::script` (Luau's) and
     `foundation::script::dotnet`. The pipeline cooks leave `pipeline` for the same namespace.
   - RTTI namespace strings stay as they are, since saved type identities name them.
   - The `foundation.script` core, `Script.Resource`, `Script.Facades` and `Script.Pipeline` keep
     their names: they stay in the engine.
4. **The code editor's language files stay in `Data/Editor/Code`, for now (user, 2026-11-26).**
   A project that did not enable a language still opens its files as text with highlighting.
   Moving them would need plugins to bring editor data, which nothing does yet; revisit when one
   does.
5. **Templates per language set.** Official plugins are compiled into the export templates and
   dormant unless enabled (plugins decision 1), so a template carries every VM. Recommended: measure
   what each VM adds to a player first, and decide after the moves (the last step below).

## Order of work

Each step leaves the tree green on clang and clang-shared.

1. **A project with no language runs.** Remove the build's requirement for a backend. Take a
   native-behavior project through editor, play in editor, cook and export with both backend
   options off.
2. **Luau as a plugin**, the pattern for the others (the smallest backend). It includes:
   - its Foundation library and its Engine and Pipeline parts, named as decision 3 says;
   - the project migration, by the languages a project's scripts use;
   - a test that a running script outlives the plugin's unload;
   - the language list from the registered cooks;
   - its suites linked through `util_link_plugins`.
3. **AngelScript as a plugin.**
4. **C# as a plugin**, with:
   - its platforms in the manifest;
   - the player hook and its build's link;
   - the bindings tool as its own;
   - its export checks in a Tooling part.
5. **Remove** `OPTION_ENABLE_*`, `OPTION_HAS_*` and the `SCRIPTLIB_*` aliases.
6. **Export templates**: measure each VM in a player, then decide decision 5.

## Risks

- **Plugin order.** A language plugin must be loaded before the first context is asked for. The
  plugin host loads enabled plugins before a run starts; assert it.
- **A project opened without its language plugin.** Its script assets have no cook. The editor
  says which plugin the project needs, as it does for any missing plugin, and does not mark the
  scripts as broken.
- **The editor and Tools.Mcp compile scripts.** A stale tool refuses a script that uses new API,
  as today; with the backends as plugins, the tool must also have loaded the project's language
  plugins before it checks a script.
- **Scope.** This changes what a project file holds and what an export contains, so it adds
  CHANGELOG lines and a section in `Documentation/Shipping/Scripting.md`.
