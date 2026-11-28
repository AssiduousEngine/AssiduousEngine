# Scene storage

> APPROVED 2026-11-27 (after a review against master that day), not started; nothing here
> measured. One decision is made: `Get` is const and `Edit` writes, for every component manager
> (decision 5). Read on `master` at `b594bd2d4`; line numbers will drift. No profiler scope exists
> around the scene's own work, so no cost below is a number: step 0 establishes which parts are
> worth building.

## Goal

The scene keeps its model: entities with a built-in transform hierarchy, one packed pool per
component type, systems run by phase. This work changes how that model is stored and stepped.

1. Finding an entity's component is an array read, not a hash lookup.
2. A scene in which nothing moved spends nothing on transforms.
3. The data a frame reads for every entity (world matrices, validity, active state) sits in
   packed arrays of its own, apart from what is read rarely (names, ids, links).
4. A consumer can ask "what moved, and which components were marked, since I last asked",
   without walking a pool. This is what
   [retained-render-scene.md](retained-render-scene.md) asks of the scene.
5. Systems that touch only their own data can run at the same time.

No caller changes for 1 to 3. 5 adds API. 4 adds API and changes callers: a component is read
through `Get`, which becomes `const`, and written through `Edit`.

**Not the goal:** an archetype store. See "Not in this work" for why.

## What exists today

**Entities** (`Scene.cppm:703`, `:781`). `Array<EntitySlot>`, one per slot: generation, three
flags (`active`, `effectiveActive`, `alive`), the persistent `Guid` and the name as a `String`.
`IsValid` reads the slot for `alive` and `generation` (`SceneImpl.cpp:129`), so every validity
check loads a struct that also holds the id and the name.

**Transforms** (`Scene.cppm:715`, `:782`). `Array<TransformData>`, parallel to the entities.
One element holds:

| Field | Bytes |
|---|---|
| `local` (`Float3`, `Quaternion`, `Float3`) | 40 |
| `worldMatrix`, `prevWorldMatrix` | 128 |
| `parent`, `firstChild`, `lastChild`, `nextSibling`, `prevSibling`, each an `EntityHandle` | 40 |
| `dirty`, `updatedThisFrame` | 2 |

210 bytes by this count, likely 216 with padding; `sizeof` was not printed.

**The transform update** (`SceneImpl.cpp:577`):

- `SetLocalTransform` writes the local transform and calls `MarkDirty`, which sets the flag on
  the entity and its whole subtree, downward only, and returns at once on an entity already
  dirty (`:998`). `SetParent` and the sibling splices call it on the moved entity (`:478`,
  `:572`). A new entity starts clean, with an identity matrix (`:945`).
- Links are checked as they are walked: `MarkDirty`, the recompute and the destroy all ask
  `IsValid` of each child link, and the destroy cuts a link that names a dead entity (`:972`).
- `UpdateTransforms` clears last frame's flags from the list of what moved, then **visits every
  transform slot** looking for a dirty one whose parent is clean (`:604`), and recomputes from
  each such top, depth first through the sibling links (`:1031`).
- Each recomputed entity is appended to `m_transformsUpdatedThisFrame`.
- The list is rebuilt by every call, and callers outside the frame's update make one (the scene
  page, the MCP tools, the thumbnail stages; listed in the retained scene spec). A consumer that
  reads it once a frame can miss moves.
- `GetWorldMatrix` returns the cached matrix, or composes a fresh one up the parent chain when
  the entity is dirty (`:334`).

