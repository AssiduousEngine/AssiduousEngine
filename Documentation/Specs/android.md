# Android: a native player and its export

> STATUS: PROPOSED 2026-11-10, on branch `android`. Sized L, phases A0 to A5, green after each.
> A0 to A3 built 2026-11-10, A4 built 2026-11-11 (see the week-2026-10-24 entries).
> Origin: the 0.2 backlog's "Android: a native player and its export" (user 2026-10-11), started
> by the user 2026-11-10. The test device is the user's Google Pixel 6 (Tensor G1, Arm Mali-G78,
> Vulkan 1.3). Read CONVENTIONS.md first.

## Goal

A game made in the editor exports to an Android app (an APK, and an AAB for a store) that runs
natively on an arm64 phone on Vulkan: touch, rotation, pause and resume, the back button, saves
in the app's storage. The editor stays on the desktop; it builds the app.

Not goals: the editor, on-device MCP or play-in-editor on Android; 32-bit or x86 Android ABIs
(arm64-v8a only); iOS; a GLES fallback (Vulkan 1.3 or nothing, as on the desktop).

## What exists (checked 2026-11-10, at 83ad7de18)

- **Touch and orientation are already the engine's.** The shell's input has touch and pinch
  (`Shell/InputTypes.cppm:289-312`), the GUI dispatches them, the web export runs on phones today,
  and `ProjectSettings::orientation` (`Engine.Project/ProjectModule.cppm:200`) with the present
  copy's `ContentRotation` exist (`Archive/present-transform-history.md`, which already plans the
  native case: section 8, "Native, later").
- **Third-party code knows Android.** SDL 3.4.10 is vendored with its Android C sources
  (`ThirdParty/SDL3/src/core/android`), but its Java glue (`android-project`) was pruned
  (`ThirdParty/SDL3/README.vendored.md`). Jolt picks NEON on aarch64 Android (`Jolt/Core/Core.h`).
  miniaudio picks AAudio or OpenSL ES (`miniaudio.h`, `MA_ANDROID`). AngelScript's `as_config.h`
  selects native arm64 calls on Android (`AS_ARM64`), whose assembly
  (`as_callfunc_arm64_gcc.S`) our build does not compile yet (`ThirdParty/CMakeLists.txt:363-374`).
- **Optional GPU features are optional.** Mesh shaders, ray tracing and bindless are requested
  only when a device desc asks (`RHI.Vulkan/VkDevice.cppm:59-62`), and the player asks for none
  (`Graphics.cppm:108`). Required: Vulkan 1.3 with `dynamicRendering`, `synchronization2`,
  `shaderDemoteToHelperInvocation`, and 1.2's `timelineSemaphore` (`VkDevice.cppm:141-150`). The
  Mali-G78 has these on Android 13 and later.
- **Shaders and textures cook on the host.** DXC, Naga and Tint are host tools; the Steam Deck
  template already ships a host-cooked SPIR-V `shaders.dpak` (`scripts/build-steamdeck.sh:58`).
  ASTC already cooks for the web (`Pipeline.Core/Asset.cppm:74-98`, target `web-astc`); Mali
  reads ASTC and ETC2, not BC.
