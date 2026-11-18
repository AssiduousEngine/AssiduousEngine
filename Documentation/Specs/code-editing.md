# Code editing: a project's Code folder, outside code editors, log references

> STATUS: APPROVED 2026-11-17 (the user answered its questions). Sized L: E0 to E6 below, each green on its own. Asked for by the user
> before Holdout ships: the built-in code page is not where a real project's code gets written, so
> the engine works with an outside editor (VS Code first, any editor by a command), keeps code apart
> from imported art, and lets a log line take the user to what it is about. The built-in page keeps
> working and stays the default. Every statement under "What exists" was read in code on
> 2026-11-17 (branch dotnet, rebased on master); nothing here was run. Language-neutral, so the port
> can follow it one for one. Read CONVENTIONS.md first.

## Goal

- A project's code lives in `Code/` at its top level, apart from `Sources/` (imported art); a code
  editor opened on `Code/` sees code and nothing else.
- `Sources/` follows the asset groups (`Sources/Models/Guns/Pistol.gltf`), not one flat folder.
- A script file made in an outside editor becomes a script asset by itself.
- The user picks a code editor once (built-in, VS Code, or a command of their own); opening a script,
  an error or a stack frame goes there, at its line.
- Each language's outside tooling works from the files the engine writes: C# (the project and its
  props, which exist), Luau (`engine.d.luau`, `.luaurc`), AngelScript (`as.predefined`).
- A log line carries references (a file and line, an asset, an entity); a click on one goes to it,
  through handlers registered by kind, so the console knows no kind itself.

## What exists

- **Imports land flat.** `SourcesNameFor` (Pipeline.Importer/Importer.cppm:917) names the copy by
  the file's own name in `context.sourcesRoot` ("a.2.png" on a clash); `asset_import`
  (Editor.Mcp/ProjectTools.cppm:367) and the editor's import (Editor.App/AssetImport.cppm:369) use
  the group only for the content database. New scripts are `Sources/<name>.<ext>`
  (Editor.Mcp/ScriptCreate.cppm:74). A source asset's file is `pipeline::Asset::fileName`, a
  `vfs::SourcePath` relative to `Sources/` (Pipeline.Core/Asset.cppm:56).
- **Scripts.** `.as`, `.luau`, `.cs` are each a `ScriptClassAsset` (Script.Pipeline/ScriptAsset.cppm:44);
  a `.csproj` is a `ScriptAssemblyAsset` (:72) whose project builds every `.cs` under its folder
  (`ListSourcesUnder`, :1194 and :1221); the C# cook copies them and runs `dotnet build`.
- **Watching.** `NativeChangeSource` (VFS/NativeFileSystem.cppm) sweeps `Sources/` by stat every 2 s
  (CookService.cppm:176) and reports added, changed and removed files; any change asks for a cook
  (CookServiceImpl.cpp:329). A new file gets no asset.
- **Console.** `LogView` (Editor.App/LogView.cppm:38) over `EditorLogBuffer`
  (Editor.Project/LogBuffer.cppm:37); an entry holds level, category, message, sequence and time.
  A click selects; nothing follows a line. Core's `RingLogSink` keeps fixed 64/192-character
  records (Core/Log/RingLogSink.cppm:24).
- **Preferences.** Sections of `editor.settings.xml` (Editor.Core/EditorSettings.cppm), each type
  registered in `RegisterEditorSettingsTypes()` (:532); the dialog is Editor.App/PreferencesDialog.cppm.
- **Opening a script** goes to `ScriptEditorPage` (Editor.Script/ScriptPage.cppm:41), a
  `CodeAssetPage` reading `SourcesRoot/fileName`. A `.csproj` has no page.
- **Declarations.** The MCP tool `script_declarations` (Mcp.Script/ScriptTools.cppm:174) writes each
  backend's file (`engine.d.luau`, `as.predefined`) into a folder it is given.
