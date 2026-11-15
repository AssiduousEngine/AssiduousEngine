# Asset pipeline

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[editor-track]]

Deterministic, incremental asset cooking on a typed content DB. Source assets (import settings
as typed objects) cook to runtime products only when their inputs actually change, ordered by a
real dependency graph, in the background in the editor and headless from a CLI. All file access
goes through the VFS - never the raw filesystem.

## Content databases (`foundation.content`)

A project opens two `ContentDatabase`s over VFS mounts:
- **Source DB** (`Content/`, XML `.xasset`): a typed primary `Asset` object (external `fileName`
  + import settings) + named binary streams, GUID identity, a `Group` tree of `Instance`s.
- **Cooked DB** (`Cooked/`, binary `.rasset`): the cooked runtime products for the host target.
  An export for another target cooks a sibling DB per target (`Cooked-web-bc/`, `Cooked-web-astc/`,
  `Cooked-android/`; see Cook targets below).
- `.cache/` holds the pipeline DB (`cook.db`, and `<target>/cook.db` per target DB) + thumbnails.

`Instance` = one typed object in a `Group`; import settings persist AS the typed Asset (so a
reimport is deterministic).

## Identity + staleness (the deterministic core)

Every source `Instance` whose primary type has a builder gets a **recipe hash**:

    recipeHash = fold( serialized Asset bytes,
                       content hash of each source file,
                       builder Version(),          // per-builder u32, bump on cook-logic change
                       recipeHash of each READ dependency )   // sorted by Guid

Folded ORDERED into one u64 (field-tagged, deps sorted) - never commutative sums (which collide
on reordered children). Staleness is by CONTENT HASH, not mtime (deterministic across
checkouts/machines); an (path, mtime, size) -> contentHash memo in the pipeline DB is only a
fast path to skip re-hashing untouched large files.

**Two dependency kinds** (the one Traktor distinction kept):
- **read**: the build consumes that instance's content (a material bakes a texture) -> chains
  into the recipe hash and induces build order.
- **reference**: the product stores another instance's Guid for RUNTIME resolution (a scene
  references a mesh) -> the product only needs to EXIST (a cook-order hint); editing the mesh
  never re-cooks the scene.

**Dirty <=> product missing or recipe-hash mismatch.** One rule subsumes cook-on-demand,
incremental build, and farm build. The **pipeline DB** (`.cache/cook.db`, one binary file,
atomic tmp+rename) stores per source Guid `{recipeHash, product Guids, file deps
(path,mtime,size,contentHash), read-dep Guids}` + a reverse-dependency index rebuilt on load;
corruption/missing = full rescan (never wrong output, only wasted work).

## Builders (`pipeline.core`)

    class IAssetBuilder {
        virtual const TypeInfo* AssetType() const = 0;
        virtual const TypeInfo* ProductType() const = 0;              // stamped on the product
        virtual u32 Version() const;                                  // bump on cook-logic change
        virtual BuildVariance Variance() const;                       // PlatformInvariant (default) or PlatformVariant
        virtual u64 EnvironmentHash() const;                          // what it compiles against (a script cook's surface)
        virtual void ScanDependencies(const Asset&, AssetBuildContext&, AssetDependencies&);
        virtual Status Build(const Asset&, AssetBuildContext&) = 0;
    };
    // AssetDependencies { files; sourceStreams; reads; references; }
    class BuilderRegistry { Register(...) -> key; Unregister(key); Find(assetType); };  // type -> builder

One product per source instance. Multi-product is the importer FAN-OUT pattern (a model importer
creates many real source instances at import - the DB shows the true graph, no hidden synthesized
outputs). Builders are GPU-free (cooked data is raw pixels/verts) so the headless CLI can run
them. The builder set + all type/product/importer registration is assembled at ONE composition
root, [[pipeline-registration]]; a plugin's Pipeline part adds its own builders and importers to a
host's registries (`PipelineRegistries`), and they are unregistered by key when it unloads.

## Cook driver (`pipeline.cook`)

