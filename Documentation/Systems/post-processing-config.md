# Post-Processing Configuration

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[post-processing-config]]

The post stack (tonemap/AgX, bloom, AO, SSR, SSGI, auto-exposure, color grading, TAA, FXAA) is
authored data, not hardcoded knobs: edited per scene (or shared through a profile), resolved and
applied PER VIEW, serialized with the scene, shippable, with editor viewport overrides. What each
effect may cost, and which antialiasing runs, is the quality level's (below). Phases 1 + 2a + 2b + 3
shipped, and auto-exposure and grading from phase 4; the rest of phases 4-5 (breadth) is pending.

## Authoring surface (per scene)

A dedicated `PostProcessSystem` + reflected `PostProcessSettings` on the render subsystem
(`RenderComponents`), added to every scene beside `EnvironmentSystem`, auto-surfacing as a
"Post Processing" scene-settings inspector section (undoable) that serializes with the scene
(settings id `postprocess`, payload version 5). Exposure is authored in EV/stops (tonemap applies
`2^EV`; 0 = neutral). Fields: `exposureEV`, `tonemapOperator` (Clamp / AgX), bloom (enable +
threshold/knee/intensity), AO (`AoMode` Off / GTAO / SSAO + strength/radius/intensity), SSR (enable +
intensity), SSGI (enable + intensity), auto-exposure (enable + key/speed/minEV/maxEV), a grading LUT +
intensity, and how TAA and FXAA look where they run (`taaBlendFactor`, `taaVarianceGamma`,
`fxaaSubpixel`). Defaults MATCH the old hardcoded RenderSubsystem values, so authoring changed no
visuals until edited. Scripts edit the live block as `scene.postProcess`.

Which antialiasing runs is not the scene's: the `AaMode` (Off / FXAA / TAA) is a field of the quality
level (`RenderQualitySettings::antialiasing`). A scene or profile saved with the older layout (payload
3 or 4) still reads; its mode is dropped and, when it was TAA or FXAA, logged for its level to take.

**Profiles.** `source` (`SettingsSource` Scene / Profile) picks where the values come from: the
scene's own, or a shared Post Process Profile asset (`PostProcessProfile`, `engine.render:profiles`)
that `profile` references. `PostProcessSystem::Effective()` answers the profile's values while the
source is Profile and it is loaded, else the scene's. The editor's profile mode edits the profile.

## Resolved per view

Per-view because the engine renders multiple views per frame (split-screen) and per-view transient post
state (TAA history/jitter/sub-rect) already lives on `ViewSettings`. `ViewPostConfig` (primitives, in
the snapshot `:data` layer) sits on `ViewSettings`; `RenderSubsystem::ViewPost` resolves each view's
post from the scene's `PostProcessSystem` via `ResolveScenePost` (exposure EV -> linear `2^EV`; `AoMode`
-> u32; `tonemapOperator` -> the AgX bool; the auto-exposure EV window -> linear clamps; a strip LUT ->
view/uid/size), or a latched global override (the programmatic setters, for samples' debug panels).
Then `ApplyQuality` lays the view's quality level over either (next section). The compose reads
`v->Settings().post`. TAA-per-view is made tractable by pre-resolving `needsMotion` onto
`ViewPostConfig` (TAA, or temporal SSR, or SSGI, whose temporal resolve reprojects by velocity; the
forward pass reads it at execute), so jitter, motion vectors, the TAA resolve, and FXAA are all
per-view; the Halton phase advances once/frame if ANY view used TAA. SSR overrides the frame-global
`SsrPass::Params` intensity per view (and lowers its step count to the view's). The tonemap operator
is a push float with a Clamp branch (saturate + sRGB OETF) in the shader; AgX stays default.

## Quality levels

A Render Quality asset (`RenderQualityLevel`, its values a `RenderQualitySettings`) is one level a
project offers: a label, the scene pass's MSAA count (`msaaSamples`), the antialiasing, a
`QualityTier` (Off / Low / Medium / High) each for shadows, ambient occlusion, reflections (SSR),
global illumination (SSGI) and bloom, a cap on the shadow reach (`shadowDistanceMax`, 0 caps nothing)
and an `environmentResolution` (`ResolutionTier` Low / Medium / High). The defaults are the engine's
highest, which is how a scene drew before the levels.

`RenderSubsystem::SetQuality` sets the level every view draws at; `SetViewportQuality` gives one
viewport its own (the Game tab plays at its game's level while the editor's views keep theirs).
`ApplyQuality` over a view's `ViewPostConfig`: an Off tier turns its effect off whatever the scene
asks, Low to High set the cost (SSAO samples 8/12/16, GTAO slices 2/2/3 and steps 4/6/6, SSR steps
32/64/96, SSGI rays 1/2/4, bloom levels 4/5/7), the view takes the level's MSAA count and, unless a
global override is latched, its antialiasing. No tier turns an effect on. Over the extracted scene,
shadows Off draws none and the cap shortens the scene's shadow reach. The level also sizes the shadow
maps (cascade 512/768/1024, atlas tile 256/512/512) and the environment (sky prefilter 128 at Low,
else 256; reflection probes 64 at Low, else 128).

The project lists its levels and the one it starts at, an export preset may pick its own, and the
player keeps a user's choice in its settings (`--quality <label>` starts at one without keeping it).
Each sample game offers Low, Medium and High. Scripts read and switch levels through the
`RenderQuality` facade (`levels()`, `current()`, `set(label)`).

## Editor viewport overrides

`ViewPostOverride` (`disablePost` master + per-effect `disableBloom`/`Ao`/`Ssr`/`Ssgi`/`Aa`,
`disableCulling`, and `msaaOverride` for a forced 1x/2x/4x) is an optional `RenderScene` param (mirrors
`cameraOverride`), applied via `ApplyViewPostOverride` to the resolved per-view post before the
`needsMotion` finalize - ephemeral, never serialized. `disablePost` also freezes auto-exposure at the
authored EV. The scene viewport's Shading chip (`SceneHud`) opens a menu with an Effects submenu of
checkable show flags (Post-processing, Bloom, Ambient Occlusion, Reflections, Global Illumination,
Anti-aliasing, Frustum Culling), an MSAA submenu and a Quality Level submenu, all bound to page-local
state.

## Deferred

- **Phase 4 - breadth**: color-grading LUT, depth of field, vignette, motion blur, auto-exposure.
  Auto-exposure and the grading LUT are built. `ExposurePass` (`foundation.render:exposure`)
  measures geometric-mean luminance into a persistent per-view 1x1 ping-pong (a 16x16 sample grid
  over the view's sub-rect, exponential adaptation eased by the frame dt via
  `RenderFrame::SetDeltaSeconds`) imported into the graph so the tonemap's read is a real edge;
  the tonemap multiplies exposure by clamp(key/avg, 2^minEV..2^maxEV). Grading is a
  display-referred 2D strip LUT (author a neutral 256x16, grade it in an image editor, import
  with Color Space = Linear; height = size, width must equal size^2 or the resolve ignores it)
  applied after BOTH tonemap operators with an intensity mix. Not built: depth of field, vignette,
  motion blur.
- **Phase 5 - localized looks**: per-camera overrides + post-process volumes (blend by region). Not
  built.
