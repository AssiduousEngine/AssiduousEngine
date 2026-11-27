# Particles / VFX

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[particles-plan]]

A data-driven particle system drawn by its own billboard renderer, the instanced-mesh path and the
light list. The CPU simulator + all behavior modules + render integration shipped; the GPU-compute simulator is a wired seam
whose implementation is deferred. Authoring + the cooked runtime resource: `particles-authoring.md`.

## Modules

Particles are an official plugin, `particles` (`Code/Plugins/Particles`), which a project enables in
its `plugins` list; projects written before it left the engine enable it on reading. Its code is in
the `particles` namespace.

- **`particles.foundation`** (`Particles.Foundation`) - the runtime, in partitions: `:effect`
  (`ParticleEffect`, `ParticleSystem`, `ParticleEmitter`, `ParticleEffectInstance`; the sim driver +
  mode resolution), `:modules` (the initializer and behavior modules, `CPUSimulator`), `:streams`
  (SoA particle data, `ParticleStreamId`), `:types` (enums, ranges, curves, emission shapes).
- **`particles.foundation:resource`** (`Particles.Foundation`) - the cooked `ParticleEffectResource`
  (see authoring doc).
- **`particles.pipeline`** (`Particles.Pipeline`, the Pipeline part) - `ParticleEffectAsset` + builder.
- **`particles.engine`** (`Particles.Engine`, the Engine part) - `ParticleEffectComponent` +
  `ParticleEffectComponentManager` (the scene system that ticks every instance and extracts render
  data), the `ParticleRenderer` (billboards and trails), and `ParticleSubsystem`, which injects the
  manager into every scene and registers the renderer and itself as a render-data provider with
  `RenderSubsystem`, taking both back when the plugin unloads.
- **`particles.editor`** (`Particles.Editor`, the Editor part) - the effect's editor page (authoring
  doc) and the effects' thumbnails.

## Runtime (shipped)

An effect is a list of systems. Each system has an emitter (spawn timing only: a continuous rate or
bursts), initializers that run once per spawned particle (position, velocity, lifetime, color, size,
rotation, mesh orientation) and behaviors that run every frame over the live particles (gravity,
drag, wind, turbulence, vortex, attractor, radial force, collision, and color / alpha / size /
rotation / speed over lifetime), over an SoA particle stream. Velocity integration and aging are a
fixed final step, not a module. Over-lifetime curves are Core's curve keys and gradients (8 keys or
stops each), so the editor draws what plays. A system can spawn into another system through
sub-emitters, on a particle's birth or death; the instance routes those events. A moving emitter
passes its velocity on over the step, a spawned one-shot bursts where its entity is, and alpha-blended
systems that ask for it are sorted back to front from the primary camera.

A system's `ParticleRenderMode` picks how it draws:

- **Billboards** (billboard, stretched, horizontal, vertical): the dedicated `ParticleRenderer`, one
  `ParticleBillboardRenderData` batch per (system, texture, blend), so a whole system is one
  instanced draw and the particle count never reaches the draw-list sort.
- **Trail**: camera-facing ribbons (`ParticleTrailRenderData`), drawn by the same renderer.
- **Mesh**: a `MultiMeshRenderData` drawn by the shared instanced mesh renderer (`instanced-mesh.md`),
  with the component's mesh winning over the effect's and a material per submesh.
- **Light**: a point light per particle (capped, its intensity fading with the particle's alpha)
  added to the clustered light list, beside the billboard glow.

Effects reference their cooked resource via a hot-reload `Proxy`. Scripts reach a component with
`ParticleEffectComponent.of(entity)` and drive its instance through `scene.particles` (`play`,
`stop`, `restart`, `pause`, `isPlaying`, `setEffect`).

## The CPU/GPU seam

Sedulous shipped the seam but stubbed the GPU simulator; the seam is ported: `SimulationMode` (CPU / GPU
/ Auto) with per-module `BehaviorSupport` (CPUOnly / GPUOnly / Both), and `ResolveSimulationMode`
picking a system's mode (Auto -> GPU when all its behaviors support it and its max particles exceed
1024, else CPU). The CPU path is fully implemented behind this seam.

## Deferred

- **The GPU-compute simulator.** Only the seam exists (mode resolution + per-module support flags);
  there is no compute-shader simulator (no particle `numthreads` shaders, no `GPUSimulator` dispatch).
  The plan (Godot/Flax-style): a per-effect compute sim writing the existing `InstanceData` buffer +
  an indirect draw keyed on the GPU particle counter. CPU is fine for typical emitters; GPU is the
  ceiling-breaker.

---

The 8-engine architecture survey (ezEngine / Sedulous / Flax / Lumix / ogre-next / Godot 4 / PlayCanvas /
Babylon.js) and the recommended-plan writeup are in `Documentation/Archive/particles-design-history.md`.
