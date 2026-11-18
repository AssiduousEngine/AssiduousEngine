// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script-backend benchmark: the same work in each language, timed, so a change to the call
// path is measured before and after. Include from a doctest TU and call RunScriptBackendBenchmark
// inside a TEST_CASE marked doctest::skip(); run it with --no-skip from an optimised build
// (build/clang-reldbg), since a Debug build's times say little.
//
// It prints one line per measure, `[bench] <language> <measure> <value> <unit>`, for the weekly.
// RunNativeBenchmark does the per-call measures' work in C++, as `native`: the baseline each
// language is read against.
#pragma once
#include <doctest/doctest.h>
#include <cstdio>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.script;

namespace foundation::script::benchmark
{
    /// What a script calls in the engine for the per-call measures: reflected statics, so every
    /// language reaches them as `BenchProbe.Twice(x)` and the native baseline calls the same
    /// functions.
    struct BenchProbe
    {
        static foundation::core::f32 Twice(foundation::core::f32 x) { return x * 2.0f; }
        static foundation::core::i32 Length(foundation::core::String text) { return static_cast<foundation::core::i32>(text.Size()); }
    };

    REFLECT_VALUE(BenchProbe, "rtti::tests::benchmark")
    {
        builder.Method<&BenchProbe::Twice>("Twice", {"x"}).Method<&BenchProbe::Length>("Length", {"text"});
    }
}

namespace foundation::script::benchmark
{
    using namespace foundation::core;

    /// The per-call measures' sizes, the same in every language and natively.
    inline constexpr i32 kCalls = 1000000;
    /// The string the string measure passes: 16 characters.
    inline constexpr StringView kSixteen = u8"sixteen-chars-ab";

    struct Dialect
    {
        StringView languageId;
        // A class `Bench`, made with no arguments, with these methods:
        //   tick(dt)   adds dt to a field (a handler call with one number);
        //   math(n)    n iterations of Float3 arithmetic and a Color made from the result,
        //              returning a number so the work is not optimised away;
        //   spawn(n)   starts n coroutines that each wait 1000 seconds;
        //   churn(n)   makes n script objects that each refer to themselves and hold a Float3,
        //              then drops them, leaving garbage only the collector frees;
        //   loops(n)   the per-call measures' loop with no call in it, summing its index;
        //   calls(n)   n calls of BenchProbe.Twice, summing what they return;
        //   crosses(n) n calls of Float3.Cross, summing a component of the result;
        //   texts(n, s) n calls of BenchProbe.Length(s), summing what they return.
        StringView benchClass;
    };

    namespace detail
    {
        inline void Report(StringView language, const char* measure, f64 value, const char* unit)
        {
            std::printf("[bench] %.*s %s %.3f %s\n", static_cast<int>(language.Size()),
                        reinterpret_cast<const char*>(language.Data()), measure, value, unit);
        }
    }

    /// The per-call measures in C++, through function pointers the compiler cannot see through,
    /// reported as `native`: what a call costs with no language in the way.
    inline void RunNativeBenchmark()
    {
        f32 (*volatile twice)(f32) = &BenchProbe::Twice;
        Float3 (*volatile cross)(Float3, Float3) = &Cross;
        i32 (*volatile length)(String) = &BenchProbe::Length;
        {
            const Stopwatch clock = Stopwatch::StartNew();
            f32 sum = 0.0f;
            for (i32 i = 0; i < kCalls; ++i)
                sum += twice(static_cast<f32>(i));
            detail::Report(u8"native", "engine-call", clock.Elapsed().AsSeconds() * 1e9 / kCalls, "ns/call");
            CHECK(sum > 0.0f);
        }
        {
            const Stopwatch clock = Stopwatch::StartNew();
            f32 sum = 0.0f;
            const Float3 a{1.0f, 2.0f, 3.0f};
            for (i32 i = 0; i < kCalls; ++i)
                sum += cross(a, Float3{static_cast<f32>(i), 0.5f, 0.25f}).x;
            detail::Report(u8"native", "value-method", clock.Elapsed().AsSeconds() * 1e9 / kCalls, "ns/call");
            CHECK(sum != 0.0f);
        }
        {
            // A script passes its own string, so each call here makes one as well.
            const Stopwatch clock = Stopwatch::StartNew();
            i64 sum = 0;
            for (i32 i = 0; i < kCalls; ++i)
                sum += length(String(kSixteen));
            detail::Report(u8"native", "string-argument", clock.Elapsed().AsSeconds() * 1e9 / kCalls, "ns/call");
            CHECK(sum == static_cast<i64>(kCalls) * 16);
        }
    }

