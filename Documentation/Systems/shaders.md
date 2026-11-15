# Shaders - out of C++, cooked per backend

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[shaders-track]]

Engine shaders are HLSL source `.hlsl` files (not C++ string banks), compiled on demand by runtime DXC
in dev and cooked to per-backend bytecode for export. Shipped: the source lift, cook-time bytecode,
the WGSL translation target, a project's own custom shaders, and the shader editor page.

## Decisions

- **HLSL is the authoring language** (the whole ported corpus is HLSL; DXC is the front end). WGSL is a
  cook-time TRANSLATION target, not a source language.
- **All engine built-ins live as `.hlsl` files** in the data root's `Engine/Shaders/` folder
  (`Data/Engine/Shaders`, `kShaderFolder`), read through the
  application's data mount (`Documentation/Systems/data-root.md`), sharing code via `.hlsli` - served
  to DXC by the file provider as the compiler's include resolver (no native include paths). Passes
  fetch by name. The old C++ string-bank partitions are deleted.
- **Dual-mode cooked form**: dev/editor keeps runtime-DXC compile-on-demand (instant hot reload, no
  variant pre-enumeration); EXPORT cooks declared variant sets to per-backend bytecode - SPIR-V (Vulkan)
  / DXIL (DX12) / WGSL (WebGPU). A shipped dist drops the DXC sidecar entirely. Android has no
  shader compiler at all: its shaders are always cooked on the host, into a SPIR-V pack the
  Android player reads.

## Modules

- **`foundation.shaders`** (`Code/Foundation/Shaders`, `+ /Cook`) - the dlopen'd DXC `Compiler`, the
  `CookedShaderPack` cooked format (cooked by `CookEngineShaders` in `ShaderPackCooker.cppm`), the
  properties and effect-block parser (`:properties`), and the `WgslTranslator`.
- **`foundation.shaders.resource`** (`Shaders.Resource`) - `ShaderResource` / `ShaderFactory` /
  `ShaderSource` (a cooked custom shader's programs and property table).
- **`foundation.shaders.system`** (`Shaders.System`) - the `ShaderSystem` variant registry, the host
  that picks dev-compile vs pack mode over the data mount (`ShaderSystemHost`: `Engine/Shaders/`
  sources, or the cooked `Engine/Shaders/shaders.dpak` - `kShaderPackPath`) + `FileShaderSourceProvider`
  (the hot-reload source AND the include resolver). Dev mode wins whenever a compiler and the
  sources exist; pack mode is entered when it cannot, or by `ShaderPackPolicy::ForcePack`, the
  `OPTION_USE_SHADER_PACK` environment variable, or an `Engine/Shaders/.pack-first` marker (an
  editor distribution, which ships the sources beside its pack so custom shaders still compile).
- **`shaders.pipeline`** (`Pipeline/Shaders.Pipeline`) - the custom shader cook: `ShaderAsset`,
  `ShaderAssetBuilder`, `CheckShaderSource` (what the cook proves, as data, for the editor and the
  MCP tools), the File > New shader creators and the `.hlsl` drop importer.
- **`editor.shaders`** (`Editor/Editor.Shaders`) - `ShaderEditorPage`, a custom shader's page (see
  below).
- **`Tools.ShaderPack`** - the CLI shader-pack cooker. `--entries <dir>` cooks shaders that
  name their entry points in their own comments (`// entry: vertex VSMain`, `// model: 6_5`),
  each under `<file>:<entry>`: the RHI samples' (`Data/Samples/RHI`), which compile as they run
  on the desktop (`SampleApp::LoadShader`) and read that pack where there is no compiler
  (Android). Both take `DirectCompileOptions`, so a cooked module is the one compiled.

## Depth convention

`Data/Engine/Shaders/depth.hlsli` is the shader-side twin of `core::projection` / `rhi::depth`: REVERSE-Z
(near = 1, far = 0, cleared = 0, nearer = larger). A shader that tests for background, walks depths
to the closest, biases a shadow compare or linearizes explicitly includes it and names its helpers
(`IsBackgroundDepth`, `NearerOf`, `BiasTowardViewer`, `LinearizeDepth`); one that reconstructs
position through an inverse projection is convention-free. See renderer.md 8.1.

## The forward library

The forward pass is built from includes a custom shader uses too (`Archive/custom-shaders-history.md`):

- `view.hlsli`: the frame's View constants (set 0, b0), the one declaration of their layout.
  Mirrors the CPU view block.
- `forward_vs.hlsli`: the whole vertex stage (skinning, instancing, wind, the motion-vector clip
  positions), `main` unless the includer names it (`#define FORWARD_VS_ENTRY VSMain`).
