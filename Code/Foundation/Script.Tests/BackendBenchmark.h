// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script-backend benchmark: the same work in each language, timed, so a change to the call
// path is measured before and after. Include from a doctest TU and call RunScriptBackendBenchmark
// inside a TEST_CASE marked doctest::skip(); run it with --no-skip from an optimised build
// (build/clang-reldbg), since a Debug build's times say little.
//
// It prints one line per measure, `[bench] <language> <measure> <value> <unit>`, for the weekly.
#pragma once
#include <doctest/doctest.h>
#include <cstdio>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;

namespace foundation::script::benchmark
{
    using namespace foundation::core;

    struct Dialect
    {
        StringView languageId;
        // A class `Bench`, made with no arguments, with these methods:
        //   tick(dt)   adds dt to a field (a handler call with one number);
        //   math(n)    n iterations of Float3 arithmetic and a Color made from the result,
        //              returning a number so the work is not optimised away;
        //   spawn(n)   starts n coroutines that each wait 1000 seconds;
        //   churn(n)   makes n script objects that each refer to themselves and hold a Float3,
        //              then drops them, leaving garbage only the collector frees.
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

    inline void RunScriptBackendBenchmark(const Function<RefPtr<IScriptManager>()>& factory, const Dialect& dialect)
    {
        RegisterCoreTypes();
        RefPtr<IScriptManager> manager = factory();
        REQUIRE(manager.Get() != nullptr);
        RegisterReflectedTypes(*manager);
        RefPtr<IScriptContext> scriptContext = manager->CreateContext();
        REQUIRE(scriptContext.Get() != nullptr);
        REQUIRE(scriptContext->Load(dialect.benchClass, u8"bench").IsOk());
        RefPtr<ScriptObject> bench = scriptContext->CreateInstance(u8"Bench", Span<Variant>{});
        REQUIRE(bench.Get() != nullptr);
        const StringView language = dialect.languageId;

        // A handler call: the cost of crossing into script and back.
        {
            constexpr u32 kCalls = 100000;
            Variant dt[] = {Variant::From<f64>(0.016)};
            const Stopwatch clock = Stopwatch::StartNew();
            for (u32 i = 0; i < kCalls; ++i)
            {
                REQUIRE(bench->Invoke(u8"tick", Span<Variant>{dt, 1}).HasValue());
            }
            detail::Report(language, "handler-call", clock.Elapsed().AsSeconds() * 1e9 / kCalls, "ns/call");
        }

        // Math: Float3 arithmetic and a Color, in a script loop.
        {
            constexpr i32 kIterations = 100000;
            Variant n[] = {Variant::From<i32>(kIterations)};
            const Stopwatch clock = Stopwatch::StartNew();
            REQUIRE(bench->Invoke(u8"math", Span<Variant>{n, 1}).HasValue());
            detail::Report(language, "math-iteration", clock.Elapsed().AsSeconds() * 1e9 / kIterations, "ns/iteration");
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
