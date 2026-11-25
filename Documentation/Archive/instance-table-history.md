# A persistent instance table for meshes

> ARCHIVED 2026-11-25, merged to master. Steps 1 and 2 were built on branch `instance-table` and
> measured (Results): in this form the table does not pay on the CPU, and its GPU cost was not
> settled. The user merged it after playing the sample games on it without regressions, as the
> store a retained scene for meshes would build on. The commit before the merge is tagged
> `pre-instance-table` (93cf7e941), for bisecting a regression. Non-authoritative: the spec as
> approved and measured, kept as the record of what was built and why. Read on branch
> `rhi-handles` at `0ecd5188a`; line numbers will drift.

## Goal

A mesh's per-object data (world matrix, last frame's world matrix, tint) lives in one GPU buffer
that persists across frames. A record is written when it changes, once per frame at most, and
every pass of every view reads it by index.

This is the first step toward a GPU-driven renderer and is specified on its own: it stands
without the later steps (indirect draws, culling in compute, bindless materials), and none of
them is designed here. It is also the cheap test of that direction: what it nets is measured
before any larger step is specified, and the numbers decide whether, and which, comes next.

## What exists today

`MeshRenderer` writes per-object data inside `Resolve`, which runs per pass, per view, after
culling. There are two paths, chosen by run length (`MeshRendererImpl.cpp:1188`):

| Path | Run | Per-object data goes to | Shader variant |
|---|---|---|---|
| Single (`ResolveSingle`, `ResolveDepthSingle`) | 1 item | `GpuObject` (176 bytes) in `m_objectRing`, a dynamic-offset uniform slot per draw | not `INSTANCED`, reads `cbuffer Object` (`object_block.hlsli`) |
| Instanced (`ResolveInstanced`, `ResolveDepthInstanced`) | 2 or more items sharing mesh and materials | `GpuInstance` (144 bytes) in `m_instanceRing`, plus a 16 byte `GpuDrawOffsets` entry in `m_offsetsRing` | `INSTANCED`, reads `Instances[drawOffsets.x]` |

The two paths carry the same values. What `GpuObject` holds beyond `GpuInstance` (bone bases,
fade, pick id) travels in the offsets entry on the instanced path (`GpuDrawOffsets`,
`GpuPickDrawOffsets`).

What is rewritten every frame:

