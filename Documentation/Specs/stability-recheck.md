# Stability: what the audit recheck found still open

> STATUS: APPROVED 2026-11-16 (user). Sized L, phases U0 to U6, each green on the lanes before the next.
> Origin: `Documentation/Audits/engine-audit-recheck-2026-11-16.md` (the 2026-11-11 audit reread
> against master after the leftovers series). Its rows were weighed again here on 2026-11-16 (master
> 5b01569a0): the defects with a trigger are in the phases, the design notes and unmeasured
> performance items stay in the backlog. Read CONVENTIONS.md first.

## Goal

What the recheck still finds open goes, worst first: an editor user losing work or undo before a
tool's exit code, a tool reaching outside its project before a latent RHI flag. Every fix lands
with a test that fails before it, in the suite that owns the code. Where no suite can reach the
code (a window, a Windows-only socket error), the item says so.

Not goals: the design notes (deferred destruction in the RHI, an error channel from `Submit`,
pooled allocation, one reliable stream, IPv4 only, client prediction, hot reload state migration,
the script surface as the whole registry, two persistence channels, Play from the saved scene) and
the unmeasured performance items (set 0 rebuilt with shadows on, the hierarchy rebuilt per
revision, the code editor's line operations, the player's envelope scan). They stay in the backlog
with what the recheck said of them.

## Weighed again: not to do, or done

- **Web streamed audio does not stall.** The recheck (Simulation 20) found no
  `process_next_job` call under `Code/`; miniaudio makes it itself: on Emscripten with
  `MA_RESOURCE_MANAGER_FLAG_NO_THREADING` the engine's data callback processes one job a call
  (`miniaudio.h:77491-77498`). It stays unrun in a browser; the mechanism is there.
- **Done since the recheck, from it:** a crowded cluster group lost every light past its region's
  end (49deed3aa: equal shares, directional lights first); two carets on one line cut two lines
  (5b01569a0).
- **The leftovers spec was wrong on two of its "no longer to do" rows,** and both are taken here:
  glTF tangents are never generated (the loader always declares a tangent stream), and the
  JobSystem leaves a job scheduled in one window at shutdown unrun.
- **Enum values are not checked on read** (Core 9b): left as decided in the leftovers series (engine
  enums have a fixed underlying type; a check against the enumerators refuses combined flags).

## Phases

Each item: what is wrong, where (the recheck's lines), the test that fails now, the fix.

### U0: the editor loses work or undo

- **A dirty script page saves over a file changed outside the editor, with no warning.**
  `Editor.App/CodeAssetPage.cppm:122-139` compares the text with what it last read, never the file;
  `OnAssetExternallyModified` is the base's, which rereads the asset object, not the source. Test:
  open a script, edit it, change the file on disk, Save: refused (or asked) and the disk text kept.
  Fix: the page notes the file's content hash when it reads or writes it; an outside change marks it
  (a clean page reloads, a dirty one shows the conflict), and Save over a changed file asks.
- **Prefab Apply and Revert bypass the command stack and leave the page clean.**
  `Editor.Scene/PrefabFlows.cppm:126-155, 170-198`. Test: Revert, then Undo restores the override;
  the page is dirty after Apply. Fix: both as commands (one step each).
- **Inspector edits of one type coalesce across separate actions** (add X then add Y is one undo).
  `Editor.Scene/InspectorViewImpl.cpp:758-762, 823-826, 870-874` never lock their groups. Test: two
  adds, one Undo takes one away. Fix: lock each discrete action's group.
- **Gizmo, Delete and Duplicate act on the primary selection only; the scale clamp removes negative
  scale.** `GizmoImpl.cpp:102-104, 178, 302-304`, `SceneActions.cppm:433-448`. Tests: move three
  selected entities by the gizmo, all move (in one command); Delete and Duplicate take all; a
  mirrored entity scaled by the gizmo keeps its sign. Fix: act on the selection's roots (no child
  twice); clamp the magnitude, not the sign.
- **A generic asset page has no external refresh.** `Editor.Generic/AssetFormPage.cppm:98-108`.
  Test: change the asset on disk, the page shows it (clean) or warns (dirty). Fix: as
  AssetObjectPage does (`AssetObjectPage.cppm:199-209`).
- **Unreadable editor settings that cannot be set aside are replaced on the next save.**
  `Editor.Core/EditorSettings.cppm:574-577`. Fix: refuse to save over them, as export presets do,
  and say so.
