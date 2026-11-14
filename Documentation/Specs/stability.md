# Stability

> STATUS: APPROVED 2026-11-13 (user: "write up the spec and work through it on master"). Sized L, in
> phases S0 to S8, each green on the lanes before the next.
> Origin: a read-only source audit of 2026-11-11 (eleven readers over the tree, nothing built or
> run). Its 61 findings in the areas below were re-checked against master at a0c59a159 on
> 2026-11-13, each read again in source with a failing test and a fix named: 58 hold as stated
> and three in part (one of those, Android's CA roots, only on the `android` branch, to be fixed
> there). This spec is the re-checked list. Read CONVENTIONS.md first.

## Goal

Nothing a user does in the editor, and nothing a game's scripts or a peer sends, loses work,
corrupts memory or hangs. Every fix lands with a test that fails before it (deterministically, or
under ASAN where the bug is a use-after-free), in the suite that owns the code.

Not goals: autosave and crash recovery, an allocation-failure policy, sandboxing MCP tools to the
project, and the audit's unread areas (RHI.Null, most backend pipeline code, Windows beyond file
I/O, several editor domains, Android and web export packaging). Those stay in `backlog-0.2.md`
under "Stability: left over".

## Decided (by this spec; change on review)

1. **One run-scoped mutation queue**, not a fix per call site. Destruction is deferred only inside
   `Scene::Update` and `SceneManager::Update` today; scripts also run in the fixed step, the run
   bus drain, destroy hooks, the network pump and GUI input, where mutation is immediate. The
   queue covers entity destroy, component remove, scene destroy and adopt, network stop and
   connect, and window close, all enqueued while a re-entrancy depth is above zero and flushed by
   the frame driver between phases.
2. **A synchronous `run.loadScene` adopts at the safe point** the asynchronous one already uses
   (`PumpScriptLoads`), so a script never frees the scene it runs in.
3. **No borrowed pointers in component data or script handles.** A script handle resolves its
   scene each call through the scene's life (`WeakSceneRef`, cleared as the scene is destroyed;
   not through the run, since scenes outside every run run behaviours too); a mesh asks the animation system
   for its palette at extract; the inspector holds re-resolving refs, as `MakeComponentRef`
   already does.
4. **A save never truncates in place**: write a unique temp file beside the target, flush it to
   disk, rename over. A store that failed to read is never written back: it is quarantined
   (renamed aside, with a notice) or read-only. Defaults replace a file only when it does not
   exist.
5. **Save during Simulate saves the pre-Simulate scene** (from the snapshot Simulate already
   keeps), so Ctrl+S keeps working; Discard during Simulate stops Simulate first.
6. **A fixed budget that overflows grows next frame or says so once**; nothing is dropped
   silently.
7. **Script budgets**: a running game's calls into script run under a generous budget that
   reports and aborts the call, not the process. The cook and `script_validate` stay
   compile-only (`Check` never runs a script, by design: it checks code as it is typed), so a top
   level that never ends is stopped when the game loads it, its error naming the script.
   (Revised 2026-11-13: running top levels at cook and validation was dropped.)

## What exists (checked 2026-11-13)

- `Core::WriteFileAtomic` (`Core/IO/FileSystem.cppm`): one user (`RunSaveImpl.cpp`), a fixed
  `<path>.tmp`, no flush. `NativeFileSystem::Save` truncates in place; every content database
  write goes through it, so fixing it fixes asset and scene saves at once.
- The plugin settings guard (`SettingsRead`, `PluginPart.cppm`): the pattern S0 extends to the
  other stores.
- Deferred destroy inside `Scene::Update` (`m_isUpdating`), `SceneManager::Update` (`m_updating`)
  and GUI `Node::Close` (deferred `RemoveFromParent`): the mechanisms S2 generalises.
- `MakeComponentRef` (`ScriptFacades.cppm`): a component ref that re-resolves through its
  manager; S3's inspector refs follow it.
- The asynchronous scene load adopted in `PumpScriptLoads`: S2's safe point.
- `SceneSimulation`'s snapshot: S1's save source.
- `PipelineConfig::operator==` exists and the pipeline cache does not use it.

## Phases

Each item names its finding, where it is on master, the test that proves it, and the fix.

