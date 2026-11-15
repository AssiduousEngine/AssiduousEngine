# Web platform - Emscripten + WebGPU

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[web-platform-track]]

The engine runs in the browser on WebGPU: a WebGPU RHI backend, an Emscripten web runner, single-
threaded execution, and cook-time WGSL. Shipped - a full 3D scene + web player + AngelScript-on-web run
in-browser. Backend and clip-space fixes: [[web-render-fixes-and-flip]].

## The approach that shipped

- **Native-first bring-up via wgpu-native.** The WebGPU RHI backend (`foundation.rhi.webgpu`,
  `Code/Foundation/RHI.WebGPU`) is written against the standard `webgpu.h` C header and validated ON
  DESKTOP by loading wgpu-native (a prebuilt shared library, the DXC runtime-sidecar pattern - see
  [[dxc-runtime-sidecar]]), giving a full gdb/RenderDoc dev loop; it doubles as a bonus desktop backend.
  The Emscripten build is a relink against the browser's WebGPU, not a port. The backend's
  bring-up order was: device/queue/fence lifecycle -> resources -> pipelines/bind groups ->
  encoders + swapchain (first triangle) -> transfer/queries/bundles (full renderer). Every stage
  has shipped; a NotSupported in the backend today is a genuine WebGPU API limit, never a
  missing stage.
- **Single-threaded web v1.** wasm pthreads need SharedArrayBuffer, which needs COOP/COEP host headers
  (rules out plain static hosts); the JobSystem has an inline mode (0 workers, jobs execute at submit).
- **WGSL from the shaders track.** The browser accepts WGSL only (no dlopen, no DXC), supplied by the
  shaders track's cook-time HLSL -> SPIR-V -> naga -> WGSL pack (`shaders.md`).

The engine was web-shaped by construction: `ApplicationHost` / the runners were designed for the
desktop-blocks / web-yields split (`foundation.runtime.web`'s runner is a thin
`emscripten_set_main_loop`, not a refactor); the RHI is the WebGPU-safe subset (the bundle-safe command
subset, WebGPU texture-format conventions, no bind group past set 3 = WebGPU's 4/4 budget on purpose).

## Modules

`foundation.rhi.webgpu` (the backend), `foundation.runtime.web` (the web runner), `foundation.shell.web`
(the web shell), and the Web player entry (`Engine.Player/WebMain.cpp`). Web export ships the wasm + js +
html shell + `serve.py` + two content paks + `player.xml` + the WGSL `Data/Engine/Shaders/shaders.dpak`
(+ `Data/.dataroot`) through the standard export path (the "Web" export template - see
`Documentation/Systems/export.md`); the web player fetches the `Data/` tree into MEMFS so the one
data-root discovery finds it at `/Data`. The paks are `Content-bc.pak` (desktop browsers) and
`Content-astc.pak` (mobile browsers), cooked for the `web-bc` and `web-astc` targets; at startup the
player asks the adapter which compressed-texture family it reads and fetches that pak as
`Content.pak` (a dist with a single `Content.pak` still loads). Two templates are built
(`scripts/build-export-templates.sh web`): `web-release` from the `wasm-shipping` preset (the
smallest, no profiler; the one each release publishes) and `web-profile` from `wasm-release`, with the
profiler and its overlay compiled in (`?profile-overlay` in the page's address shows the overlay from
the start; F3 toggles it).

## Saves and settings

The user data directory (a game's save, the user's settings) is mounted over the browser's IndexedDB
by the web player before the game starts (`MountUserData` in `WebMain.cpp`, IDBFS, linked with
`-lidbfs.js`), and what the page stored there before is loaded into it. A writer pushes what it wrote
back with `core::PersistUserData()` (a run's save on flush, the player's settings), a background
`FS.syncfs` on the web and a no-op on desktop. A browser that refuses storage (some private windows)
keeps the directory in memory for the page.

## Testing from another device

WebGPU exists only in a secure context (localhost, or HTTPS). `serve.py` (staged beside every web
export) serves the folder; `python3 serve.py 8443 --https` serves it over HTTPS with a self-signed
certificate, so a phone or another computer on the network can open it by address after accepting
the certificate once. Plain HTTP from another machine loads the page without WebGPU.

## Networking

A browser cannot open UDP, so a web game joins over WebSocket: `foundation.net.websocket`'s client
socket is the browser's WebSocket in a web build, and a desktop host accepts browser peers beside its
UDP ones through `WebSocketHybridSocket` (`ws://`, or `wss://` with a TLS backend and certificate).
Both are `IDatagramSocket`s, so the session, reliability and replication layers run unchanged. A web
build joins and does not host (`NetworkManager::CanHost`, `Net.canHost` to scripts, is false there).

## Deferred (none block a playing game)

- **wasm pthreads + SharedArrayBuffer** opt-in (hosting-header-gated).
- **Hosting from a browser.**
- **Script-debugger remote transport, native file dialogs, Memory64.**
