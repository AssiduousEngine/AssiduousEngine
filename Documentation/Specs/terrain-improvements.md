# Terrain improvements (tiling, steep faces, LOD transitions, authoring)

> STATUS: PROPOSED 2026-11-14, not scheduled. Sized L: T0 to T6 below, each green on its own.
> Origin: an outside assessment of our terrain against another terrain system listed 17 gaps. On
> 2026-11-14 every one was rechecked against the tree and none had been addressed (table under
> "What exists"). This spec takes the gaps a player or an artist would notice and that fit the
> terrain we have, and names the ones it leaves out and why. Every design below is a proposal;
> every statement under "What exists" was read in code on that date. Nothing here was run.
> Decisions are language-neutral so the port can follow them one for one. Read CONVENTIONS.md
> first.

## Goal

A painted terrain that does not show its texture grid, whose cliffs are not smeared, whose detail
levels change without a visible step, and that an artist can inspect and paint with more than one
brush.

Not goals, with what would reopen each (see "Left out"): streamed regions, a clipmap mesh, float
heights, procedural ground past the edge, camera-following collision.

## What exists (checked 2026-11-14)

- **Pixel stage** (`Data/Engine/Shaders/terrain.ps.hlsl`, 393 lines). Up to four palette layers per
  splat texel plus a base layer, blended by hand over the 2x2 splat neighbourhood (`:214-354`).
  Every layer samples at one scale in terrain-local XZ: `i.localXZ / TileScales[layer]`
  (`:289-290, 310-311`), the base at `BaseTileScale` (`:202`). Palette taps use `SampleGrad` with
  the derivatives of `localXZ` taken once outside the data-dependent branches, because WGSL
  forbids implicit derivatives in non-uniform control flow (`:217-221`). Height blend and
  coverage masks are in place (`:240-347`). With no material the colour is a two-colour ramp by
  height and slope (`:195-198`).
- **Vertex stage** (`terrain.vs.hlsl`, 88 lines). One 65 by 65 grid per chunk, heights read with
  an integer `Load` from an R16 texture (`:52-56`), normals by central differences (`:79-85`), and
  a skirt ring that drops by `SkirtDepth` (`:62-66`). No morph term. `terrain_depth.vs.hlsl` and
  `terrain_pick.vs.hlsl` place the same vertices and must stay bit-identical in clip position.
- **Mesh and LOD** (`Code/Foundation/Terrain/Terrain.cppm`). `kChunkQuads = 64` (`:32`), per-chunk
  discrete LOD from the shared screen coverage metric (`:327-339`), skirt walls built per LOD
  (`:127-149`).
- **Layer data** (`Code/Foundation/Terrain.Resource/TerrainResource.cppm:164-173`). A layer has its
  maps and one `tileScale`. The GPU gets the scales as `StructuredBuffer<float> TileScales`
  (`terrain.ps.hlsl:86`).
- **Heights** are `u16` over `[minY, maxY]` (`Code/Foundation/Heightfield/Heightfield.cppm:28`).
- **Brushes** (`Code/Editor/Editor.Terrain/Sculpt.cppm:37-42`, `Heightfield.cppm:506-540`): raise,
  smooth and flatten, with one cosine falloff. The brush core already takes the weight per sample
  from one place (`:540`).
- **Holes**: an R8 mask sampled in the pixel stage plus per-chunk index buffers for cut quads
  (`terrain.ps.hlsl:63-73`, `Engine.Terrain/HoledChunkMesh.cppm`). Collision and the navigation
  bake see the same holes.
- **Navigation**: the surface is static geometry for the bake
  (`Engine.Terrain/TerrainComponentsImpl.cpp:25`), with no per-sample mask.
- **Pick pass**: `terrain_pick` writes the terrain's entity id for selection
  (`Engine.Terrain/TerrainRenderer.cppm:794-813`). It is not a depth readback for the brush.
- **Tests**: `Engine.Terrain.Backend.Tests/TerrainPixelProbeTests.cpp` renders and reads pixels
  back on the GPU backends. This is where every phase below proves itself.

Status of the 17 assessed gaps, by search and by reading the shaders:

| Gap | Today | Here |
|---|---|---|
| Texture grid visible (no detiling) | open | T1 |
| Stretched textures on steep faces | open | T2 |
| Far-field repetition (one scale per layer) | open | T3 |
| No macro colour variation | open | T3 |
| No per-texel rotation or scale, no mip bias knob | open | T3 (mip bias only) |
| LOD steps visible (no geomorphing) | open | T4 |
| Unpainted ground is a flat colour ramp | open | T5 |
| No debug views in the editor | open | T0 |
| One brush shape | open | T6 |
| Brush does not pick the surface as drawn | open | T6 (check first) |
| No per-sample navigation mask | open | T6 |
| No channel packing tool | open | T6 |
| One fixed grid, whole-asset load and save | open | left out |
| Chunk array and quadtree, not a clipmap | open | left out |
| `u16` heights | open | left out |
| World ends at the terrain's edge | open | left out |
| One collision shape over the whole terrain | open | left out |

