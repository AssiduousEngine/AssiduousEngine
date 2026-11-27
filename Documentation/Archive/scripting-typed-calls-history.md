# Scripting: measured, then typed calls and bound names

> ARCHIVED 2026-11-27. Built 2026-11-18 to 20, steps 1 to 6 (the general typed path on master, the C# half merged with dotnet), with the numbers in the spec. The web measures and the C# runtime's allocator are in weekly_backlog.md; the call costs its last section leaves each to its own decision are in backlog-0.2.md. Non-authoritative: the spec as approved, kept as the record of what was built and why; line numbers will have drifted.

> APPROVED 2026-11-18. Nothing here is built. Read on master at `6cebdf698` (AngelScript, Luau,
> the engine layer) and on `dotnet` at `094a81e4b` (C#); `dotnet` has been rebased since, so the
> line numbers below are read again before each step is built. No timings were taken: every
> statement about cost says what work a path does, not how long it takes. Step 1 puts numbers on
> it, against native code.
>
> Moving the language backends into plugins is a separate proposal; neither depends on the
> other.
> A context per language in one run is a feature rather than a speed change, and is in
> backlog-0.2.

## Goal

Reflection describes the script surface; it stops being the run-time call path for the
languages that know their types. Nothing a script author writes changes. How far down the list
of changes this goes is decided by measurement, first and after each step.

## Step 1: measure against native code

Every later step names the measure it should move, and is measured before and after. This step
builds the measures and takes the first numbers; it changes no engine code.

### What is timed

Each measure runs the same work in native C++ (the baseline), AngelScript, Luau and C#.

| Measure | The work | Native baseline | Reports |
|---|---|---|---|
| Engine call | 1,000,000 calls of a reflected test method taking and returning an `f32` | the same method through a function pointer the compiler cannot inline | ns per call, allocations |
| Value method | 1,000,000 `Float3.Cross` | the C++ function | ns per call, allocations |
| Object return | 1,000,000 fetches of a component from an entity | `Get<T>` on the entity | ns per call, allocations, collections |
| String argument | 1,000,000 calls passing a 16-character string | a `StringView` argument | ns per call, allocations |
| Handler dispatch | 1,000 behaviours with an empty update, 1,000 frames | 1,000 native components updated by the same loop | ms per frame (median, 99th percentile), allocations per frame |
| Movers | N entities each reading its transform, doing `Float3` arithmetic and writing it back, N = 100, 1,000 and 5,000, 1,000 frames | the same as a native component | ms per frame (median, 99th percentile), allocations per frame |
| Collector | the benchmark's `churn`: objects made and dropped, freed by frame steps | none | the longest frame |

The existing script benchmark (`Script.Tests/BackendBenchmark.h`: a handler call, value math,
waiting coroutines and the collector, one `[bench]` line per measure) is extended with the
first four measures and gains a native runner. Handler dispatch and Movers run a real scene
through the script subsystem, so they live in `Code/Integration` as a backend-neutral suite (one
source, every language), marked `doctest::skip()` and run with `--no-skip`.

### Where it runs

- **Desktop:** `build/clang-reldbg` (an optimised build; Debug times say little). Native,
  AngelScript and Luau on master; C# under CoreCLR on `dotnet`, where C# exists. The native
  baseline is on master, since it is not C#.
- **Web:** C# in the browser runs through NativeAOT-LLVM, where the bridge's .NET reflection
  runs interpreted; the Holdout lag was seen there. A small benchmark project exports the
  Movers and Handler dispatch scenes to the web and prints the same `[bench]` lines to the
  browser's console; the user runs it on the desktop browser and on the phone.
- Each run records the machine, the build tree and the commit beside its numbers.

### What it produces

A table in this spec (Measurements, below) and in the week's plan: each measure in each
language as a multiple of native. That table decides how far the order of work goes before a
release, and each later step adds its after-numbers to it.

## Measurements

**Step 1, desktop, 2026-11-18.** i7-10875H, `build/clang-reldbg` with C# on, `dotnet` at the
measures' commit (master's engine with the C# backend); no other build or tool running; two runs,
each figure their range. Every language makes the same calls into the engine (`Math.Abs`,
`Float3.Cross`, `Entity.setName` on an entity with no scene, `PathFollowComponent.of`); C# runs its
assembly built Debug (as the engine's cook builds a game's) and Release. All from
`Integration.ScriptScene`.

Per call: a million calls in a script loop, "alone" being the measure less the loop with no call.

| Per call | native | AngelScript | Luau | C# Debug | C# Release |
|---|---|---|---|---|---|
| Loop with no call (per iteration) | | 21 to 28 ns | 28 ns | 2.5 ns | 1.2 ns |
| Engine call (`Math.Abs`), alone | 3.6 ns | about 205 ns | about 160 ns | about 210 ns | about 150 ns |
| Value method (`Float3.Cross`), alone | 3.5 ns | about 690 ns | about 550 ns | about 240 ns | about 240 ns |
| String argument (`setName`, 16 characters), alone | 5 ns | about 230 ns | about 265 ns | about 245 ns | about 215 ns |
| Handler call (engine into script) | | 270 to 310 ns | 240 to 250 ns | 725 to 735 ns | 870 to 880 ns |
| Value math, one iteration (`math`) | | 1.5 to 1.6 µs | 0.78 to 0.80 µs | 0.40 to 0.41 µs | 0.24 to 0.25 µs |

C#'s value math is written as C# writes it today: the sum, the scaling and the Color field by
field (its bindings give Float3 no operators and Color no constructor), so only `Float3.Dot`
crosses; the other languages' version crosses for the operators and the Color too.

Per scene: a whole frame through the script systems, 1,000 frames after a warm-up, the median; the
native column is the same work in a component manager on the same scene, and every column includes
the scene's own phases.

| Median frame | native | AngelScript | Luau | C# Debug | C# Release |
|---|---|---|---|---|---|
| 1,000 empty update handlers | 0.0056 ms | 0.33 ms | 0.27 ms | 0.37 to 0.38 ms | 0.37 ms |
| 100 movers | 0.0039 ms | 0.12 ms | 0.091 to 0.095 ms | 0.084 to 0.086 ms | 0.084 to 0.087 ms |
| 1,000 movers | 0.039 to 0.041 ms | 1.25 ms | 0.95 to 0.98 ms | 0.85 to 0.88 ms | 0.86 ms |
| 5,000 movers | 0.20 ms | 6.3 ms | 4.85 ms | 4.3 to 4.4 ms | 4.3 to 4.4 ms |
| 1,000 component fetches | 0.0048 ms | 0.49 ms | 0.40 to 0.43 ms | 0.36 to 0.39 ms | 0.35 to 0.38 ms |

Allocations, counted through each language's own allocator: none per call or per frame, except
AngelScript's component fetch, once per fetch. C#'s are not visible to this count (its objects
live on .NET's heap, and the bridge allocates the engine objects it hands C# from the allocator
the runtime started with), so a C# zero says nothing yet.

What the numbers say:

- Every crossing from script into the engine costs 150 to 270 ns in every language, 40 to 75 times
  a native call; the bridge, not the language, sets it. Part 1's typed entry is aimed at exactly
  this.
- A value method is about 3.4 times an ordinary call in AngelScript and Luau, and little more than
  one in C# (240 against 150 to 210 ns), whose bindings pass a Float3 as its bytes: building and
  unpacking `Float3` Variants is the difference.
- C# runs its own loop 10 to 20 times faster than the other two, but the engine calling a C#
  handler costs 2.5 to 3 times what it costs them (Part 3's handler binding), and in a scene,
  where the crossings dominate, C# is level with Luau. Release over Debug changes C#'s own code
  (the loop, the math), not the crossings: a game's Debug build costs little in a scene.
- 5,000 movers take 26 to 38% of a 60 Hz frame in script against 1.2% natively.

**After step 3 (C#), desktop, 2026-11-18.** The same machine and tree, three runs, with C#'s
handlers bound (3a), its calls on the typed entry (3b), its value math in C# (3d), its components
held as values (3e) and the engine's C# runtime built Release. The measures gained `entity-call`
(`Entity.isValid()` on an entity with no scene), a call every language makes into the engine:
`Math.Abs` is pure now, so C# computes it without crossing. AngelScript and Luau did not change;
their columns are this run's.

Each measure now runs warm: a per-call measure repeats for at least a second and until two runs
in a row agree, and a scene measure runs a second of frames before the timed ones. C#'s JIT
compiles a method unoptimised first and recompiles it, optimised, in the background once it is
hot, after a delay; a single run of a million calls was partly timed before that landed, by an
amount that depended on what ran before it in the process. That is why C# strings looked slower
from a Release assembly: it ran second. The C# figures for step 1 and 3a to 3d carry the same
error, more in the per-call measures than in the scene ones.

| Per call, alone | native | AngelScript | Luau | C# Debug | C# Release |
|---|---|---|---|---|---|
| Engine call (`Entity.isValid`) | 3.1 to 3.4 ns | 205 to 215 ns | 150 to 160 ns | 53 to 55 ns | 54 to 55 ns |
| Pure function (`Math.Abs`) | 3.5 to 3.7 ns | 200 to 210 ns | 150 to 155 ns | 1.4 ns, in C# | about 0.1 ns, in C# |
| Value method (`Float3.Cross`) | 3.6 to 3.7 ns | 635 to 695 ns | 515 to 545 ns | about 12 ns, in C# | under 0.1 ns, in C# |
| String argument (`setName`, 16 characters) | 4.7 to 5.1 ns | 215 to 225 ns | 245 to 255 ns | 80 to 82 ns | 80 to 82 ns |
| Handler call (engine into script) | | 260 to 265 ns | 180 to 195 ns | 63 to 64 ns | 58 to 63 ns |
| Value math, one iteration | | 1.5 to 1.6 µs | 0.75 to 0.80 µs | 40 to 42 ns | 1.0 to 1.1 ns |

| Median frame | native | AngelScript | Luau | C# Debug | C# Release |
|---|---|---|---|---|---|
| 1,000 empty update handlers | 0.0053 to 0.0056 ms | 0.32 to 0.33 ms | 0.26 ms | 0.13 ms | 0.13 ms |
| 100 movers | 0.0037 to 0.0039 ms | 0.12 ms | 0.087 to 0.091 ms | 0.033 to 0.034 ms | 0.034 ms |
| 1,000 movers | 0.037 to 0.038 ms | 1.19 to 1.20 ms | 0.91 to 0.93 ms | 0.34 to 0.36 ms | 0.34 to 0.35 ms |
| 5,000 movers | 0.19 ms | 6.05 to 6.1 ms | 4.6 ms | 1.76 to 1.81 ms | 1.76 to 1.78 ms |
| 1,000 component fetches | 0.0047 to 0.005 ms | 0.46 to 0.47 ms | 0.39 to 0.40 ms | 0.235 to 0.24 ms | 0.235 to 0.25 ms |

- A C# call into the engine costs 55 ns, a quarter to a third of the other languages' 150 to
  215 ns: the typed entry and a Release runtime. A Debug game pays the same per crossing, since
  the crossing runs the runtime's code; only its own code is slower.
- Pure value functions no longer cross at all, so C#'s value math runs at native speed from a
  Release assembly.
- 5,000 movers take 11% of a 60 Hz frame in C#; AngelScript and Luau still take 28 to 36%.
- A C# string argument costs about 25 ns more than a call without one: the call encodes its
  UTF-8 into stack storage and the engine copies it into a String. Before the stack storage it
  cost 45 to 55 ns more (100 to 110 ns a call), counting the UTF-8 first and allocating and freeing
  native memory for it each call; a string over 85 characters still does.
- That a component fetch no longer allocates a C# object follows from its being a struct; .NET's
  heap is not counted here, so it is not measured.

**After steps 3 to 6, desktop, 2026-11-18.** The same machine and tree, warm, three runs; C#'s
third run was disturbed by something else on the machine (its entity call 58 against 40 to 43
ns), so C# is from the first two. "In C#" means C# computes it without calling the engine.

| Per call, alone | native | AngelScript | Luau | C# Debug | C# Release |
|---|---|---|---|---|---|
| Loop with no call (per iteration) | | 22 to 25 ns | 7 to 9 ns | 2.2 ns | 0.9 ns |
| Engine call (`Entity.isValid`) | 3.6 to 3.8 ns | 45 to 47 ns | 116 to 121 ns | 40 to 41 ns | 39 to 42 ns |
| Pure function (`Math.Abs`) | 3.5 ns | about 50 ns | 74 to 79 ns | 1.3 ns, in C# | about 0.1 ns, in C# |
| Value method (`Float3.Cross`) | 3.4 to 3.5 ns | 147 to 155 ns | 184 to 196 ns | about 12 ns, in C# | under 0.1 ns, in C# |
| String argument (`setName`, 16 characters) | 5.3 to 5.9 ns | 168 to 180 ns | 136 to 142 ns | about 67 ns | 69 to 70 ns |
| Handler call (engine into script) | | 239 to 251 ns | 151 to 157 ns | 62 to 63 ns | 60 to 65 ns |
| Value math, one iteration | | 0.82 to 0.87 µs | 0.41 to 0.43 µs | 42 ns | 1.0 ns |

| Median frame | native | AngelScript | Luau | C# Debug | C# Release |
|---|---|---|---|---|---|
| 1,000 empty update handlers | 0.006 ms | 0.30 to 0.31 ms | 0.21 to 0.22 ms | 0.12 ms | 0.12 ms |
| 100 movers | 0.004 ms | 0.060 to 0.063 ms | 0.060 to 0.064 ms | 0.029 ms | 0.029 ms |
| 1,000 movers | 0.039 to 0.042 ms | 0.62 to 0.65 ms | 0.61 to 0.64 ms | 0.31 ms | 0.30 ms |
| 5,000 movers | 0.20 to 0.21 ms | 3.1 to 3.3 ms | 3.1 to 3.3 ms | 1.57 ms | 1.54 to 1.55 ms |
| 1,000 component fetches | 0.005 ms | 0.42 to 0.44 ms | 0.36 to 0.37 ms | 0.225 ms | 0.22 ms |

- 5,000 movers, against the start of the day: C# 4.35 to 1.55 ms, AngelScript 6.3 to 3.2, Luau
  4.85 to 3.2; native 0.2. A 60 Hz frame spends 9% on them in C#, 19% in AngelScript and Luau.
- A call into the engine is now 40 to 120 ns in every language, from 150 to 270; the rest of a
  script's cost is its own code and the handler call into it.
- Luau's handler call (150 ns) and AngelScript's (240 ns) are now the larger part of a behaviour:
  the engine into script, a pooled thread or context per call.
- Strings still cross by copy into a String in every language.

**Found while measuring (C#).** The process's C# runtime keeps the allocator its first script
manager was made with, for every engine object it hands C# from then on (the bridge's
`EngineFunctions`). A first manager with a shorter-lived allocator than the process leaves it
dangling: the next object handed to C# crashes. The application run and the editor make their run
hosts over the process's root allocator, so they do not hit it today; the measures' suite starts
the runtime on one before any manager. To decide: the allocator a process-wide runtime owns.

The web (C# through NativeAOT-LLVM, and the other languages compiled to WebAssembly) follows.

## What stays as it is

These are the parts that are right and that the changes below build on:

- One reflected surface bound in every language, with name collisions refused at registration
  (`Script/RegisterTypes.cppm`).
- The backend registry by language and file extension, and capability flags for optional
  features (`Script/BackendRegistry.cppm`, `Script/IScriptManager.cppm`).
- Cooked script classes with declared handlers, defaults and per-class bytecode, so a player
  ships no compiler.
- Call budgets, a fault disabling one behaviour, and the object-gone error.
- Messages and engine events queued and drained at the top of the tick.
- Services found by type per run (`CurrentService<T>`), not process globals.

## Part 1: a typed call path beside the Variant one

### Today

Every call from script into the engine goes through `MethodInfo::invoke`, which takes a
`Span<Variant>` and returns a `Result<Variant>`. Each backend builds the `Variant`s, and the
reflected thunk then checks and converts each one again (`InvokeMemberImpl`, `Reflection.cppm`):

| Backend | How a call is built | Where |
|---|---|---|
| AngelScript | generic calling convention, a stack array of 16 `Variant`s | `MethodDispatch`, `AngelScriptScriptImpl.cpp:3888` |
| Luau | a heap `Array<Variant>` per call | `LuauScript.cppm:1438`, `:1955` |
| C# | `DotNetValue`s on the C# stack, then a heap `Array<Variant>` on the native side | `DotNetBridgeImpl.cpp`, on `dotnet` |

AngelScript and C# know every argument's type when the script is compiled. For them the
`Variant` is built only to be taken apart again.

### The change

`MethodInfo` gains a second entry, generated by the same template that generates `Invoke`:

```cpp
struct MethodInfo
{
    // ... as today ...
    Result<Variant> (*invoke)(const Instance&, Span<Variant>);

    // The typed entry: args[i] points at a value of params[i]'s type, result at storage for
    // the return type. No Variant and no checks: the caller has matched the signature.
    // Null where a parameter has no fixed native form (a script list for an Array<E>).
    void (*call)(void* self, void* const* args, void* result) = nullptr;
};
```

Reading one argument from its pointer, beside `ConvertArg`:

```cpp
template <typename A>
decltype(auto) RawArg(void* slot)
{
    using Bare = std::remove_cvref_t<A>;
    if constexpr (ArgRefPtr<Bare>::value) // RefPtr<U>: the slot holds a U*
    {
        using U = typename ArgRefPtr<Bare>::Pointee;
        return RefPtr<U>(*static_cast<U**>(slot));
    }
    else // scalars, enums, value structs, StringView, object pointers
    {
        return *static_cast<Bare*>(slot);
    }
}
```

The call, beside `InvokeMemberImpl`:

```cpp
template <auto Member, typename C, typename R, bool Const, typename... A, usize... I>
void CallMemberImpl(void* self, void* const* args, void* result, std::index_sequence<I...>)
{
    auto* object = static_cast<std::conditional_t<Const, const C, C>*>(self);
    if constexpr (std::is_void_v<R>)
    {
        (object->*Member)(RawArg<A>(args[I])...);
    }
    else
    {
        new (result) R((object->*Member)(RawArg<A>(args[I])...));
    }
}
```

`MethodReflect` gains `static void Call(void*, void* const*, void*)` beside `Invoke`, and the
builder that stores `&Reflect::Invoke` (`Reflection.cppm:2084`, `:2127`) stores `&Reflect::Call`
with it. No reflection site changes: every method reflected today gets the typed entry.

`PropertyInfo` and `ConstructorInfo` take the same pair by the same pattern.

This is the shape Godot uses: `MethodBind` has `call` (Variants), `validated_call` and `ptrcall`
(`core/object/method_bind.h:116-119` in the local clone).

### Rules the typed entry needs

- **Checking moves to bind time.** A backend may use `call` only where it has matched each
  parameter's type when it bound or compiled the call. A backend that cannot (Luau) stays on
  `invoke`.
- **The gone-object check stays in the backend**, before the call, as `RefuseGone` does now.
- **A returned object** is written as a pointer with a reference already added; the caller
  releases it.
- **A returned string** is constructed in the caller's storage as a `String`; the caller
  destroys it.
- **`call` is null** for a method with a parameter that has no single native form. The backend
  falls back to `invoke` for that method. The conformance tests assert which methods have it, so
  a method silently losing its typed entry is caught.

### What each backend does

**C#.** The generator emits pointers instead of `DotNetValue`s:

```csharp
void** args = stackalloc void*[2];
args[0] = &a;
args[1] = &b;
Float3 returned;
s_engine.Call(s_m3, null, args, &returned);
return returned;
```

The native side of the bridge for a call becomes `method->call(self, args, result)`. The
argument array, the scalar compare chain and the second type check all go. The layout check C#
already runs at start (`Layouts.FirstMismatch`) is what makes passing struct bytes safe.

**AngelScript.** `MethodDispatch` passes the addresses AngelScript already has:

```cpp
void* args[kMaxArgs];
for (int i = 0; i < argc; ++i)
    args[i] = gen->GetAddressOfArg(static_cast<asUINT>(i));
method.call(self, args, gen->GetAddressOfReturnLocation());
```

This holds where AngelScript's stack form of an argument is the native type. Built: a binding is
marked typed at registration when every parameter and its return are held natively (a number or
bool at its width, an enum of AngelScript's 32-bit int, a script value by `const &in`); values on
AngelScript's 4-byte-aligned stack that need more alignment are copied aside first. Strings and
handles keep `invoke`. Value constructors and property reads and writes take their typed entries
the same way.

**Luau.** The plan was to stay on `invoke` with a stack array; the stack array measured no gain
(Luau's allocator is cheap), and matching per call turned out cheap enough to make the typed
entry worth it. Built: a script value crosses as a value box (its type, then its bytes), and a
call whose arguments are already in their parameters' native form (a number, a boolean, a vector
for a Float3, a value box of the exact type, a string) takes `call` with them where they are,
matched per call as C# matches; anything else takes `invoke`. Value constructors likewise.

## Part 2: names bound once

| Today | Where | Change |
|---|---|---|
| `HasHandler` compares strings down the class's handler list on every dispatch | `ScriptResource.cppm:382` | the cooked class resolves the fixed lifecycle handlers to bits at load; `InvokeHandler` tests a bit |
| The engine names a handler by text on every call | `BehaviorObject::Invoke(StringView, ...)` | `BehaviorObject::Bind(name)` returns a handle once at instantiate; `Invoke(handle, args)` per call |
| A Luau property access compares C strings down the type's property list | `FindProperty`, `Reflection.cppm:377` | a per-type table from interned name to property, built at type emission |
| C# finds a handler through .NET reflection on every call | `Runtime.cs`, on `dotnet` | a delegate or generated function per handler, resolved at class load; see the C# cost audit |

Luau looks a handler up on the instance on every call on purpose, so that a script can assign
`self.onUpdate` at run time (`LuauScript.cppm:3088`). That stays. `Bind` for Luau returns the
interned name, which removes the string hash per call and keeps the lookup.

## Part 3: per language

- **Luau value types.** Only `Float3` is a native Luau value; every other value type is pushed
  as a collected userdata holding a `Variant` (`LuauScript.cppm:1192`). Tagged userdata sized to
  the value's own bytes, one tag per value type, removes the `Variant` and lets the property
  thunks read fields directly. It does not remove the allocation; Luau has one native vector
  type.
- **C#.** Beyond the typed entry, from reading the bridge on `dotnet` (2026-11-17), in order of
  value:
  1. **The value types' math as C# code.** A struct's reflected methods (`Float3.Cross` and the
     like) are generated as bridge calls today. The operators and methods are written in C#
     (partials beside the generated fields, or generated from a table) and the generator skips
     the reflected methods they cover, so value math does not cross.
  2. **Handlers bound once.** The engine calls a C# handler by name: a managed string from the
     name, a dictionary lookup, `GetParameters()`, an `object?[]` with every argument boxed, and
     `MethodInfo.Invoke`. At class load each handler resolves to a delegate or a generated static
     function, and the engine calls it by index (Part 2's `Bind`). A source generator in the
     game's project makes this free of .NET reflection, which NativeAOT runs interpreted.
  3. **No argument array on the heap.** A stack buffer for the common argument counts in the
     bridge's `Invoke` and `ConstructOf`, and in `DotNetRuntime::Lend`.
  4. **The scalar kind passed, not found.** The generator knows each argument's type; a kind in
     `DotNetValue` (its `Reserved` field is free) replaces the compare chain in `ScalarVariant`.
     The typed entry removes this where it applies.
  5. **No allocation per returned object.** One wrapper cached per engine object, or component
     and entity wrappers as structs over a handle with no finalizer, the engine owning the
     lifetime. A finalizable object per fetch is collector load.
  6. **The release queue drained once a frame**, not on every call.
- **AngelScript.** Nothing beyond Part 1.

## Order of work

Each step stands on its own and leaves the tree green.

1. **Measure** (Step 1): the measures, native and every language, desktop and web.
2. **Part 1 in Core** (built 2026-11-18), with tests in Core.Tests that call every reflected
   method kind through both entries and compare. Moves nothing by itself; measured with step 3.
   Beyond the sketch above: ParamInfo carries each parameter's form (ParamForm), since a
   StringView parameter reads as String and a caller must know which to pass.
3. **C# onto the typed entry**, with the C# items of Part 3. It has the most to gain and its
   bindings are generated, so the change is in one generator. Moves: Engine call, Value method,
   Object return, String argument (C#).
4. **Part 2**: handler bits and `Bind`. Moves: Handler dispatch (every language). Built
   2026-11-18 as `Bind` and lifecycle handles per behaviour; it moved nothing measurable: the name
   compares were not the cost. A behaviour spends 50 to 90 ns of engine work outside the script
   call in every language, not yet attributed.
5. **Luau stack buffer**, then **AngelScript onto the typed entry** after the parameter-form
   check. Moves: Engine call (Luau, AngelScript). Built 2026-11-18 for AngelScript, with its value
   constructors and properties; the Luau stack buffer measured nothing and was dropped.
6. **Luau tagged value userdata**, if the Value method and Movers measures show value math in
   Luau matters. Built 2026-11-18 as value boxes, with Luau's calls and value constructors on the
   typed entry over them.

Found by profiling on the way (perf), and fixed: the call budget read the clock at every Luau
safepoint (a quarter of Luau's time, and its loops too), each behaviour's component was looked up
twice, AngelScript built sixteen Variants per call and C# sixteen Strings per call whatever they
passed.

## What remains

Steps 1 to 6 are built (2026-11-18). Left, each for its own decision:

- **Handler calls in AngelScript and Luau** (about 220 and 140 ns, against C#'s 60) are now the
  interpreters' own work. AngelScript: Prepare and Unprepare and the reference counts of every
  call; keeping a context prepared per handler function would skip them. Luau: a resume on a pooled
  thread, which the debugger relies on to break inside a handler; a plain pcall when no debugger is
  attached would be cheaper. Both change what the debugger depends on.
- **Luau's per-call method lookup** stays: it is what lets a script replace its own handler
  (`self.onUpdate = walking`).
- **Strings** cross by copy into a String in every language.
- **The behaviour list's memory**: each entity's behaviours are a heap array of their own, a cache
  miss each when a frame walks them; storing them contiguously would remove it.
- **The C# runtime's allocator** (Found while measuring (C#), above) is undecided.
- **The web**: the measures have not run there.

## The Beef mirror

Sedulous binds its scripts through compile-time reflection, so its calls are typed when they
are built and Part 1 has no counterpart there. The measures are the useful part to share: Step
1's harness and its native baseline can be ported, so Sedulous can find its own hot paths, and
Part 2's handler binding and Part 3's C# items may apply to it as they stand. Each step's
commits say which parts are C++ only.

## Risks

- **A typed call with a wrong pointer is memory corruption, not a script error.** The `Variant`
  path fails safely on a mismatch. The defence is that only generated or compiler-checked call
  sites use `call`, plus the conformance tests under ASAN.
- **Two call paths can drift.** A method could behave differently through `invoke` and `call`.
  Both are generated from the same template arguments, and the Core test in step 2 runs every
  method kind through both.
- **Scope.** No part changes behaviour a script author sees. A step that speeds up a path
  enough to notice adds a CHANGELOG line with its numbers.