- **Where it does not build or run yet** (the blockers):
  - Core's system backend is chosen by `CMAKE_SYSTEM_NAME STREQUAL "Linux" OR EMSCRIPTEN`
    (`Core/CMakeLists.txt:119-127`): "Android" gets none.
  - `Prelude.h:60-68` has no `PLATFORM_ANDROID` (the NDK defines `__linux__`, so it reads as Linux).
  - The Vulkan instance needs xlib or wayland on `__linux__` (`VkBackend.cppm:284-316`); there is
    no `VK_KHR_android_surface`, no `SurfacePlatform`/`WindowSystem` for it
    (`RHI/Device.cppm:42-49`, `Shell/ShellModule.cppm:34-42`), and the SDL shell's `Native()` has
    no Android branch (`SDL3ShellImpl.cpp:210-245`).
  - The swapchain sets `preTransform = currentTransform` with no rotation of the content
    (`VkSwapChain.cppm:271`), and out-of-date, suboptimal and surface-lost are errors nobody
    recovers from (`VkDevice.cppm:1063-1097`, `Graphics.cppm:574-628`).
  - Nothing handles the app going to the background: no shell event for it
    (`ShellModule.cppm:217-229`), the runner loops on (`DesktopRunner.cppm:36-58`), the surface is
    made once (`Graphics.cppm:386-430`), audio plays on.
  - The player is a `main()` executable that finds its data beside `/proc/self/exe`
    (`Engine.Player/Main.cpp`, `PlayerMain.cpp:134-159`, `VFS/DataRoot.cppm:31-133`); the user
    data directory comes from `XDG_DATA_HOME`/`HOME` (`LinuxSystem.cpp:114-129`).
  - Content is read through real paths only (`sys::FileOpen`); `PakFileSystem` reopens the
    `.pak` by path on every open (`VFS.Pak/PakModule.cppm:57-95`).
  - Prebuilt x86_64 runtime libraries would be picked on Android: DXC and wgpu-native are
    chosen by `UNIX AND NOT APPLE` (`ThirdParty/CMakeLists.txt:77-88, 114-124`).
  - The platform tag is "Linux64" on any non-web `__linux__` (`LinuxSystem.cpp:133-137`); it names
    plugins, templates, presets and the cook's formats (`ExportImpl.cpp:118-135, 460-495`).
  - Logging goes to stdout and stderr; Android shows neither (it wants logcat).
  - SIMD is not a concern: `Core/Math/Simd.cppm` has an SSE2 path and a scalar one
    (`SimdConfig.h:10-21`), but nothing outside it uses it, so every platform runs scalar math
    today. Where SIMD is needed is investigated later, for every platform (user, 2026-11-10).

## Decisions to take (recommendations first)

1. **The platform tag is `Android64`.** It sits beside `Linux64` and `Win64` in plugins,
   templates, presets and the Bin folder (`Bin/<Config>/Android64-Clang`), and `PLATFORM_ANDROID`
   joins `Prelude.h` ahead of the `__linux__` test. Android keeps the POSIX system backend
   (`LinuxSystem.cpp`, `LinuxThread.cpp`) with `PLATFORM_ANDROID` blocks for what differs, as the
   web does today, rather than a copy of it.
2. **Content stays in the APK and is read in place.** `Content.pak` and the engine's data pack are
   stored uncompressed in the APK (`noCompress`), and a pak reads from a file region: the APK's
   descriptor, offset and length from `AAsset_openFileDescriptor64`. The engine's `Data/` folder
   becomes a pak for Android (the data root mounts a pak as readily as a folder). The other
   choice, copying the packs out to app storage on first launch, needs no VFS change but doubles
   the space a game takes and slows its first start.
3. **The export runs Gradle.** The Android template holds the player (`libmain.so`) and a Gradle
   project; the exporter fills in the app's id, name, version, icon and orientation, adds the
   packs, runs the Gradle wrapper and signs (a debug key made for the developer, or their release
   keystore; passwords never in project files). It needs a JDK and the Android SDK on the
   exporting machine, found through `ANDROID_HOME`, which the editor checks and names when
   missing. The other choice, a prebuilt APK the exporter patches (its binary manifest edited,
   then zipaligned and signed), needs no SDK but owns a manifest writer.
4. **Where the Android pieces live.** The player's Gradle project and its activity under
   `Code/Engine/Engine.Player/Android/`; SDL's Java glue re-vendored into
   `ThirdParty/SDL3/android-project/` (the prune rule in `README.vendored.md` changes to keep it).