**S0: files and content never lose data.**
- Sidecars matched by `"<name>."` prefix: delete, rename, move, clone and copy-forward in
  `Content/ContentModuleImpl.cpp` (173-199 `FirstFreeName` makes `"Rock.2"`; 549-596, 624-668,
  698-727, 774-793, 870-895), and the export prune `FileOwnerReachable`
  (`Editor.Project/Export.cppm:200-222`; a stripped shader `Water` also drops `Water.2`). One
  helper `IsSidecarOf(name, file)`: the name, a dot, a stream name with no dot, a known suffix.
  `WriteData`/`ReadData` refuse a stream name with a dot. Content.Tests (Rock and Rock.2 through
  delete, rename, move, clone), Editor.Core.Tests ExportTests (the prune).
- Saves truncate in place (`VFS/NativeFileSystem.cppm:233-245`): temp, flush, rename in `Save` and
  `WriteFileAtomic` (unique temp name, `ReplaceFile` on Windows); the change watcher ignores temp
  names; a private-mode option for secrets, used for the MCP token (written 0644 today,
  `EditorMcpHostImpl.cpp:88-97`). VFS.Tests (a held read handle still reads the old bytes),
  Editor.App.Tests (the token is 0600 on Linux).
- XML parse failures ignored (`Xml.Serialization/XmlSerializer.cppm:713-717`, `(void)pr`): the
  serializer fails on a parse error or a short read. This is the root of the loaders below
  believing a damaged file. Xml.Serialization.Tests.
- Unreadable stores written back: the player save (`RunSaveImpl.cpp:23-67`, quarantined),
  the model viewer's library (`ModelStore.cppm:613-638`, read-only), export presets
  (`ExportController.cppm:37-44`, saved on every dialog close) and editor settings
  (`EditorSettings.cppm:550-559`): defaults only on NotFound, else the `SettingsRead` guard.
  Engine.GameInstance.Tests (extends "survives a bad file"), ModelViewer.Tests, Editor.App.Tests,
  Editor.Core.Tests.
- Paths taken from data: Remove and Import Template join an id from template.xml
  (`ExportTemplateImpl.cpp:126-163`; one path segment only, Remove deletes the scanned directory);
  `CreateLinkedTextAsset` writes `Sources/<name><ext>` flat and truncating while names are unique
  per group, and MCP `asset_create` skips `AssetNameRefusal` (`Pipeline.Core/Asset.cppm:330-357`,
  `ProjectOperations.cppm:172-183`; refuse the name, pick a free file, never overwrite); a glTF
  `uri` that is absolute escapes `CopyGltfSidecars` (`FileImport.cppm:928-1035`; reject absolute,
  drive, UNC, `..`, and check the result stays under the sources root). Editor.Core.Tests,
  Editor.App.Tests, ModelImporter.Tests.

**S1: the editor never loses work.**
- Save during Simulate writes simulated state (`ScenePageImpl.cpp:693-739`): saves the snapshot
  (Decided 5). Editor.Scene.Tests.
- Discard during Simulate hangs (`Page.cppm:109-117` loops on `CanUndo` while `Undo` refuses when
  locked): `CanUndo` is false when locked, the scene page stops Simulate first. Editor.Core.Tests.
- Discard undoes past the last save and marks clean: the command stack keeps a saved index
  (invalidated when truncated past), Discard returns to it or reloads from source, dirty derives
  from it. Editor.Core.Tests.
- Terrain edits (`EditorPages.cppm:193-267, 512-526`; `SculptImpl.cpp:340-380`,
  `HoleToolImpl.cpp:278-320`): Save All and close-Save drain pending asset edits; one heightfield
  persist for every tool (envelope, heights, holes); pending edits belong to their page and are
  dropped, the grid reverted, on Discard. Editor.Terrain.Tests, Editor.App.Tests.
- The editor export ignores cook failures (`EditorExport.cppm:65-92, 237-250`): wait for `IsIdle`,
  abort with a notice on failed or unreadable items (a source with a builder that does not read;
  "unbuildable" also counts scenes and prefabs, which have no builder by design). Editor.App.Tests.
- A failed nav bake writes an empty navmesh (`NavigationBakeImpl.cpp:158-210`): write only a
  baked result. Editor.Navigation.Tests.

**S2: one mutation queue for a run.**
- The queue and its depth (Decided 1), owned by the run, flushed after the fixed lane, the run
  bus drain, the network pump and GUI input.