- `lighting.hlsli`: the lights, shadows, image-based light, reflection probes and clusters
  (sets 0 and 3) and the functions over them: `DirectLight` (every light reaching a pixel,
  Cook-Torrance shaded and shadowed), `AmbientLight` (IBL, probes and the flat ambient),
  `EvaluateLight`, `ShadowFactor`, and for a shader that shades lights its own way
  `LightsAt`/`LightAt` (the pixel's cluster list, or every light), `LightDirection` (direction
  and falloff) and `ShadowOf`.
- `forward_ps.hlsli`: the pixel stage's contract: `GBufferOutput` (the five targets),
  `ScreenDoorFade` (DITHER), `OctEncode` and `MotionVector`.

- `shadow_depth_vs.hlsli`, `pick_ids_vs.hlsli`: the depth and pick vertex stages, which
  `shadow_depth.vs.hlsl` and `pick_ids.vs.hlsl` are under `main`.
- `object_block.hlsli`: the per-draw block of a single (non-instanced) object (set 1, b0), one
  declaration shared by the forward, shadow depth and pick vertex stages.
- `wind_sway.hlsli`: the wind sway as a function of its parameters (`wind.hlsli` declares the
  `Wind` effect block and binds the sway to it).

`forward.ps.hlsl` is these plus its material and its surface code; the split (2026-10-12) left
its compiled WGSL the same but for names and one constant's placement.

**Surface shaders** (`surface/`, `Archive/custom-shaders-history.md` phase 6). The cook assembles a
surface shader's three programs as `surface/surface.hlsli` (its API: `SurfaceInput`,
`SurfaceOutput`, `VertexInput`, `Time()`), the author's text, `surface/glue.hlsli` (the calls
into `Surface()` and `Vertex()`), and a pass template: `surface/forward.hlsli`,
`surface/depth.hlsli` or `surface/pick.hlsli`. The vertex libraries above run `Vertex()` where
`SURFACE_PROGRAM` is defined (in an `#ifdef`, so the engine's own shaders compile as before); a
material on a surface shader with `Vertex()`, or masked, draws the depth prepass, the shadows and
the pick with the shader's `.depth` and `.pick` programs (`Material::depthShaderName`).

**Effect blocks.** An include can declare a vertex effect's own parameters with the properties
grammar: `effect Wind : WIND { float Strength = 0 [0, 2] (gate); ... }` names the effect and the
variant flag its code compiles in, takes uniforms only and exactly one float marked `(gate)`. It
expands to `struct WindEffect { ... };` and `ConstantBuffer<WindEffect> Wind : register(b1, space2)`,
so its code reads `Wind.Strength`; the compiler's include handler expands it wherever the include
is served. The pack records each effect (pack format v4) and `ShaderSystem` serves them by name and
by flag (`FindEffect`, `EffectForFlag`); a material carries the effect groups its shader's vertex
stage declares, and the renderer turns an effect's variant on while its gate is above zero. Wind is
the one effect today, so every shader that declares `WIND` can sway.

## Variant model + cooked pack

RHI takes BYTECODE only (`ShaderModuleDesc.code` = SPIR-V / DXIL, or WGSL text; HLSL never crosses
the RHI boundary). `ShaderSystem` is a per-(name, stage) source registry + `ShaderFlags` (permutation bits ->
`#define`s) + compile-on-demand caching. Cook: shaders declare their meaningful variant subset of the
flag space; the cooker compiles each `(name, stage, variant, backend)` to a `CookedShaderPack` entry (SPIR-V /
DXIL bytecode, or WGSL text), binary + versioned + self-describing (carries the name table so the
runtime provider resolves by name). A shipped dist reads the pack and carries no DXC or naga.

**Entry points are data, not an assumption.** Every source is registered per (name, stage) with
the name of that stage's function in it (`RegisterSource(name, stage, hlsl, entryPoint)`; a
provider returns it with the text): one file may serve both stages as `VSMain` and `PSMain`, and
the engine's own corpus states its convention once, a file per stage whose function is `main`
(`kCorpusEntryPoint`). The compiler, the WGSL translation and the pack carry it (pack format v3
added a per-stage entry table; the pack is v4 now, and an older pack is refused and cooked again), and every module the
system creates records it (`ShaderModuleDesc::entryPoint` -> `ShaderModule::entryPoint`). A
pipeline stage runs the entry it names, else its module's (`rhi::EntryPointOf`; `rhi::StageOf`
builds a stage that runs the module's), so no pass or cache writes `main`; a stage with neither
is refused by every backend and by the validation layer.

