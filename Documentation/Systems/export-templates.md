# Export Templates

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[settings-and-export-track]]

An export template is a portable, prebuilt RUNTIME bundle for one `(platform, config)`: a player
executable, that build's runtime sidecars, optional symbols, and a `template.xml` manifest. A project's
preset references a template and adds the game half (cooked content + extras + naming). Template
(runtime) + preset (game) -> dist. Shipped. Overview: `Documentation/Systems/export.md`.

The line held: a template is the RUNTIME you ship against - never game content, never editor code (the
player links zero editor), never a toolchain (the bundle is prebuilt, so using it needs no compiler -
the whole point vs an SDK-toolset model).

## The config axis

A template is `(platform, config)`: Debug / Release / RelWithDebInfo are SEPARATE templates, not
profiles inside one. Rationale: it is what the build already produces (each `Bin/<Config>/<Platform>-
<Compiler>` dir is a self-contained runtime with its own `<player>.runtime-libs`); sidecars are
config-specific (debug vs release link different native runtimes); and size (tripling a player+deps for
three configs is wasted disk when most projects ship Release only). Product stance: config is
first-class so "release template" and "debug template" are both expressible, but the product defaults to
and optimizes for RELEASE - Debug/RelWithDebInfo are opt-in dev/power-user variants.

Each release publishes five templates beside the editor downloads (`.github/workflows/release.yml`):
Linux, Windows, Web, Steam Deck and Android. The Linux, Windows and Android ones are the exception to
the Release default: they are RelWithDebInfo (the static `clang-reldbg`, `msvc-reldbg` and
`android-arm64-reldbg` presets; a player links its plugins statically), so a crash in a game shipped
on them can be resolved. The player is optimised all the same; its debug info is split off before
packaging (the Linux and Android players are stripped, the Windows PDB never enters the template)
into a `-symbols` archive published beside the template. Their ids say so
(`assiduousengine-linux64-relwithdebinfo-<version>`), and a preset asking for Release finds them by the
platform fallback below. The Web template is Release (the `wasm-shipping` preset). The Steam Deck
template is the Linux player built Release in an Ubuntu 22.04 container for the Deck's older glibc
(`scripts/build-steamdeck.sh`), with an id of its own (`assiduousengine-steamdeck-release-<version>`)
and the `handheld` icon, so it sits beside the desktop Linux template and a preset picks it by id.

The host implicit template inherits the running tool's config (develop the editor in Debug -> the
zero-setup host template is a Debug player), so shipping requires an explicit Release template; the
config axis makes this a non-special-case (the host template carries whatever config built it).

## Identity + resolution

Identity = `platform x config x engineVersion` (e.g. `assiduousengine-win64-release-0.1.0`); `compiler` is
METADATA for traceability, not a selector; `arch` folds into the platform tag. A preset selects
`(platform, config)` (config defaulting to Release), resolved by `TemplateRegistry::FindBy(platform,
config)` with an explicit `templateId` winning and a platform-only fallback (nearest config, preferring
Release) so old presets resolve. `EffectiveConfig()` treats an unstamped (v1) template as Release.

## Payload + manifest

`template.xml` (shipped): `id`, `name`, `platform`, `config`, `compiler`, `engineVersion`,
`playerBinary`, categorized runtime files - `sidecars` (REQUIRED, always staged, read from the build's
`runtime-libs` manifest) + `symbols` (optional PDB/DWARF, staged only when the preset opts in) - and
`notes`, `icon`, the SVG in the bundle the editor shows the template by (`icon.svg`; a manifest
from before it shows its platform's built-in icon), and `plugins`, each plugin its player links (id and
version, from the build's `<player>.plugins`; the bundle carries each one's Plugin.xml and deploy files
for its platform under `plugins/<id>/`, and its own `<player>.plugins` pointing at them), plus
runtime-resolved, non-serialized `directory` / `isHost`. An export uses the template's player when it
carries every plugin the project enables on the preset's platform at a version at least the one known
there and the project enables none of its own packages there; otherwise it builds the project's
player from the engine checkout (`Engine.GamePlayer`, with `ENGINE_PROJECT_PLUGINS` or, without
packages, `ENGINE_GAME_PLAYER`), and it stages each enabled plugin's deploy files beside the player
(see `Documentation/Archive/plugins-history.md`, Decision 6). Symbols do not justify a
separate template; they are an optional group on Debug/RelWithDebInfo templates, and export defaults to
NOT staging them into the shipped dist (ship stripped, retain symbols beside the template for
symbolication). The player renders from the cooked shader pack, so it carries no shader compiler: the
player build lists none, and an export leaves out any DXC library a template still lists.

