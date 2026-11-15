# Particle authoring + cooked runtime resource

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[particles-plan]]

The two tiers that turn the shipped CPU particle runtime (`particles.md`) into an artist-authorable,
cooked-at-build asset: the cooked runtime resource, the editor asset + builder, and the editor's
authoring page. Implemented; LUT curve baking is the deferred piece.

## Cooked runtime resource (`foundation.particles.resource`)

`ParticleEffectResource` - the cooked runtime input - with its factory (`ParticleEffectFactory`) +
resource-manager registration,
content-DB GUID keying, and a bidirectional serializer (polymorphic modules round-trip by their
reflected type id; a module the build cannot construct fails the read rather than being skipped).
The factory binds each system's texture, mesh and materials as `Proxy`s (`SystemTexture`,
`SystemMesh`, `SystemMaterials`), so a hot-reloaded texture is picked up without rebinding.
`ParticleEffectComponent` carries an opaque ref
resolved through the standard `resource::Ref` + scene resolve pass, and reloads live: ref resolution +
hot-reload `Proxy`s are done (not deferred), so an edited + recooked effect swaps behind the component.
The runtime path is `Data/Output/ParticleFX/*.rasset` -> load -> render.

## Editor asset + builder (`particles.pipeline`)

`ParticleEffectAsset` (source, embedding the authored effect) + `ParticleEffectAssetBuilder`
(`Status Build(const pipeline::Asset&, pipeline::AssetBuildContext&)`) that cooks the source effect to
`ParticleEffectResource`, resolving each system's texture path to its cooked GUID, registered with
the pipeline registration composition root. The builder declares every texture, mesh and material
the effect's systems draw with as references, so the cook, `asset_uses` and the export's reach see
them. `RegisterParticleCreators` gives File > New and `asset_create` a particle effect. The
`ParticleFX` sample exercises the full authored -> cooked -> loaded -> rendered path.

## Editor authoring page (shipped)

`ParticleEffectEditorPage` (`Editor.Particles`) edits an effect on the GUI, built from its markup
(`Data/Editor/ParticleEffectPage.xml`) on the editor's asset page base (`AssetObjectPage`). A TREE
shows the effect's systems, each with its emitter, initializers, behaviors and sub-emitters; its bar
adds a system and removes the chosen module (or system, while another remains), and a system is
renamed in place. The GRID shows the chosen system through reflection: its options (each shown only
where its render mode uses it), its emitter, and its modules as lists, where Add offers the types
that derive from the list's base, so the page names no module type. Curves and gradients use the
GUI toolkit's editors over the particle curve types. A sub-emitter row spawns into another system,
chosen by name, on its particles' birth or death ("On death into Sparks"). Every edit is one undo
step. The LIVE PREVIEW simulates the authored effect itself, so an edit shows at once; it restarts
when modules or systems change and rebinds when a texture, mesh or material does, and its transport
(play, pause, clear, speed) never writes the effect. The preview also shows a live particle count
and the chosen system's emission shape. Save writes the asset and requests a re-cook. Covered by
`ParticleEffectPageTests`.

## Deferred

- **LUT curve baking.** Over-lifetime behaviors evaluate their curves directly today; baking curves to
  LUTs would mean changing those shipped behaviors, so it is deferred until a measured need (it is
  independent of authoring).
