# Export

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[settings-and-export-track]]

Turning an authored project into a shippable per-platform dist, driven uniformly from the CLI, the
editor and the MCP tools (the same way cook is uniform). Shipped end to end: the template/preset
architecture, template creation + the config axis, reachability pruning, the uniform driver, the
Android app build, the plugins' export checks and hooks, and both surfaces' UIs. Lives in
`editor.project` (`:export_pipeline` + `:export_preset` + `:export_template` + `:export_roots` +
`:export_hooks` partitions); the CLI is `Tools.Export`.

## Templates vs presets (the core split)

Separate the portable prebuilt bundle from the project-local config:

- **`ExportTemplate`** (`ExportTemplate.cppm`) - a per-platform, per-CONFIG prebuilt player + its
  runtime sidecars + a `template.xml` manifest, living in a templates ROOT outside any project (shared
  across projects, referenced by id, never by absolute path). Fields: `{ id, name, platform, config,
  compiler, engineVersion, playerBinary, sidecars[], symbols[], notes, icon, plugins[] }`. A
  `TemplateRegistry` (Refresh / FindById / `FindBy(platform, config)` / Resolve) indexes them, and a
  HOST implicit template is synthesized from the running tool's own `Bin/<Config>/<Platform>-<Compiler>/`
  dir (id `host-<platform>-<config>`) so a dev export works with zero setup.
- **`ExportPreset`** (`ExportPreset.cppm`; project-local, committed to `<project>/export_presets.xml`,
  `ExportPresetSet` data v3): references a template by id + game choices `{ name, platform, config,
  templateId, playerName, outputSubdir, additionalFiles[], stageSymbols, pruneToReachable }`, plus
  appended fields: its own render size, window and orientation for its platform (each behind an
  `overrides*` flag), `includeShaderSource`, `qualityLevelId` (the project quality level this
  platform starts at; nil is the project's default), and the Android app's fields (below). No machine
  paths, so it is safe to commit. Its reflection (`RegisterExportPresetReflection`) labels every
  field, and the Android ones carry a `platform` attribute so an editor shows them only for an
  Android preset.

The platforms a game exports to are one list, `kExportPlatforms`: `Linux64`, `Win64`, `Web` and
`Android64`, each with the built-in icon its templates get and how its build names the player (an
executable, a page, or `libEngine.Player.so` on Android; `ExportPlayerFile`).

## Template creation + the config axis

A template is `platform x config`: Debug / Release / RelWithDebInfo are SEPARATE templates (the host
template carries the config/compiler that built the running tool). `CreateTemplate(configDir, destRoot,
mode, ..., identity)` synthesizes + materializes a template bundle from a `Bin/<Config>/<Platform>-<Compiler>`
build dir (reusing `SynthesizeHostTemplate` to read the sidecars; the platform comes from the
directory, so a host packages another platform's build), either installing it into the templates root
or writing it to a chosen folder. CLI:
`Tools.Export --template create <configDir> [--install | --out <folder>] [--id] [--name] [--notes] [--icon]`,
plus `--template list|import`. Config-driven sidecars: `util_copy_runtime_deps` emits
`<player>.runtime-libs` at build time and `util_link_plugins` `<player>.plugins`; a template reads
both manifests (authoritative, replacing a hardcoded known-sidecars list). Each release publishes
Linux, Windows, Web, Steam Deck and Android templates (`.github/workflows/release.yml`); detail in
`export-templates.md`.

## Reachability pruning

Opt-in per preset (`ExportPreset::pruneToReachable`; default off = pack everything). When on, a
dist ships only the CLOSURE of its entry points instead of the whole cooked dir. Detail:
`Documentation/Systems/export-reachability.md`.

## Uniform driver + surfaces

`ExportContent` (cook-less pack/stage) is factored out of `ExportProject` (cook + content); `ExportOne`
/ `ExportAll` take a `cook` flag, an `ExportProgress` sink, the project's plugins (`ExportPlugins`)
and `contentOnly`, and return `ExportResult { content, filesStaged, outputDir, engineVersionWarning,
pruning, playerReason, refusal }`. A resolved template whose `engineVersion` differs warns and still
exports (soft-match). An export whose cook has a failure stops there. Scene/prefab source streams
pre-transcode to binary before packing (`CollectSceneStreams`, through a stager the caller supplies:
the editor's scene services, or `engine::CollectSceneStreams` in the CLI); file ops use native
`core::FileCopyPreserving` / `CreateDirectories`. Every export also cooks the engine shaders for the
target platform from the caller's data root (`ExportOne` / `ExportAll` take `dataRoot`; the editor,
`Tools.Export` and the MCP `project_export` tool resolve it the one way - `Documentation/Systems/data-root.md`),
together with the project's custom shaders (as bytecode; their HLSL ships only when the preset sets
`includeShaderSource`), and stages `Data/.dataroot` + `Data/Engine/Shaders/shaders.dpak` beside the
player (`StageShaderPack` - SPIR-V on Linux and Android, SPIR-V and DXIL on Windows, WGSL for Web):
the data-root layout the player's discovery walk finds, so a dist renders with no external dependency
(engine shaders are cooked `.hlsl`, not baked into the binary, and no template stages the DXC runtime).
`ApplyPresetOverrides` writes the preset's render size, window, orientation and quality level over
the dist's `player.xml`; a preset whose level is not one of the project's is refused
(`PresetQualityRefusal`).

- **Players and plugins**: an export uses the template's player when it carries every plugin the
  project enables on the preset's platform and the project has no native packages of its own there;
  otherwise it builds the project's player from the engine checkout (`PlayerChoice`, reported as
  `playerReason`). Before anything cooks, the project's scripts are checked against what the preset's
  platform has of the plugins; a script using one absent there refuses the export. A plugin's Tooling
  part adds hooks through `ExportPluginServices` (`:export_hooks`); they run once the dist is staged,
  and one that fails fails the export. Detail: `Documentation/Specs/plugins.md`.