## Create / Import / Resolve

- **Create** - `CreateTemplate(configDir, destRoot, mode, ..., identity)` synthesizes the descriptor from a build dir
  (`SynthesizeHostTemplate` reads platform + `runtime-libs`), stamps config/compiler/engineVersion,
  copies the player + sidecars (`FileCopyPreserving`, keeps +x), writes the icon (`icon.svg`: a
  built-in one by name, `--icon handheld` for the Steam Deck build, or a given `.svg`; else the
  platform's built-in icon: windows, linux, web, phone for Android, desktop for any other) and
  `template.xml`; `TemplateIdentity` (the CLI's `--id`, `--name`, `--notes`, `--icon`) replaces the
  canonical id, name and notes, so a second bundle for one platform sits beside the first, and
  installing an id replaces its bundle whole. A missing sidecar refuses the template, and each plugin
  the player links is copied with its Plugin.xml and deploy files. The platform is the build directory's (`Bin/<Config>/<Platform>-<Compiler>`), so a
  host packages another platform's build: an Android build gives an "Android64" template whose player
  is the library the app loads (`libEngine.Player.so`) and whose sidecars are the app's Gradle project
  (`android/`, with SDL's Java glue in `android/sdl-java/`), which the Android build stages beside
  the library; `scripts/build-export-templates.sh android` makes it (through the NDK at
  `ANDROID_NDK_HOME`; exporting with it needs the Android SDK and a JDK, making it does not).
  `scripts/build-export-templates.sh linux` makes the Linux template and
  `scripts/build-export-templates.ps1` the Windows one. Two modes:
  install into the templates root (usable immediately) or export to a folder (for zip + distribution).
  A Web build synthesizes a "Web" template (the player is the `.html`, sidecars from the manifest, and
  export stages page + sidecars + `Content.pak` + `player.xml` + the WGSL `Data/Engine/Shaders/shaders.dpak`
  with its `Data/.dataroot` marker). `scripts/build-export-templates.sh web` makes two: `web-release`
  from the `wasm-shipping` preset (smallest, no profiler; the samples' Web presets and the published
  demos use it) and `web-profile` from the `wasm-release` preset (Release with the profiler, its
  capture and its overlay compiled in, the overlay off by default; given its own id since both
  builds land in `Bin/Release/`), to profile a game in a browser.
- **Import** - `ImportTemplate` (validate `template.xml` -> recursive copy under `<id>`).
- **Remove** - `RemoveTemplate` deletes an installed bundle (the editor's Export Templates window;
  the synthesized host template cannot be removed).
- **Resolve** - as above.

## Baseline engine content (shipped)

The shaders-out-of-C++ track shipped, so the player boots against a COOKED shader pack, not baked-in
HLSL: engine shaders are `.hlsl` source cooked by `foundation.shaders`' `ShaderPackCooker`, and export
stages `Data/Engine/Shaders/shaders.dpak` (+ `Data/.dataroot`) beside the player for the preset's platform
(`StageShaderPack`, cooked from the resolved data root's `Engine/Shaders/`) so the dist renders with no external
dependency (Web stages the WGSL variant) - the same `Data/` layout every executable discovers
(`Documentation/Systems/data-root.md`). This baseline runtime content is
produced by EXPORT per platform rather than carried inside the template bundle - so the template stays
just the player + sidecars, and the render-pass shaders are dist content. Fonts/fallback textures the
player needs to boot are similarly minimal today.

## Deferred

- **`capabilities`** manifest field (compiled-in RHI backends so a preset can require one) - forward-
  looking, not built.
- **Downloadable templates** in the tools (fetch a versioned archive; same install path as import) -
  future. Today a release's archives are downloaded by hand and installed with Import.
- **Baseline content INSIDE the template** vs produced by export. Today the shader pack is export-
  produced per platform; folding a prebuilt baseline pak into the template bundle is an option only if
  a real need appears (open, low priority).