- `run.loadScene` from the fixed step or a bus handler frees the running scene
  (`SceneManager.cppm:141-196`, `GameInstanceImpl.cpp:355-369`): adopted at `PumpScriptLoads`
  (Decided 2). Scene.Tests, Engine.GameInstance.Tests.
- EventBus unsubscribe and subscribe during Drain (`EventBus.cppm:58-102`; reached through
  `OnSceneStopped`): dead entries marked, new ones pending, compacted after. Messaging.Tests.
- `Net.disconnect` in a handler frees the manager mid-pump (`NetworkManager.cppm:183-237`,
  `NetworkControllerImpl.cpp:88-96`): stop and connect enqueued. Integration.ScriptFacades.
- Closing a floating window from input inside it (`GuiHost.cppm:351-465, 893-965`): close
  enqueued while routing. GUI.Docking.Tests.
- Script handles hold a raw `Scene*` (`ScriptFacades.cppm:119-132, 405, 449, 464, 653`): resolve
  through the run (Decided 3). Engine.DefaultApp.Tests or Engine.GameInstance.Tests.

**S3: re-entrancy and borrowed pointers.**
- `DestroyEntityImmediate` destroys a stale sibling (`SceneImpl.cpp:843-872`): validity check,
  loop on `firstChild`. Scene.Tests.
- `RemoveComponent` reuses its index after the hook (`Component.cppm:129-148`): unlink, then call
  the hook on the moved-out value. Scene.Tests.
- Luau `AdvanceCoroutines` holds a reference across `lua_resume` (`LuauScript.cppm:2132-2316`):
  pending starts, swept cancels, re-index after resume. Script.Luau.Tests.
- `AddChild` makes a cycle; `popup(ancestor)` reaches it (`Node.cppm:221-238`,
  `GUIReflectionImpl.cpp:621-636`): refuse an ancestor. GUI.Tests, Integration.GuiScript.
- Menu and ListBox keep freed rows after a script destroys one (`Menu.cppm:1238`,
  `ListBox.cppm:314`): prune in `OnChildrenChanged`. GUI.Tests.
- `SetFocusNode` re-entered from FocusLost leaves `:focus-within` stuck
  (`EventDispatcher.cppm:924-950`): a pending request settled in a loop. GUI.Tests.
- A failed Play start leaves debug listeners on a closed tab (`GamePageImpl.cpp:462-549, 917-955`):
  one teardown for Stop and every failed start. Editor.Play.Tests.
- The inspector caches raw component pointers; component add and remove do not move its signature
  (`InspectorViewImpl.cpp:287-305, 502, 662-671`): re-resolving refs, and a per-manager structural
  revision. Editor.Scene.Tests.
- A mesh keeps its animator's bone matrices after the animator goes
  (`AnimationComponents.cppm:312-318, 400-410, 497-503, 566-576, 757-770`; `ExtractImpl.cpp:79-84`):
  extract asks for the palette (Decided 3). Engine.Animation.Tests, Engine.Render.Tests.
- An instanced set read in place keeps its version (`RenderComponents.cppm:518-532`,
  `ExtractImpl.cpp:252-339`): bump on read, recompose on a size change. Engine.Render.Tests.
- A kinematic body over a mesh, plane or heightfield is made static, then moved (Jolt dereferences
  null; `PhysicsSubsystem.cppm:255-269`, `PhysicsWorldImpl.cpp:751-887`): the subsystem drives the
  body's effective motion, `MoveKinematic` refuses a body that is not kinematic.
  Engine.Physics.Tests.

**S4: Core.**
- Growth frees before reading an aliasing argument: `Array::PushBack/Resize/Insert`,
  `String::Append/Insert` (also wrong without growth), `HashMap::InsertOrAssign` (rehashes before
  the lookup), `UniquePtr::operator=` (`head = Move(head->next)`). Build first, then free; look up
  before growing. Core.Tests under ASAN.
- The JobSystem worker slot is one `thread_local` for every pool (`JobSystemImpl.cpp:25-29`):
  store the owner with the slot. Core.Tests (pools of 6 and 2).
- `FixedStepper` loops forever on a huge or infinite delta (`FixedStepper.cppm:40-44`): ignore
  non-finite, drop whole steps past `maxSteps` at once. Core.Tests.

