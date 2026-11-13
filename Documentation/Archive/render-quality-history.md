# Render quality

> ARCHIVED 2026-11-12, built Q0 to Q5; Q6 (cheaper effects: a render scale and half-resolution
> traces) is in Documentation/Plans/backlog-0.2.md. Non-authoritative: the spec as approved, kept
> as the record of what was built and why; the code and the tests are the truth.
>
> STATUS: APPROVED 2026-11-12 (user), with its three questions decided the same day. Sized L, in phases Q0 to Q6, green after each.
> Origin: the sample games measured on a Pixel 6 with their scenes' post-processing applied
> (2026-11-12) run at 8 to 55 fps, and what costs most there is what a scene asks for (Lamplight's
> SSGI is about a third of its frame, Masquerade's AO and SSR about 34 ms together). Profiles are
> per scene, not per platform, so nothing today lets a game ask less of a phone than of a desktop.
> The user asked how the reference engines organise this; the survey is under "Prior art". Read
> CONVENTIONS.md first.

## Goal

A game looks the same everywhere and costs what each platform can pay. The scene keeps deciding
what it looks like; a **quality level** decides what that costs. A project offers a few levels
(Low, Medium, High, its own) and starts at one; each export preset (a platform's target) can pick
another, and the player can change it from a game's options menu, kept across runs. The editor
previews any level, and the Game tab plays at the level of the preset it previews.

Not goals: a separate mobile renderer (one render path; the cost is in a few effects a level turns
down or off), choosing a level from the GPU at runtime (no reference engine does; a platform and a
preset choose), per-tier values authored in a scene (Decided 4), and texture size or
compression per platform (the export preset's, decided at cook, as today).

## Decided (user, 2026-11-12, on the proposal that led to this spec)

1. **A quality level is an asset**, a Render Quality, made and edited like the render profiles.
2. **The project lists its levels and the one it starts at; each export preset picks its own**, by
   reference (ruled 2026-11-12 over per-platform defaults in the project): presets are the
   targets, every run on a platform comes from one, and each already names its platform. The
   project's default serves play-in-editor and a run from the project folder. Per-platform defaults
   can come later as a layer under the presets if a new preset's starting level proves a nuisance.
3. **Antialiasing is the level's**, with MSAA: a cost decision. The post profile's `aaMode` goes (a
   data version bump); Lamplight's and Masquerade's FXAA move to their levels. (Flax keeps the AA
   mode in the authored post; Traktor makes it quality.)
4. **One authored value per scene, scaled by the tier.** Values per tier in a scene (Traktor's
   shadow settings per tier) are left for later evaluation, in `backlog-0.2.md`.
5. **Named levels made of per-effect tiers**: a platform and a preset pick one name. Per-effect
   overrides from an options menu (shadows Off at High) are left for later evaluation, in
   `backlog-0.2.md`.

## What exists (checked 2026-11-12)

- **The scene's look.** `EnvironmentSettings` and `PostProcessSettings`
  (`Code/Engine/Engine.Render/RenderComponents.cppm`) are a scene's blocks; a block whose `source`
  is Profile takes its values from an Environment Profile or Post Process Profile asset
  (`RenderProfiles.cppm`, cooked by `Code/Pipeline/Render.Pipeline/RenderProfileAsset.cppm`, whose
  `SettingsProfileAsset` is how the editor edits any profile). They mix the look (sky, sun,
  ambient, exposure, tonemapper, grading, bloom's threshold and intensity, each effect's radius and
  strength) with what costs (`aoMode`, `ssrEnabled`, `ssgiEnabled`, `aaMode`, `shadowDistance`).
- **What the project sets.** `ProjectSettings::renderMsaaSamples` (`Engine.Project/ProjectModule.cppm`,
  default 4), applied by the player (`Engine.Player/PlayerApplication.h`, `SetMsaaSamples`) and by
  play-in-editor (`Editor.Play/GamePage.cppm`, `GameViewPost`). `RenderSubsystem::ViewPost` resolves
  a view's post from the scene (or its profile) and applies the host's MSAA count over it.
- **What is fixed in code.** Sun shadow cascades: 4 (`kShadowCascadesCount`, `RenderData.cppm`) of
  1024 each (`kShadowResolution`, `ShadowSystem.cppm`); point lights: a 2048 atlas of 512 tiles.
  IBL prefilter 256 (`IBLSystem.cppm`), reflection probes 128 (`ReflectionProbeSystem.cppm`).
- **What each effect costs, as parameters.** SSAO `sampleCount` 16 (`AoPass.cppm`), SSR `maxSteps`
  96 (`SsrPass.cppm`), SSGI `rayCount` 4 and `maxSteps` 12 (`SsgiPass.cppm`), bloom up to
  `kMaxLevels` 7 (`BloomPass.cppm`).
- **Platforms and presets.** An export preset names its platform (`Linux64`, `Win64`, `Web`,
  `Android64`; the Steam Deck preset is a `Linux64` one) and can override the project's render
  size, window and orientation (`Editor.Project/ExportPreset.cppm`); the export writes the result
  into the dist's `player.xml`. The Game tab's resolution dropdown lists the presets
  (`GamePageImpl.cpp`, `CollectGameResolutions`), with a Phone preview.
- **Kept per player.** `foundation.settings` stores typed sections in the user data directory;
  `SaveValues` is a game's keyed saved values.

## Prior art (surveyed 2026-11-12)

- **Traktor**: a tier per effect, Disabled to Ultra (shadows, AO, reflections, AA, motion blur,
  volumetric fog, irradiance, particles, terrain), each a flat config key. Deploy targets pick
  "Feature" assets; the Android system feature sets AO, AA and motion blur Disabled and
  reflections and shadows Low; a target's quality features override by priority, the user's config
  last. A scene authors one shadow settings entry per tier and the tier picks it; Disabled gates an
  authored effect off. The world renderer (Deferred, Forward, Simple) is itself a feature. A script
  reconfigures at runtime by recreating the world renderer.
- **Flax**: Low to Ultra per effect in one graphics settings asset (AA, SSR, SSAO, volumetric fog,
  shadow filtering, shadow map size, global SDF, GI). Quality only sets cost (samples, resolution,
  shader permutation); the authored post settings decide whether an effect runs and which AA mode.
  No per-platform selection: mobile and web lean on compile flags and GPU feature checks.
- **ezEngine**: a platform profile picks the render pipeline asset, the CVar defaults file
  (`CVars_<Profile>.cfg`) and texture limits; chosen by name at launch. Runtime quality is CVars;
  presets exist only as an options menu's tables.
- **Godot**: any project setting has tagged overrides (`key.mobile`, `key.web`, custom export tags).
  It ships mobile defaults this way (shadow size 2048, no soft shadows, small reflection probes) and
  picks its renderer per tag; the mobile renderer silently skips SSAO, SSR, SSIL and SDFGI.
- **Doriax**: quality is per scene only; MSAA is hard-coded per platform.

What four of five share, and this design takes: the scene authors the look and which effects it
wants; quality is per effect and sets the cost; Off gates an effect but quality never turns one on;
a platform gives the default, a target overrides it, the player's choice overrides both.

## Design

### The Render Quality asset

A Render Quality is a data asset, made by `asset_create` and edited on a page as the profiles are.
Its cooked record is `RenderQualitySource`; its product, `engine::render::RenderQualityLevel`. Fields:

| Field | Values | What it does |
| --- | --- | --- |
| `label` | text | The name a game's options menu shows ("High") |
| `msaaSamples` | 1, 2, 4 | The scene pass's MSAA count, clamped to the device (was the project's) |
| `antialiasing` | Off, FXAA, TAA | The AA after the scene pass (Decided 3); was the post profile's `aaMode` |
| `shadows` | Off, Low, Medium, High | Off: no sun or point shadows. Low to High: cascade resolution 512, 768, 1024; point atlas tiles 256, 512, 512 |
| `shadowDistanceMax` | metres, 0 for none | Caps the scene's `shadowDistance` |
| `ambientOcclusion` | Off, Low, Medium, High | Off gates the scene's AO. SSAO samples 8, 12, 16; GTAO slices by steps 2x4, 2x6, 3x6 |
| `reflections` | Off, Low, Medium, High | Off gates SSR. March steps 32, 64, 96 |
| `globalIllumination` | Off, Low, Medium, High | Off gates SSGI. Rays and steps 1x12, 2x12, 4x12 |
| `bloom` | Off, Low, Medium, High | Off gates bloom. Pyramid levels 4, 5, 7 |
| `environmentResolution` | Low, Medium, High | IBL prefilter 128, 256, 256; probes 64, 128, 128 |

High is each effect's full cost as it was before the levels, so a game at High draws as it did; Low
and Medium cost less. The numbers are a starting point, measured and tuned in Q5. What a scene authors stays the
scene's: a gated effect keeps its values and comes back at a level that allows it.

A tier never turns an effect on: a scene without AO has none at High. A tier never changes the
look's values (radius, strength, intensity); it changes how many samples get there.

### What the project and a preset hold

- `ProjectSettings::qualityLevelIds`: the project's levels, in the order an options menu lists them.
- `ProjectSettings::defaultQualityId`: the level the game starts at where no preset picks another
  (play-in-editor, a run from the project folder); nil is the list's last level (by convention the
  highest). It must be one of the levels.
- `ExportPreset::qualityLevelId`: the level the preset's platform starts at (the Steam Deck's Medium
  beside the Linux desktop's High), one of the project's levels; nil is the project's default.
- `renderMsaaSamples` leaves the project: each level carries its count. A project saved before is
  migrated (a data version bump): the editor makes one level, `Default`, with the project's count
  and the effects at High, and lists it as the default. A project without levels runs at the engine's built-in
  level (every tier High, MSAA 4), which is what today's player does.

### Which level applies

At startup, in this order, the last one found wins:
1. The project's default (`defaultQualityId`, else the list's last level).
2. The export preset's level (written into the dist's `player.xml` as its default).
3. The player's own choice, if the game saved one (a `foundation.settings` section in the game's
   user data, by level guid; a guid the project no longer lists is ignored).

