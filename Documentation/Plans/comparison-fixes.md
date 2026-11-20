# Fixes from the engine comparison of 2026-11-19

> The defects the comparison found in our engine, and the improvements it recommended, as one list
> to work through. The comparison itself stays local (`Documentation/Comparisons/`); each item
> here stands on its own, with the code it concerns. "Checked" means the claim was rechecked
> in our code; "reviewer" means it was not, so the first step is to confirm it.
>
> Status: **open**, **started** (name the commit or branch), **done** (name the commit; then move
> the line to the week it shipped), **dropped** (say why). Size is a guess: S (a day or less),
> M (a few days), L (its own spec).

## Defects

| # | Status | Size | Defect | Where | Found |
|---|---|---|---|---|---|
| D1 | done (a211f51fb) | S | GUI cleanup calls `WaitIdle` once per removed render texture, inside the frame's recording. On the web a mid-frame wait pumps the event loop and likely drops the frame's submit. Resizing a world panel waits the same way. Fix: retire the target's textures and views through the render subsystem's `GpuRetireQueue` (frames in flight + 1); `VGPassTargets` needs a retire path beside `Destroy()`; keep `WaitIdle` only as the no-queue fallback and in the destructor. Test: removing and resizing panels never idles a device that counts its waits. | `Engine.GUI/GuiSubsystemImpl.cpp:2225`, `:680`; `VG.Renderer/PassTargets.cppm` | checked |
| D2 | open | S | Player rebinding never reaches a game: the run loads no binding overrides and the Input facade has no rebind method. `describe`'s comment says it reads bindings after the player's rebinding. | `Engine.Input/InputSubsystem.cppm:300-302`; only `Samples/InputActions` calls `ApplyBindingOverrides` | checked |
| D3 | done (479c7dc24) | S | An EntityBehavior's `onFixedUpdate` is never called; `Systems/scripting.md:103` implies it is. Call it, or correct the document. | `Engine.Behavior`; `Documentation/Systems/scripting.md:103` | checked |
| D4 | done (a4bd1f5b7): says it runs in order | S | The `AsyncUpdate` phase is documented as parallel and runs serially; nothing outside tests registers into it. Make it say what it does, or remove it (the parallel schedule is among the larger items). | `Scene/Phase.cppm:29`; `Scene/SceneImpl.cpp:1123` | checked |
| D5 | done (c7e4b3433) | M | A crossfade cannot be interrupted: a state machine evaluates no transitions while one runs. | `Animation/AnimationGraph.cppm:1205` | checked |
| D6 | open | M | Hot reload drops script state in every language; the C# spec's claim that state carries "as the other backends do" is wrong as written. The fix is I2. | script backends; the C# spec | checked |
| D7 | open | S | The driver pipeline cache is never used, so every launch compiles every pipeline. The fix is I1. | `RHI/Device.cppm:213`; `RHI/Descriptors.cppm:422` | checked |
| D8 | open | L | Replication sends whole components `ReliableOrdered` to every ready peer each fixed step, with no rate limit, byte budget or quantization; `WriteFloatRanged` has no caller outside tests. The fix is I6. | `Net.Manager/NetworkManager.cppm:283-295`; `Net/WireStream.cppm:80` | checked |
| D9 | open | S | The SDL3 shell never asks for `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so on a scaled Wayland desktop a window likely draws at scale 1 and is stretched. Not seen: the dev box runs at scale 1; check with GNOME at 125% or above. | `Shell.SDL3/SDL3ShellImpl.cpp:284` | checked (behaviour inferred) |
| D10 | open | S | The C# backend declares only `Bytecode` although script delegates work, and stores its budget without reading it, so a runaway C# loop is not stopped. | `Script.DotNet`, on the dotnet branch | reviewer |
| D11 | done (a4bd1f5b7) | S | Stale comments claim the render graph aliases transients; `RenderGraph.cppm:8-9` says it does not. | `RenderGraphModule.cppm:7`; `PipelineImpl.cpp:1491` | reviewer |
| D12 | done (91df62354) | M | RHI device methods with default bodies, against the pure-interface rule: `MaxColorDepthSampleCount`, `SupportsSampleCount`, the mesh and ray-tracing methods. | `RHI/Device.cppm:177`, `:183`, `:245-288` | reviewer |
| D13 | started: every primitive seeds, no list | S | Starter seeding picks primitives by their display label, a hard-coded list. | `ProjectSeedImpl.cpp:451-452` | reviewer |
| D14 | open: with the backends-as-plugins spec | S | The Luau backend's implementation sits in its module interface, with Luau's headers in the global fragment. | `Script.Luau` | reviewer |
| D15 | open | M | Paks have no compression and no integrity check. The fix is I7. | the pak format | reviewer |
| D16 | open | M | World UI is styled, laid out and drawn every frame it shows, with no skip for an unchanged frame (whether layout already skips an unchanged tree is unchecked). | `Engine.GUI/GuiSubsystemImpl.cpp`, the canvas loop | reviewer, drawing checked |
| D17 | open | L | Navigation is fixed once baked, and scripts cannot query it. | `Engine.Navigation` | reviewer |
| D18 | open | M | The localisation library has no game caller; only the GUISandbox sample uses it. | `Foundation/I18N` | reviewer |
| D19 | open | M | No GPU crash diagnostics and no hang watchdog: a device lost or a stalled main thread leaves only the CPU log. The fixes are I4 and I5's watchdog. | RHI backends; `Core/System` | reviewer |

## Improvements

In the comparison's recommended order; the first group is cheap.

| # | Status | Size | Improvement | Notes |
|---|---|---|---|---|
| I1 | open | S | Persist the driver pipeline cache between launches. | Every backend has it already; fixes D7. |
| I2 | open | M | Hot reload that keeps state, with an `onReload` handler. | Re-apply the harvested properties' live values, not only the overrides; fixes D6. |
| I3 | open | S | A paged sparse array in `ComponentManager<T>` in place of the hash map lookup, plus a debug check for a pool growing during a walk. | Local to `Scene/Component.cppm`; no API change; needs a benchmark. |
| I4 | open | M | GPU crash breadcrumbs per backend, and spirv-val in development builds. | Fixes half of D19. |
| I5 | open | S | A main-thread hang watchdog that dumps every thread's stack. | Reuses the backtrace code; fixes the other half of D19. |
| I6 | open | L | Numbered unreliable property packets with acks, quantization, a byte budget per peer, and a hash of the replicated layout checked at connect. | Fixes D8. |
| I7 | open | M | Compression and a hash per pak entry, keeping the in-place reads (APK included). | Fixes D15. |
| I8 | open | M | Typed MCP tools: parameters and results as reflected structs with generated schemas and parsing, and a per-tool thread declaration so read-only tools answer while the main thread is busy. | Over `REFLECT_*` and `Mcp.Reflection`; replaces hand-built `SchemaBuilder` schemas (83 uses). |
| I9 | open | M | A read-only entity query for scripts over a manager's dense pool. | |
| I10 | open | S | Console variables for engine settings at run time. | |
| I11 | open | L | Texture mip streaming. | The largest content item. |
| I12 | open | L | A material node graph compiling to custom shader source, over `GraphView`. | The largest editor gap. |
| I13 | open | M | Inertialization in the animation state machine, after D5. | |

Larger items, each its own spec when taken up: declared-access parallel scheduling (with D4), GPU
occlusion culling, async compute, fog and atmosphere, client prediction and lag compensation, an AI
decision layer, animation montages, world save capture, multiplayer play in editor.

Not recommended: a libclang reflection generator (a toolchain and a codegen step outside CMake), a
global `new` replacement allocator (against our threaded allocators), and an RHI shape that rules
out WebGPU.