**S5: what the renderer draws.**
- Instanced runs compare slot 0's material only (`MeshRendererImpl.cpp:1196-1211, 1308-1316`,
  `ExtractImpl.cpp:78`): the submesh materials join the run predicate and the batch key.
- Skinned-ness from the run's head (`MeshRendererImpl.cpp:1489-1531, 1689`): per item, breaking the
  run.
- Shadow casters and the depth prepass always cull back faces (`MeshRendererImpl.cpp:2044-2106`):
  the material's cull mode.
- Skinned meshes culled by the bind-pose sphere (`ExtractImpl.cpp:66-69`): pose bounds from the
  bones (per-bone boxes at import).
- The GPU mesh cache serves stale vertices after an in-place rebuild and never frees
  (`GpuMesh.cppm:93-166`): a content version on the mesh; retire and reuse ranges.
- Reflection probe slots never released, 16 per process (`ReflectionProbeSystemImpl.cpp:356-368`):
  reclaim unseen slots; key by scene and entity.
- The static shadow layer is cleared whole while only dirty tiles redraw (`PipelineImpl.cpp:1505`):
  load the layer, clear each dirty tile. Render.Backend.Tests.
- Ambient SH L20 three times too strong (`lighting.hlsli:54`, `ssgi_resolve.ps.hlsl:64`):
  `0.247708 * (3z^2 - 1)`, one shared evaluator. A visible change: the sky's diffuse light on a
  surface changes with how the sky varies along world z (front to back: the SH's z axis is
  horizontal in the Y-up world, so it is not the vertical gradient); the user checks the samples.
  Render.Backend.Tests (an environment varying with z).
- The render graph orders a reader after its nearest writer only (`RenderGraph.cppm:804-839`;
  latent): every overlapping writer until the range is covered. RenderGraph.Tests.
- The pipeline cache is keyed by hash alone and retries a failed build per draw
  (`PipelineStateCache.cppm:57-128`): an owned key with equality, failures cached by shader
  version, `ReleaseRetired` from the frame ring. Materials.PipelineCache.Tests.
- CPU-side tests in Render.Tests and Engine.Render.Tests over `NullDevice`, as
  `MultiMeshLifecycleTests.cpp` does, unless marked.

**S6: budgets.**
- The light ring takes 256 per `Resolve` call, and an overflow reads another view's lights
  (`MeshRendererImpl.cpp:297, 1058-1077`; `lighting.hlsli:313-315`): one upload per view and
  frame.
- Sprites and particles take a view slot per run, 8 per frame (`SpriteRendererImpl.cpp:111-135`,
  `ParticleRenderer.cppm:255-422`): one slot per view and frame.
- The terrain chunk ring cannot grow (`TerrainRenderer.cppm:570-943`): count demand, not
  successes.
- The vector renderer never grows on index overflow (`VG.Renderer/Renderer.cppm:326-354`); its
  clipped stencil cover paints outside the clip (`:1518-1531`, `Less` against 0x80).
  VG.Renderer.Tests, VG.Backend.Tests.
- Every remaining fixed ring reports its first overflow once (Decided 6).

**S7: untrusted input.**
- Network: a cookie handshake and `maxPeers` before a peer exists; the session token in data
  packets; backoff while the round trip is unsampled (`Reliable.cppm:333-460, 599-702`); a
  fragment count checked against `maxMessageBytes`, a receive window for reordering (`:900-963`);
  a cap and a window on unacknowledged reliable messages, whose u16 id wraps past 32768 today; a
  zero-length datagram is data, not "no data" (`UdpSocket.cppm:96-101`, `LinuxSystem.cpp:775-797`,
  `Win32System.cpp:827-850`); replication lengths checked against what is left
  (`ReplicationImpl.cpp:46-57, 767-773`, `WireStream.cppm:219-225`); TLS send backpressure
  (`NetTlsImpl.cpp:80-185`); WebSocket handshake and idle deadlines and a disconnect when the
  transport drops a client (`WebSocketGatewayImpl.cpp:447-734`); a redirect to another origin
  drops Authorization, Proxy-Authorization and Cookie (`HttpClientImpl.cpp:416-497`). Net.Tests,
  Net.Replication.Tests, Net.Tls tests, Net.WebSocket.Tests, Http.Client.Tests.
