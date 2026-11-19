# Runtime Host

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[runtime-and-engine-buildout]] / [[runtime-host-v3]]

The host layer: a single `IApplication` IS the app/game; a generic `ApplicationHost` drives it over a
shared GPU device + N windows; the editor embeds the SAME application against a persistent runtime
subsystem manager. Shipped (multi-window platform + graphics host + the inverted single-application
model + the embedded host + the app lifecycle). Run ownership (scenes + script brain) lives one layer
up in `ApplicationRun` and the runs - see [[game-instance-track]].

## The inversion

`IApplication` (`foundation.runtime.client`, `Application.cppm`) IS the app - exactly ONE per host. It
owns subsystem registration in `Configure(host)` (the host forces nothing in), so the subsystem set is
the app's alone and identical standalone or embedded. Hooks: `Settings`, `MainRenderWindow` (the main
window's descriptor), `OnCommandLine`, `Configure`, `OnStartup`, `OnLaunch`, `OnUpdate`,
`OnFixedUpdate`, `OnRenderWindow`, `OnEnterBackground`, `OnEnterForeground`, `OnExit`,
`OnShutdown`. There is no `IApplicationModule`; behavior lives in the one `IApplication`.

`DefaultApplication` (`engine.defaultapp`) is the conventional `IApplication`: it registers all the
standard engine subsystems, takes its scene composition from the one composition root
(`engine::FullSceneComposition()`), owns the primary `ApplicationRun` plus any extra ones, and owns the
`RunRegistry` it hands to the subsystem manager. The player is a thin subclass; the editor embeds
the same class. A game `: DefaultApplication` gets the defaults + its own; a game `: IApplication`
gets only what it declares.

## `ApplicationHost` + windows + graphics

`ApplicationHost` (`foundation.runtime.client`, `ApplicationHost.cppm`) is the concrete, generic host
that drives exactly one `IApplication`. It owns a `SubsystemManager`, a `TaskPool`, a borrowed optional
`GraphicsDevice`, and the LIST of `RenderWindow`s it presents. It is infrastructure - NOT subclassed -
and LOOP-AGNOSTIC: the shell layer drives `Start`/`Tick`/`Stop` (a blocking loop on desktop and
Android via `RunApplication` in `foundation.runtime.desktop`; a callback on Emscripten via
`foundation.runtime.web`), so the host holds no run loop. It implements `IApplicationHost`, the view
the application gets (`Subsystems()`, `Shell()`, `Graphics()`, `Tasks()`, `MainRenderWindow()`,
window open/close, `RequestExit`). `Start` also owns the engine-wide `JobSystem`'s lifetime
(`InitGlobalJobSystem` before `Configure`, `ShutdownGlobalJobSystem` last in `Stop`), so every
`ResourceManager` built during startup with `HasGlobalJobSystem() ? &GlobalJobs() : nullptr` gets
the workers. A test that wants them makes its own `JobSystem`.