- **Processes.** `core::OpenPathInFileManager` opens a folder (detached `xdg-open`, Explorer);
  `core::RunProcess` blocks and captures output. Nothing starts a program with arguments and leaves
  it running.

## Design

### The Code folder (E0, E1)

- A project has two source folders, `Sources/` (imported files) and `Code/` (code). A source
  asset's `fileName` stays relative to `Sources/`, and one top-level name is reserved: a path
  starting `Code/` names the project's `Code/` folder (`Code/Weapons/Weapon.cs`); any other path is
  under `Sources/` (`Models/Guns/Pistol.gltf`). The cook, export and the tools read one file system
  joining both folders that way, so a path keeps one form and the import ledger its keys (the
  user's choice, 2026-11-17, over every path naming its folder: little churn, only scripts move).
  A folder already at `Sources/Code/` would be hidden by it; the migration moves it aside first.
- Which root a file goes to is asked of the importer, not listed: an importer says whether its
  files are code (the script importers and the project-file importer do). New scripts
  (`script_create`, the editor's New Script) go to `Code/`.
- Under either root a file sits in its asset group's folder (`Code/<group path>/`,
  `Sources/<group path>/`), so the folders read as the Assets browser does. Moving an asset to
  another group moves its file and keeps its id (a drag in the Assets browser moves the file on
  disk); a clash takes the next free name as today.
- Migration: a project layout version in the manifest (as the quality-levels upgrade is done, the
  manifest backed up first). Opening an older project moves each script and project file from
  `Sources/` to `Code/` (prefixing its `fileName` with `Code/`) and, with E1, every other file into
  its group's folder, rewriting each asset's `fileName` and keeping every id. The cook database
  keys on the paths, so the moved files cook once more. The sample projects migrate in the same
  commit, through the tools.
- `.gitignore` of a new project: unchanged (both roots are the project's own); a C# project's
  `obj/` and `bin/` under `Code/` are ignored as they are under `Sources/` today.

### New files become assets (E2)

- The watcher sweeps `Code/` as it sweeps `Sources/`. A file it finds added under `Code/` whose
  extension a registered script backend or project build claims is imported where it lies (no
  copy), into the group its folder names (made if missing). A removed file's asset is marked
  missing, as a missing source is today; it is not deleted unasked.
- A C# class needs its project: a `.cs` with no `.csproj` in its folder or above is imported and
  reports that, as its cook does now.

### The code editor (E3)

- A preference section, Code Editor: `Built-in` (the default), `VS Code`, or `Command`: a program
  and its arguments with `{file}`, `{line}`, `{column}` and `{folder}` (the project's `Code/`), and
  a second command that opens the folder (empty: the file manager). VS Code is a preset of Command
  (`code {folder} --goto {file}:{line}:{column}` and `code {folder}`); a second editor is another
  preset, not new code.
- Core gains `LaunchDetached(program, args)`: starts it, does not wait, returns whether it started
  (the double fork `OpenPathInFileManager` does on Linux, CreateProcess with no wait on Windows).
- Opening a script (Assets browser, `page_open` excepted: an agent's tool keeps its page), a
  `source` reference (E5) and the menu's Open Code Folder go through one call, `OpenCode(file,
  line, column)`, which launches the preference or opens the built-in page at the line.
- A script asset's context menu has Open in Built-in Editor beside it, whichever is preferred.
- The built-in page opens `Code/`'s other files (a `.csproj`, `.luaurc`, `as.predefined`) as plain
  text.

### Each language's outside tooling (E4)

- The editor keeps a project's tooling files in `Code/`, written on project open (when the project
  has a `Code/` folder), when the code editor preference is saved, and before an outside editor
  starts. The declarations come from the API the script checks use (the engine and the plugins
  loaded) and are written whenever they differ:
  - C#: the `.csproj` and `obj/Engine.props` (exist; they move with the project file).
  - Luau: `engine.d.luau`. luau-lsp finds a definitions file only through its settings
    (`luau-lsp.types.definitionFiles`) or its command line, not through `.luaurc`, so no `.luaurc`
    is written.
  - AngelScript: `as.predefined`.
  - VS Code: `.vscode/extensions.json` recommending each language's extension (a script backend
    names it: `JohnnyMorganz.luau-lsp`, `sashi0034.angel-lsp`), and `.vscode/settings.json` with the
    settings each backend names (luau-lsp's definition file, its standard platform, no sourcemap).
    Written only while VS Code is the preference, and never over a file the user changed or that
    was there first (a ledger of the hashes written is kept in the project's `.cache/`).
- `script_declarations` keeps working and writes the same files.

### Log references (E5, E6)

- **A reference** is a kind and a value: `source` {file, line, column}, `asset` {guid}, `entity`
  {scene, path}, `node` {document, id}. Kinds are open: a plugin or a language adds its own.
- **Attaching.** A log call may carry references (`LOG_WARNING(..).Refer(AssetRef(id))` or an
  equivalent the logging API settles on). They travel to the sinks beside the text; the fixed-size
  `RingLogSink` drops them; `EditorLogBuffer` keeps them on each entry.
- **Recognizers** turn text into references for lines logged without them (most of today's, a
  compiler's output, a stack trace): registered by kind, each a matcher over the message returning
  spans and their references. Built in: `file(line,col)` and `file:line` paths under the project
  (`source`), `{guid}` of a known asset (`asset`).
- **Handlers** are registered in the editor by kind: `source` calls `OpenCode`; `asset` selects it
  in the Assets browser (opening its page on a double click); `entity` selects it in its open scene
  page. A reference whose kind has no handler shows as text.
- **The console** shows a reference's span underlined (always, not on hover: the GUI's rich text
  has no hover underline); a click on it follows it; a double click on the line follows its first
  reference.
- **MCP:** `log_read` returns each entry's references, so an agent gets the file and line too.
- **E6, the languages:** a C# exception's stack frames (the symbols exist) and `dotnet build`'s
  errors become `source` references; Luau's and AngelScript's error reports carry theirs.

## Phases

| Phase | What | Checked by |
|---|---|---|
| E0 | (built 2026-11-17) `Code/` reserved in source paths, one file system over both folders; importers say code or not; new scripts to `Code/`; layout-version migration | Pipeline tests: an old project's assets move and keep ids; Integration.Mcp: each sample reads and cooks after migrating |
| E1 | Files in their group's folder; a group move moves files (built 2026-11-17: a file several groups share stays; a project file another includes by path is not followed) | Content tests: import into a group, move a group |
| E2 | (built 2026-11-18) `Code/` watched; new code files imported in place | Editor.Project tests: a file written to `Code/` becomes an asset, a removed one goes missing |
| E3 | (built 2026-11-18) Code Editor preference, `LaunchDetached`, `OpenCode`, menu and context items | Core test for the launcher (a script that records its arguments); editor test for the placeholder expansion |
| E4 | (built 2026-11-18) Tooling files kept in `Code/` | Editor tests: written on open, rewritten after an API change, a user-changed file left alone |
| E5 | (built 2026-11-18; the entity handler waits for the first line that names an entity) References, recognizers, handlers, console links, `log_read` | Core and editor tests per piece; the user clicks through |
| E6 | (built 2026-11-18; C# on the dotnet branch) C# stack frames and build errors, Luau and AngelScript errors as references | Script suites per language |

The C# speed work (measure first, backlog-0.2) runs as its own spec after E3, beside E4 to E6 if
the user wants it sooner.

## Decisions (the user, 2026-11-17)

1. A group move moves its file (E1).
2. In the console a click on a reference's underlined span follows it; a double click anywhere on
   the line follows its first reference.
3. The built-in page opens `Code/`'s other files as plain text (E3).
4. Debugging in the outside editor (a `launch.json` attaching to the editor's run) is left for later.
5. Source paths stay relative to `Sources/` with `Code/` reserved, not relative to the project (E0).