`CookDriver` (`pipeline.cook`): `Plan(force) -> CookPlan` (dirty list + order, inspectable for UI),
`PlanFor(roots, force)` (the roots plus their read and reference closure, no orphan sweep: one group
or asset at a time, and export pruning's closure), and `Execute(plan, progress)`, which the editor
splits into `PrepareProducts` (main thread: sweep orphans, create product instances, snapshot the
source pointers, since the DBs are not thread-safe) and `ExecuteBuilds` (worker-safe, builds only).
Read-deps induce a topological order; independent assets in one dependency level cook in PARALLEL on
the JobSystem. A failed build keeps the last good product + marks the record failed (listed in the
Console; rebuild retries; a failed record never counts as clean). An **orphan sweep** in Plan deletes
cooked products whose source is gone or whose type lost its builder, so `Cooked/` never accumulates
garbage. Builder `Version()` is manual (the known failure mode; Build > Rebuild All is the one-click
hammer).

**Cook targets.** A `CookTarget` (`pipeline.core`) is a capability profile: `host` (BC, the editor's
always-warm target), `web-bc`, `web-astc` and `android` (ASTC), resolved by `CookTargetFor(id)` on
capability, never platform. `CookDriver::SetTarget` hands it to every build
(`AssetBuildContext::target`) and salts the recipe of `PlatformVariant` builders (textures), so the
same source cooks to a distinct product per target. `CookForTarget` cooks a target DB with
copy-forward: an invariant product whose recipe matches the host record is copied from the host
`Cooked/` rather than recooked. Both `Tools.Cook --target <id>` and the export's per-platform variants
(`export.md`) drive it.

## VFS integration (`foundation.vfs`)

Everything routes through mounts (`Content/`, `Cooked/`, `Sources/`) - the pipeline never touches
the raw filesystem. `IStatFileSystem` (`AsStat()`, native backend) provides the (size, mtime) for
the memo (Pak returns none - immutable content). `NativeFileSystem` implements
`IWatchableFileSystem` / `IChangeSource`: v1 is a THROTTLED STAT SWEEP inside `Poll()` (portable,
no platform event plumbing; inotify/ReadDirectoryChangesW are drop-in upgrades behind the same
Poll). The cook service tracks `Sources/` and polls (~2s); a changed file invalidates the memo
and re-queues dependents via the reverse map.

## Resource-reference layer (`foundation.resource`)

The bridge from cooked products to live scenes:
- **`resource::Ref<T>`** (serializable): a Guid + a Bind()-attached proxy handle; GUID-first
  resolution survives renames (a path string is a human-readable hint only). A direct-`RefPtr`
  override keeps procedural call sites source-compatible.
- **`ResourceManager`** (`ResourceHandle` replaceable slots + `IResourceFactory` registry): Guid
  -> loaded-object cache with auto dependency edges + transitive `Reload`. Subsystems register
  their factories (mesh/skinned-mesh/material/texture); rename is a cache re-key, not eviction.
- **`ResolveSceneResources`** is a POST-LOAD pass (after deserialize, via the
  `ComponentManagerBase::ResolveResources` ADL hook) - no serializer-context coupling.
- **Hot reload**: a recooked product reloads in place behind its proxy handle and Generation-bumps;
  dependents reload transitively via the manager's recorded edges.

## Surfaces

- **CLI** (`Code/Tools/Tools.Cook`): the same `CookDriver`, headless (no shell/GPU), console
  progress, exit code = failures. `--rebuild`, `--dry-run` (print the plan), `--target <id>` (also
  cook `Cooked-<id>/`), `--data-root <dir>`.
- **Editor** (`EditorCookService`, `editor.project`): background-thread cook (one at a time,
  progress -> status bar + Console), per-instance cook badges (Cooked / Missing / Failed /
  NoBuilder), a Build menu (Cook All / Rebuild All, and Cook This Asset / Rebuild This Asset /
  Reimport... for the open page), a per-project `ResourceManager` over the cooked DB (owned by the
  `EditorSession`) + `ResolveSceneResources` on scene open, and resource-ref picker rows
  (`ResourceRefEditor`, exact `Ref<T>` TypeInfo match, undoable). Besides `Sources/`, the service
  watches `Content/` (a `ContentWatcher`): a record changed outside the editor is taken into the
  source DB and recooked. Import: a file dropped on the window (`IShell::DrainDroppedFiles`) or
  chosen with Import... opens the import window (`ImportDialog` over an `AssetImport` session) ->
  `ImporterRegistry` (by extension; a chooser when several importers claim one) -> copy into
  `Sources/` + a typed Asset (e.g. `TextureFileImporter` for png/jpg/jpeg/tga/bmp/hdr/dds).
- **What a texture cook costs** (2026-09-23, measured through the MCP `asset_cook` tool on
  the RTHomes1 4k JPEGs): decode ~0.2 s, mips ~0.1 s, BC7 ~0.8 s, write ~0 - about 1.2 s for
  one 4k texture alone, 7 s for six together on 16 cores; it was 84 s and 506 s. Four things
  made the difference and each is a rule now: the block encoder fans its block rows out over
  the cook's job system (`AssetBuildContext::jobs`, `EncodeBlockCompressed(..., jobs)`,
  byte-identical to the inline loop) and so does the mip filter; the vendored encoders
  (bc7enc, astcenc) and stb build at -O2 in EVERY configuration on Clang and GCC (a Debug editor
  cooks too);
  Default compression is bc7enc's uber 0 (its own default; the mid level cost ~4x for a
  fraction of a dB) and Quality is uber 4; and `Array::Resize` grows geometrically with a
  block relocation for trivially copyable elements (the 85 MB mip chain used to reallocate
  byte by byte per level - 6 s at -O0). The builder logs each texture's phase split and the
  driver logs each build's time, so a slow phase names itself in the console.