- **Every visible mesh, once per pass that draws it.** The camera depth prepass shares its fill
  with the forward pass (`m_instShareCache`). Each shadow cascade, each local-light tile, each
  probe face and each pick pass fills again. `InstanceSlotsPerFrame` sizes the rings for it:
  the larger of the camera's draws and the casters, times `2 + shadow passes + extra camera
  passes`.
- **Last frame's world matrix**, through `PrevWorldFor`: a per-scene pair of `Float4x4` arrays
  indexed by entity index (`MotionHistory`), written on every resolve and swapped in
  `FinishFrame`.

What already persists: a MultiMesh set (`MultiMeshSet`) keeps its own instance buffer and
uploads only when its version changes, and the bone pool is a GPU-only buffer filled by one copy
per frame (`UploadSkinning`). The bone pool copies into a region of its own each frame (the block
comes from a ring sized for the frames in flight), so it never writes what an earlier frame still
reads; the table writes the same slots every frame and needs a barrier instead (Updating it).

Plain `MeshRenderData` is produced in one place, `Engine.Render/ExtractImpl.cpp`
(`FillMeshRenderData`), once per mesh component per frame, with `entityId` (the entity's
generation in the high 32 bits, its index in the low) and `scene` set. A multi-material mesh whose
slots draw in several categories is extracted as up to five items (`SplitMeshByCategory`,
`Extract.cppm`): copies with the same world and tint, each drawing its own category's submeshes.

`PrevWorldFor` indexes its history by the entity index alone, so an entity that takes a freed
index reads the old entity's matrix as its previous one on its first frame, and its motion
vectors smear for that frame.
Vegetation and mesh particles produce `MultiMeshRenderData`, which this proposal does not touch.

The backlog's renderer list already names this cost: item 1b, "shadow instance reuse across
cascades", records `shadow.resolve` at 8.96 ms with each of the 4 cascades filling its own
instance data, and item 1c is the same de-duplication across depth, forward and shadow
(`Documentation/Plans/weekly_backlog.md:1143-1147`). That figure is the backlog's, from
2026-10; it was not measured again.

## The design

### The table

One `InstanceTable`, owned by `MeshRenderer`, in its own file in `Code/Foundation/Render`:

- **A GPU-only storage buffer** of `GpuInstance` records, bound whole at set 1 as `Instances`,
  where the instance ring is bound today. The record layout does not change, so the `INSTANCED`
  shaders and the MultiMesh buffers are untouched.
- **A CPU mirror** of the same records.
- **A slot per entity**, found by `(scene serial, entity index)`: per scene, a flat array
  indexed by entity index, the lookup `PrevWorldFor` uses today, whose entry holds the slot
  number and the entity's generation. An entry whose generation is not the item's is another
  entity's: the item takes a new slot. Freed slots go on a free list.
- The items one entity is split into share its slot: they carry the same world and tint, so the
  second finds the slot and its compare finds nothing to write.

### Updating it

A new `MeshRenderer::UploadInstances(scenes, encoder)` runs once per frame, beside
`UploadSkinning`, before any pass resolves. For each plain mesh item of each extracted scene:

1. Find or assign the item's slot (a new slot when the entry is empty or holds another
   generation).
2. Build the wanted record: `world` and `tint` from the item, `prevWorld` from the mirror's
   `world` (a new slot takes `world`, so nothing smears on its first frame, a reused entity index
   included).
3. Compare it with the mirror. If it differs, write the mirror and mark the slot changed.

A mesh that moved is written on the frame it moves and once more on the next, when its
`prevWorld` catches up. A mesh at rest costs one compare and no upload. This replaces
`MotionHistory`: the mirror is the history.

The changed records are copied into a staging ring (a region per frame in flight, as every
CPU-written ring), and the encoder copies them into the table before the first pass, where
`UploadSkinning` copies bones. Adjacent changed slots merge into one copy; above a threshold of
changed slots, one copy covers the whole used range.

The table itself has no region per frame in flight: every frame copies into the slots the frame
before read. So the copy sits between two transitions, shader read to copy destination before it
(an earlier frame's passes may still be reading on the GPU) and copy destination to shader read
after. A frame with nothing changed records neither.

A slot is freed when its entity's scene was extracted this frame and the entity was not
(destroyed, hidden, inactive). A scene not extracted this frame (a hidden editor page) keeps its
slots untouched; one not extracted for a while drops its slot array and frees its slots, as its
motion history is dropped today. An entity index reused by a new entity takes a new slot by its
generation.

A mesh item with no entity (`entityId` 0) takes a slot for the frame only, with `prevWorld`
equal to `world`.

Growth doubles the buffer, uploads the whole mirror and bumps the bind group generation
(`m_instanceBGGen`).

### Drawing from it

- **Instanced runs** write only their offsets entries: `drawOffsets.x` is the mesh's table
  slot in place of an instance ring index. `m_instanceRing` goes.
- **Single draws go through the instanced path** as a run of one. `ResolveSingle`,
  `ResolveDepthSingle`, `m_objectRing`, `GpuObject` and `object_block.hlsli` go, and the mesh
  renderer stops using the non-instanced variants of the forward, depth and pick shaders. A
  mesh with material overrides still draws alone, with its own material instance per slot.
- **The offsets ring stays** per pass: 16 bytes per drawn instance (slot, bone bases, fade or
  pick id). Bone bases are assigned per frame, and fade and pick id are per pass layouts today.

### What does not change

- Culling, sorting, run building and draw counts. A frame issues the same draws.
- MultiMesh sets, the bone pool, materials, the pipeline's `EmitDraw`.
- The RHI. The table uses buffers, copies and bindings every backend already has.
- `RenderData` and extraction.

## What it buys

| | Today | With the table |
|---|---|---|
| Bytes written per visible mesh, per pass | 160 (instanced) or a uniform slot (single) | 16 |
| Bytes written per mesh, per frame | none outside passes | 144 when it changed, else none |
| Previous world matrix | two `Float4x4` copies per resolve | the mirror, no copy |
| Mesh paths in `MeshRenderer` | single and instanced, for forward and depth | instanced |

Two costs move the other way:

- **Every extracted mesh is compared every frame**, visible or not. Today a culled mesh costs
  the renderer nothing.
- **A single draw binds an offsets vertex stream** in place of a dynamic-offset uniform.

Neither was measured.

Two behaviours change, by reading the code: today a mesh's history is written only on frames it
is drawn, so a mesh coming into view can read a stale previous matrix; with the table every
extracted mesh keeps a true one. And an entity that reuses a freed index no longer reads the old
entity's matrix on its first frame.

A smaller fix for the measured cost exists: backlog item 1b, building the casters' instance data
once and giving each cascade an offsets list. It would cover `shadow.resolve` without a persistent
table. The table is proposed instead because it also removes the per-pass fills of depth, forward,
probes and pick, the motion history and the single path, and it is the record a GPU-driven
renderer reads.

## To decide

1. **Single draws through the instanced path** (proposed above), or keep the single path with
   a ring that holds only a table index. The first removes two resolve functions, a ring, a
   struct and a shader include; the second keeps the non-instanced shader variants in use.
   Recommended: the first.
2. **Fade and pick id in the record.** They are per object and would leave one offsets layout
   for all passes (slot and bone bases), removing `GpuPickDrawOffsets`. It grows the record to
   160 bytes, and MultiMesh sets share the record layout, so their buffers and the pick pass's
   set ids change too. Recommended: not in this work; raise it with culling in compute, which
   wants bounds in the record as well.
3. **Whether the non-instanced variants are still cooked.** No other renderer uses them: the
   per-object block is reached only through `forward_vs.hlsli`, `shadow_depth_vs.hlsli` and
   `pick_ids_vs.hlsli`, which the mesh renderer's forward, depth and pick shaders and every
   surface shader include (the RHI samples' `Object` blocks are their own). But custom shaders
   declare `INSTANCED` as a variant in their header (`variants(vertex): SKINNED INSTANCED`:
   SkyHopper's and PaperKid's `Toon.hlsl`, Shader Gallery's 10 and 11, the CustomShader sample),
   so dropping the non-instanced build changes what that header means, and the shipping shader
   documentation with it. Recommended: not in this work; a step of its own after it, with the
   format change, its documentation and the samples' headers.

## Order of work

Each step leaves the tree green.

1. **`InstanceTable`**: slots, mirror, compare, staging, copy and its two transitions. Tests in
   `Render.Tests` on the Null device: an unchanged record uploads nothing and records no
   transition; a moved one uploads on two frames; a freed slot is reused; an entity that reuses
   an index with a new generation takes a new slot with `prevWorld` equal to `world`; the items
   one entity is split into share a slot; two scenes with the same entity index stay apart; a
   scene not extracted this frame keeps its slots; growth keeps every record; an item with no
   entity gets a slot for one frame.
2. **Instanced runs read the table.** `UploadInstances` joins the frame; `ResolveInstanced` and
   `ResolveDepthInstanced` write offsets only; `m_instanceRing`, `MotionHistory` and
   `PrevWorldFor` go. `InstanceSlotsPerFrame` sizes the offsets ring alone.
3. **Single draws join** (decision 1). The single resolve functions, `m_objectRing`, the object
   layout and bind group, the single-draw shadow layout and `GpuObject` go, and with them the
   `instanced` flag the mesh renderer's helpers took; `Integration.ShaderBlocks` loses its
   `Object` rows. `object_block.hlsli` stays while the non-instanced variants are cooked
   (decision 3's own step): those variants include it, though nothing draws with them.
4. **Measure and write up**: what the table netted, against the baseline (Measuring), in the
   weekly and `Documentation/Systems/renderer.md`; backlog items 1b and 1c closed or restated by
   what the numbers show. The write-up ends with a recommendation on the next GPU-driven step,
   for the user to decide; nothing further is specified before it.

## Measuring

The numbers are this work's result, so the baseline comes first: measured on the commit before
step 1, with the same scenes, settings and machine as every later measurement, and recorded
before any code changes.

In `build/clang-reldbg` (Debug on Vulkan says nothing about speed), before step 1 and after
steps 2 and 3, each scene, with `RenderStressTest`'s measuring mode (its sun casts 4 cascades;
`--measure` warms up 120 frames, then averages the frames asked for from the profiler and exits):

- many static meshes, where the table should show most:
  `RenderStressTest --spheres 32000 --measure 600`
- the same with every mesh moving each frame, its worst case: `... --spheres 32000 --bob --measure 600`
- few meshes, each drawn alone, to see what a run of one costs against the single path:
  `... --spheres 200 --unique --measure 600`
- many meshes, each drawn alone, the single path at scale (step 3's case):
  `... --spheres 8000 --unique --measure 600`

What is read, from the profiler, averaged over a fixed run of frames after warm-up:

- the frame's CPU time, and the render thread's share of it
- `shadow.resolve` and the depth and forward resolves (the per-pass fills the table removes)
- the new upload's own cost (`UploadInstances`: the compare and the copies), so a saving in the
  resolves is not paid back there
- bytes written per frame to the instance and offsets rings and the table's staging, from the
  renderer's counters (added in step 1 if missing)
- GPU frame time, to show the table's reads cost nothing visible on the GPU side

Reported as a table per scene: baseline, after step 2, after step 3, with the change in each. The
forest benchmark project is not the scene to use. Step 2's numbers alone say whether the table
pays; step 3 is judged on its own (it removes code, and may cost a little on the few-meshes
scene).

## Risks

- **A read of the table before its copy.** Every pass that draws meshes must record after
  `UploadInstances`. The bone pool has the same rule and the same place in the frame.
- **A copy over a read in flight.** The table is one region written every frame, so a missing
  transition before the copy lets it overwrite records an earlier frame's passes still read,
  which shows only as a mesh drawn with another frame's matrix, now and then. The engine does
  not enable Vulkan's synchronization validation (a search for it found nothing), so nothing
  reports it today; step 1's test that the transitions are recorded is the guard, and turning
  synchronization validation on for a run of the backend suites is worth doing once.
- **Slot lifetime across scenes.** Two editor pages and a play scene reuse entity indices; the
  slot arrays are per scene serial, as the motion history is. The two-scene test in step 1 pins
  it.
- **Tests tied to the removed pieces.** A search found two: `PipelineTests.cpp:2203-2208` pins
  `InstanceSlotsPerFrame`, and `Integration.ShaderBlocks` checks `GpuObject` against the
  compiled shaders. A test that expects a lone mesh to draw without the instanced flag would
  also change; the search for one was by name only, so the first run of the suites decides.
  Draw counts themselves do not change.
- **Many small copies.** A scene where scattered meshes move gives many short runs. The
  threshold that falls back to one whole copy bounds it; where to set it comes from the
  measurements.
- **The branch.** `rhi-handles` is changing how the renderer names RHI objects. This work
  starts after that lands, on its result.

## Results (2026-11-23)

Measured with `--measure` (RelWithDebInfo, Vulkan, the RTX 2060 box): `RenderStressTest` at 32000
and 100000 spheres, at rest and all moving, and the Megascans forest's 40000-entity Canopy scene.
The baseline is master before step 1 with the measuring commits applied.

**Method.** Only the paired run counts: baseline and table built and measured back to back in one
session. Earlier comparisons put a baseline measured in one hour against the table measured in
another, and the same baseline differed by 15 to 20% between them in scopes neither build changed
(extraction, update, the GPU frame); the gains they showed (21% at 32000 spheres, 8% of the
forest's GPU time) were that drift. Even the paired run measured the table second; runs
interleaved A, B, A, B are what settle the GPU difference below.

| Scene | Render thread work | Resolves | Table upload | GPU frame |
|---|---|---|---|---|
| 32000 at rest | 9.53 to 9.48 ms | 3.24 to 1.88 ms | 0.70 ms | 7.68 to 8.57 ms |
| 32000 moving | 10.25 to 10.86 ms | 3.30 to 1.94 ms | 1.39 ms | 7.38 to 8.50 ms |
| 100000 at rest | 32.68 to 32.24 ms | 10.21 to 7.07 ms | 3.13 ms | 16.16 to 19.45 ms |
| 100000 moving | 34.72 to 37.44 ms | 11.53 to 8.01 ms | 5.16 ms | 15.78 to 19.69 ms |

- **The resolves save what was expected**, 1.4 to 3.5 ms: every pass writes 16 bytes per instance
  in place of 144. These scopes are steady across every run.
- **The table's upload gives it back.** At rest it is the compare of every record each frame,
  about 22 ns a mesh at 32000 and 31 ns at 100000 (cache misses on the slot entries and the
  mirror). Moving, it is also 14.4 MB a frame staged and copied at 100000. Its parts at 100000
  moving: claim 1.76 ms (serial), refresh 1.62 ms (parallel), staging and copy 1.27 ms, mark
  0.39 ms. The workers' summed time also rose, 21.7 to 36.7 ms at 32000 moving: the parallel
  refresh costs CPU across threads.
- **Net**: level at rest, 6 to 8% worse moving.
- **GPU**: 0.9 to 3.9 ms worse with the table in every paired scene, the depth and shadow passes
  most. A run's instances are no longer contiguous in memory but scattered table slots, which fits;
  the table also ran second, on a warmer machine. Not settled.
- **Unique materials** (200 and 8000 spheres, each its own material): no change from step 3. Their
  10 ms of resolves at 8000 is per-draw material and pipeline work, which both paths do.
- **The forest**: 141 to 148 ms a frame in both builds, GPU-bound. Its pipeline statistics say why:
  82 million primitives through the forward and 37 million through the shadow cascades a frame,
  for 24 million fragments. Its 40000 entities draw in 82 draws, so batching already works; the
  cost is triangles, which LOD and culling would cut, not per-object data.

**What would change the answer**: claiming slots in parallel too (only new entities need the
serial path), assigning slots so a batch's instances sit together (the GPU's reads and the copy's
runs), and a cheaper rest check than comparing each mesh's world against its record (a transform
version from extraction would let a mesh at rest cost nothing). Each is measurable with the same
scenes; none is built.

**Recommendation.** Not to merge the table as it is. The measurements point the next renderer work
elsewhere: the forest's triangle load (LOD, then culling on the GPU) and, for scenes of many unique
materials, the per-draw material and pipeline work. Step 3 (one draw path) is a simplification
worth having on its own, so it went to master over the instance ring instead of the table: a lone
mesh writes its record there as an instanced run does. The measuring mode and the report's counters
are on master too, so this branch holds only the table.

## Not in this work

Indirect draws, real `firstIndex` and `baseVertex`, culling or LOD selection in compute,
bindless materials, a compact 3x4 matrix record (backlog item 4), and the terrain, sprite,
particle, decal and GUI renderers.
