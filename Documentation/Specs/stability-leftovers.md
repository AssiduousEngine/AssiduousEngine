# Stability: what the first series left out

> STATUS: APPROVED 2026-11-15 (user). Sized L, phases T0 to T7, each green on the lanes before the next.
> Origin: `backlog-0.1.md`, "What the stability spec left out, by area" (the 2026-11-11 audit's
> findings the first series did not take). Every item was read again in today's source on
> 2026-11-15 (master fa7afa1ba); this spec is what still holds. Read CONVENTIONS.md first.

## Goal

The engine's remaining crashes, hangs, silent data loss and wrong results go, worst first: what a
player of a shipped game or a user of the editor meets before what a tool or a benchmark meets.
Every fix lands with a test that fails before it (deterministically, or under ASAN where the bug
is a use-after-free), in the suite that owns the code.

Not goals: the design items at the end (deferred destruction in the RHI, pooled allocation, a
persisted pipeline cache and the rest); they stay in the backlog with what this re-check said of
them.

## Re-checked: no longer to do

- `SceneManager::DestroyScene` dedupes now (both paths check `Owns`).
- glTF import generates tangents when none are authored (`MeshConvert.cppm:471-476`).
- `PointerDown` and `PointerMove` pin their hit node (`EventDispatcher.cppm:1595, 1631`).
- The JobSystem destructor drains: workers run until no job is left.
- `RefCounted` has no converting move (only a converting copy; the destructor is virtual).
- Web streamed audio does not stall: miniaudio processes a job per device callback under
  `NO_THREADING` on Emscripten.
- MeshRenderer caches its set-0 bind group per view (keyed on ring and resource generations).
- The glTF vertex count mismatch cannot overflow: `cgltf_validate` rejects attributes of differing
  counts in a primitive.

## Phases