The export cooks every listed level into the pak and lists them as entry points, so a pruned
export keeps them. The player loads the starting level before the first scene, since the MSAA
count and the shadow resolution shape render targets.

### In the renderer

`RenderSubsystem` holds the active level. `ViewPost` resolves the scene's post (as now),
then applies the level: gates (an Off tier clears the effect's enable), caps (`shadowDistanceMax`)
and the MSAA count. The cost parameters (samples, steps, rays, levels) go to the passes per frame
from the level, not from the scene.

Changing the level at runtime:
- MSAA, the shadow resolution and the environment resolution recreate their targets, through the
  retire queue (`RetireBindGroup` and the textures' equivalent), never with a wait.
- The environment resolution marks every IBL context and probe dirty, so they bake once again.
- TAA's history resets.

### In the editor

- The Render Quality page edits a level's fields, as the profile pages do.
- Project Settings has a Quality tab: the levels in order (add, remove, reorder) and the default.
  The Export dialog's preset settings gain the preset's level.
- The Game tab plays at the level of the preset its resolution dropdown previews, and at the
  project's default for the project's own entry. A Quality dropdown beside it overrides that for the
  session.
- The scene viewport draws as authored (no level) by default, and can preview any level from its
  toolbar, an editor-only setting.

### For scripts

A `RenderQuality` facade:
- `RenderQuality.levels()`: the levels' labels, in the project's order.
- `RenderQuality.current()`: the active level's label.
- `RenderQuality.set(label)`: switch now and save the choice; false for a label the project lacks.

Per-effect overrides from an options menu (shadows Off at High) are for later evaluation
(Decided 5); the facade leaves room for them.

### MCP

- `asset_create` makes a Render Quality by its creator's name; `asset_data_write` edits it.
- `project_settings_set` takes `qualityLevelIds` (the whole list, as `uiFontIds`) and
  `defaultQualityId` (one of them).
- `export_preset_set` takes `qualityLevelId` (one of the project's levels).
- `pie_quality` sets the Game tab's override, as `pie_resolution` sets its resolution.
- McpGuide.md documents them in the same commits.

## Phases

**Q0: the asset, the project and the preset.**
- `RenderQualitySource`, `RenderQualityLevel`, its factory and resource module (`engine.render`), the
  pipeline asset beside the profiles (`render.pipeline`), and its creator.
- `ProjectSettings::qualityLevelIds` and `defaultQualityId`; `ExportPreset::qualityLevelId`;
  `player.xml` carries the list and the default (the preset's level when it has one). Nothing reads them yet:
  `renderMsaaSamples` and the post profile's `aaMode` still drive, so every game draws as before.
- The export cooks the listed levels and keeps them in a pruned pak.
- MCP: `project_settings_set` and `export_preset_set` take the new fields. McpGuide.md.
- Tests: Engine.Render.Tests (the record round-trips; the factory builds the product),
  Editor.Core.Tests' ProjectTests (the fields round-trip) and ExportTests (the export writes the
  default, a preset's override wins, a pruned export keeps every listed level), Integration.Mcp
  (create, list, set, export).

**Q1: the renderer applies a level.**
- `RenderSubsystem::SetQuality`; `ViewPost` gates, caps and takes the MSAA count and the AA from it.
- The old fields go, with a data version bump each: `renderMsaaSamples` from the project (12),
  `aaMode` from the post settings (5), the post profile asset and its cooked record (2). Opening a
  project saved before makes its `Default` level from its MSAA count, lists it and saves the
  manifest. The AA is not carried over (ruled 2026-11-12, as reading it back would keep a removed
  field alive for the migration): an older post block or profile whose AA was on says so in the log
  when it loads, for its level to take it. The samples are migrated through MCP in the same phase.
- The player resolves its starting level (preset, project default, saved choice) through one
  function in `engine.project` (`ProjectSettings::StartupQuality`), and applies it before the first
  scene. Play-in-editor plays at the project's level on the Game tab's own views
  (`RenderSubsystem::SetViewportQuality`, keyed by the page), since one render subsystem serves every
  editor page.
- Tests: Engine.Render.Tests (an Off tier gates the scene's effect and keeps its values; a tier
  never turns an effect on; the shadow cap and Off hold; the MSAA count and the AA come from the
  level; a viewport's own level), Editor.Core.Tests' ProjectTests (an old manifest upgrades into a
  `Default` level with its MSAA count, once), Editor.Play.Tests (the Game tab's level), and a new
  Engine.Project.Tests (the module has none yet) for the order of the three sources and a saved guid
  the project no longer lists.

**Q2: the tiers reach the passes.**
- Sun cascade resolution and the point atlas tile size become runtime values (from the level), as
  do the IBL and probe prefilter sizes; their targets recreate through the retire queue on a change.
- AO samples, SSR steps, SSGI rays and steps and bloom levels come from the level per frame.
- Tests: Render.Tests (each tier declares its passes with the level's sizes and counts; a change of
  shadow tier retires the old targets and makes new ones; a change of environment tier rebakes once).

**Q3: the editor.**
- The Render Quality page; Project Settings' Quality tab; the Export dialog's override.
- The Game tab's level (the previewed preset's) and its Quality dropdown; the scene toolbar's
  preview; `pie_quality`.
- Tests: Editor.Play.Tests (the Game tab takes a preset's level; the dropdown overrides it),
  Editor.Core.Tests (the settings tab writes the lists); the pages checked by running the editor.

**Q4: scripts.**
- The `RenderQuality` facade in AngelScript and Luau; the saved choice in a settings section.
- Tests: in Code/Integration, a script lists, reads and sets a level in both languages; a saved
  choice is the player's starting level on the next run.

**Q5: the samples.**
- Each sample game gets Low, Medium and High levels (made through MCP), its presets' levels,
  and an options entry where the game has an options menu.
- The tier numbers are tuned on the Pixel 6 and the desktop; the phone measurements are taken again.
- Integration.Mcp's sample checks read the new data versions.

**Q6 (backlogged 2026-11-12, in `backlog-0.2.md`): cheaper effects.**
- Why, from Q5's measurements on the Pixel 6: an effect there costs mostly by being on, at full
  resolution. Medium's smaller counts saved little on the samples with SSGI and AO (Lamplight 94 ms
  at Medium against 129 at High, 21 at Low with them off), because the passes that do not scale
  with the counts (SSGI's resolve, composite, denoise and blur, about 20 ms) dominate.
- A render scale (the 3D scene at a fraction of the output, then upscaled; the UI at full size) as a
  level field.
- Half-resolution SSR and SSGI traces as a tier.

## Gotchas

- A level that changes MSAA or a target size mid-frame must not destroy what the GPU still reads:
  the retire queue, as the MSAA resolve and TAA now do.
- The environment resolution's rebake is the bake that just became once-only: a level change marks
  it dirty, it must not reintroduce a warm-up window.
- `pruneToReachable` follows references from scenes; the levels are referenced from the project, so
  the export names them as entry points, or a pruned pak loses them.
- The Game tab's level follows the previewed preset; the scene viewport's does not change unless
  its user picks one, so editing a scene's look never happens at Low by accident.
- The cascade count stays 4: changing it reaches the shaders' cascade loop, which is not this spec.

## Tests (the contract)

Listed per phase above. Every phase is green on the lanes (clang, gcc, clang-shared, the wasm
build, ASAN) before the next starts.
