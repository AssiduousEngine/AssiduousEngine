# Projects, settings, and the built-in project manager

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[project-manager-track]] / [[settings-and-export-track]]

The applied settings model + the Godot-style single-exe project manager. The settings PRIMITIVE
(`foundation.settings::Settings`) is `Documentation/Systems/settings.md`; this is how it is layered
into concrete files and how projects open/close.

## The three files

| File | Path | Owner module | Holds |
|---|---|---|---|
| Shared project manifest | `<project>/Project.xml` | `engine.project` | name, engineVersion stamp, default scene/script/input-map/bus-layout refs, the UI fonts, the GUI's sheets and loading screen, the display (render size and fit, window, orientation), the enabled plugins with their platforms and settings, and the quality levels (`qualityLevelIds`, `defaultQualityId`). The SAME payload ships as `player.xml` in a dist (minus editor concerns; an export preset may write its own display and quality level over it). Versioned (`ProjectSettings`, data v12; versions 9 to 11 still read, carrying the scene-pass MSAA count the quality levels now hold); every save re-stamps `engineVersion` to the running engine. |
| Per-user editor settings | `<user-data>/editor.settings.xml` (`<user-data>` is `$XDG_DATA_HOME/assiduousengine`, else `~/.local/share/assiduousengine`, on Linux, under `%LOCALAPPDATA%` on Windows) | `editor.core` (sections may live with their domain) | machine-wide state: `EditorExportSettings` (templates root, the Android SDK and JDK an export runs), `RecentProjectsSettings` (the project-manager registry), `EditorUiSettings` (the look), `EditorShortcutSettings`, `EditorMcpSettings`, ... |
| Per-project editor state | `<project>/Editor/editor.project.settings.xml` | `editor.app` (+ contributor modules) | `EditorDockLayoutSettings`, `EditorFavoritesSettings`, `EditorOpenPagesSettings`, `EditorAssetBrowserSettings`, `MaterialPreviewSettings` (from the material page), ... |

The two editor files are `foundation.settings::Settings` stores (typed `ISerializable` sections,
versioned envelopes) over `XmlSerializerFactory`; the manifest is one versioned `ProjectSettings`
payload through the same serializer stack (`LoadProjectSettings` / its save). The engine/player side
reads ONLY the manifest (`engine.project` is editor-free); the two settings stores are editor
concerns. A game keeps its own per-user store (`<user-data>/<project>.user.settings.xml`: volumes and
the chosen quality level; settings.md). Layering is by SEPARATE files (settings.md), not an in-store
layer stack.

Rules:
- Sections must be REGISTERED (type registry + serializable registry) before a store loads, or they
  read as unknown and their owners see defaults. Per-module sections register in that module's
  `RegisterXxxEditor` entry point, which runs before the app loads the per-project store.
- The registry lives OUTSIDE any engine install or project on purpose: every engine version on a machine
  lists the same projects, and a future separate launcher/hub reads the same file. One of the four
  hub-extraction seams (below).

## The project manager (single-exe, Godot-style)

- `Tools.Editor` with NO project argument starts on the manager screen (`ProjectManagerView`, over
  `Data/Editor/ProjectManager.xml`): a Projects tab (New Project..., Open Folder..., a search, the
  sort, list or grid; the recent projects live-probed, pinned ones first, newer-engine stamps
  flagged, missing ones listed apart with Locate... and Remove from list) and a Samples tab that
  downloads a sample project.
- `Tools.Editor <dir>` or `--project <dir>` opens directly: the single-project lifecycle (Close
  Project does not return to a manager; the CLI path keeps the scaffold-on-open fallback).
- Decisions live in `ProjectManagerController` (`editor.project`, headless, tested): `DecideOpen`
  probes the manifest and gates it (`ProjectOpenGate`: NotAProject / OpenDirectly / PromptOlderBackup /
  PromptNewerEngine) and composes the prompt copy; New Project's check line and the registry's edits
  (pin, locate, remove, sort) are there too. The App layer only renders dialogs.
- Version prompts: older/unstamped projects offer Back Up and Open (`BackupProjectManifest` copies
  `Project.xml` to `Project.xml.<oldver>.bak`, "unstamped" when it has none; the project opens and is
  re-stamped); newer-engine projects get a hard warning. Content assets migrate through their own
  versioned serializers as they re-save - whole-project safety is version control's job (the prompt
  says so).
- File > Close Project (when the manager launched the editor) asks about unsaved pages, then closes
  the project (`EditorSession`: the views and pages first, then the cook, the thumbnails, the
  resources detached from the embedded runtime with `DefaultApplication::AttachResourceManager(nullptr, ...)`,
  the plugins and the store, the project last) and returns to the manager. Teardown is queued, never
  run mid-event-dispatch.
- Every successful open (manager or CLI) touches the registry, so CLI-opened projects appear in the
  manager next launch.

## Future-hub extraction seams (kept deliberately)

1. Engine-version stamp in `Project.xml`, rewritten on every save.
2. The registry in the per-user store, independent of engine installs.
3. `--project <dir>` direct-open CLI - a hub is just "pick a version, exec with the flag".
4. Versioned manifest + explicit backup-then-migrate at open.

## Editor app lifecycle

App-lifetime (once per run): the data root, user settings, the editor window (`EditorWindow`: the GUI
host, fonts, the editor's sheet and shell frame), the embedded runtime (starts WITHOUT resources),
registerEditors, actions and menus. Per-project (`EditorSession`): manifest + content DBs
(`EditorProject`), the per-project settings store, favorites, the ResourceManager (late-attached to the
embedded runtime, which registers its standard factories into it), the project's plugins (those this
editor links and its own packages, reloadable), the cook service, thumbnails, the game UI's defaults,
saved pages + dock layout, and the registry touch. The manager is a screen the editor window shows over
its root in place of the shell frame (`EditorWindow::ShowScreen`).
