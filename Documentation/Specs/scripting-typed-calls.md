# Scripting: measured, then typed calls and bound names

> PROPOSED 2026-11-18. Nothing here is built. Read on master at `6cebdf698` (AngelScript, Luau,
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

None yet.

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

This holds where AngelScript's stack form of an argument is the native type. To check before
building: how each parameter kind is declared to AngelScript today (width of numbers, handles
against values). Handles need one pointer hop first.

**Luau.** Stays on `invoke`, with a fixed stack array in place of the heap `Array<Variant>`, as
AngelScript already has.

## Part 2: names bound once

| Today | Where | Change |
|---|---|---|
| `HasHandler` compares strings down the class's handler list on every dispatch | `ScriptResource.cppm:382` | the cooked class resolves the fixed lifecycle handlers to bits at load; `InvokeHandler` tests a bit |
| The engine names a handler by text on every call | `ScriptObject::Invoke(StringView, ...)` | `ScriptObject::Bind(name)` returns a handle once at instantiate; `Invoke(handle, args)` per call |
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
2. **Part 1 in Core**, with tests in Core.Tests that call every reflected method kind through
   both entries and compare. Moves nothing by itself; measured with step 3.
3. **C# onto the typed entry**, with the C# items of Part 3. It has the most to gain and its
   bindings are generated, so the change is in one generator. Moves: Engine call, Value method,
   Object return, String argument (C#).
4. **Part 2**: handler bits and `Bind`. Moves: Handler dispatch (every language).
5. **Luau stack buffer**, then **AngelScript onto the typed entry** after the parameter-form
   check. Moves: Engine call (Luau, AngelScript).
6. **Luau tagged value userdata**, if the Value method and Movers measures show value math in
   Luau matters.

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