5. **Which Androids.** arm64-v8a only. `minSdkVersion` 33 (Android 13; user ruling 2026-11-10,
   in place of the 29 first planned): Android's `libvulkan` exports the Vulkan 1.3 calls the RHI
   makes directly (`vkCmdBeginRendering`, `vkCmdPipelineBarrier2`, ...) only from API 33, and
   Vulkan 1.3 drivers are an Android 13 matter in practice. A device without Vulkan 1.3 and the
   features above gets a clear message and exits, as a desktop GPU does.
6. **Textures cook to ASTC.** A cook target `android` (ASTC on, BC off, as `web-astc`), one
   `Content.pak`.

## Phases

- **A0 - Foundation builds for Android and its tests pass on the phone.** An `android-arm64`
  preset over the NDK's `android.toolchain.cmake` (Debug; tools off, as for the web; the host's
  Bin for the shader cook); `PLATFORM_ANDROID`; Core's backend for Android (the user data
  directory set by the host at startup, the executable path, a backtrace through `<unwind.h>`
  in place of `execinfo.h`, no file manager or child processes, a logcat console sink);
  ThirdParty guards (SDL's Android build, no DXC or wgpu-native runtime, AngelScript's arm64
  `.S`). `scripts/android-tests.sh` pushes the headless test executables to the phone
  (`adb push` to `/data/local/tmp`) and runs them there. Accept: Core.Tests and the other headless
  Foundation and Engine suites build, and pass on the Pixel 6.
