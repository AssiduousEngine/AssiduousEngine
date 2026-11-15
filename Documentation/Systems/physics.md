# Physics

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[physics-p1]]

Jolt-backed physics in the value-pool component style: rigid bodies, colliders, characters,
joints, cooked collision shapes, a designer collision-group matrix, fixed-timestep stepping with
render interpolation, and real queries + contact events. Shipped end to end and wired into
`DefaultApplication`. Jolt types never leak above `foundation.physics`; there is no second-backend
abstraction seam (deliberate - see the design history).

## Modules

- **`foundation.physics`** (`Code/Foundation/Physics`) - the Jolt wrapper: `PhysicsWorld`,
  `ShapeKind`/`ShapeDesc`, bodies, queries, layer/group filtering, contact buffering, temp
  allocation, the job adapter. `ObjectLayer = (broadphaseLayer << 8) | (group & 0x1F)`.
- **`foundation.physics.resource`** (`Code/Foundation/Physics.Resource`) - `CollisionShape`, the
  cooked shape product (Jolt `SaveBinaryState` bytes) + its runtime factory.
- **`physics.pipeline`** (`Code/Pipeline/Physics.Pipeline`) - `PhysicalMaterial` + the collision-
  shape / material asset builders (cook via Jolt).
- **`engine.physics`** (`Code/Engine/Engine.Physics`) - `PhysicsSubsystem` (an `ISceneObserver`;
  its managers and the per-scene `PhysicsSceneSystem`, which owns the scene's `PhysicsWorld`,
  install through the physics domain's scene installer - [[scene-composition]]), the components,
  fixed-step + interpolation, and the `ScenePhysics` script facade (`scene.physics`).
- **`editor.physics`** (`Code/Editor/Editor.Physics`) - `CollisionShapePage`, the collision-shape
  asset editor, and the collision-shape thumbnail. (The collision-group matrix grid is
  `Editor.Scene`'s `CollisionMatrixEditor`, in the inspector.)

## Components (value-pool)

- **A moving body (dynamic OR kinematic) needs a CONVEX shape** (box, sphere, capsule, convex
  hull). Jolt's `Shape::MustBeStatic()` names the rest - triangle mesh, plane, heightfield, and
  any compound or decorated shape holding one: no mass, no collision path between two of them,
  and Jolt sets mass properties for every non-static body, so it asserts "Invalid mass" at
  creation (a release build gets a body with no inertia instead). `PhysicsWorld::CreateBody`
  demotes such a body to static with an error, and the scene system names the entity (plane
  and heightfield from the shape kind, a cooked mesh from the collision resource's `convex`
  flag). The world reports how a body was made (`PhysicsWorld::MotionOf`, kept as the component's
  `effectiveMotion`) and the subsystem drives it by that, so a demoted body is never moved as a
  kinematic one (`MoveKinematic` refuses a body that is not kinematic). A convex shape degenerate to zero volume (a hull cooked from a flat quad; Jolt refuses
  a zero scale at shape creation) still collides, so it gets the mass of a solid box over its
  bounds and keeps simulating.
- **`RigidBodyComponent`** - `motion` (`MotionKind` Static / Kinematic / Dynamic), the broadphase
  `layer` (`PhysicsLayer` Static / Dynamic / Kinematic / Trigger), the body's own shape (a
  `ShapeKind`: Box / Sphere / Capsule / Plane / Cooked / Heightfield, with the matching params, a
  `Ref<CollisionShape>` for Cooked and a `Ref<Heightfield>` for Heightfield), friction /
  restitution / linear and angular damping, `isTrigger`, `continuousCollision` (Jolt LinearCast),
  `mass` (0 = from density), a `collisionGroup` (`u8`, `[0, 32)`) and an optional
  `Ref<PhysicalMaterial>` that wins over the inline friction and restitution. Runtime control
  (`SetLinearVelocity`, `AddForce`/`AddImpulse`, `Teleport`, ...) is on the world/body surface.
- **`ColliderComponent`** - an extra shape on a descendant entity (the same `ShapeKind` params),
  folded into the nearest ancestor body's compound at start with its offset relative to the body
  (the hierarchy walk stops at nested bodies). One collider component with a kind, not
  per-primitive `BoxShapeComponent` etc.
- **`CharacterComponent`** - Jolt `CharacterVirtual`. Scriptable directly: reflected `move(velocityX,
  velocityZ)` + `jump(speed)` + `launch(speed)` methods (caller-owned velocity). The standard recipe
  moves a grounded character only by its input, so it stands still on a slope; `drive(x, y, z)`
  hands the whole velocity to the script (gravity included) until the next `move`, and the
  read-only `velocity` (the motion over the last step) and `groundNormal` (up in the air) let a
  board or a sled integrate momentum along the ground. `launch` lifts a character into the air
  (where a `jump` waits for the ground); `setPosition`, `grounded` and `positionX/Y/Z` round it out.
  Authored: `radius`, `halfHeight`, `maxSlopeDegrees`, `mass`, `maxStrength`, `stepUp`, `stepDown`.
  A character walking into or out of a trigger raises `TriggerEnter` / `TriggerExit`.
- **`JointComponent`** - ONE component with a `JointKind` enum (Fixed / Point / Hinge / Slider /
  Distance) referencing a second entity; `motorEnabled` + `motorTargetVelocity` + `motorLimit`
  motors are actually applied. Deviation: one joint component with a kind, not five component types;
  a nil target binds to the nearest ancestor body.
- **Removal** - a removed or destroyed body, character or joint leaves the world: the component
  managers hand its id to the scene's `PhysicsSceneSystem` (`IPhysicsReleaser`), which drops it at
  its next reconcile (joints first).

## Scene settings, layers + groups

Per-scene (the `PhysicsSceneSettings` block, the Scene tab's Physics section, saved with the scene
- NOT project settings): `gravity` (`Float3`), `collisionSteps`, the collision-group `groupNames` +
`groupCollides` matrix (up to 32 named groups, NxN mask), and a `debugDraw` toggle. Two-level
filtering: a fixed semantic broadphase layer in the high byte x the designer group in the low byte.
Queries take a group mask, and it is actually applied.

## Stepping + interpolation

The per-scene fixed-update lane (`PhysicsSceneSystem::OnFixedUpdate(fixedDeltaTime)`) runs 0..N
times per frame off the runtime accumulator. Per fixed step: kinematic bodies take scene transforms via `MoveKinematic`
(velocity-correct), the coalesced world update runs, the contact buffer drains to events, and
dynamic poses are saved as `prev`/`curr`. Every RENDER frame `ApplyInterpolation(alpha)` writes scene
transforms as `lerp(prev, curr, alpha)` + `Slerp` on rotation (scale untouched), so gameplay reads
the smoothed pose. `Teleport` snaps both poses (no ghost lerp). Transform ownership: dynamic = physics
owns pos/rot; kinematic = scene owns; static = immutable while simulating.

## Queries + events

Ray casts, sweeps, and overlaps carry REAL surface normals, body handle, entity, and
distance/fraction. Contact/trigger events are appended to a lock-free buffer on Jolt worker threads
(entity ids packed in body user data - O(1) reverse mapping, no re-entry into Jolt) with an overflow
counter surfaced as a warning, then dispatched on the main thread after the step (to each
registered `IContactListener`, the script subsystem among them). Contact data (relative velocity,
combined friction/restitution) comes from Jolt's manifold.

Two scene capabilities let systems that do not depend on physics use it: the rigid body manager is
an `IStaticGeometrySource` (the static, non-trigger bodies' world triangles, which the navigation
bake reads) and `ISceneEntityBounds`, and `PhysicsSceneSystem` is an `ISceneRayQuery` (rays
against solid bodies, triggers never; foot IK finds the ground through it).

## Resources + cooking

- **`CollisionShape`** (cooked product) - `ConvexHull` or `TriangleMesh` cooked from a source model,
  stored as Jolt `SaveBinaryState` bytes; the factory restores a shared `ShapeRefC` (real Jolt
  refcounting). (Convex decomposition is NOT built - see backlog.)
- **`PhysicalMaterial`** (cooked product) - friction / restitution with combine modes, density, and a
  gameplay `tag`, applied through Jolt's combine callbacks.
- Both resolve through `resource::Ref` and stage into paks; the player needs zero cooking code.

## Editor

`CollisionShapePage` edits the collision-shape asset (cook type + params); the model-import path can
emit a collision shape + rigid body on the generated prefab. The collision-group matrix is a grid
editor in the inspector (`Editor.Scene`'s `CollisionMatrixEditor`, covered by
`Editor.Scene.Tests/ScenePageTests`).
Component gizmos draw the shape wires.

## Script facade

`scene.physics` - a reflected, backend-neutral scene-bound value handle, a property of the scene:
queries (`rayCast`, `sphereCast`, `nearestOverlap`, `overlapSphere`, by `Float3` or separate
numbers, each with an optional group mask; a hit is a `RayCastHit` with `hit`, `distance`,
`position`, `normal`, `surface`, `entity()`, `material()`, `impulse(x, y, z)`), gravity
(`setGravity`, `gravity`, `gravityY`), `bodyCount`, the character verbs by entity
(`moveCharacter`, `jumpCharacter`, `launchCharacter`, `setCharacterPosition`,
`isCharacterGrounded`), and the body ops keyed by entity:
`applyImpulse`, `setVelocity` / `velocity`, and `setForce`, a HELD force applied every fixed step
until set again (Jolt clears a force after each step and behaviors run per frame, so a per-frame
AddForce would land on whichever step came next, or none). An impulse or velocity given to a body
not built yet (a prefab spawned this frame) waits on the component for its creation; queues and
held forces are run-scoped. Physics ops that need world context are keyed on the scene handle
(the component data cannot reach the world); per-character control is on `CharacterComponent`
directly. Certified on AngelScript and Luau (`Engine.Script.Tests/ScriptSceneTests`); the body
ops natively in `Engine.Physics.Tests/PhysicsSceneTests`.

## Portability

Jolt compiles to WASM (single-threaded job fallback via the adapter seam) and ARM/NEON.
Cross-platform determinism is explicitly NOT pursued (revisit only if lockstep networking appears).

## Deferred

Parked, deliberate (convex decomposition / V-HACD, gravity volumes, `JPH_DEBUG_RENDERER` wiring,
per-world job-pool consolidation): `Documentation/Archive/physics-followups-history.md`.

---

Design rationale (the reference survey - ezEngine / Godot / Flax and others - the no-abstraction-
theater call, the layer-model choice, the deviations and their reasons) is in
`Documentation/Archive/physics-design-history.md`.
