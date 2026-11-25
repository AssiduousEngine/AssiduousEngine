# Building

## Requirements

- CMake 3.28+ and Ninja.
- Linux x64: Clang 21 and/or GCC 15 (both are kept green; Clang is the daily driver), the Vulkan
  development libraries, and SDL3's build dependencies. On Ubuntu or Debian:

  ```
  sudo apt install cmake ninja-build clang lld pkg-config \
      libvulkan-dev vulkan-tools vulkan-validationlayers mesa-vulkan-drivers \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
      libxtst-dev libwayland-dev wayland-protocols libxkbcommon-dev libasound2-dev
  ```

  Clang 21 is what development and CI use. Ubuntu 24.04's default Clang 18 does not work (it
  rejects flags the build passes under `-Werror`); 19 and 20 are untested. `build-clang.sh` picks
  the newest Clang installed.

  Clang builds link with LLD (`-fuse-ld=lld`), about five times as fast as GNU ld, and configure
  stops if Clang cannot find it. apt.llvm.org's `llvm.sh 21` installs `lld-21` beside `clang-21`,
  where Clang finds it. With a Clang that has no LLD of its own version beside it, any `ld.lld` on
  `PATH` works, for example a link to another version's: `ln -s /usr/bin/ld.lld-22
  ~/.local/bin/ld.lld`.

  The GCC that works is Ubuntu's packaged GCC 15 (`g++-15` on 24.04 backports or 26.04 LTS). The
  upstream 15.2.0 release cannot resolve transitive module CMIs from CMake's module maps ("Bad
  import dependency"), and the toolchain-r noble backport miscompiles module interfaces (defaulted
  constructors diagnosed as overloading themselves). CI's gcc job runs in an `ubuntu:26.04`
  container for this reason.

- Windows x64: Clang 21 (LLVM) or MSVC, and the Windows SDK. The MSVC presets run from a Developer
  Command Prompt (vcvars64), so `cl.exe` and the SDK are on the path.

  The MSVC that works is Visual Studio 2026's (toolset 14.51, compiler 19.51), which CI builds and
  tests with. Visual Studio 2022's (14.44, compiler 19.44) does not: a shared build fails to link
  one library's module constants from another (`LNK2001` on `Color32::Black` when `Image.dll`
  links against Core), so configuring with an MSVC older than 19.51 stops with an error naming the
  one to install. With both installed, `build-msvc.cmd` picks the newer toolset itself.
- A Vulkan 1.3 (or, on Windows, D3D12) capable GPU to run anything that draws.
- The .NET 10 SDK (`dotnet` on the path), for C# scripting, which a desktop build includes by
  default: it generates the C# bindings and builds the engine's managed runtime and the C# tests.
  On Ubuntu 26.04, `sudo apt install dotnet-sdk-10.0`; elsewhere, Microsoft's installer for the
  platform. Without it, configure with `-DOPTION_ENABLE_DOTNET=OFF`. Web and Android builds leave C# out
  unless an export of a C# game turns it on.
- For web builds, emsdk 6.0.5 with the project's response file patch applied; see
  [Guides/emscripten-windows.md](Guides/emscripten-windows.md).
- For Android builds, the Android NDK r30 (`30.0.16248370`) named by `ANDROID_NDK_HOME`; to
  package an app, the Android SDK (`ANDROID_HOME`, with `platforms;android-36` and
  `build-tools;37.0.0`) and a JDK 17. The first Gradle build downloads Gradle and the Android
  plugin.

## Presets

Everything goes through CMake presets. Outputs (executables, cooked shader packs, runtime
sidecars) land in `Bin/<Config>/<Platform>-<Compiler>/`, for example `Bin/Debug/Linux64-Clang/`.

```
cmake --preset clang                  # configure once; or gcc, clang-release, wasm, ...
cmake --build --preset clang
ctest --preset clang                  # the unit and integration tests
```

| Preset | Meaning |
|---|---|
| `clang` / `gcc` / `msvc` | Debug, the development configuration |
| `clang-shared` / `msvc-shared` | Debug with shared libraries |
| `clang-reldbg` / `msvc-reldbg` | RelWithDebInfo, for performance work and the editor download |
| `clang-release` / `gcc-release` / `msvc-release` | Release |
| `clang-shipping` / `gcc-shipping` / `msvc-shipping` | Shipping (no asserts, stripped) |
| `wasm` / `wasm-release` / `wasm-shipping` | Emscripten wasm32: Debug, Release with the profiler, Shipping |
| `android-arm64` / `android-arm64-reldbg` | Android arm64 through the NDK: Debug, and RelWithDebInfo (the export template, timing on a phone) |

## Notes

- **Debug is the development configuration.** A change must build green on both clang and gcc on
  Linux before it is done.
- Each library has a doctest sibling (`GUI.Tests`, `Engine.Render.Tests`, ...) you can build and
  run on its own, which iterates much faster than the full suite. New test targets appear after
  a re-configure.
- The web build (`--preset wasm`) needs the emsdk environment sourced first and the project's
  emsdk patch applied. New shaders must translate to WGSL (through naga) to run on the web.
- **Android** builds on a Linux host, with no tools (the editor and the cookers run on the
  desktop). `scripts/android-app.sh Engine.Player --game Data/SampleProjects/SkyHopper --install`
  builds the player, packs a game into a development app and installs it on a phone attached
  over adb (`--run` starts it; `CONFIG=RelWithDebInfo` builds from `android-arm64-reldbg`). The
  suites run on the phone with `scripts/android-tests.sh`, which pushes them and their data over
  adb; ctest does not run them on the host. `scripts/build-export-templates.sh android` makes the
  Android export template.
- Sanitizer trees: configure with `-DBUILDSYSTEM_OUTPUT_SUFFIX=-ASAN` so output lands in
  `...-ASAN/` and never overwrites the regular `Bin/` tree.

## Running the editor

```
Bin/Debug/Linux64-Clang/Tools.Editor               # the project manager
Bin/Debug/Linux64-Clang/Tools.Editor <projectDir>  # open a project
```

Add `--mcp` to serve the open project to an agent over MCP while you work in the editor.

## Debugging handles

GPU resources (and the engine's other pooled ids) are generational handles: in a debugger a handle
is a slot, a count and a pool, not the object. The printers show those, and in a live process the
label the resource was made with and whether it is still live:

```
(gdb)  source scripts/debug/handles_gdb.py
(lldb) command script import scripts/debug/handles_lldb.py
```

`$1 = Buffer handle slot 0 count 1 pool 1 'mesh.vertices'`, or `stale (was 'mesh.vertices')` once it
is destroyed. Visual Studio shows the slot, count and pool through `Core.natvis`, linked into every
binary that uses Core; the label is a call in the Immediate window,
`DebugHandleLabel("Buffer", h.index, h.generation)`. A stale handle used at run time is logged
once, with its label: `[rhi] stale Buffer handle used (...), last created as 'mesh.vertices'`.