    /// `factory` makes the manager over the allocator it is given: the runner counts what goes
    /// through it, so a measure says how many allocations it made as well as its time. Those are
    /// the language's own; what the engine side allocates from the default heap is not counted.
    inline void RunScriptBackendBenchmark(const Function<RefPtr<IScriptManager>(IAllocator&)>& factory,
                                          const Dialect& dialect)
    {
        RegisterCoreTypes();
        RttiRegisterValue_BenchProbe();
        GlobalTypeRegistry().Register(TypeOf<BenchProbe>());
        TrackingAllocator counted(DefaultAllocator());
        RefPtr<IScriptManager> manager = factory(counted);
        REQUIRE(manager.Get() != nullptr);
        RegisterReflectedTypes(*manager);
        RefPtr<IScriptContext> scriptContext = manager->CreateContext();
        REQUIRE(scriptContext.Get() != nullptr);
        // What a script reports (a compile error, a failed call) is printed, so a failed run says why.
        struct Printer final : IScriptErrorHandler
        {
            void OnError(const ScriptError& error) override
            {
                std::printf("[bench] script error %.*s:%d: %.*s\n", static_cast<int>(error.module.Size()),
                            reinterpret_cast<const char*>(error.module.Data()), error.line, static_cast<int>(error.message.Size()),
                            reinterpret_cast<const char*>(error.message.Data()));
            }
        };
        Printer printer;
        scriptContext->SetErrorHandler(&printer);
        REQUIRE(scriptContext->Load(dialect.benchClass, u8"bench").IsOk());
        RefPtr<ScriptObject> bench = scriptContext->CreateInstance(u8"Bench", Span<Variant>{});
        REQUIRE(bench.Get() != nullptr);
        const StringView language = dialect.languageId;

        // A handler call: the cost of crossing into script and back.
        {
            constexpr u32 kCalls = 100000;
            Variant dt[] = {Variant::From<f64>(0.016)};
            const u64 allocations = counted.TotalAllocations();
            const Stopwatch clock = Stopwatch::StartNew();
            for (u32 i = 0; i < kCalls; ++i)
            {
                REQUIRE(bench->Invoke(u8"tick", Span<Variant>{dt, 1}).HasValue());
            }
            detail::Report(language, "handler-call", clock.Elapsed().AsSeconds() * 1e9 / kCalls, "ns/call");
            detail::Report(language, "handler-call-allocations",
                           static_cast<f64>(counted.TotalAllocations() - allocations) / kCalls, "per call");
        }

        // Math: Float3 arithmetic and a Color, in a script loop.
        {
            constexpr i32 kIterations = 100000;
            Variant n[] = {Variant::From<i32>(kIterations)};
            const u64 allocations = counted.TotalAllocations();
            const Stopwatch clock = Stopwatch::StartNew();
            REQUIRE(bench->Invoke(u8"math", Span<Variant>{n, 1}).HasValue());
            detail::Report(language, "math-iteration", clock.Elapsed().AsSeconds() * 1e9 / kIterations, "ns/iteration");
            detail::Report(language, "math-iteration-allocations",
                           static_cast<f64>(counted.TotalAllocations() - allocations) / kIterations, "per iteration");
        }

        // The per-call measures: one crossing from script into the engine, kCalls times in a
        // script loop. The loop is part of each; its cost alone is what `native` does not pay.
        {
            const auto measure = [&](StringView method, const char* name, Span<Variant> args)
            {
                const u64 allocations = counted.TotalAllocations();
                const Stopwatch clock = Stopwatch::StartNew();
                const Result<Variant> ran = bench->Invoke(method, args);
                const f64 seconds = clock.Elapsed().AsSeconds();
                if (!ran.HasValue())
                {
                    FAIL_CHECK(name << " did not run"); // a failed call times nothing
                    return;
                }
                detail::Report(language, name, seconds * 1e9 / kCalls, "ns/call");
                char allocationsName[64];
                std::snprintf(allocationsName, sizeof(allocationsName), "%s-allocations", name);
                detail::Report(language, allocationsName, static_cast<f64>(counted.TotalAllocations() - allocations) / kCalls,
                               "per call");
            };
            Variant n[] = {Variant::From<i32>(kCalls)};
            measure(u8"loops", "loop-alone", Span<Variant>{n, 1}); // subtract it from the others for a call's own cost
            measure(u8"calls", "engine-call", Span<Variant>{n, 1});
            measure(u8"crosses", "value-method", Span<Variant>{n, 1});
            Variant texts[] = {Variant::From<i32>(kCalls), Variant::From<String>(String(kSixteen))};
            measure(u8"texts", "string-argument", Span<Variant>{texts, 2});
        }

        // Waiting coroutines: what a frame's scheduler pays for them.
        {
            constexpr i32 kCoroutines = 100;
            constexpr u32 kFrames = 1000;
            Variant n[] = {Variant::From<i32>(kCoroutines)};
            REQUIRE(bench->Invoke(u8"spawn", Span<Variant>{n, 1}).HasValue());
            const Stopwatch clock = Stopwatch::StartNew();
            for (u32 i = 0; i < kFrames; ++i)
            {
                manager->AdvanceCoroutines(0.016);
            }
            detail::Report(language, "coroutines-100-frame", clock.Elapsed().AsSeconds() * 1e6 / kFrames, "us/frame");
        }

        // The collector: a frame's collection with nothing to free, then with garbage.
        {
            constexpr u32 kFrames = 100;
            const Stopwatch idle = Stopwatch::StartNew();
            for (u32 i = 0; i < kFrames; ++i)
            {
                manager->CollectGarbage();
            }
            detail::Report(language, "collect-idle", idle.Elapsed().AsSeconds() * 1e6 / kFrames, "us/frame");
            const Stopwatch step = Stopwatch::StartNew();
            for (u32 i = 0; i < kFrames; ++i)
            {
                manager->StepGarbage(manager->Budget().collectSeconds);
            }
            detail::Report(language, "step-idle", step.Elapsed().AsSeconds() * 1e6 / kFrames, "us/frame");

            Variant n[] = {Variant::From<i32>(1000)};
            REQUIRE(bench->Invoke(u8"churn", Span<Variant>{n, 1}).HasValue());
            const Stopwatch garbage = Stopwatch::StartNew();
            manager->CollectGarbage();
            detail::Report(language, "collect-1000-cycles", garbage.Elapsed().AsSeconds() * 1e6, "us");
            // The same garbage freed by frame steps: the longest frame is what a player feels.
            REQUIRE(bench->Invoke(u8"churn", Span<Variant>{n, 1}).HasValue());
            f64 longest = 0.0;
            for (u32 i = 0; i < kFrames; ++i)
            {
                const Stopwatch frame = Stopwatch::StartNew();
                manager->StepGarbage(manager->Budget().collectSeconds);
                const f64 took = frame.Elapsed().AsSeconds() * 1e6;
                longest = took > longest ? took : longest;
            }
            detail::Report(language, "step-1000-cycles-longest-frame", longest, "us");
        }
    }
}