The assessment also called our hole handling heavy. It stays: it is what makes holes correct for
collision and navigation.

## Decisions

1. **Per-layer parameters become one struct buffer.** `TileScales` grows into `LayerParams`
   (tile scale, detile strength, steep projection on or off, far scale, far start, far end), one
   entry per palette layer and one for the base. New per-layer values go here and nowhere else;
   terrain-wide values (macro variation, mip bias, morph range) go in the terrain's own block.
   No value rides in a pad slot of an unrelated block.
2. **Every new sampling path is opt-in per layer and gated by the quality level.** The worst case
   today is already 4 corners by 4 slots by 4 maps. Detiling and the far scale each double a
   layer's taps and projection can double them again, so a layer pays only for what its author
   turned on, and Low turns all three off.
3. **All taps stay `SampleGrad` with derivatives hoisted.** Each new path computes its own UV and
   its own gradients from `ddx`/`ddy` taken in uniform control flow, exactly as the existing taps
   do. This is what keeps the WebGPU backend valid.
4. **Geomorphing is per vertex, by distance, and the same in every pass.** A morph factor per chunk
   cannot close the seam between two chunks at different levels. The colour, depth, pick and shadow
   vertex stages share one function and one camera position (the main view's, also in the shadow
   passes) so their clip positions agree. Skirts stay as the safety net.
5. **Data versions move with the data.** New layer fields bump the terrain asset's version with a
   migration that reads old assets as "everything off", so an old terrain draws as it does today.

## Phases

### T0: groundwork and debug views

- `LayerParams` buffer replacing `TileScales` (decision 1); the base layer gets an entry.
- The vertex stage passes the terrain-local position (three components) and the local normal, which
  T2 needs.
- A debug view selector on the terrain renderer, as a shader variant so the shipping variant pays
  nothing: height, slope, geometric normal, splat weight per slot, palette index, layer tile cells,
  hole mask, chunk LOD (and the morph factor after T4), sampled mip level.
- Editor: a view dropdown on the terrain page. MCP: the same selector, so a screenshot of a view can
  be asked for.
- Tests: one pixel probe per view on a small fixed terrain.

The views come first because every later phase is easier to prove, and to hand over for a look,
with them.

### T1: detiling

Per layer, when its detile strength is above zero: the layer is sampled twice, with two different
offsets (and rotations) chosen per tile cell from a hash of the cell's integer position, and the two
are blended by a smooth low-frequency index so no cell border shows. The gradients are rotated with
the UV so anisotropic filtering stays right, and the tangent-space normal is rotated back by the
same angle before it joins the blend. Albedo, normal, ORM, height and mask of one layer all use the
same two placements, so the height blend still agrees with the albedo.

- Files: `terrain.ps.hlsl`, the layer struct and its GPU upload, the layer inspector.
- Tests: a probe on a layer whose texture has one marked corner: with detiling off the mark repeats
  on the grid, with it on the grid period is gone; a normal-mapped layer keeps its lit direction
  under rotation.
- Gotcha: a single rotated tap per cell would be cheaper and shows a seam at every cell edge. Do not
  ship that form.

### T2: steep faces

Per layer, when projection is on: where the local normal's Y falls below a threshold, the layer is
sampled on the vertical plane that faces the slope (local XY or ZY, chosen by the larger horizontal
normal component) and crossfaded with the top-down sample over a band, so there is no hard switch.
The tangent frame for the normal map follows the plane used.

- Files: `terrain.ps.hlsl`, layer struct, inspector.
- Tests: a probe on a near-vertical ramp: the texel density along the slope matches the density on
  flat ground within a tolerance, where today it stretches by the slope.
- Gotcha: inside the crossfade band the layer costs two taps per map. The band is narrow; keep it a
  terrain-wide value.

### T3: distance scale, macro variation, mip bias

- **Far scale** per layer: a second, coarser tile scale crossfaded in between a start and an end
  view distance. Outside the band it is one tap.
- **Macro variation**, terrain-wide: two octaves of low-frequency noise tint the blended albedo,
  faded out on steep ground. Strength and scale are terrain values. The noise is a small engine
  texture, so all backends agree to the bit.
- **Mip bias**, terrain-wide: one value applied to the hoisted gradients.
- Tests: probes at a near and a far camera for the far scale; a probe that the tint averages to no
  change over a large flat area.

Per-texel rotation and scale painted into the splat data is not taken: it needs bits in the splat
format, and T1 removes most of the reason for it.

