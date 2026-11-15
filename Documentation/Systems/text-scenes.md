# Text Scenes

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[text-scenes]]

Scene + prefab SOURCE streams are XML (diffable, mergeable, hand-editable); staged/cooked products and
in-memory snapshots are binary. One `SerializeScene` path feeds both encoders, so the source DB is
text all the way down while the player still loads binary. Shipped. The read path sniffs, so a binary
stream at the current format still loads; only one layout per format is supported (the scene stream
header is magic + version 3, and a stream with another or no header fails to load).

## The model

- **Scene/prefab sources are XML**, written by the same `SerializeScene` walking the same records
  through `foundation.xml.serialization`'s `XmlSerializer`. `SceneSourceText(scene, includeSettings)`
  is the one text writer: `SaveScene` / `SavePrefab` and the MCP `scene_write` / `prefab_write` all
  store what it produces, so a file reads the same whoever wrote it.
- **Scenes stay NON-cooked assets** - no SceneBuilder, no cook badges. Export staging transcodes the
  scene stream to binary: the staged pak carries binary. A prefab is loaded as a resource
  ([[prefabs]]), whose factory may transcode a text source to binary as it loads.
- **One representation**: the in-memory `Scene` + record structure are singular; XML and binary are two
  encoders on one path. Edit-while-play operates on live scenes; `SceneSnapshot` stays in-memory binary.
- **A scene reference is an id**: `SceneRef` holds a scene's guid and never loads it (an id-only
  reference, so the inspector offers scenes for it and a pruned export keeps the scene it names).

Rejected: text-everywhere (player pays XML parse on load) and scenes-as-cooked-assets (pulls scene/
component libs into the cooker for a 1:1 re-encode export staging already owns).

## Format

`detail::DetectSceneStreamEncoding` sniffs the first non-whitespace byte: `<` => `SceneStreamEncoding::Text`,
else `Binary` (a binary stream starts with its magic, never `<`). The XML stream is the same section
sequence (header, name, `entities`, `components`, `systemSettings`, `prefabInstances`) with two
encoding differences:

1. **Component records are INLINE**, not length-prefixed blobs. Each record is its own `<object>` scope
   (`owner`, `type`, then the component's own `WriteComponent` output nested); diffs show real fields,
   not hex.
2. **System-settings records likewise inline**, one `<object>` scope per system.

Shared verbatim: header magic/version, guids, floats via shortest round-trip (text saves cannot
produce noise diffs or phantom overrides), prefab records. Prefab `componentOps` override payloads
also serialize INLINE; an op whose manager is unknown at write keeps the hex form (`form=0`) so
transcodes never lose it, and one unknown at read is dropped with a warning.

**Unknown component and settings records** (a plugin not loaded) are not dropped: the load keeps
them as unresolved records with a warning, a save writes them back verbatim in the encoding they
were captured in, and they become real components once the manager arrives
(`ResolveUnresolvedComponents`). A save in the other encoding cannot re-encode an unknown type, so
those are dropped with a warning.

## Touch points

- `detail::SceneStreamEncoding` + the sniff + `detail::SceneStreamReader` (owns the parsed
  `XmlDocument` + serializer storage). `foundation.scene.resource` depends on
  `foundation.xml.serialization` (a core-level lib; the player links it but only exercises the
  binary path).
- `SerializeScene(..., encoding)`: write-side takes the encoding, read-side derives it from the sniff.
  The write side also emits parked pending prefab instances so a load->save transcode is lossless
  without resolving/spawning instances.
- Writers emit XML: `SaveScene`/`SavePrefab`, `CapturePrefab`, `CaptureInstanceAsTemplate`.
  Readers sniff: `LoadScene`, `SpawnPrefab`, `RevertPrefabInstance`, `RebuildPrefabInstances`,
  `ComputeInstanceDeltasVsTemplate`.
- **Runtime spawn** (`PrefabSpawnSystem`, Scene.Resource): ONE recipe for every runtime spawn - a
  script's `scene.spawn`, the replicated spawn resolver. A scene system in the full composition
  (domain `prefabs`), inert until the host points it at its `ResourceManager` with `SetSource`
  (`DefaultApplication` at every scene's `SystemsReady`). `Spawn(prefab, position, rotation,
  parent)`, by id or by `Ref<PrefabResource>`, binds the prefab through the manager (synchronously
  when it is not loaded, so a spawn is never a frame late), spawns its payload with the prefabs it
  nests resolved through `PrefabPayloads`, places the root in the parent's space, and binds ONLY
  the spawned subtree (`ResolveEntityResources`). Unknown, deleted or unreadable prefab, or no
  source = invalid handle, never a partial spawn. The static `SpawnInto(scene, resources, prefab,
  parent)` is the same recipe for a caller with no system in hand
  (`DefaultApplication::ResolveNetworkPrefab` delegates to it).
- **Export staging**: `TranscodeSceneStreamToBinary(in, scratch, includeSettings)` re-encodes a
  stream; `engine::CollectSceneStreams` (`Engine.Composition/SceneExportSupport.cpp`) walks a
  project's scenes and prefabs and transcodes each over a scratch `Scene` assembled with
  `engine::AddAllSceneManagers` (the full composition: a hand-listed manager set would silently
  drop component types). The export CLI, the editor and the MCP hosts all use it, and MCP
  `scene_validate` reads through the same full set. The runtime and every headless consumer
  instantiate from the one composition root in `Engine.Composition` ([[scene-composition]]).
  Scene streams are structure-only (KBs), so transcode is milliseconds; a stream that does not
  transcode is staged verbatim and the runtime sniffs.

## Invariants

One `SerializeScene` path feeds both encoders (divergence can only be an encoder bug), guarded by the
equivalence test: author -> save XML -> load -> save binary -> load -> deep-compare. An unknown record
is skipped past (and preserved) while the rest of the scene loads; XML prefab payloads spawn (nested
included); staged/cooked products stay binary.

## Deferred

Optional pretty-formatting knobs (indent width, attribute order) if diff noise shows up in practice -
not built (no demand yet).