- **DDS sources** (`foundation.image.dds`, 2026-09-22): a DDS is a GPU-ready container
  (BC1-BC7 / BC6H blocks, a mip chain, legacy FourCC or DX10 header). The module reads it as-is
  (`LoadDds` keeps the payload bytes), decodes any level through bcdec (`DecodeLevel`: RGBA8,
  or RGBA32F for the float formats; BC5 / RG8 get a reconstructed Z so a two-channel normal
  reads whole), and writes the DX10 form. Every generic image loader sniffs the magic and
  hands back level 0, so thumbnails, pages and model loaders read DDS without knowing it.
  The texture builder PASSES THE LEVELS THROUGH untouched when the target reads BC, the
  authored compression is not None, the block format fits the asset's usage by the policy
  table's own rules (Colour = BC1/BC2/BC3/BC7; Normal = BC7 only - never BC5, until the shaders
  reconstruct Z; Mask = BC4/BC7; HDR = BC6H) and the file carries the mip chain the asset asks
  for; otherwise level 0 decodes and cooks like any image (the ASTC route, the None escape hatch,
  a BC5 normal, a chain-less file wanting mips). The sRGB / linear twin of a block format is the
  asset's colour space (same bytes, another GPU view); a DX10 header settles it at import for a
  colour map, and BC5 / BC4 / float headers pick the normal / mask / HDR presets. A model that
  references DDS files (Bistro) keeps them on disk: the loaders leave a DDS undecoded
  (`ModelTexture::sourceFile`), the model importer copies it into Sources/ and creates a
  FILE-BACKED TextureAsset with the material slot's usage, and the cook passes it through.
  The glTF loader prefers the `MSFT_texture_dds` image over a texture's PNG `source` and maps
  `KHR_materials_pbrSpecularGlossiness` (diffuse -> base colour, 1 - glossiness -> roughness).
  Cubemap / array / volume DDS: not yet (the cook refuses with NotSupported).
- **Asset browser** (`AssetsView`, the Assets panel): source-DB-backed - a `Group` tree + the
  group's subgroups and assets as tiles or a list, with cook badges, favourites, Always Export marks,
  a search (by name or guid start, through the group's subtree) and a type filter, and context menus
  (Open, Rename, Duplicate, Copy Path / GUID, Always Export, New Group..., Import..., ...); Enter or a
  double click opens an asset's page or enters a group; a dragged selection carries its assets
  (`AssetDrag`) to slots and scenes.
- **Thumbnails** (`ThumbnailService`, `editor.core`): per-type generators run on the editor task
  pool's light lane and cache to `.cache/thumbs/<guid>-<recipeHash>.png` (a stale hash is a filename
  mismatch); the scene domain's GPU generators (mesh, material, prefab, scene, particle effect) render
  through the preview stage.

## Deferred

Cubemap and array DDS sources (the cook refuses them, above), an ETC2 cook target, and a platform
event backend for the native change source (inotify / ReadDirectoryChangesW behind the same `Poll`).

---

Design lineage (the Lumix / Traktor / Sedulous synthesis and what was rejected/fixed) is in
`Documentation/Archive/asset-pipeline-design-history.md`.