- **Two assets can differ only in case in one group** ("Rock" and "rock"), which a case-insensitive
  file system makes one file. `Content/ContentModuleImpl.cpp:187-198`. Test: creating the second is
  refused. Fix: compare case-folded where names are claimed.

### U1: tools that reach too far or say too little

- **MCP tools are not confined to the project.** `Editor.Mcp/ProjectTools.cppm:127, 166, 364`
  (`directory`, `source`), `EditorWindowToolsImpl.cpp:93` (screenshot `path`). Test: a path outside
  the project (and a `..` escape) is refused. Fix: resolve every path argument and require it under
  the project (sources to import: under the project or an import root the user named); the tools
  that must read outside (an import from Downloads) say so in their annotations
  (`openWorldHint`, which `Mcp/Server.cppm:128-143` always sets false).
- **Tools.Cook exits 0 on an unreadable source.** `Tools/Tools.Cook/Main.cpp:140-193` never reads
  `plan.unreadable`. Test: a source that cannot be read gives a non-zero exit and names it.
- **The MCP tool table can change under a call (suspected).** `Mcp/Server.cppm:589-613` keeps a
  pointer into `m_tools` across the handler. Confirm first: a tool whose call unloads a plugin (and
  so unregisters tools). Fix if real: copy the handler before the call, or defer table changes to
  after it.
- **A Luau constructor's error leaks its arguments.** `Script.Luau/LuauScript.cppm:1950-1966`
  raises with `Array<Variant> args` and a `ScriptCallScope` alive (longjmp skips their destructors).
  Test (ASAN, leaks): a constructor call that matches no constructor, in a loop. Fix: as the method
  thunk does, close them in an inner block before `lua_error`.

### U2: networking

- **Windows UDP receive stops on any error, a connection reset included.**
  `Core/System/Win32/Win32System.cpp:961-964`: one ICMP port-unreachable (a client that left) ends
  the frame's receive loop with the rest still queued. Fix: `SIO_UDP_CONNRESET` off on the socket,
  and a reset read as no data. Test on the Windows lanes: a send to a closed port, then a datagram
  from a live peer is read in the same pump.
- **The WebSocket client writes its address's path and host into the upgrade request unchecked.**
  `WebSocketGatewayImpl.cpp:1229-1232`, `ParseWebSocketAddress` (:826). Test: a ws:// address with a
  line break in its path is refused. Fix: the header check the HTTP clients use.
- **The WebSocket masking key and handshake nonce are seeded from ticks.**
  `WebSocketGatewayImpl.cpp:1040`. Fix: `Random::FromSystemEntropy()`, as the transport does.
- **The gateway's 64 clients and the physics budgets are fixed constants with no setting.**
  `WebSocketGatewayImpl.cpp:475`, `Physics/PhysicsWorld.cppm:258-260`. Fix: from the project's
  network and physics settings, the defaults as today.

### U3: import and content

- **glTF tangents are never generated.** The generator runs only when a mesh has no Tangent element
  (`ModelImporter/MeshConvert.cppm:471-479`), but the glTF loader always declares one and fills
  (1,0,0,1) when the file has none (`Model.GLTF/GltfLoader.cppm:622-625, 797-810`);
  `ModelMesh::hasTangents()` has no reader. Test: DamagedHelmet-shaped data (no TANGENT) imports
  with tangents perpendicular to the normals. Fix: generate when the source had none (the loader
  sets `hasTangents`); check the FBX loader's block the same way.
- **A navigation bake with a zero cell size divides by zero.** `Navigation/NavigationImpl.cpp:116-120`,
  `Editor.Navigation/NavigationBakeImpl.cpp:275`; the property has no range. Test: a bake with cell
  size 0 is refused. Fix: a minimum on the property and a check in the bake.

### U4: rendering

- **Two scenes drawn in one frame share motion-vector history and material override instances.**
  `MeshRendererImpl.cpp:3228-3237` (previous world matrices by entity index), `:2358-2367`
  (overrides by entity id). Fix: key them as 26523363b keyed the instanced sets (scene serial and
  entity). Test: two scenes, one entity handle each, distinct overrides stay distinct.
- **Light fields have no finite check** (camera fields do). `ExtractImpl.cpp:613-621`. Test: a NaN
  intensity or position draws nothing from that light rather than NaN-ing the frame. Fix: as the
  camera's (`RenderComponents.cppm:300-330`).
