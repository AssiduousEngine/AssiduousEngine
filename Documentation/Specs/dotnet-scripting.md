# C# scripting

> STATUS: APPROVED 2026-11-16 (user): engine-hosted, the web test first. Sized L, phases D0 to D5,
> on the `dotnet` branch; an experiment until D0 and D1 show it holds. Targeted suites per change,
> the full lanes at the end of the day. Read CONVENTIONS.md first.

## Goal

Game code in C#, beside AngelScript and Luau: behaviours written as C# classes, the same engine
surface the other languages see, hot reload in the editor, and exports to desktop and the web. The
engine stays the host everywhere: the editor, the player and the tools are C++ programs that load
C#, never the other way round.

Not goals here: Android and iOS (NativeAOT's support there is its own question, later), C# editor
extensions, and replacing either existing language.

## How it runs, per host

| Host | C# runs as | Started by |
|---|---|---|
| Editor, desktop development, `Tools.Mcp` | the JIT runtime (CoreCLR), loaded through `hostfxr` | the engine, from the project's built assembly |
| Desktop export | NativeAOT: the game's C# compiled into one native library | the player, through the library's `[UnmanagedCallersOnly]` exports |
| Web export | NativeAOT-LLVM: the game's C# as a static library of LLVM bitcode | the web player, linked with it, through the same exports |

One C# codebase serves all three. What makes that possible is the boundary:

- **Engine to C#:** a few `[UnmanagedCallersOnly]` entry points (initialise, create a behaviour,
  call its lifecycle, hot reload) the host finds by name: through `hostfxr`'s function-pointer
  loader on CoreCLR, as exported symbols under NativeAOT.
- **C# to engine:** one table of C function pointers the engine hands C# at initialisation. No
  `DllImport` resolution, no exported engine symbols, the same on every platform, the web included.
- **No reflection marshalling, no runtime code generation:** NativeAOT has neither. Delegates
  across the boundary are function pointers with a static trampoline and a `GCHandle`.

## The bindings

Generated from the engine's reflection, as the declarations for AngelScript and Luau already are
(`ScriptDeclarations`, written by `script_declarations`): the C# source of every bound type, facade
and component, the C side of the function table, and the layouts of the value types passed by
value (Float3, Quaternion, Color and the like), each with a size and offset check on both sides,
for 64-bit and wasm32. Nothing is copied by hand. The generator runs as part of the build, so the
bindings always match the engine they ship with.

## Fit with the script model

`IScriptManager` loads source text at run time; C# cannot (and under NativeAOT, nothing loads at
run time at all). C# is a project-level assembly instead: the project's `.cs` files build, through
`dotnet build` run by the cook, into one assembly whose classes behaviours name by class, as they
name AngelScript and Luau classes. In the editor the assembly loads into a collectible load
context and reloads when it rebuilds; at export it is compiled into the player.

## Phases

### D0: the web test (the deciding one)

The riskiest part first. NativeAOT-LLVM is experimental (`Microsoft.DotNet.ILCompiler.LLVM`,
10.0.0-rc.1 on the dotnet-experimental feed), and its bitcode needs the LLVM of emsdk 3.1.56,
while our web build uses emsdk 6.0.5. A minimal C# static library, built with NativeAOT-LLVM for
`browser-wasm`, linked into a minimal program built with our emsdk:

- C# exports an entry point the C++ side calls; C++ hands it a function table; C# calls back
  through it and returns a value the C++ side checks.
- Runs headless under Node (no browser needed to answer the question), then in the web player.
- Single-threaded, as our web build is.

Done when it links and runs with both Emscripten versions in play, or when we know it cannot (and
what would have to change). The second emsdk is installed outside the repository.

### D1: desktop hosting

The player and the editor load .NET 10 through `hostfxr` (from the installed SDK; how a snap
install exposes `hostfxr` is checked), load a test assembly, and make one round trip each way
through the same entry points and function table as D0.

### D2: the bindings generator

From reflection: core value types, entities, components, the script facades. Generated layouts with
their checks; the function table's C side. Tested by a C# test assembly calling through it.

### D3: behaviours

A C# class as a behaviour: the same lifecycle, properties in the inspector and saved with the
scene, the same `X.of(entity)` component access. A project may mix C# with the other languages
across runs; one run still has one script language.

A project's C# is one assembly, so its files share types freely. The cook gains a project-wide
product for it, the script assembly:

- Its builder gathers every `.cs` in the project's sources and builds them together against the
  engine's runtime (`dotnet build`), and writes the assembly image, its debug symbols (so a fault
  names its file and line) and every public class's description (its handlers, its inspector
  fields with their defaults, tooltips and replication). Its dependencies are all the `.cs` files.
- A build that fails writes nothing: the last good assembly keeps running, as a failed class cook
  keeps the last good class. One broken file holds back every C# class until it builds again.
- Each `.cs` asset still cooks to a `ScriptClass` (language `csharp`), so scenes, the inspector
  and replication are unchanged; its handlers and properties come from the assembly's
  description, and the class names the assembly instead of carrying bytecode of its own.
- At run time the run host hands the backend the class's assembly image; an image loads once
  however many classes share it, and a rebuilt one loads beside the old (where D4 starts).
- The editor's code page reports C#'s errors from the project build. Errors as one types would
  need the compiler in the editor (Roslyn), later.

### D4: editor hot reload

Rebuild on change, reload into a new collectible load context, carry live behaviours' state across
as the other backends do; the old context unloads.

### D5: export

NativeAOT for the desktop player; NativeAOT-LLVM for the web export, through the export pipeline
(templates gain the C# runtime pieces they need).

## Known risks

- NativeAOT-LLVM is unversioned and experimental; its emsdk differs from ours, and LLVM bitcode from
  one version linked by another may not hold. D0 exists to find out.
- A snap-installed SDK may not expose `hostfxr` where an embedder looks for it.
- Build times: `dotnet build` per change in the editor, NativeAOT at export.