Each item: what is wrong, where (today's lines), the test that fails now, the fix.

### T0: memory, hangs and silent loss

- **A cross-root drag's drop target dangles.** `EventDispatcher.cppm:3066` keeps `m_dropTarget`
  raw; `NotifySubtreeRemoved` (:1547) reaches only the removed node's own dispatcher
  (`GUIClusterImpl.cpp:233`). Test (ASAN): drag from root A over node T in root B, B removes T,
  move. Fix: the cluster tells every dispatcher of a subtree removed; a drag's target is pinned.
- **`ScreenStack::Settle` loops while handlers pop screens.** `ScreenStack.cppm:244-252`. Test
  (ASAN): a `screenhidden` handler that pops two screens. Fix: settle over a pinned snapshot.
- **Overlapping multi-caret edits corrupt text.** `CodeEditor.cppm:1678` (`EditEach`). Built
  2026-11-15, corrected: the text was not corrupted (the document's listener carries each caret
  through the edits already made, so a swallowed caret's own edit finds nothing to do), but that
  caret kept its place from before the edit, a stale caret in the text after it.
- **A read-only code editor changes on Undo, Redo, Move and Duplicate lines.** `CodeEditor.cppm:2446,
  2917, 2952`. Test: read-only, `code.move-lines-up`, text unchanged. Fix: the edit commands check
  `m_readOnly` in one place.
- **CurveView hangs drawing its grid.** `CurveView.cppm:114` clamps a zero range to 1e-6, lost in
  f32 at |value| >= 16, and NaN passes; the loop at :895 never ends. Test: `SetValueRange(1000,
  1000)`, Draw returns. Fix: a minimum span relative to the magnitude, finite checks, a step floor.
- **A reflected u8 takes 300 as 44.** `ReflectedSource.cppm:1171-1188` narrows with `static_cast`.
  Test: write Variant(300) to a u8 field, it reads 255 (clamped) or is refused. Fix: range-checked
  narrowing for every integer width.
- **`BufferedStream::Write` reports success when its flush fails.** `Stream.cppm:339, 408`. Test:
  over a stream whose Write returns 0, `Write` returns less than asked. Fix: report the flush's
  bytes; a failed flush fails the write.
- **Recursive delete follows a symlinked root, and `DT_UNKNOWN` children.** `FileSystem.cppm:177-223`,
  `LinuxSystem.cpp:652`. Test: `RemoveDirectoryRecursive(link)` leaves the target's files. Fix:
  `lstat` the root and children; a link is unlinked, never entered.
- **The cook mutates shared instances unlocked.** `ContentModuleImpl.cpp:383-384` writes
  `m_source`/`m_sourceKnown` while builders run under `ParallelFor` (`CookDriverImpl.cpp:398`).
  Test (TSAN or a stress test): cook two clips on one skeleton in parallel. Fix: resolve the source
  under a lock, or once before the parallel section.
- **An export ships stale scenes.** `ExportImpl.cpp:622-623` never clears `.stage-scenes`; early
  returns (:641, :671) skip the cleanup at :821. Test: plant a file in the staging folder, export,
  the pak lacks it. Fix: clear on entry, clean up on every return.
- **`Tools.Cook` exits 0 after 256 failures.** `Tools.Cook/Main.cpp:190` returns the count. Test:
  256 failing assets exit nonzero. Fix: exit 1 on any failure, print the count.
- **An XML array count is trusted.** `XmlSerializer.cppm:160-164`. Test: `count="4000000000"` fails
  cleanly. Fix: bound the count by the element's children.
- **A DDS mip count is unbounded.** `DdsImpl.cpp:255`; `width >> level` is undefined past 31 and
  `arraySize * 6` overflows. Test: a 1x1 header with mipCount 0xFFFFFFFF returns InvalidArgument
  at once. Fix: mips at most log2(max side) + 1, checked sizes.
- **A stroke's round joins and caps are uncapped.** `Tessellation.cppm:1040, 1082`. Test:
  strokeWidth 1e7 with round caps stays under a vertex budget. Fix: cap segments per arc.
- **`NumberField`'s evaluator recursion is unbounded.** `NumberField.cppm:66-84`. Test: 1e6 '('
  returns an error. Fix: a depth limit.
- **`NextInt(INT_MIN, INT_MAX)` divides by zero.** `Random.cppm:77-81`. Test: it returns a value.
  Fix: unsigned range arithmetic.
- **`Engine.GUI`'s `PumpInput` uses the root and bridge after handlers run** (suspected).
  `GuiSubsystemImpl.cpp:1584-1635`; `Run::Stop` removes the tier synchronously
  (`RunImpl.cpp:92`). Checked 2026-11-15: not reachable. A script ends a run only through
  `run.requestExit`, which both hosts defer (the player's `ApplicationHost::RequestExit`, the Game
  tab's `m_exitRequested`), and a scene load waits for the safe point; nothing a GUI handler calls
  stops a run synchronously. Left in the backlog, to pin both if that changes.

### T1: the renderer and the RHI

- **More than 8 views alias the post passes' per-view slots.** `TonemapPassImpl.cpp:100`
  (`viewIndex % 8`), and FXAA, exposure, debug blit, debug draw and sky; a shared slot destroys a
  bind group recorded earlier that frame. Test: 9 views in one frame, view 0's group alive after
  view 8 records. Fix: slots grow with the frame's views. Built 2026-11-15, wider than listed: the frame's
  history slots (TAA, SSR, SSGI, exposure, previous camera) and the cluster build (which skipped
  views past the eighth) had the same cap; all grow now.
- **Probe capture can exhaust the mesh view ring.** `MeshRendererImpl.cpp:296` reserves by draws,
  not passes; each `Resolve` takes a slot (:1112). Test: 1 mesh and 1 probe capturing, the forward
  allocates. Fix: reserve passes (views x 6 faces for captures). Built 2026-11-15: the ring is a
  `ViewUniformRing` (one slot per distinct view block, as the sprite renderer's) plus the
  frame's capture faces.
- **An unbound material TextureCube gets the 2D white view.** `MaterialSystem.cppm:609` against a
  cube layout (:136). Test: such a material's bind group validates. Fix: a cube fallback. Built
  2026-11-15: a 1x1 white cube.
- **A camera may target any texture.** `ExtractImpl.cpp:557-569`. Test: a Sampled-only target is
  refused with a warning. Fix: check RenderTarget usage and a color format. Built 2026-11-15.
- **Light and camera fields are not validated.** `RenderComponents.cppm:296-299`,
  `ExtractImpl.cpp:581-595`. Test: nearZ 0 and farZ < nearZ give a finite projection. Fix: clamp at
  extraction. Built 2026-11-15, with the field of view, aspect, orthographic planes, light range
  and intensity, and a spot's inner cone.
- **Render graph culling compares subresources without their counts.** `RenderGraph.cppm:763`.
  Test: a reader of mip 2 (count 0, all) keeps a writer of mip 2. Fix: pass the real ranges.
  Built 2026-11-15.
- **DX12: a full descriptor heap hands out a null handle that is written.** `DxDevice.cppm:93-96`,
  `DxDescriptorHeap.cppm:59`, callers in `DxTextureView.cppm` and `DxSampler.cppm`. Test: 4097 SRV
  views give an error status. Fix: check, report; grow or page the heaps.
- **DX12: `base + ~0u` wraps, so a "rest of the mips" barrier emits nothing.**
  `DxCommandEncoder.cppm:279-281`. Test: baseMip 1, count ~0u emits mips - 1 barriers. Fix: resolve
  "all remaining" before adding.
- **DX12: a resize drops the tearing flag.** `DxSwapChain.cppm:149`. Test: Immediate mode, resize,
  Ok. Fix: pass the creation flags.
- **DX12: every bind group takes its own sampler slots** (2048). `DxBindGroup.cppm:65`. Test: 2049
  groups on one sampler succeed. Fix: dedupe sampler tables.
- **DX12: a buffer barrier trusts the caller's `oldState`.** `DxCommandEncoder.cppm:195`. Fix: use
  the tracked state.
- **Vulkan: an upload leaves the tracked layout stale, waits from TOP_OF_PIPE, and ignores the
  submit's result.** `VkTransferBatch.cppm:296-330, 113`. Test: after `WriteTexture`, the tracked
  layout is shader-read. Fix: set the tracked layout; real stages; check the result. Built
  2026-11-15.
- **Vulkan: a submit with a null fence drops its command buffers** (and a frame's acquire
  semaphore). `VkDevice.cppm:1155`. Test: `Submit(cbs, nullptr, 0)` runs them. Fix: submit
  without a fence. Built 2026-11-15.
- **Vulkan: descriptor pools of both kinds share a list.** `VkDescriptorPoolManager.cppm:45-61`.
  Fix: one list per flag set. Built 2026-11-15.
- **More than 256 lights in a view are cut in extraction order.** `ClusterSystemImpl.cpp:164-168`,
  `MeshRendererImpl.cpp:1064`. Test: 257 lights, only the last bright, the pixel is lit. Fix: rank
  by contribution to the view before cutting. Built 2026-11-15: `ExtractedScene::RankLights` past
  `kMaxLightsPerView`, from the view origin.
- **TAA and AO ignore a view's sub-rectangle.** `PipelineImpl.cpp:2226, 2247`. Test: split-screen
  AO matches a half-width view. Fix: pass the viewport as SSR's call does.
- **The cascade fit clips tall casters.** `ShadowSystem.cppm:126-130`, no depth clamp. Test: a
  caster 4x the radius toward the light appears in the cascade. Fix: depth clamp (or pancaking)
  for shadow pipelines. Built 2026-11-15 (depth clamp, mesh and terrain casters).
- **IBL rebakes whole on any sun change, even for HDR skies.** `IBLSystemImpl.cpp:165-170`. Test:
  an HDR sky's sun moves, the IBL generation holds. Fix: only a procedural sky depends on the sun;
  a threshold. Built 2026-11-15 (procedural and analytic skies; half a degree).
- **An engine `.hlsli` change does not rebuild project shaders.** `FileShaderSourceProvider.cppm:173-181`.
  Fix: track includes per registered source. Built 2026-11-15: a registered source that
  includes anything rebuilds on any include change (its text is kept).
- **Cluster index buffers take about 50 MB per view and frame slot at 1080p.**
  `ClusterSystemImpl.cpp:255`. Fix: a compact list with offsets.

### T2: physics, navigation and animation

- **A navmesh of more than 256 tiles never loads.** `NavigationImpl.cpp:775-776`. Test: bake a
  17x17 grid, it loads. Fix: `DT_POLYREF64`, or tile and poly bits sized to the bake.
- **Trigger exit, sleep and sub-shape contacts.** `PhysicsWorldImpl.cpp:459-487`; the bridge
  `ScriptPhysicsContactBridge.cpp:38-45`. Tests: a body through a sensor raises onTriggerExit; a box
  settling raises no onContactEnd; a two-child compound raises one Begin. Fix: kind from the sensor
  flag; ignore removal of sleeping pairs; count per body pair.
- **Characters are invisible to raycasts, overlaps and each other.** `PhysicsWorldImpl.cpp:1082-1094`.
  Test: a raycast at a character hits. Fix: an inner body and character-vs-character collision.
- **A dynamic body cannot be teleported, runtime edits are ignored, primitives ignore scale.**
  `PhysicsWorldImpl.cpp:877`, `PhysicsSubsystem.cppm:739-769`, `PhysicsSubsystemImpl.cpp:222-240`.
  Tests: a moved transform moves the body; a changed shape rebuilds it; a box scaled x2 is hit at 2x.
- **Terrain collision ignores the entity's scale, and a hole's drawn rim has none.**
  `PhysicsSubsystemImpl.cpp:212-218`, `Terrain.cppm:155-157`. Tests: raycasts at a scaled terrain
  and at a rim. Fix: scale the heightfield; the same hole rule as rendering.
- **Physics budgets fail silently, and each world makes its own thread pool.**
  `PhysicsWorld.cppm:258-260`, `PhysicsSubsystem.cppm:929, 996`, `PhysicsWorldImpl.cpp:715-717, 862`.
  Test: 4097 bodies log why. Fix: say so once; one pool for every world.
- **A failed navigation target is swallowed.** `NavigationSubsystem.cppm:186`. Test: a target off
  the mesh finishes as failed. Fix: report it on the agent.
- **Clip events reach nothing.** `AnimationPlayer.cppm:132` has no caller. Test: a clip's event
  reaches a script handler. Fix: route player events to the entity's scripts.
- **glTF cubic-spline keys read the wrong element, and their tangents are dropped.**
  `GltfLoader.cppm:1081-1110`, `AnimConvert.cppm:270-274`, `ModelAnimation.cppm:111-113`. Test:
  key 0 equals output[1]; the curve passes through the tangent's slope. Fix: read 3k+1, keep and
  evaluate tangents.
- **Graph triggers are consumed every update; nothing transitions mid-blend.**
  `AnimationGraph.cppm:1067-1070, 1190`. Test: a trigger set during a crossfade fires.
- **Particles.** `ParticleEffect.cppm:66-69, 180, 408-410, 856-870, 937`. Tests: a one-shot stops
  playing; rate 30 at half detail spawns; the first step has no inherited velocity; no UB at x=-1.
  Also `ParticleRenderer.cppm:643, 762` never records the shader version.
- **Audio: a stolen looping autoplay never restarts; 2D stealing always takes slot 0.**
  `AudioSubsystem.cppm:338-343`, `AudioEngineImpl.cpp:1276-1325`.
- **Vegetation's version restarts after a cache reset.** `VegetationComponentsImpl.cpp:318-319`
  against `MeshRendererImpl.cpp:865`. Test: a rescatter rewrites the instance buffer.

### T3: input and runs

- **The web shell has no blur handler, and a mouse release outside the canvas is lost.**
  `WebInput.cppm:396-411`. Fix: blur and visibility release every key and button; mouseup on the
  window. (Checked in a browser; no native suite runs the web shell.)
- **Actions read a frame late, fixed-step edges drop or double, quick taps are lost, and every
  action reads pressed before the first update.** `ActionRuntime.cppm:536-540, 583-584, 684, 733,
  777, 1135`; `InputSubsystem.cppm:159`; `DefaultApplicationImpl.cpp:125`. Tests: one per symptom.
  Fix: update actions before scenes; per-step edges for the fixed lane; read the latch's Pressed
  and Released; start frames at 1.
- **SDL: removing a pad renumbers the others; the file dialog's callback runs on SDL's thread.**
  `SDL3ShellImpl.cpp:1021-1034, 1148-1165`.
- **A run's bus survives Stop; `SceneManager::Clear` frees scenes without stopping them; subsystems
  shut down before scenes; `maxSteps` is fixed at 4.** `RunImpl.cpp:85-103`,
  `SceneManager.cppm:46, 218, 243-253`, `ApplicationHost.cppm:294`.
- **`MarkDirty` dirties the whole root tree.** `SceneImpl.cpp:980-1003`. Test: moving A leaves its
  sibling B clean.
- **Plugin parts carry no ABI or build stamp.** `PluginPart.cppm:175-181`, `PluginLoader.cppm:70-90`.
  Test: a part with another stamp is refused. Fix: a stamp in the part, checked on load.

### T4: networking

- **A late joiner keeps authored entities destroyed before it joined.** `ReplicationImpl.cpp:1016-1022`.
  Test: destroy E, join, E is gone on the client. Fix: a joining peer's baseline starts from the
  authored set, so a missing one is sent as destroyed.
- **A scene change keeps replication's maps.** `NetworkManager.cppm:321`, `Replication.cppm:355-357`.
  Test: after swapping scenes, an old id finds nothing. Fix: reset the maps and baselines.
- **Every entity goes to every peer.** `SetRelevance` has no caller. Fix: a default relevance hook
  (distance) the game can replace.
- **The WebSocket gateway listens on every interface and parses a byte at a time with a rescan.**
  `NetworkControllerImpl.cpp:31`, `WebSocketGatewayImpl.cpp:449, 553, 1116`, `HttpImpl.cpp:296`.
  Fix: listen where the game's UDP port does; push whole reads; resume the scan where it stopped.
- **The rest:** the frame buffer grows without bound (`WebSocketGatewayImpl.cpp:334`); a header
  value can carry CRLF (`HttpImpl.cpp:895-910`); an unresolved host connects "successfully"
  (`NetworkManagerImpl.cpp:166`); 1xx is final (`HttpImpl.cpp:241`); UDP sets `SO_REUSEADDR`
  (`LinuxSystem.cpp:765`); the 32-bit authored id hash has no collision check
  (`ReplicationImpl.cpp:563-586`).

### T5: content and cook

- **A Reload while a decode is in flight is reverted by it.** `ResourceModule.cppm:684, 1091, 939`.
  Test: BindAsync, Reload, WaitAll, the product is the new one. Fix: complete or cancel pending
  work first, as Evict does.
- **Nothing is ever unloaded.** `ResourceModule.cppm:387`. Fix: release unreferenced products at
  collection.
- **The cook's change memo keys on size and whole seconds; a recipe lacks the product's data
  version.** `CookDriverImpl.cpp:481-482, 550-605`. Tests: a same-size rewrite in the same second
  recooks; a data version bump recooks.
- **Duplicate guids are accepted silently.** `ContentModule.cppm:446`. Test: two envelopes with one
  guid are reported.
- **The player opens every envelope at startup, copies entries twice and searches the TOC
  linearly.** `ContentModule.cppm:499`, `PakModule.cppm:92-102, 245`. Fix: lazy open, a view over
  the region, a hashed TOC.
- **glTF: only the first skin is imported; reimport identity is name and type.**
  `FileImport.cppm:1384, 1055-1057`, `Cook.cppm:361`.
- **XML drops whitespace-only strings.** `Document.cppm:629`.

### T6: text, VG and fonts

- **CJK text never wraps.** `TextLayout.cppm:1375`. Test: 150 CJK characters in 100 units wrap.
  Fix: break opportunities by Unicode line-break class (UAX 14, at least CJK ideographs and
  kana).
- **A child index goes stale under a hidden ancestor.** `Node.cppm:2150-2154, 3014`.
- **A percent size on an unbounded axis stops the relayout walk.** `LayoutImpl.cpp:36-41, 1188`.
- **SVG: `viewBox` ignored beside width and height; `fill-rule` never parsed; round and bevel joins
  keep the miter spike; `1e999`.** `VG.SVG/Loader.cppm:53, 767`, `Context.cppm:687`,
  `Tessellation.cppm:982`, `Parsers.cppm:757`.
- **A failed font bake is retried on every `GetFont`, and fonts are never evicted.**
  `TrueTypeFontService.cppm:121, 243`.
- **GraphView's "Add transition" is cancelled by its own release.** `GraphView.cppm:2201, 912`.
- **DockSpace rebuilds a kept entry on every `ShowLayout`.** `DockSpace.cppm:822-826`.
- **A multi-caret paste keeps a trailing CR.** `CodeEditor.cppm:3264`, `CodeDocument.cppm:521`.

### T7: Core and tooling

- `BinarySerializer` reads raw bytes into bool and enum; a Float4x4 count other than 16 desyncs
  (`BinarySerializer.cppm:81, 198-218`, `Serialize.cppm:166-176`).
- `RunProcess` drops arguments past 62, keeps 64 KiB of output, has no timeout
  (`LinuxSystem.cpp:245-252, 327-346`).
- Files of 2 GiB and more fail on Linux (one syscall) and Windows (a DWORD cast)
  (`LinuxSystem.cpp:425-435`, `Win32System.cpp:425, 435`).
- `PathParent("/file")` is empty; UTF-8 accepts overlong forms and surrogates; `FileOpen` lacks
  `O_CLOEXEC`; environment values past 1023 bytes are cut; `BoundedArray`s share one TypeId;
  `BoundedArray` get and set are unchecked.

## Left in the backlog (design, or low with no user effect)

Deferred destruction in the RHI, an error channel from `Submit`, pooled allocation, async uploads,
a persisted pipeline cache, a deeper RHI.Validation, RHI.DX12 tests of its own, `TypeRegistry`
reads without a lock (by contract), the two fixed-step accumulators and time scales,
`UpdateTransforms` scanning every slot, plugin subsystems removed by hand, the AsyncUpdate phase
documented parallel, the pak's double copy and missing checksums, the render graph's generation
and deletion ring, the particle sort per primary camera, reverb parameters written unlocked, audio
device changes, blend tree allocations, a zero navigation cell size, the code editor's O(N*M) line
operations and Replace All's limit, soft wrap's per-keystroke walk, `ItemView` and
`RemoveAllChildren` being O(n^2), overflow-wrap reshaping, undo copying the text, `@import` beyond
the GUI's sheet, docking with infinite weights, `Viewport::Resize` waiting idle, the prepass and
forward LOD pick, `PrepareDirtyInstances` unused, WebSocket masking seeded from ticks, DNS outside
the timeout.

## Gotchas

- Several fixes change behaviour a game may lean on (trigger exit, contact end on sleep, actions a
  frame earlier, a navigation target failing): each says so in the changelog.
- T1's DX12 items build and test only on Windows (CI's MSVC lanes); the Vulkan ones run on
  lavapipe in CI.
- The web shell's blur fix has no native suite; check it in a browser.
- Items marked suspected are confirmed before they are fixed.

## Tests (the contract)

Every item above names the test that fails today; it lands in the suite that owns the code (the
Integration suites for what crosses collections), and the phase is green on clang, gcc,
clang-shared, the wasm build and ASAN before the next begins.