Multi-window is uniform: the main window is `windows[0]`; every frame renders the whole list;
`OpenWindow`/`CloseWindow` run at runtime with close deferred to frame end. The GPU layer is three
modules to keep the core host backend-agnostic and dodge a GCC modules bug (importing a backend
module into the core interface breaks GCC's reader):

- **`foundation.graphics`** - `GraphicsDevice` (shared: backend/adapter/device/queue + the
  frame-in-flight ring), `RenderWindow` (one per OS window: surface + swapchain + per-window present
  sync), `FrameContext` (the per-window per-frame host/consumer boundary).
- **`foundation.graphics.gpu`** - `CreateGraphicsDevice` (Vulkan/DX12).
- **`foundation.graphics.null`** - `CreateNullGraphicsDevice` (headless).

Headless stays first-class: no `GraphicsDevice` => no windows => render skipped (null-platform tests
untouched).

## Subsystems + per-window render

`Subsystem` (`foundation.runtime`) has the frame phases `BeginFrame` / `Update` / `PostUpdate` /
`EndFrame` plus lifecycle (`OnRegister` / `OnInit` / `OnReady` / `OnPrepareShutdown` / `OnShutdown`
/ `OnUnregister`) and the app lifecycle (`OnEnterBackground` / `OnEnterForeground`). There is NO
fixed phase on subsystems: the fixed step is configuration on the `SubsystemManager`
(`SetFixedTimeStep`), which the scene bridge reads to seed per-scene steppers; the host's own
accumulator drives only the app-level `OnFixedUpdate` hook. There is NO `Subsystem::Render` phase
either: per-window rendering is driven by `IApplication::OnRenderWindow` (given a `FrameContext` per
window per frame), and rendering subsystems draw through that path. The subsystem manager also
carries the app-wide time scale and the run manager (`RunManager()`), through which subsystems
reach every run.

## App lifecycle (background and foreground)

A platform that takes the app out of view (Android's home button, another app in front) puts it in
the background, where it draws nothing, its sound stops, and it may be ended without being asked.
The shell reports it: `IShell::InBackground()` and its `OnEnterBackground` / `OnEnterForeground`
callbacks (the SDL3 shell raises them from SDL's will-enter-background and did-enter-foreground
events; a desktop never goes there). `ApplicationHost` installs the callbacks at `Start`:

- **Entering the background**: the app hears it first (`IApplication::OnEnterBackground`, so what
  it saves there reaches the subsystems), then the subsystems, then each window `Suspend`s, letting
  go of its surface and swapchain. `DefaultApplication` writes every instance's save; the audio
  subsystem suspends its engine, every voice kept where it is.
- **In the background** `Tick` returns at once: nothing ticks or draws.
- **Coming back**: the subsystems hear it, then the app; each window makes its surface again on its
  first frame with a native handle.

## Screenshots

`DefaultApplication::CaptureScreenshot(path)` writes the next presented frame of the main window
as a PNG. `ScreenshotCapture` (`foundation.rhi.capture`, `Foundation/RHI.Capture`) is the whole path:
armed by the request, `FinishFrame` records the copy off the backbuffer inside the frame
(RenderTarget -> CopySrc -> RenderTarget, so the host's own Present transition still holds) into a
256-byte-pitch GpuToCpu buffer; the next `OnUpdate` waits the device idle (a one-off hitch),
unpacks the rows into RGBA8 (BGRA surfaces swizzled, a pre-rotated frame turned back), writes the
file through `foundation.image.io`, and logs `Screenshot: wrote '<path>' (WxH)`. Non-8-bit surfaces
are refused with a log line, never a silent no-op. Three ways in:
- **F11** in any DefaultApplication: `screenshot_<ticks>.png` in the working directory.
- **`--screenshot <png> [--screenshot-frame N | --screenshot-after S] [--screenshot-count N]
  [--screenshot-exit]`** (`ScreenshotOptionsFromArguments`, read by `OnCommandLine`): capture at
  rendered frame N (default 30) or at the first frame past S seconds (frame-rate independent), and
  exit once the file is written when asked - a screenshot with no hand on the keyboard and no
  desktop capture tool (Wayland has none an unprivileged process may use). With a count, that many
  consecutive frames from there, as `<png stem>-<i>.png`, exiting after the last. Exit code 1 when
  the capture could not be produced.
- The base `OnRenderWindow` is `RenderFrame` (the scenes + window overlays) then `FinishFrame`. A
  subclass that draws its own overlay overrides it and calls the two around the overlay, so the
  overlay is in the shot.
- `IApplication::OnCommandLine(argc, argv)` is how the flags reach an app: the desktop `APP_MAIN`
  calls it before running, a hand-written main calls it itself (the player, samples with their
  own entry). Web entries have no argv and never call it.
The WebGPU swapchain asks for `CopySrc` when the surface offers it (the Vulkan surface carries
TRANSFER_SRC where the driver allows). The player's `--exit-after <seconds>` sets
`SetExitAfterSeconds`. Tests: `RHI.Capture.Tests/ScreenshotTests` (flags, the row unpack + swizzle,
pre-rotation, and an RGBA8 + BGRA8 clear captured, written and loaded back on Vulkan + WebGPU).

## Embedded host (the editor)

The editor holds two subsystem managers: the editor-app one (chrome/tools) and ONE persistent
embedded runtime manager owning all scene hosting (editing scenes, Simulate, preview pages, and the
Game tab's runs) plus every engine + game subsystem. `EmbeddedApplicationHost` (`EmbeddedHost.cppm`)
is the adapter: it routes `Subsystems()` to the embedded runtime manager while SHARING the outer
host's shell, task pool and REAL `GraphicsDevice`, so subsystems see the device from `Configure` on.
The embedded app renders into viewport textures, so `MainRenderWindow` is null and window
open/close are refused; "exit" stops the play session (the embedder's exit handler, deferred to
frame end).

This is what lets a native game module `Configure()` ONCE and have its managers inject into the same
`SceneSubsystem` that hosts editing scenes - custom components are inspectable while editing and
playable in the Game tab, identical to standalone.

## Time + run ownership

Time is layered `host dt x context x group/instance x scene`; `SceneSubsystem` builds one
`FrameTime` per frame from the subsystem manager's time scale and fixed step, each `SceneManager`
adds its group term and each scene its own and owns its fixed-step accumulator (see
[[game-instance-track]], [[scene-composition]]). Run ownership - the script brain, the scenes,
per-instance net/input/save - lives in `ApplicationRun`, one run each; `ApplicationHost` and the
subsystems hold no run state of their own beyond what they key by run.

---

Design history (the original v1/v2 module-list model, the wart table it corrected, the
GraphicsDevice/RenderWindow/FrameContext API sketches, and the v3 embedded-host phasing) is in
`Documentation/Archive/runtime-host-design-history.md`.
