# Prefabs

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[prefabs-plan]]

Prefabs are reusable entity subgraphs in the scene serialization format, with derived (not tracked)
per-instance overrides and nesting. Shipped end to end and user-confirmed. The spawner lives at
the scene level (`foundation.scene` / `foundation.scene.resource`), NOT editor-only, because gameplay
spawns from prefabs and scene load reconstructs instances.

## Asset + payload

A prefab is a content-DB instance whose primary is a `PrefabDocument` (it carries the name) and whose
`scene` data stream is a serialized entity subgraph in the SAME record format as scenes (one
serializer to maintain, shared with the editor clipboard subtree). The type is distinct from
`SceneDocument` so pages, creators and pickers tell prefabs from scenes, but the stream is
identical, so the scene editor page opens prefabs unchanged. A prefab has exactly one root:
`SavePrefab` refuses a multi-root layout.

## Prefabs as resources

At run time a prefab is a resource. `PrefabFactory` (in the scene resource module, which the
`prefabs` domain brings) loads the `PrefabDocument` record's scene stream into a `PrefabResource`:
its id, its name and the payload bytes, reached only through `OpenPayload`. The record is authored,
never cooked (`ReadsAuthoredRecord`), so a database layering cooked content over sources lets the
sources answer for prefabs; a record of any other type is refused, so a scene never loads through a
prefab reference. Given the host's `SceneComposer`, the factory transcodes a text source to the
binary wire once as it loads (the read on a worker, the transcode on the main thread); a prefab
holding a component no installed manager reads stays text, so nothing is dropped. A component or
script field of type `Ref<PrefabResource>` binds to one like any other reference.

`PrefabPayloads` opens loaded prefabs for one spawn or scene load, binding each through the
`ResourceManager` on first use, with a `Resolver()` for the prefabs a payload nests. A payload stays
open until its stream is destroyed, so a prefab that nests itself (or a chain `kMaxDepth` deep) is
refused with a warning rather than spawned again.

## Spawn + instance tracking

`SpawnPrefab(scene, payload, prefabId, parent, ...)` instantiates a payload: every entity gets a
FRESH guid (or a caller's preassigned one: scene load preserves saved identities this way), parents
relink inside the instance, and roots re-parent under `parent`. Each spawn registers a per-scene
`Scene::PrefabInstanceState` (found by root with `FindPrefabInstanceByRoot`, visited with
`ForEachPrefabInstance`, which prunes states whose root is gone), holding the instance's `prefabId`,
the parallel `sourceIds` / `liveIds` member map, and the spawn-time baselines (`baselineTransforms`
and `componentBaselines`, each a `PrefabComponentBaseline` blob keyed by source entity). Component
ops whose manager is absent (a plugin not loaded) are kept verbatim as `unresolvedComponentOps`,
re-emitted on save and applied once the manager arrives. EntityRef fields in a spawned subtree are
remapped to the instance's own members.

`PrefabSpawnSystem` is the one runtime spawn recipe (a script's `scene.spawn`, the replicated
spawn): see [[text-scenes]] for its shape.

## Overrides - derived, not tracked

There is NO stored "which paths are overridden" set. On scene save, for each instance the template
baselines are compared against the live components (keyed by source entity), and only DIFFERING
properties are written, through the versioned component `Serialize` path. Because the diff is
derived on demand, undo/redo/revert can never desynchronize override state. A nested instance
records whether its root still matches its baseline (`applyPlacement`), so an unmoved nested
instance takes the owner template's placement on respawn while a moved one keeps the scene's.

Scene file: a `prefabInstances` section (prefab ref + root placement + sparse component overrides);
instance members are excluded from the main entity records. `ScenePrefabMode` picks the layout:
`Referenced` (scene and prefab sources: ref and deltas; loading parks pending descriptors that
`ResolveScenePrefabs` respawns) or `Expanded` (in-memory `SceneSnapshot`, as Simulate uses: members
serialize flat with the instance state verbatim, so a restore needs no payload resolver).

## Nesting + wire

Prefabs nest. A payload written with nesting carries a trailing record section, the flat forest of
every instance the template contains at any depth (each relative to its own template, so spawning
never recurses). Records resolve through a `PrefabPayloadResolver` (a function from prefab id to
payload stream): the editor reads its source DB; a running game's scene loads use
`ResolveScenePrefabs(scene, resources)`, which places prefabs from the loaded `PrefabResource`s, so
a level placing one prefab fifty times reads it once. The prefab-section mode tag has two wire
values, `kPrefabWireReferenced3` and `kPrefabWireExpanded2`; any other value (the retired layouts
included) fails the read. The read/write paths must stay SYMMETRIC and count-guarded (see
[[snapshot-prefab-wire-asymmetry]]); a nested record whose payload cannot be resolved is skipped
with a warning, not a crash.

## Editor + propagation

- Create-from-entity (hierarchy context menu: saved under `Prefabs/`, the original replaced by an
  instance, one undo group), spawn through the asset picker or by dragging a prefab onto the
  hierarchy, Apply to Prefab (rewrites the asset and rebuilds every instance in the open scenes;
  asks first), Revert (discards an instance's overrides; asks first), and a per-component Revert to
  Prefab in the inspector, enabled while that component overrides its baseline. The flows live in
  `Editor.Scene/PrefabFlows.cppm`.
- **Model -> prefab import** (`Pipeline/Scene.Pipeline/ModelPrefab.cppm`) generates a spawnable
  `PrefabDocument` named "Prefab" inside an imported model's group; a reimport reuses its guid, so
  placed instances rebuild.
- **Propagation / hot reload**: `RebuildPrefabInstances` rebuilds every affected top-level instance
  (the prefab itself or any instance whose payload references it) from its template, preserving
  the user's deltas; entities added to the template appear, removed ones vanish, and user entities
  parented under members are detached and re-attached rather than destroyed. At run time a change
  reaches a spawn only through a reload of that `PrefabResource`.

---

The reference survey and the original design proposal / phasing / open questions are in
`Documentation/Archive/prefabs-design-history.md`.
