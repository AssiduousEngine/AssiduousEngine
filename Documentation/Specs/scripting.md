# Scripting: faults reported, cook and run agreeing, a cheaper call path

> STATUS: APPROVED 2026-11-14 (user: "spec approved"). Sized L, in phases C0 to C6, each green on
> clang and gcc before the next and on the five lanes at the end.
> Origin: a read-only review of the AngelScript integration (2026-11-14, kept local), and the
> scripting items of backlog-0.1's "What the stability spec left out". Every finding below was
> read again in source on master at d470797aa; the ones that did not hold as stated are corrected
> in place. Read CONVENTIONS.md first.

## Goal

A script fault always reaches the user with where it happened; what the cook accepts, the runtime
runs; and a call into script, a math expression and a frame of a game's scripts cost what the
work needs, not what the plumbing adds. Both languages, AngelScript and Luau, behave the same
wherever both have the feature: a fix in one is made in the other, or the spec says why not.

Not goals: typed messages and delayed posting, an update scheduler for interval updates, interned
string literals, generated up and down casts, stripping the compiler or debug information from
shipped builds, validating hostile bytecode. Those go to `backlog-0.2.md` under "Scripting: later".

## Decided (by this spec; change on review)

1. **Math types are values in script** (user, 2026-11-14). A reflected type that is trivially
   copyable and trivially destructible (Float2, Float3, Float4, Color, Quaternion, Transform,
   Float4x4, Float3x3, Guid) crosses into script by value: AngelScript holds it inline
   (`asOBJ_VALUE | asOBJ_POD`, its real size), and a property read returns a copy, so
   `light.color.r = 1` changes a temporary, as with a C# struct. Luau gets the same semantics, so
   a script means the same in both. A script property of such a type is declared by value
   (`Color tint`), no longer as a handle; the cook's rule flips, and the samples' scripts migrate.
2. **One reflection fact decides it**, not a list of type names: `TypeInfo` gains a flag set when
   the type is built, from the type's own traits.
3. **`#include` is refused at cook** with an error naming the line, as the runtime already
   refuses it. Tracked includes are a feature for later, not a fix.
4. **The garbage collectors step each frame within a time budget** and run a full cycle only where
   a pause is expected (a scene loaded or unloaded, a run stopped). Both languages.
5. **Every phase that claims a cost is measured** before and after in `build/clang-reldbg`, on a
   benchmark kept in the repository, and the numbers go in the weekly.
6. **What both languages share lives once**, in `Foundation/Script` or `Script.Pipeline`, as the
   check manager does (`MakeCheckManager`, `CachedCheckManager`).

## What exists (checked 2026-11-14 at d470797aa)

- Every reflected type is an AngelScript reference type (`AngelScriptScriptImpl.cpp:2314`,
  `RegisterObjectType(scriptName, 0, asOBJ_REF)`); a value crossing in or out is a `BoxedVariant`
  allocated from `DefaultAllocator()` (`:449-471`), so `a + b` on two `Float3` allocates.
- `TypeInfo` records size, alignment, fields with `PropertyInfo::address`, constructors and
  operator-marked methods (`CoreReflectionImpl.cpp:123-164` for Float3), but no plain-value fact.
  `Variant` holds up to 24 bytes inline (`Variant.cppm:596`).
- Calls take a context from `RequestContext` and give it back with `ReturnContext` (`:951-993`,
  `:3563`); nothing installs `SetContextCallbacks`, so the library makes and frees one per call.
- A handler is found by building a `core::String` of its name and scanning the class's methods on
  every call (`:3867-3879`).
- `ScriptRunHost::Advance` collects garbage every frame (`ScriptSubsystem.cppm:336-343`), which is
  a full cycle in both backends: `asGC_FULL_CYCLE` (`AngelScriptScriptImpl.cpp:688`) and
  `lua_gc(LUA_GCCOLLECT)` for each Luau context (`LuauScript.cppm:410-416`).
- A coroutine that faults is dropped silently, and any abort is reported as a budget stop
  (`ResolveCoroutineExecution`, `:2129-2144`). The debugger is armed only in `ExecuteCall`
  (`:3583-3586`), never for a coroutine's context.
- Exception reports carry file, line and message, no call stack, although the debugger walks one
  (`:4132-4137`). AngelScript's compiler warnings are dropped (`:2172-2175`).
- Calls marshal at most 16 arguments (`kMaxArgs`, `:446`); the rest are dropped with a warning.
- The runtime adds raw script sections (`:718`, `:809`) and so refuses `#include`; the cook's
  `CScriptBuilder` has no include callback and reads an included file from disk.
- The script surface hash covers method names, parameter counts and enumerator names, not
  parameter or return types or enumerator values (`ScriptAssetImpl.cpp:104-139`), so a signature
  change of the same arity leaves stale bytecode that fails at load.