- Scripts: Luau boxes tagged and the self type checked; `newproxy` removed
  (`LuauScript.cppm:974-1393, 2382`); budgets (Decided 7: Luau interrupt and allocator cap,
  AngelScript line callback and stack limits). Script.Luau.Tests, the cook tests,
  Integration.Mcp (`script_validate` on `while true do end`).
- SVG: a number after `Z` is an error and the path loop always advances
  (`VG.SVG/Parsers.cppm:516-684`); self-closing tags detected (`Loader.cppm:187, 578-587`); a
  nesting limit. VG.SVG.Tests.
- glTF: a validation failure fails the load (`GltfLoader.cppm:121-126`). Model.GLTF.Tests.
- Fonts: the sfnt table directory checked before stb_truetype (`TrueTypeFont.cppm:38-51`).
  Fonts.TrueType.Tests.
- XML: a nesting limit (`Xml/Document.cppm:412, 571`). Xml.Tests.
- Style sheets (added 2026-11-13 from the GUI comparison, open at 6056a2f0e): with
  `grid-auto-flow: column`, an item whose `grid-row` lies past the explicit rows loops for ever in
  `PlaceGridItems` (`GUI/Layout/LayoutImpl.cpp:505-506, 538-539`), and with both lines given writes
  out of bounds (`:493, :532`); grid line numbers have no upper bound (`StyleApplier.cppm:313-327`;
  `grid-column: 100000000` allocates that many tracks); nested `@media` and `:not()`/`:is()` recurse
  without a limit (`CSSParser.cppm:225`, `StyleSelector.cppm:1049-1052`); `:nth-*` parsing overflows
  a signed integer (`StyleSelector.cppm:631`); `var()` expansion has a depth cap but no size cap
  (`StyleValue.cppm:103-155`); a stray `}` drops the rest of the sheet (`CSSParser.cppm:253`).
  GUI.Tests.
- GUI events: the wheel path uses the hit node after routing without the pin PointerUp takes
  (`EventDispatcher.cppm:501-505` against `:1689-1695`); reported for pointer down and move,
  `Node::SendEvent` and `RunListeners` too (to re-read). GUI.Tests.
- Not on master: TLS reads the Linux CA bundle paths on Android (`NetTlsImpl.cpp:53-73`), so every
  https and wss connection fails there. To fix on the `android` branch: read the system store,
  `/system/etc/security/cacerts`, and the user-added one.

**S8: the swapchain.**
- A failed rebuild leaves empty semaphore arrays; its status is ignored; out-of-date at an
  unchanged size never rebuilds (`VkSwapChain.cppm:337, 610-616`, `Graphics.cppm:594-706`): build
  the new chain first and swap on success, a distinct OutOfDate status that marks the window for
  rebuild. Graphics.Tests over a NullSwapChain fault hook.
- Acquire and present semaphores latched device-wide and taken by the first fenced submit
  (`VkDevice.cppm:981-1222`; `GraphProfiler::SyncPoint` takes it when timestamps are uncalibrated,
  so on the Pixel 6): the submit that renders the backbuffer names its swapchain.
  RHI.Vulkan.Tests, and an RHI.Validation check.

## Gotchas

- An ASAN-only test must still fail without ASAN where it can (an assert, a count, a validity
  check); the lanes run Debug suites without the sanitizer.
- Stream names with a dot: the terrain's four palette arrays had them ("palette.normal" and on);
  they were renamed and the terrain builder's version bumped, so cooked terrain rebuilds. Sources
  never held them (the cook writes them), so no source data migrates.
- Atomic rename over a file an editor watches fires the watcher on the temp name and the target;
  the watcher ignores temp names or the editor reloads its own save.
- A set-aside store keeps every byte under "<file>.unreadable" (numbered when taken) and the log
  names it; a store from a newer build that this one cannot read is set aside too, which the newer
  build undoes by renaming it back. Telling a game its save was set aside (a run event) is not in
  this spec.
- The mutation queue changes when a script sees a destroyed entity vanish (end of the phase, not
  at once); `isValid` stays true until the flush, as inside `Scene::Update` today.
- Sedulous ports this series; every phase's commits go to it with the hashes.

## Tests (the contract)

Listed per item above. Every phase is green on clang, gcc and clang-shared before the next starts;
the wasm build and ASAN run at the end of the day or of the series, and the ASAN lane is the proof
for the use-after-free items.
