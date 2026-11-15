# Reflection Probes

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[reflection-probes-plan]]

Local, parallax-corrected, blended cubemap reflections: per-region probes replace the single global IBL
specular so a reflective surface reflects ITS surroundings and the reflection tracks geometry as the
camera moves. The probe core shipped + user-verified; cluster/froxel probe selection is the one deferred
piece (the forward loops the probe buffer directly today).

## Shipped core

- **`ReflectionProbeComponent`** (`engine.render`, data version 1) - a box-proxy probe
  (`halfExtents`) with a blend-distance falloff for influence weighting, plus `intensity`,
  `priority`, `update` (`ProbeUpdateMode` Static / Realtime / Manual), `parallax` (box-project the
  reflection ray, or sample it as an infinite environment) and `enabled`; parallax is a ray-vs-box
  against the proxy. The box is world-axis-aligned. The component has no resolution of its own (a
  version 0 probe's is read and dropped): every probe shares one.
- **`ReflectionProbeSystem`** (`foundation.render:probes`) - runtime capture from the live scene +
  static caching: a probe is captured when it takes a slot, when its transform or box changes, every
  frame while Realtime, and when a script asks (`ReflectionProbeComponent.of(entity).capture()`
  counts a request; a count the system has not seen recaptures the probe once in any mode, so a
  Manual probe captures only when asked after its first, and a Static one can refresh after its room
  changed). Captures run one probe a frame, round-robin across the dirty probes. A capture renders
  the probe's own scene, lit by that scene's IBL context.
- **Per-probe prefilter**: the system blits a probe's six captured faces into a prefiltered cube
  array (mip 0, flipping the RH look-at mirror) and GGX-convolves mips 1..4 with its own prefilter
  pass (`probe_prefilter_ps`; `kPrefilterMips` 5, roughness -> mip via `m/(mips-1)`; the forward
  samples `R` at `roughness * 4`). Both cube arrays are RGBA16F with `kMaxReflectionProbes` (16)
  slices of six faces, at one resolution for every probe: 128 per face, 64 at a quality level whose
  environment resolution is Low (`SetResolution` remakes the arrays and recaptures every probe).
  Persistent probe slots, keyed by the probe's scene and entity; when all 16 are taken, a new probe
  takes the slot seen longest ago that no scene assigned this frame. A resize retires the old arrays
  and the per-face capture bind groups through the frames-in-flight retire queue.
- **Multi-scene**: each extracted scene's probes are a record range in the frame's probe buffer
  (`RangeFor`); a view's `ProbeBase`/`ProbeCount` pick its scene's records.
- **Multi-probe blend**: the forward blends the influencing probes by their box falloff weights,
  parallax-correcting each, then fades to the global IBL by the total weight at the box edges -
  beyond Sedulous (which has probes but no parallax, no blending, just
  per-camera nearest-probe with a CPU IBL view-swap).

## Deferred

- **Cluster/froxel probe selection.** The locked design was to bin probes into froxels via the
  `ClusterSystem` `SphereVsAABB` machinery so the forward reads only its cluster's probes; what shipped
  instead is a direct forward path that LOOPS the probe buffer. Cluster assignment is deferred (a scale
  optimization, not a blocker at current probe counts).
- **SH9 diffuse per-probe** (per-probe irradiance, not just specular), an **OBB proxy** (oriented box,
  vs today's AABB-aligned box), and a **capture budget** beyond the one-probe-a-frame cap (filter the
  capture draw list). All noted + deferred with the user.

---

The design rationale (the locked capture/selection/proxy decisions and the froxel-selection design) is
in `Documentation/Archive/reflection-probes-design-history.md`.