- Luau marshals f32, i32, i64, u32 and u64 parameters, not i8, u8, i16 or u16
  (`LuauScript.cppm:1264` on), though it declares them as numbers (`:108-110`).
- AngelScript's memory does not go through the engine's allocators (no
  `asSetGlobalMemoryFunctions`); Luau's does, with its cap (the stability series).
- Not rechecked here, rechecked when their phase starts: `EnsureClassLoaded` compiling every
  known class for each new one (`ScriptSubsystem.cppm:447`); old behaviour modules kept after a
  reload; handlers found by scanning source text at cook (`ScanScriptHandlers`); Luau's
  `waitUntil` predicate running outside a `ScriptCallScope`; `ContainerDispatch` and a gone
  object; Luau's `lua_error` unwinding past C++ destructors.

## Phases

**C0: a benchmark.**
- A benchmark case in each backend's suite, skipped unless asked for, run from
  `build/clang-reldbg`: a behaviour's handler called N times; a script doing Float3 and Color
  arithmetic in a loop; a hundred coroutines waiting; a frame's collection, idle and with a
  thousand objects of garbage. It prints per-call and per-frame times; the baseline goes in the
  weekly. Allocation counts join in C3, once boxes come from the manager's allocator, which a
  test can wrap in a tracking allocator (`DefaultAllocator()` keeps no counts).

**C1: every fault reported, with where.**
- A coroutine that faults is reported like any call's fault, and an abort says why (budget,
  debugger stop, the run ending), in both backends.
- Exception reports carry the script call stack, from the walk the debugger already does; Luau
  gives its traceback the same shape.
- AngelScript's compiler warnings reach the problems list as warnings; Luau's lint warnings
  likewise, where its compiler gives them.
- The debugger is armed for coroutine contexts, so a breakpoint inside a coroutine hits.
- A call or registration past 16 arguments is an error at registration, not a silent drop.
- Luau marshals i8, u8, i16 and u16; `waitUntil`'s predicate runs inside a `ScriptCallScope`.
- `ContainerDispatch` refuses a gone object, and a failed invoke reports; Luau's error paths raise
  only after the C++ objects of the thunk are destroyed.
- Tests: Script.AngelScript.Tests, Script.Luau.Tests, and the shared conformance battery for
  what both languages must do alike.

**C2: the cook and the runtime agree.**
- `#include` is a cook error (Decided 3).
- The surface hash covers parameter and return types and enumerator values; a signature change
  recooks.
- Handlers come from the compiled class, not from scanning source text (recheck first).
- `EnsureClassLoaded` compiles a new class without rebuilding every other, and a class that
  fails does not fail the ones after it (recheck first), in both languages.
- A reload discards a generation's modules once no live instance uses them.
- Tests: Script.Pipeline.Tests, Script.AngelScript.Pipeline.Tests, Engine.Script.Tests.

**C3: the call path.**
- Each class's handler functions are resolved once, when its module loads, and called through
  the cached pointer; Luau caches its function references the same way.
- A context pool through `SetContextCallbacks`, so a call reuses a context.
- The collectors step within a budget per frame (Decided 4).
- Boxes come from the manager's allocator; a coroutine's wait finds its record without a scan.
- Measured against C0.

**C4: math as values** (Decided 1, 2).
- The `TypeInfo` flag; AngelScript registers such types as values with their size, fields at
  their offsets, constructors and operators through generic thunks that copy bytes in and out.
- Luau gives the same copy semantics.
- The cook's handle rule flips to by-value for these types; the samples' scripts migrate, and a
  script still declaring `Color@` gets an error naming the change.
- Measured against C0: the arithmetic loop allocates nothing for Float3, Float4, Color and
  Quaternion (a `Variant` holds them inline); Transform and the matrices are measured and said.
- Tests: the backends' suites, Integration's script flows, and every sample cooking and running
  its levels.

**C5: AngelScript's memory.**
- AngelScript allocates through an engine allocator, with the same cap Luau has. The library's
  memory functions are process-wide, not per engine, so the cap is one for every AngelScript
  manager in the process (accepted with the spec).

**C6: conveniences** (each its own commit, each optional on review).
- Default argument values from reflection; bitflag enums; an export of the bound API as a
  declarations file for external editors (`DescribeBoundApi` already holds it).

## Gotchas

- gcc 15 writes an unreadable module for an out-of-class inline definition that instantiates a
  template; define such members in the class.
- A new `DefaultAllocator()` outside the allowlist fails Core.Tests' tripwire; thread the owner's.
- Measure in `build/clang-reldbg`, never Debug.

## Tests (the contract)

Every behaviour changed lands with a test in the suite that owns it, failing on the old code
where it can (or under ASAN for a lifetime bug); what both languages must do alike goes in the
conformance battery or a per-language test body shared as `CachedCheckManagerTests.cpp` does.