Two source directives, parsed from the stage file by every compile site (the runtime system, the
pack cooker, the WGSL translator): `// variants: A B C` declares the flag lattice; `//
preserve-interface` keeps every declared stage input / output in the SPIR-V interface (DXC's
`-fspv-preserve-interface`; `-O3` otherwise strips unread inputs). The second exists for a
fragment stage that is paired with another shader's vertex module and declares that module's
full output struct while reading one member - terrain_depth.ps under HOLES, whose vertex stage is
the colour pass's `terrain` module so the prepass depth stays bit-identical. Vulkan matches a
stage interface by location and treats a shorter input list as an error (or, when the locations
happen to line up, a silent garbage read), never as a subset; the terrain holes probe runs with
validation ON and counts `rhi::vk::ValidationErrorCount()` around the render for exactly this.

## Custom shaders (a project's own)

A custom shader is a `ShaderAsset`: ONE `.hlsl` with a `properties { ... }` block declaring its
material parameters, of one of two kinds (`Documentation/Archive/custom-shaders-history.md`):

- **Low-level**: both stages written out (entry points as asset fields, `VSMain` / `PSMain` by
  default) with a variant lattice per stage (`// variants(vertex): ...`,
  `// variants(fragment): ...`), keeping the forward contract with the library above.
- **Surface** (`// surface: lit` or `unlit`): a `Surface()` and an optional `Vertex()`, which
  the cook wraps in the `surface/` templates as three programs (forward, `.depth`, `.pick`) with
  the templates' lattices.

The cook (`ShaderAssetBuilder`, through `CheckShaderSource`) replaces the block with the generated
set-2 declarations (the `Material` cbuffer by HLSL packing, `PackCbufferMember`, then the textures
and samplers), keeps the author's line numbers with `#line`, and COMPILES every program's every
variant with the engine's library (`<data root>/Engine/Shaders`) on the include path: a failure fails
the cook with `File.hlsl:line: message`, and the cook keeps the last good product. The product
(`ShaderSource`) carries each program's expanded source, entries and lattices and the property
table; the runtime registers each program with its lattices, so requests canonicalize as the
corpus's do, and a failed runtime compile is remembered until the source changes (or a reload).
A material on a custom shader is cooked by the shader's table (its order and offsets; values by
name where the type matches, else the shader's default) and re-cooks when the shader does; it
draws the depth-like passes with the shader's own programs when the shader has a `Vertex()` or
the material is masked.

**In a dist**: the export cooks every program to the preset's formats into the dist's pack beside
the corpus, and strips the products' HLSL (the preset's `includeShaderSource` keeps it); a dist
serves a registered shader from its pack when it has no compiler or the source is empty.

**The shader page** (`ShaderEditorPage`, `editor.shaders`, markup `Data/Editor/ShaderPage.xml`) is
built on the GUI's code pages: the `.hlsl` with the shared HLSL colours
(`Data/Editor/Code/hlsl.xml`), one undo history and a problems list. The text is checked shortly
after each edit (`CheckShaderSource` on each stage's base variant); errors in the file show on their
lines, errors in an include in the list. A sphere beside the code draws with the shader being
edited, under a preview name of its own, its material laid out by the properties block; while the
code does not compile the preview keeps the last version that did. Save writes the file and the
asset and recooks.

## WGSL target (naga)

`WgslTranslator` cooks HLSL -> (DXC, vulkan1.1 target env + engine Standard binding shifts) -> SPIR-V ->
(naga) -> WGSL, at cook time on the dev/CI host only. naga was chosen over tint by a spike over the full
corpus. `--keep-coordinate-space` is load-bearing (the WebGPU clip-space unification - see
[[web-render-fixes-and-flip]]); tint validates the WGSL it writes unless turned off
(`SetValidateWithTint`). Export stages the WGSL pack for the Web platform, SPIR-V and DXIL for
Windows, and SPIR-V for the other desktops and Android (see `Documentation/Systems/export.md`).

## Hot reload

Dev hot reload is a portable POLLED stat-sweep, NOT native OS watchers: `NativeFileSystem::AsWatchable`
exposes a change source (no inotify/RDCW plumbing - deterministic + portable), and
`FileShaderSourceProvider::PollChanges` stat-sweeps every ~60th call (~1s at 60fps, throttled because
the sweep is O(files)). The render subsystem calls `m_shaders->PumpReloads()` each frame and idles the
GPU on a reload. Details of the resource-swap + PSO-invalidation plumbing are in
`Documentation/Systems/shaders-materials-hot-reload.md`.

---

The translator spike (tint vs naga comparison over the corpus) and the phase-by-phase history are in
`Documentation/Archive/shaders-design-history.md`.