- **CLI** (`Tools.Export`): `<project> [--out <dir>] [--preset <name> | --all] [--rebuild] [--data-root <dir>]`,
  `--template list|import|create`; host-preset fallback when no `export_presets.xml`. `--platform <name>`
  exports the chosen preset for another platform, and `--content-only` (with `--platform`) stages what
  the game reads with no player and no template (`ExportOne`'s `contentOnly`: the content pak,
  `player.xml` and the shader pack), for a build that packs the player itself.
- **Texture variants**: a preset's content is cooked per its platform's target (`VariantsForPlatform`):
  one host pak on desktop, a BC and an ASTC pak for Web (`Content-bc.pak`, `Content-astc.pak`, cooked
  into `Cooked-web-bc/` and `Cooked-web-astc/`), and for `Android64` one `Content.pak` cooked for the
  `android` target (ASTC) into `Cooked-android/` beside the host's `Cooked/` (`CookVariantTargets`).
- **Android**: an `Android64` preset builds an app. `CheckAndroidExport` checks, before anything
  cooks, the application id, the icon (a square PNG of 512 pixels or more), the keystore (with its
  alias and its password in `ANDROID_KEYSTORE_PASSWORD`), the Android SDK (`ANDROID_HOME`) and that
  the template carries the app's project. The content is exported into the app's work folder
  (`<project>/.cache/android/<preset>`, `AndroidWorkDir`); `StageAndroidApp` copies the template's
  Gradle project there and stages the player as `jniLibs/arm64-v8a/libmain.so`, the game (`Content.pak`,
  `player.xml`) in `assets/Game`, the shader pack and the engine's other data (its fonts) in
  `assets/Data`, the template's notices and the launcher icon; `BuildAndroidApp` runs the Gradle
  wrapper with the preset's values as properties and copies `<name>.apk` and/or `<name>.aab` into the
  dist (`androidPackage`: Apk, Bundle or Both; Debug for a Debug template, Release otherwise). The
  preset's app fields: `androidApplicationId`, `androidVersionCode`, `androidVersionName`,
  `androidLabel`, `androidIcon`, `androidPackage`, `androidKeystore`, `androidKeyAlias` (empty
  keystore: the developer's debug key). Passwords are never in a preset. The player in the app reads
  its game and packs in place from the APK: `assets/Game` and `assets/Data` through a zip mount, and
  `Content.pak` (stored uncompressed) through a `PakFileSystem` over its region of the APK; user data
  and temporary files go in the app's own storage (`AndroidMain.cpp`). A project with its own native
  packages does not export to Android yet.
- **Editor**: `Project > Export...` (`ExportDialog`) lists the project's presets beside the selected
  one's settings in sections (General, Content, Display, and Android for an Android preset) under a
  card naming the template it resolves to here; edits save as they go. An export runs NON-BLOCKING (cook
  via `EditorCookService`, then pack/stage on the editor task pool's build lane), with status-bar
  progress and a sticky success toast + Open Folder (native reveal). `Edit > Export Templates...`
  (`TemplatesDialog`; the templates are the editor's, shared by every project) lists the registry's
  templates by the icon each carries, with their details, Install from Folder, Create from Build,
  Reveal and Remove. The editor mutation lock covers background jobs (closes the export-vs-UI DB race).
- **MCP**: `project_export` runs an export; `export_presets` / `export_preset_set` read and edit the
  presets from the same reflection the dialog uses.

Templates root resolution: `ResolveTemplatesRoot(override -> $ENV_TEMPLATES_DIR ->
<user-data>/templates)`, the override being `EditorExportSettings.templatesRoot` (edited in
`Edit > Preferences...`). The same per-user section holds the Android SDK and JDK folders an export
runs (`androidSdk`, `javaHome`; empty for `ANDROID_HOME` / `JAVA_HOME` or `java` on the PATH), which
`ApplyExportToolEnvironment` sets for the exports and their Gradle builds.

## Companion docs

- Template config axis + creation detail: `Documentation/Systems/export-templates.md`.
- Reachability closure + Always-Export roots: `Documentation/Systems/export-reachability.md`.

Non-goal: cross-compiling players at export time - we stage PREBUILT player binaries (a project with
its own native packages builds its player from the engine checkout instead).

---

The original problem framing + the historical design sections are in
`Documentation/Archive/export-design-history.md`.
