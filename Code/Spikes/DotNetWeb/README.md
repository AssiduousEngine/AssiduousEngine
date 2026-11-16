# DotNetWeb: the C# web test

D0 of [the C# scripting spec](../../../Documentation/Specs/dotnet-scripting.md): can C# compiled by
NativeAOT-LLVM (LLVM bitcode for `browser-wasm`, built with the emsdk it needs, 3.1.56) be linked
into a program built with the engine's own Emscripten (6.0.5), and run?

- `Entry.cs`: one `[UnmanagedCallersOnly]` entry point taking a table of the host's functions; it
  calls back through it, allocates and collects, and returns a value.
- `host.cpp`: the host, passing the table and checking the result.
- `build.sh [dotnet-emsdk] [engine-emsdk]`: builds both, links, and runs it under Node. Exit 0 is a
  pass. Needs the .NET 10 SDK and network access for the dotnet-experimental package feed.

Not part of the engine's CMake build.

## Result (2026-11-16)

It holds. With NativeAOT-LLVM 10.0.0-rc.1.26357.1, the bitcode built by emsdk 3.1.56 linked under
emsdk 6.0.5 with undefined symbols as errors and no feature-check override, and ran under Node:
the call back through the table, a thousand allocations, a collection and the return value all
correct. Two things it needed:

- Publish copies a `.js` and `.wasm` page a static library never makes: `StubPageFiles` creates
  empty ones first.
- The runtime asks its JavaScript host for `SystemJS_RandomBytes`; the native host defines it from
  `getentropy`.

Size: the whole program, runtime included, is 748 KB of wasm (294 KB gzipped), single-threaded,
optimised for size.