- **A1 - a window and a triangle on the phone.** `WindowSystem::Android` and
  `SurfacePlatform::Android` (the `ANativeWindow` from SDL's window properties);
  `VK_KHR_android_surface`; SDL's Java glue re-vendored; the minimal Gradle project; the RHI
  triangle sample as `libmain.so` with `SDL_main`. The RHI samples compile their HLSL with DXC as
  they run, which Android has not: their shaders move out of each sample's C++ into `.hlsl` files
  beside it (user ruling 2026-11-10: one source, as the engine's shaders are), the desktop
  compiling them as now, and `Tools.ShaderPack` cooking them on the host at build time into a
  SPIR-V pack the sample framework reads on Android (as the web samples' packs are cooked). Every
  RHI sample comes with it, not the triangle alone. Out-of-date and suboptimal acquire and present
  recreate the swapchain; `preTransform` is identity for now (the compositor turns the picture).
  The SDL shell reports one window and no placing on Android (`WindowCapabilities`), so nothing
  floats a panel into a window of its own. Two findings of A0's run of the RHI suites on the Mali
  GPU come here: an RGBA16F target reads back as zeros (`screenshot: a cleared RGBA8 and BGRA8
  target ...`, its `vulkan-rgba16f` case), and the timestamp calibration's GPU and CPU clocks
  disagreed once (`vk.queries: a calibration ...`; it passed on the next run). Both settled: the
  readback was intermittent (a readback's mapped memory, cached and maybe not coherent, never
  invalidated: 3e5d7020d), and the calibration test's 30 ms was inside Mali's calibration
  deviation (d731de059, 200 ms). Until A2 read the packs in place, the app's activity copied its
  data out of the APK on the first run of each version; A2 removed the copy.
  Accept: the triangle draws, in both orientations, on the Pixel 6.
- **A2 - the player and a sample game on the phone.** The player builds as `libmain.so`;
  Decision 2's packs (a pak over a file region; the data root over a pak); the `android` cook
  target; the SPIR-V shader pack; the user data directory in the app's internal storage. A dev
  script builds an APK holding a sample's packs and installs it. Accept: Sky Hopper starts and
  plays on the phone (touch through its touch controls), and its save survives a restart.
  **Built 2026-11-10, accepted on the Pixel 6:** Sky Hopper starts from its APK, plays by touch,
  turns, and keeps its stars and best run across restarts. What the run found and fixed on master
  is in the weekly (the lost device, the flicker, the stretch after a turn, the quick tap). Owed
  to later phases: immersive (A3: the status bar covers the top of the game's frame in landscape),
  and the first seconds' frames of the Debug build take 250 to 600 ms (A5 measures in
  `clang-reldbg`).
- **A3 - the app lifecycle.** Shell events for entering the background and the foreground (SDL's
  are delivered through an event watch, since the background one must be answered before
  returning); the runner stops ticking and presenting while in the background; the surface and
  swapchain are released and made again (`VK_ERROR_SURFACE_LOST_KHR` recovered); audio stops and
  resumes (`ma_engine_stop/start`); user data is persisted on going to the background (Android may
  end the process unasked). The back button reaches the game as a key it can bind (a new
  `KeyCode` for it, what a menu's Escape does by default); unhandled, the app goes to the
  background. The game fills the screen: the status and navigation bars hidden (immersive, the
  user swiping them back for a moment), seen on the Pixel 6 in A1 where they stay (user, 2026-11-10).
  In A2 the status bar covered the top of Sky Hopper's HUD in landscape. The game also keeps out
  of what the bars and the camera's cutout cover, immersive or not (the Pixel 6's camera hole
  stays in immersive): the shell reports the window's safe area (`SDL_GetWindowSafeArea`, which
  Android's window insets feed; a new `IWindow` query, the whole window on a desktop), and the
  player fits the game's frame into it rather than into the whole window, the GUI's screens
  with it. Targeting API 36 leaves no way out of drawing under the bars, so the safe area is the
  way to keep out from under them. Accept: home, switching apps, locking the phone and rotating it, each and back,
  on the Pixel 6, with the game where it was and its sound back, and none of it under the bars or
  the camera's cutout in either orientation.
  **Built 2026-11-10, accepted on the Pixel 6:** home, switching apps, locking and rotating come
  back where they were with their sound; the game is immersive; Back reaches the game, and a Back
  nothing in the game handles sends the app to the background (`Activity.moveTaskToBack`, since the
  activity's own back ends an activity not opened from the launcher). The engine side is on master
  (the shell's background events, the host and windows suspended, audio stopped, saves written, the
  Back key, the safe area, an unhandled Back); the Android shell's part is on this branch. Sky
  Hopper binds nothing to Back, so it pauses nothing (user, 2026-11-10: left as it is).
- **A4 - the export.** The Android template (`scripts/build-export-templates.sh android`, the
  template's `Android64` platform; `Android64` among the platforms a plugin package names, in the
  scaffold, the MCP plugin tools and their checks); the preset's Android fields (application id, version name and
  code, label, icon, orientation into the manifest, the signing choice); the exporter builds the
  APK and the AAB through Gradle and signs; the editor's export dialog and the MCP export tools
  show them; `KnownIssues.md` and the docs repo's export page. Accept: Sky Hopper exported from
  the editor installs and runs on the phone; the MCP export flow test covers the Android preset
  (Gradle itself skipped where no SDK is present).
  **Built 2026-11-11, accepted on the Pixel 6:** Sky Hopper's Android preset exported to an APK that
  installs and runs (its name and icon in the launcher, landscape, play by touch with sound, the save
  kept), and the export from the editor's Project > Export. The template carries the player as
  `libEngine.Player.so` and the app's Gradle project, which the Android build stages beside it; the
  exporter copies the project into `<project>/.cache/android/<preset>`, stages the app there and
  runs the template's Gradle wrapper with the preset's values (no daemon left running). Beyond the
  plan: the keystore's password is asked for in the editor or read from `ANDROID_KEYSTORE_PASSWORD`,
  never stored; the app icon is picked from the project's PNGs and the keystore with the system's
  dialog; the Android SDK and JDK folders are in Preferences (else `ANDROID_HOME`, `JAVA_HOME`).
  The MCP side is tested through `export_preset_set` (the Android fields, their platform, the id
  check) and the export itself through Editor.Core.Tests with a stand-in Gradle wrapper, rather
  than an MCP export flow test. Left: a game with its own native packages is refused on Android
  (`TODO(export)`, its player not built for Android yet); the docs repo's export page waits for its
  next release; Run on device is in `backlog-0.2.md` (user, 2026-11-11).
- **A5 - rotation without the compositor, and the first performance pass.** `preTransform =
  currentTransform` with the extent swapped and the present copy turned
  (`present-transform-history.md`, section 8), input turned with it. This closes the
  `TODO(rhi)` on the swapchain's identity `preTransform` (`VkSwapChain.cppm`). What A2 saw of the
  Pixel 6's surface, for it: the current extent is reported already turned to the phone's
  orientation (2400x1080 in landscape, with `ROTATE_90`), and both the extent and the transform
  lag a turn, so a swapchain rebuilt on the window's resize can read the old ones (the swapchain
  takes the window's size within the surface's range for this, 62a841eff; with pre-rotation the
  images are the window's size swapped on a quarter turn); and every present in landscape returns
  `VK_SUBOPTIMAL_KHR` while `preTransform` is identity, which pre-rotation ends. The render stack stays as it
  is on the other platforms (user ruling, 2026-11-10): nothing is turned off for Android up front.
  Measure first: the frame time of each sample on the phone, in `clang-reldbg` terms (no Debug
  timing), and what each full-screen pass costs there (SSAO, SSR, screen-space GI, bloom, TAA:
  a tiler may pay more for them than a desktop GPU), with the profiler's GPU timings. What to do
  about any of it is decided with the user from those numbers. Accept: every sample game runs on
  the phone, and the measurements are written in the weekly.
  **Rotation built 2026-11-11, accepted on the Pixel 6** but for a brief squish on the first turn
  of a run (`backlog-0.2.md`). None of it is Android's alone, so it is on master: a swapchain asked
  to pre-rotate takes the surface's current transform, swaps its images' sides on a quarter turn
  and rebuilds when a suboptimal present comes with a new turn; the frame and the screen overlays
  carry the turn and the window's size; the player asks for it and turns its scene's fit, its
  screens and its screenshot with the window; Dear ImGui draws as a screen overlay, turned the same
  way. Input needed no turning: the pointer stays in the window's space, where the screens lay
  out. What A2 saw held: Android reports the window's new size a frame before the surface's new
  turn, so a turn rebuilds twice. Left: the measurements.

## Gotchas

- **The NDK's Clang is 21.0.0**, Android's snapshot from before 21.1 (the Linux lanes and CI use
  21.1.8); a module bug fixed in 21.1 may still show. The fallback is this box's `clang++-21`
  targeting `aarch64-linux-android` against the NDK's sysroot and libc++ (backlog-0.2.md, "The
  Linux dev box is ready").
- **`__linux__` is defined on Android**, so every `__linux__` test in first-party code is checked
  in A0 (the Vulkan instance's surface extensions, the shell's `Native()`, `SDL_WINDOW_VULKAN`).
- **The background event must be answered before SDL returns** (rendering must stop before the
  `ANativeWindow` goes away), which is why A3 uses an event watch rather than the event queue.
- **A phone's natural orientation is portrait**, so a landscape game has a quarter-turn
  `currentTransform` from its first frame: A1 must not treat `VK_SUBOPTIMAL_KHR` as an error, or
  it recreates the swapchain every frame.
- **Mali has no D24S8**; the depth format is already probed (`VkDevice.cppm:293-304`).
- **Running native tests over adb** needs the executable and its data under `/data/local/tmp`,
  and the tests' scratch writes beside them.

## Tests (the contract)

- The headless suites pass on the phone (`scripts/android-tests.sh`), as a lane when it is
  attached; the CI lane builds the `android-arm64` preset (no device).
- Unit tests where the code is the engine's: a pak over a file region; the Android export preset's
  fields and template; the shell's lifecycle events reaching the runner (desktop minimize gives
  the same pause, so it is tested on the desktop too).
- On the device, by the user (visual and input checks are theirs): A1's triangle, A2's game, A3's
  lifecycle list, A4's exported app.
