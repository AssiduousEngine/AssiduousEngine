# Reflection - the reflected surface for scripting + tooling

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[reflection-track]]

An ongoing coverage effort: reflect the useful surface of the codebase so it is usable for scripting and
tooling without hand-written bindings or serialize-scan fallbacks. The facilities + the mechanism
additions (nested members, computed properties, container primitives, collections-in-scripts,
reference-shaped values, extended properties, the generic list editor) shipped; coverage grows
consumer-by-consumer.

## Facilities (`foundation.core` `:reflection`)

Macros (`Core/Reflection/Reflect.h`): `REFLECT_MEMBERS` (intrusive + properties, via the
`TypeBuilder`), `REFLECT_VALUE` (non-Object value types + properties, patched into `TypeOf<T>()`),
`REFLECT_ENUM`, `RTTI_OBJECT` (the in-class accessors) and `RTTI_DEFINE_OBJECT` /
`RTTI_DEFINE_OBJECT_VERSIONED` (identity, with the versioned form carrying a data version).
Property attributes drive tooling: `displayName`, `range` (sliders), `description` (tooltips),
`visibleWhen` (conditional rows), `category`.

- **Enums** reflect their value names, so inspectors render dropdowns by NAME (not raw int rows) and
  scripts see named values.
- **Containers** reflect through `ContainerInfo` (`TypeInfo.container`): `Array<T>`, C-arrays exposed as
  a flat row-major view (e.g. matrices as `f32[N][N]`), bounded arrays (a fixed C-array plus a live
  count, `TypeBuilder::BoundedArray`), and the homogeneous `Array<UniquePtr<T>>` poly container
  (`RegisterUniquePtrArrayType`). Structural mutation bumps a reflection mutation generation (so a
  `Variant` borrow into a container is invalidated on structural change, never a stale address).
- **Nested + computed** members: a property can be a nested reflected value (`TypeBuilder::Nested`;
  tooling recurses into it) or a computed getter (`ComputedProperty`, or `AccessorProperty` for a
  getter/setter pair over state behind an invariant).
- **Reference-shaped values**: `ReferenceTraits<T>` marks a type's `TypeInfo` as a reference (a
  `Ref<T>`, a `SceneRef`), with the ops to read its Guid, set it and drop its binding and the type it
  points at, so tooling reads a reference by identity without naming its template.

## Consumers that light up as coverage grows

1. **Reflected asset pages** - every page over one reflected asset shares `AssetObjectPage`
   (`Editor.App`): the asset's reflected properties on the property grid with real labels, sliders,
   tooltips, conditional rows and enum dropdowns by name. An asset with no page of its own and no
   reflection falls back to the generic page (`Editor.Generic`, `GenericAssetEditorPage`), which is
   driven by the asset's `Serialize`.
2. **Script backends** (AngelScript + Luau) - both harvest reflected types, so every newly reflected
   type/enum becomes scriptable surface with no hand bindings.
3. **Bespoke inspectors** (particle modules, graph data) - reflected instead of hand-built `Cast<T>`
   dispatches.
4. **The generic list editor** - the GUI toolkit's reflected property source
   (`GUI.Toolkit/ReflectedSource.cppm`, `ReflectedPropertySource`) shows `Array<T>` and bounded
   arrays as lists with add/remove/reorder (through reflection's `emplaceDefault`, `removeAt` and
   `moveElement`) and each element by index; a struct element shows its own rows. The scene
   inspector (`SceneInspectorView`) edits components through it.

## Coverage phases

Consumer-driven, each paying off on landing:
- **Editor assets + their enums**: CLOSED (the flat asset types + their enums reflected, the
  pattern-setter being `TextureAsset`; the base `Asset::fileName` reflected as its `SourcePath`).
- **Particle modules + animation graph/emitter data**: DONE (the particle module types reflected,
  full effect traversal wired).
- **Runtime gameplay surface for scripting**: audio/physics knobs and gameplay descs, as consumers
  demand.
- **Consumer upgrades**: reflected asset pages on the property grid + enum dropdowns.

## Mechanism additions (shipped)

The sweep drove several Core reflection additions (each unblocking a coverage wave):
- **Nested reflected members** - a by-value nested reflected struct as a property tooling recurses
  into (the resolution of the nested-non-copyable-member blocker).
- **`ComputedProperty`** - a getter-backed property (distinct from the Nested kind).
- **Container primitives** - the C-array flat view, the bounded array, and the
  `Array<UniquePtr<T>>` poly container, added for the particle-module sweep.
- **Collections-in-scripts** - reflected container MEMBERS bind as script ops on the owner
  (`<name>_count` / `<name>_at(i)` / `_add` / `_removeAt` / `_move`), constructor-less reflected
  types bind as returned HANDLES, and a `Variant` BORROW mode carries nested-value / non-Object
  element handles (both backends, generation-guarded).
- **Extended properties** - `ExtendProperty<Getter>(name)` lets one module add a read-only property
  to a type another module reflected, from a free getter `R (const T&)`; every reader of the type's
  properties (the script backends, tooling) sees it as the type's own. Scene facades use it through
  `foundation::script::ExtendScene`: each domain gives the script `Scene` its facade as a property
  (`scene.physics`, `scene.render`, `scene.audio`, `scene.animation`, `scene.particles`,
  `scene.scripts`, `scene.splines`, `scene.debugDraw`, `scene.environment`, `scene.postProcess`).
- **The generic list editor** in the inspector.

## Non-goals

Not everything is reflected - identity-only `RTTI_DEFINE_OBJECT` stays for types with no tooling/script
consumer. Coverage is demand-driven, not exhaustive.

---

The working-log detail (the measured-gap census, the nested-member blocker + its decision, the
container-primitives blocker, and the unit-by-unit collections-in-scripts progress) is in
`Documentation/Archive/reflection-track-history.md`.