### T4: geomorphing

Vertices that exist at a chunk's level but not at the next coarser one slide toward the coarser
grid's position as their distance from the camera approaches the far end of the level's range, so
the level change itself moves nothing. Heights and normals morph together.

- LOD selection moves from "screen coverage of the chunk's sphere" to distance ranges derived from
  the same thresholds, so a vertex's morph depends only on its own distance and both sides of a
  chunk border agree.
- One shared vertex function for `terrain.vs`, `terrain_depth.vs` and `terrain_pick.vs`
  (decision 4).
- Motion vectors: the previous position uses the previous frame's camera for its morph, or the
  morph shows up as false motion under TAA.
- Tests: a CPU model of the morph in `Terrain.Tests` (a vertex at the range's end equals the
  coarser level's vertex exactly; a border vertex gets the same position from both chunks), and a
  pixel probe that depth and colour agree.
- Open question: vegetation and physics sit on the full-resolution surface while distant morphed
  ground is coarser. This is already true of discrete LOD; confirm nothing floats visibly sooner
  than it does today.

### T5: ground rules for unpainted texels

A terrain material may name a low layer, a steep layer, a slope threshold and band, and an optional
height window. Where no paint covers a texel, the base is chosen by those rules from real palette
layers, in place of the two-colour ramp. Painted texels are untouched. With no rules set, the ramp
stays.

- Tests: probes on flat and steep unpainted ground with and without rules.

### T6: authoring

- **Brush shapes.** A brush takes an optional mask image (R8) in place of the cosine falloff, with
  rotation and an option to turn with the stroke. The brush core already draws its weight from one
  place, so this is a second weight source, not a second brush. A small set of masks ships under
  `Data/Editor`; check each one's licence first.
- **Brush picking.** First read how the brush finds its point today and whether it can disagree
  with the drawn surface (holes, skirts, morphed ground after T4). Add a depth readback only if it
  does.
- **Navigation mask.** A paintable no-walk plane beside the holes plane, honoured by
  `CollectStaticGeometry`, with a brush mode and a debug view.
- **Channel packing.** A pipeline step and an MCP tool that build a layer's ORM (and optionally
  albedo plus height) from separate greyscale images, so an artist does not need an outside tool.
- Tests: brush core tests in `Heightfield.Tests` and `Terrain.Resource.Tests`; a bake test that a
  masked area yields no navigation polygons; a pipeline test for the packer.

## Order and size

T0, then T1 and T2 (the two a player notices first and that need no change outside the pixel stage
and layer data), then T3, T4, T5, T6. T4 is the largest single phase because it changes LOD
selection and three vertex stages. T6's four items are independent and can land in any order.

## Left out, and what would reopen each

| Item | Why not now | Reopen when |
|---|---|---|
| Streamed regions, one file each | Changes the asset, the editor page, save and undo, collision and the bake at once | A game needs a world larger than one heightfield holds in memory |
| Clipmap mesh | Constant mesh cost matters at world sizes we do not have; T4 gives the visible benefit | Regions land |
| Float heights | `u16` over the authored range is about 3 cm over 2 km | Smooth brushes show stepping, or a terrain needs its range changed after authoring |
| Procedural ground past the edge | Only useful with a far horizon and no authored data | An open-world sample is planned |
| Camera-following collision | One shape is right at one heightfield's size | Regions land |

## Acceptance

- Each phase lands with its tests green on the GPU backends the probe suite runs, and old terrain
  assets load and draw unchanged (decision 5).
- Low quality turns T1 to T3 off and costs what it costs today.
- The sample that uses terrain (Snowline) is reopened and resaved, and its look is handed to the
  user to check with these, per phase: grass and snow fields from above for a grid (T1); the cliff
  faces for stretching and for a hard line where projection starts (T2); a slow flight toward and
  away from the ground for steps or swimming (T3, T4); a fresh terrain before any paint (T5).
- `CHANGELOG.md` gets a line per phase a user would notice; the MCP guide follows any tool change.

## Gotchas

- WGSL rejects an implicit-derivative sample in non-uniform control flow. Any new tap that is not
  `SampleGrad` with hoisted gradients will pass on Vulkan and DX12 and fail on the web.
- The depth prepass and the colour pass must produce bit-identical clip positions. T4 is the one
  phase that can break this.
- Shadow passes cull and select LOD for their own view. With T4 they must morph by the main
  camera, or shadows crawl as the light's view moves.
- A rotated normal map (T1) or a re-projected one (T2) needs its tangent frame rotated to match;
  forgetting it flips the lit side of bumps without changing the albedo, which a colour-only probe
  will not catch.
- A layer buffer change is a change to a shader block and its C++ mirror together.