- **`MaterialSystem::PrepareDirtyInstances` is never called,** so its dirty list only grows and a
  released instance leaves a null entry. `Materials/MaterialSystem.cppm:212-221, 245, 722-733`. Fix:
  call it where materials are prepared each frame, or remove the list if nothing needs it (read
  which first).
- **Vulkan results still unchecked:** `vkCreateSemaphore` (`VkSwapChain.cppm:592-593`), queue submits
  beyond DEVICE_LOST (`VkDevice.cppm:1142, 1230`), begin and end of command buffers
  (`VkTransferBatch.cppm:278, 349`, `VkCommandEncoder.cppm:562, 911, 977`), surface queries
  (`VkSwapChain.cppm:473-475, 530-532`). Fix: report each, once, as b80dfc6e9 does.
- **DX12, seen in the recheck:** `DxSamplerTables` takes no lock while the heap under it does (no
  bind group is made off the render thread today); `UpdateBindless` on a plain sampler range of a
  group with a shared table stops before the shader-visible copy; the callers of a view's null
  descriptor handle were not read. Fix: a lock; a group detaches to a private table before such an
  update; the callers checked. Tests on the MSVC lanes.
- **Latent RHI and graph flags:** the graph's deletion ring takes its size from its own default,
  not the device's frames in flight (`RenderGraph.cppm:52-56`); a resource's generation never moves
  (`RenderGraphResource.cppm:168`); `ReadWriteStorage` resolves to a read-only layout
  (`VkBarrierHelper.cppm:188-197`); the cube-compatible flag goes on any 6-layer 2D texture
  (`VkTexture.cppm:54-55`). Each with a test of its own case.

### U5: Core and threading

- **Type registry readers take no lock while writers do.** `Core/RTTI/TypeRegistry.cppm:58, 95`
  against `:138-146`. Plugins register types at load while other threads may look types up. Fix: a
  reader-writer lock (or registration only while nothing else runs, asserted). Test: lookups on
  workers while a plugin loads (TSan-shaped; ASAN at least sees no corruption).
- **The JobSystem leaves a job unrun at shutdown** when it is scheduled between a worker's failed
  run and its stop check (`Core/Threading/JobSystem.cppm:446-451`). Fix: recheck `HasAnyWork()`
  before returning on stop.
- **Reflected `Array<T>` get and set index unchecked** (the assert is compiled out in Shipping).
  `Core/Reflection/Reflection.cppm:1095-1105, 1187`. Fix: as the bounded arrays' (3683f4b0a).
- **A read array reserves by count times element size** up to the bytes left
  (`Core/Serialization/Serialize.cppm:220, 233`). Fix: reserve what the stream can hold in
  elements of that size.
- **`UniquePtr`'s converting move does not require a virtual destructor.** `Core/UniquePtr.cppm:36-44`.
  Fix: a static check.

### U6: GUI, audio and the rest

- **Docking weights that are not numbers pass validation.** `GUI.Docking/DockLayout.cppm:1123-1135`.
  Test: a layout file with a NaN weight is refused (or normalised). Fix: an `IsFinite` check.
- **The GUI's input pump holds raw root and bridge pointers across handlers** that could destroy the
  scene (`Engine.GUI/GuiSubsystemImpl.cpp:1581-1641`). No such path was found; the fix is a guard
  (the tier is removed after the pump, as the scene manager defers its destroys).
- **Reverb parameters are written unlocked while the audio thread reads them.**
  `Audio/AudioReverb.cppm:77-83`. Fix: an atomic snapshot the callback copies.
- **A blend tree allocates on every evaluation.** `Animation/AnimationGraph.cppm:294-297, 435-459`.
  Fix: scratch kept on the node.
- **Replace All stops at 100,000 matches without saying so.** `GUI.Code/CodeEditor.cppm:2036,
  3141-3151`. Fix: say so (or replace past it).

## Left in the backlog

Everything the recheck marks `D` or "Design", the unmeasured performance items above, the import of
every skin and a reimport identity (already there, 2026-11-16), the gateway listening on every
interface (as UDP does), a packet-level replay window, DNS outside the timeout, an admission hook,
the profiler's 300-frame capture, the two fixed-step accumulators and time scales, plugin
subsystems removed by hand, particles sorted by the primary camera, audio device changes, the
ItemView, RemoveAllChildren and overflow-wrap costs, undo copying the text, `@import` beyond the
GUI's sheet, `Viewport::Resize` waiting idle, the prepass and forward LOD pick, the spline seam
(the recheck could not find it).