**Components** (`Component.cppm:315-320`). `ComponentManager<T>` is a sparse set: `m_dense`
(packed values), `m_owners` (the entity of each), and `m_sparse`, **a
`HashMap<u32, u32>`** from entity index to dense index (`:318`). The map is open addressing with
linear probing and tombstones on erase (`HashMap.cppm:5`). So `Get`, `Has` and `Remove` each hash
and probe, then compare the stored owner for the generation (`:304`). `Get` has a `const` and a
non-`const` overload (`:136`, `:141`). `Add` pushes a default value, calls `OnComponentCreated`,
and only then returns the component for the caller to fill (`:90`); taken off and given back in
one step (a script's `removeFrom`, then `addTo`), it returns the kept value with no hook called.
`RemoveComponent` swaps the last component into the hole (`:147`). The number of manager types
across `Code/Engine`, `Code/Foundation` and `Code/Plugins` is counted in step 0.

**Joins.** A system that needs two components walks one pool and asks the other per entity.
Render extraction walks the mesh pool and asks the scene per mesh for `IsEffectivelyActive` and
`GetWorldMatrix` (`ExtractImpl.cpp:81`, `:131`), and the instanced-mesh path asks a second
manager per member (`:251`).

**Change reporting.**

- Transforms: the list above.
- Components: a manager has `OnComponentCreated`, `OnComponentInitialized` and
  `OnComponentDestroyed` hooks, and `StorageRevision` (`Component.cppm:134`) says when the pool
  may have moved. Nothing says which components were written.
- Entities: `SceneSystem::OnEntityDestroyed` and `OnEntityActiveChanged`.

**Stepping** (`SceneImpl.cpp:883`, `:1123`). `Update` runs the phases in order, and within a
phase every system in `UpdateOrder`, one after another. `ScenePhase::AsyncUpdate` is documented
as the phase for work that touches only its own system's data, with
`TODO(scene): run its systems in parallel` (`Phase.cppm`). The job system has `ParallelFor`,
`Submit`, `SubmitAfter` and `Wait`, without fibers (`JobSystem.cppm:7`).

**Profiling.** No profiler scope is in `SceneImpl.cpp`.

## The design

Five parts. Each stands alone and is built only if step 0 says it costs something, except D,
which the retained render scene needs whatever its cost.

### A. The component index is an array

`m_sparse` becomes a paged array of `u32` indexed by entity index.

- A page holds 4096 entries (16 KB) and is allocated the first time an entity in its range
  gets this component. A missing page reads as "no component".
- `Get` is: page pointer, entry, compare `m_owners[dense]` with the handle. Two loads and the
  owner compare that is there today.
- Memory: a manager whose components are spread over all of a 100000 entity scene holds 25
  pages, 400 KB. A manager with ten components holds one page.
- Pages are never freed while the manager lives. A manager dies with its scene.

A flat array (no pages) was considered: 36 managers over 100000 entities would hold 14 MB of
mostly empty entries.

The type is private to the scene module. It moves to Core when something else wants it.

### B. Transforms: a list of what was marked

`MarkDirty` splits in two:

- the entry call (from `SetLocalTransform`, `SetLocalPosition`, a reparent, a splice) **appends
  the entity's index to `m_dirtyTops`**, then sets the flag and marks the subtree as today;
- the subtree walk only sets flags.

The entry appends **even when the entity is already dirty**. A reparent needs it: an entity dirty
only because an ancestor was marked, then moved under a clean parent before the update, is no
longer under the top that was listed for it, and nothing else would reach it. Today's full scan
finds it (the comment at `:600` names this case). Appending an entity that turns out to need
nothing costs one skipped entry.

`UpdateTransforms` walks `m_dirtyTops` in place of every slot. For each index: skip it if the
slot is dead or no longer dirty; skip it if its parent is dirty (the parent's own top is in the
list, since that is how the parent became dirty); otherwise recompute from it as today. Then
clear the list.

- A scene at rest does no transform work.
- An entity listed under an ancestor that is also dirty is skipped at the update (its parent
  is dirty), and recomputed by that ancestor's walk.
- The same index may be listed more than once (marked twice, or a slot reused in one frame).
  The second visit finds it clean and skips it.
- **Order changes.** Tops are recomputed in the order they were marked, not in slot order, so
  `TransformsUpdatedThisFrame()` lists entities in a different order. Step 0 reads every
  consumer for a dependence on it.

### C. Entity and transform data in columns

`EntitySlot` and `TransformData` become parallel arrays, all indexed by entity index.

| Array | Element | Read by |
|---|---|---|
| `m_generation` | `u32` | every validity check |
| `m_alive`, `m_active`, `m_effectiveActive` | one bit each | validity, gating, extraction |
| `m_dirty`, `m_updated` | one bit each | the transform update |
| `m_world` | `Float4x4` | extraction, physics, scripts |
| `m_prevWorld` | `Float4x4` | motion vectors |
| `m_local` | `Transform` | setters, the transform update |
| `m_links` | parent, first child, last child, next, previous: five `u32` | hierarchy walks |
| `m_persistentId` | `Guid` | load, save, references |
| `m_name` | `String` | the editor, lookups by name |

- **Links stay handles until proven never stale.** The walks check every link with `IsValid`
  today and the destroy cuts a link to the dead (`SceneImpl.cpp:972`), so the code does not
  assume a link names a live entity. As a bare index, a stale link would name whatever entity
  reuses the slot, with nothing to catch it. Links become indices (20 bytes in place of 40) only
  if step 0 shows no path leaves one stale; the `IsValid` checks then become a Debug assert that
  the linked slot's generation is the one the link was made with, which needs the generation kept
  beside the link in Debug builds. Otherwise `m_links` holds handles.
- **`IsValid`** reads 4 bytes and a bit.
- **`m_world` is a packed array of matrices** with nothing between them. A consumer that reads
  many (extraction; the retained scene's sync) reads them at a 64-byte stride in place of about
  216.
- A bit set type is needed. Core has none in `Containers` today; it is added there, with tests,
  using the scene's allocator.

The public interface of `Scene` does not change. `EntitySlot` and `TransformData` are
`protected`, and no class derives from `Scene` (searched), so nothing outside the module reads
them.

### D. Change lists a consumer clears

Two mechanisms, both "set a bit, append to a list once, the reader clears".

**Moved entities.** The retained scene spec already describes this one: every world matrix
recompute sets the entity's moved bit for a consumer and appends it to that consumer's list,
whatever calls `UpdateTransforms()` and however many updates pass before the consumer reads.

```
MovedListId AddMovedList();                 // a consumer registers once
void        RemoveMovedList(MovedListId);
Span<const u32> Moved(MovedListId) const;   // entity indices, each once
void        ClearMoved(MovedListId);
```

- A scene with no registered list pays nothing beyond today.
- A handful of consumers is expected (the mesh scene, a spatial index, replication). Each costs
  a bit test and perhaps an append per recomputed entity.
- `TransformsUpdatedThisFrame()` stays as it is for its present callers.

**Marked components.** Components are plain values written through a pointer, so the pool
cannot see a write. The manager therefore marks when write access is asked for.

On `ComponentManager<T>`:

```
const T* Get(EntityHandle entity) const;     // read; the only form Get has
T*       Edit(EntityHandle entity);          // write: marks, when marking is on
```

On `ComponentManagerBase`:

```
void SetMarking(bool on);                    // off by default
void Mark(EntityHandle entity);              // for a writer that holds no typed manager
Span<const EntityHandle> Marked() const;     // each once
void ClearMarked();
```

- **A write through `Get` does not compile.** That is the whole guarantee: no native write can
  go unseen, with nothing written per field or per component type.
- **Marking is per manager and off by default.** A manager nobody reads marks from pays one
  branch in `Edit` and keeps no list. The mesh manager turns it on when the mesh scene arrives.
  `Get` and `Edit` split for every manager at once, so the rule is the same everywhere and
  turning marking on for another manager later changes no caller.
- **The mark is per component, not per field.** The reader learns that it changed and reads it
  again.
- **Paths that write without a typed manager** call `Mark` themselves, once each, for every
  component type:
  - a script's component handle already carries its scene, entity and manager
    (`SceneImpl.cpp:775`), and resolves the component on every read and write alike, so the
    mark is not taken at the resolve (that would mark on reads). All three hosts write a property
    through `core::SetProperty` at one site each (C# `DotNetBridgeImpl.cpp:335`, Luau
    `LuauScript.cppm:2254`, AngelScript `AngelScriptScriptImpl.cpp:3829`); each follows the value
    up to its component handle (a nested field such as `body.velocity.x` included) and marks.
  - a script calling a **non-`const` method** on a component (applying a force) changes it
    without a property set. Reflected methods record `isConst` (`MethodInfo`), so a non-`const`
    method invoked through a component handle marks too, at each host's one invoke site.
  - the inspector and other tools write through `GetComponentInstance(entity)` (49 lines call
    it, by `grep`). It splits the same way: a read form that hands out a const instance, and
    `EditComponentInstance`, which marks.
  - `ReadComponent` marks, so a load and an undo are seen.
- **Iteration.** `ForEach` and `Dense()` hand out `const` components. `ForEachEdit` hands out
  writable ones and marks each it visits, for a system that changes a few fields now and then.
  A system that writes every component every frame (an animator advancing time) should not
  mark: its consumer treats that manager as "changes every frame" and walks it, as the retained
  scene spec does for skinned meshes. It uses `DenseUnmarked()`, named so that the choice is
  visible at the call site.
- **Over-marking.** Code that takes `Edit` and writes nothing still marks. It costs a wasted
  re-read, never a wrong result.
- **One list per manager, one reader.** A manager with two readers is not a case today; if one
  appears it gets the registration form the moved lists have.
- **One mark bit per pool slot**, beside `m_owners`, says whether a component is listed, so a
  component is listed once however often it is marked. A removal moves the last
  slot's bit with its component in the swap, and clears the removed one's; the list drops a
  removed entity when `Marked()` is next read (an entry whose bit is clear, or whose handle no
  longer matches the owner, is skipped and compacted out), so a removal costs no search.
- **`Add` marks**, when marking is on. `OnComponentCreated` runs before the caller has filled the
  component, so a consumer that read it there would hold defaults; the mark makes the consumer read
  it again at its next sync. The path that gives back a component taken off in the same step marks
  too, since it calls no hook at all. A consumer still learns of new components from the hook, and
  reads their values from the list.

- **A check for a missed mark, in Debug.** A manager with marking on can be given a compare
  function by its consumer ("does what I hold for this entity still match the component").
  In Debug builds the manager runs it over a slice of the pool each frame and logs an error
  naming the entity and the component type when an unmarked component no longer matches. It
  reports and does not repair: a repair would hide the missing mark.

Considered and not proposed (the retained scene spec's decision 1 has the reasoning): setters
on the component's fields, a manager method per field, an explicit `Mark` after every write.

**What other engines do** (surveyed 2026-11-27, eleven engines, by readers with file and line;
the two cited here were checked in the code):

- LunarSong, whose components are also public structs feeding retained GPU rows, has a const
  `GetComponent` that never stamps and a `GetComponentForWrite` that does, split by name on
  purpose (`Engine/Modules/ECS/Include/ECS/Entity.h:753-767`).
- LuminaEngine stamps on mutable access for the component types registered as tracked
  (`World/ECS/ComponentStorage.h:131-138`, `World/World.cpp:107-130`).
- Engines that leave the mark to the caller show the failure: one keeps a hard-coded list of
  property names in its editor, another's setter and editor disagree about a field.
- None records which field changed in the mark. Those that need it compare against what was
  last pushed, after a coarse mark has said which components to look at.
- Every retained design surveyed has a net for a missed mark (a healing sweep in editor builds,
  a debug log). The Debug check above is ours.

### E. Systems that run at the same time

The smallest step that the code already promises: the `AsyncUpdate` phase.

- `SceneSystem` gains `[[nodiscard]] virtual bool RunsConcurrently() const noexcept`, default
  false.
- In `AsyncUpdate`, `UpdateOrder` is kept. The serial systems run in order; at each run of
  consecutive systems that return true, those are submitted as jobs and waited for before the next
  serial system runs. A serial system therefore sees the same systems run before it as today, and
  may read a concurrent system's data as it does now. Every other phase is unchanged.
- The contract for a system that opts in is the phase's existing one: it reads and writes only
  its own data, calls nothing on the scene that changes it (no create, destroy, add, remove,
  set transform), posts no entity event and runs no script.
- In Debug, the scene asserts if a structural call arrives while concurrent systems run.

Declared read and write sets per system (a system names the pools it reads and writes, and the
scene derives what may overlap) are the general form. They are not proposed now: no system has
been measured as worth it, and script behaviors, the largest per-frame cost a game adds, cannot
run concurrently in any of our script hosts.

## To decide

1. **Which parts to build.** Step 0 (above) recommends D alone, for the user to confirm. The
   recommendation before it was step 0, then A and B if their scopes show a cost
   (they are small and contained), D when the retained render scene starts (its `Get` and
   `Edit` sweep is the one part here that touches every domain), C after measuring
   again with A and B in, E only for a named system that step 0 shows is slow and independent.
2. **Page size for A.** 4096 entries proposed.
3. **`Float4x4` or a 3x4 affine matrix for `m_world`.** `Float4x4` proposed: every consumer
   takes one today, and a 3x4 saves 16 bytes of 64 at the price of a conversion at each read.
4. **Whether the moved lists replace `TransformsUpdatedThisFrame()`.** Proposed: no, both stay
   until its callers have been read; replacing it is a follow-up.
5. **`Get` const and `Edit` for every manager, or only for managers that mark.** DECIDED
   2026-11-27: every manager. One rule, the compiler finds every site once, and turning marking
   on for another manager later changes no caller. The cost is a sweep of every domain, the
   plugins and the samples' native code, in one piece of work. Marking itself stays per
   manager and off by default.
6. **Names.** `AddMovedList`, `Moved`, `ClearMoved`, `Edit`, `ForEachEdit`, `DenseUnmarked`,
   `EditComponentInstance`, `SetMarking`, `Mark`, `Marked`, `ClearMarked`, `RunsConcurrently`.

## Step 0 results (2026-11-27)

Measured on branch `scene-storage` in `build/clang-reldbg` (RelWithDebInfo), RenderStressTest with
100000 spheres, `--measure 300`, runs interleaved; the scopes are 7f74919ed's, the lookup benchmark
538f0c102's, the partial move `--bob-count` ae51f34cc's.

| Case | Frame | `Scene.UpdateTransforms` | `Render.Extract` |
|---|---|---|---|
| At rest (3 runs) | 36.1 to 37.7 ms | 0.63 to 0.65 ms | 6.1 to 6.2 ms |
| 1000 of them moving (2 runs) | 36.4 to 36.8 ms | 0.67 ms | 6.2 ms |
| All moving (3 runs) | 48.5 to 49.0 ms | 4.58 to 4.64 ms | 6.7 to 7.1 ms |

- **The other scene phases** measured 0.002 ms or less each; `Scene.Phase.AsyncUpdate` holds
  nothing here.
- **A lookup** (`ComponentManager<T>::Get`, three runs): 10 to 18 ns, the most at 50% holding and
  shuffled, the least at 100% in index order.
- **Sizes:** `TransformData` 212 bytes (aligned to 4), `EntitySlot` 72, `Transform` 40,
  `EntityHandle` 8.
- **Real scenes:** the largest sample scene has 866 entities (Masquerade's Gardens); Palace and
  Canal about 770, Lamplight's Cellars 653. 100000 entities is the stress tests' case alone.
- **Reads:** four functions mark a transform dirty (`SetLocalTransform`, `SetLocalPosition`,
  `SetParent`, `MoveBefore`); nothing outside the scene's tests reads the moved list or flag, so
  B's order is free; a link could name a freed slot through an entity's own destroy hook, fixed
  (a7445ee7c), and the links stay handles; 34 component manager types outside tests.

**What it recommends.**

- **B: not built.** At rest the scan is 0.64 ms of a 36 ms frame at 100000 entities, under 2%,
  and scales with the slot count: a few microseconds at 866. With every entity moving, the 4.6 ms
  is the recompute itself, which B does not touch. The gain does not pay for the new way to leave a
  matrix stale (Risks).
- **A: not built.** A lookup is 10 to 18 ns. The frame's large scene cost, extraction, asks the
  scene's own arrays (`IsEffectivelyActive`, `GetWorldMatrix`), not a manager's map, so A does not
  reach it; at sample scale a frame's lookups are microseconds.
- **C: not built now.** Extraction is 6.1 ms at 100000 meshes, and C's packed matrices might
  shorten it, but how much of it is reading `TransformData` was not measured. It comes back only if
  a profile of extraction (or of the retained scene's sync) shows the reads dominating.
- **E: not built.** No system runs in `AsyncUpdate`.
- **D: built**, as planned, for the retained render scene: the moved lists, `Get` const and
  `Edit`, and marking.

## Order of work

0. **Establish.**
   - Add profiler scopes: `Scene.Update` per phase, `Scene.UpdateTransforms`, and one around a
     join (the mesh loop of `Render.Extract` already has a parent scope).
   - Time in `build/clang-reldbg`, interleaved runs, on `RenderStressTest` with 100000 spheres:
     at rest, with every entity moved each frame, and with 1000 of them moved.
   - Write a benchmark of `ComponentManager<T>::Get` over 100000 entities with 10%, 50% and
     100% holding the component, in pool order and in random order.
   - Print `sizeof(TransformData)` and `sizeof(EntitySlot)`.
   - Read every caller of `TransformsUpdatedThisFrame()` for a dependence on slot order.
   - For D: read where each script host sets a component property, and list the places that
     write a component through reflection. Count the non-const `Get`, `ForEach` and `Dense()`
     sites per domain, so the size of the sweep is known before it is agreed.
   - Count entities in the largest sample scene, so the numbers above are set beside a real one.
   - Count the component manager types (by the grep stated with the count).
   - For C: read every path that writes a parent, child or sibling link, and the destroy order, to
     learn whether a link can name a dead or reused slot (the destroy's cut at `:972` says the code
     guards against it).
   - Stop and report. A part whose scope is below what a frame notices is dropped from the plan.
1. **A.** The paged index, behind the manager's present interface. Tests: the existing
   `Scene.Tests` pass unchanged; new cases for an index at a page edge, a stale handle on a
   reused slot, removal by swap across pages, and a manager that never held a component.
2. **B.** The dirty list. Before it, every writer of `dirty` and every caller of `MarkDirty` is
   read, so none leaves a flag set without a listed top. Tests: a scene at rest recomputes nothing
   (counted through the updated list); a child marked before and after its parent in one frame is
   recomputed once; a reparent under a clean parent updates; **a child dirty because its parent
   moved, reparented under a clean entity in the same frame, updates**; an entity destroyed after
   being marked is skipped; a slot reused in the same frame is not recomputed from the old mark.
3. **D.** Three commits.
   - Moved lists. Tests: an entity moved in two updates before one read appears once; an
     `UpdateTransforms()` from outside the frame loses nothing; a destroyed entity's index in a
     list is refused by `IsValid`.
   - `Get` const and `Edit`, with marking off everywhere: the sweep of call sites and nothing
     else, so the commit changes no behaviour and the existing suites are its test. Scripts'
     component API and syntax do not change. A changelog line for native game code.
   - Marking. Tests: `Edit`, `ForEachEdit`, a script's property set (a nested field too), a
     script's non-`const` method call (a `const` one does not mark), `EditComponentInstance`
     and `ReadComponent` each mark; `Get`, `ForEach` and `DenseUnmarked` do not; a component
     marked twice is listed once; a removed component is not in `Marked()`; a manager with
     marking off keeps no list; `Add` marks, and a component taken off and given back in one
     step is marked; a component removed after its mark is not listed, and the component swapped
     into its slot keeps its own mark. The script case runs in `Code/Integration` for every
     language.
     The Debug check: a component written through a pointer kept from before (the bypass) is
     reported within one pass over the pool, and a marked one is not.
4. **C.** The columns, one group at a time so each commit stays reviewable: entity flags and
   generation; then links (as handles, or as indices if step 0 found none can go stale); then
   the matrices and locals. Tests: the existing suite, plus the
   hierarchy cases (splice, move before, reparent with and without keeping the world
   transform, destroy of a subtree) run against a scene of several thousand entities with
   slots reused.
5. **E.** `RunsConcurrently` and the phase's fan-out, with the first system that opts in.
   Tests: the opted-in systems all ran before the phase returns; a structural call from one
   asserts in Debug.

After each of 1 to 4, the step 0 timings are taken again and written into the weekly.

## Risks

- **Nothing measured.** Every part here comes from reading. Step 0 may show that the scene's
  own work is small beside the renderer's, in which case only D goes ahead.
- **Order of updated entities (B).** A consumer that relied on slot order would change
  behaviour without failing a test. Step 0's read is the guard.
- **A mark missed (B).** Any path that makes a world matrix stale without listing a top leaves
  it stale for good, where today's full scan would have found a flag set by any path. The
  reparent of an already dirty entity is one such path, closed by appending on every entry call.
  The audit of every writer of `dirty` is part of step 2. A Debug check can run the old full scan
  every so many frames and log an entity found dirty with no listed top.
- **Stale links (C).** If links become indices and some path leaves one naming a dead slot, a
  walk follows it into whatever entity reuses the slot. Step 0's read decides; links stay handles
  otherwise.
- **Memory (A).** Entities whose indices are scattered make a sparse manager hold more pages
  than it has components to justify. Slots are reused from a free list, which keeps indices
  low; a scene that grows large and then shrinks keeps its pages.
- **A way round `Edit` (D).** The guarantee is the type system's, so anything that drops the
  `const` breaks it: a `const_cast`, a pointer kept from `Add` and written later, a tool
  writing through a reflected property's address from a const instance. Step 0 lists the
  places that write through reflection, and the property-set path is where that one is closed.
- **The sweep (D).** Splitting `Get` touches every domain at once. It is mechanical, but a
  site changed to `Edit` where `Get` would do marks for nothing, and reviewers cannot see that
  from the diff alone. Sites inside a per-frame loop are the ones to read by hand.
- **Lists that grow (D).** A registered consumer that never clears makes its list grow to the
  entity count and stop there (each entity is listed once). It is bounded, not a leak, but it
  means every move pays the bit test for nothing.
- **Columns and the debugger (C).** One entity's state is spread over nine arrays, so it no
  longer reads as one struct in a debugger. A debug helper that gathers one entity's fields is
  part of step 4.
- **Concurrency by promise (E).** The contract is checked only for structural calls. A system
  that opts in and reads another's pool is a data race no assert sees. ASAN does not catch it;
  a TSAN lane does not exist. What a system calls on a worker thread (the profiler, logging, its
  allocator) must be safe there; step 5 reads it for the first system that opts in.

## Not in this work

- **An archetype store.** It makes joins over several plain components fast. The price is
  that adding or removing a component moves the entity's whole row, against the rules we have
  for components leaving mid-step, script handles that re-resolve, prefab baselines, managers
  with hooks and their own serialisation, and plugin managers. Our heavy domains (render,
  physics, animation) keep their own state beside the components, so the joins an archetype
  store speeds up are not where frame time has been measured.
- **Hierarchy as components.** Parent and child links stay fields of the scene.
- **A depth-ordered transform array** (parents before children, updated in one linear pass).
  It pays when most of a deep hierarchy moves every frame; reparenting then costs a reorder.
  Skeletons, the usual case for it, are not entities here.
- **SIMD in the transform update.** [simd.md](simd.md) owns `MultiplyMatrices`; C's packed
  `m_world` and `m_local` are what it would run over.
- **Declared read and write sets for systems.** See E.
- **Finding entities by position.** [spatial-index.md](spatial-index.md), which would be a
  consumer of D's moved lists in place of `TransformsUpdatedThisFrame()`.
