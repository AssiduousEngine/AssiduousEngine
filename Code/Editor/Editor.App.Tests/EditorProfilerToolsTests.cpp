// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: profiler_capture over a profiler and recorder of the test's own. The call is
// re-entered while the capture runs, then answers with the summary read back from the written file
// (frame times, the longest scopes, counter averages). It names its failures: a capture already
// running, or a capture that could not be written.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.profiler;
import foundation.profiler.capture;
import editor.app;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace foundation::profiler;
using namespace editor;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    LineOutcome Pump(McpServer& server, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"profiler_capture\",\"arguments\":{}}}}}",
                                   argumentsJson);
        return server.HandleLine(line.AsView());
    }

    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
    };
    Answer AnswerOf(const LineOutcome& outcome)
    {
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        Answer answer;
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
        {
            answer.payload = json::Parse(text.AsView()).value;
        }
        else
        {
            answer.error = text;
        }
        return answer;
    }

    app::ProfilerToolSeams SeamsFor(Profiler& profiler, CaptureRecorder& recorder, StringView folder)
    {
        app::ProfilerToolSeams seams;
        seams.profiler = &profiler;
        seams.recorder = &recorder;
        seams.folder = [path = String(folder)]() { return path; };
        return seams;
    }

    // One host frame: a scope, a counter, the end of the frame, and the recorder advanced.
    // One frame: a "Work" scope and the counter; with `gpu`, a "shadow.cascade" GPU pass whose
    // pipeline statistics are 1000 triangles, 1500 vertex shader runs and 4096 pixel shader runs.
    void RunFrame(Profiler& profiler, CaptureRecorder& recorder, u32 counter, f64 value, bool gpu = false)
    {
        profiler.BeginFrame();
        const u64 frame = profiler.CurrentFrameNumber();
        if (profiler.BeginScope("Work"))
        {
            profiler.EndScope();
        }
        profiler.SetCounter(counter, value);
        profiler.EndFrame();
        if (gpu)
        {
            GpuPassEvent pass;
            pass.name = profiler.InternName(StringView(u8"shadow.cascade"));
            pass.beginTick = 100;
            pass.endTick = 200;
            pass.hasCounts = true;
            pass.counts.inputPrimitives = 1000;
            pass.counts.vertexInvocations = 1500;
            pass.counts.fragmentInvocations = 4096;
            profiler.SubmitGpuPasses(frame, Span<const GpuPassEvent>(&pass, 1));
        }
        (void)recorder.OnFrameEnd(profiler);
    }
}

TEST_CASE("profiler_capture: registered once")
{
    Profiler profiler(DefaultAllocator());
    CaptureRecorder recorder(DefaultAllocator());
    McpServer server;
    app::RegisterProfilerTools(server, SeamsFor(profiler, recorder, u8"scratch_profiler_tool"));
    CHECK(server.ToolCount() == usize{app::kProfilerToolCount});
}

#if BUILD_PROFILING
TEST_CASE("profiler_capture: waits for the capture, then answers its summary")
{
    Profiler profiler(DefaultAllocator());
    CaptureRecorder recorder(DefaultAllocator());
    recorder.SetApplication(StringView(u8"ToolProbe"));
    const u32 draws = profiler.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count);
    McpServer server;
    app::RegisterProfilerTools(server, SeamsFor(profiler, recorder, u8"scratch_profiler_tool"));

    const StringView args = u8"{\"seconds\":0}";
    CHECK(Pump(server, args).state == LineState::NotFinished);
    CHECK(recorder.Capturing());
    CHECK(profiler.Armed());

    // Re-entered each frame until the recorder has written the file.
    LineOutcome outcome;
    for (u32 frame = 0; frame < 20; ++frame)
    {
        RunFrame(profiler, recorder, draws, 10.0);
        outcome = Pump(server, args);
        if (outcome.state != LineState::NotFinished)
        {
            break;
        }
    }
    const Answer got = AnswerOf(outcome);
    REQUIRE(got.ok);
    CHECK_FALSE(profiler.Armed());
    CHECK(got.payload.Get(u8"path").AsString() == recorder.LastCapture());
    CHECK(got.payload.Get(u8"chromeTrace").AsString().AsView().EndsWith(u8".json"));
    CHECK(got.payload.Get(u8"frames").AsNumber() >= 1.0);
    const JsonValue times = got.payload.Get(u8"frameMs");
    CHECK(times.Get(u8"min").AsNumber(-1.0) >= 0.0);
    CHECK(times.Get(u8"median").AsNumber() <= times.Get(u8"max").AsNumber());
    const JsonValue scopes = got.payload.Get(u8"topScopes");
    REQUIRE(scopes.Count() == 1);
    CHECK(scopes.At(0).Get(u8"name").AsString() == StringView(u8"Work"));
    CHECK(scopes.At(0).Get(u8"count").AsNumber() == doctest::Approx(got.payload.Get(u8"frames").AsNumber()));
    CHECK(got.payload.Get(u8"topGpuPasses").Count() == 0);
    CHECK(got.payload.Get(u8"counterAverages").Get(u8"Draws").AsNumber() == doctest::Approx(10.0));
}

TEST_CASE("profiler_capture: a GPU pass carries the triangles and shader runs it took")
{
    // What tells a pass limited by its triangles from one limited by its pixels: summed over the
    // capture, beside its time.
    Profiler profiler(DefaultAllocator());
    CaptureRecorder recorder(DefaultAllocator());
    recorder.SetApplication(StringView(u8"ToolProbe"));
    const u32 draws = profiler.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count);
    McpServer server;
    app::RegisterProfilerTools(server, SeamsFor(profiler, recorder, u8"scratch_profiler_tool_gpu"));

    const StringView args = u8"{\"seconds\":0}";
    REQUIRE(Pump(server, args).state == LineState::NotFinished);
    LineOutcome outcome;
    for (u32 frame = 0; frame < 20; ++frame)
    {
        RunFrame(profiler, recorder, draws, 10.0, /*gpu*/ true);
        outcome = Pump(server, args);
        if (outcome.state != LineState::NotFinished)
        {
            break;
        }
    }
    const Answer got = AnswerOf(outcome);
    REQUIRE(got.ok);
    const JsonValue passes = got.payload.Get(u8"topGpuPasses");
    REQUIRE(passes.Count() == 1);
    const JsonValue cascade = passes.At(0);
    CHECK(cascade.Get(u8"name").AsString() == StringView(u8"shadow.cascade"));
    const f64 count = cascade.Get(u8"count").AsNumber();
    REQUIRE(count >= 1.0);
    CHECK(cascade.Get(u8"triangles").AsNumber() == doctest::Approx(1000.0 * count));
    CHECK(cascade.Get(u8"vertexShaderRuns").AsNumber() == doctest::Approx(1500.0 * count));
    CHECK(cascade.Get(u8"pixelShaderRuns").AsNumber() == doctest::Approx(4096.0 * count));
}

TEST_CASE("profiler_capture: names a capture already running and one that could not be written")
{
    Profiler profiler(DefaultAllocator());
    CaptureRecorder recorder(DefaultAllocator());
    const u32 draws = profiler.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count);
    McpServer server;
    // A folder under a file cannot be made, so the capture cannot be written.
    {
        FileStream blocker(StringView(u8"scratch_profiler_tool_blocker"), FileMode::Write);
        REQUIRE(blocker.IsValid());
    }
    app::RegisterProfilerTools(server, SeamsFor(profiler, recorder, u8"scratch_profiler_tool_blocker/inside"));

    // A capture already started by the key (or a player's flag).
    REQUIRE(recorder.Start(profiler, 0.0, StringView(u8"scratch_profiler_tool")));
    const Answer busy = AnswerOf(Pump(server, u8"{}"));
    CHECK_FALSE(busy.ok);
    CHECK(busy.error.AsView().ContainsIgnoreCase(u8"already running"));
    for (u32 frame = 0; frame < 20 && recorder.Capturing(); ++frame)
    {
        RunFrame(profiler, recorder, draws, 0.0);
    }
    REQUIRE_FALSE(recorder.Capturing());

    CHECK(Pump(server, u8"{\"seconds\":0}").state == LineState::NotFinished);
    LineOutcome outcome;
    for (u32 frame = 0; frame < 20; ++frame)
    {
        RunFrame(profiler, recorder, draws, 0.0);
        outcome = Pump(server, u8"{\"seconds\":0}");
        if (outcome.state != LineState::NotFinished)
        {
            break;
        }
    }
    const Answer failed = AnswerOf(outcome);
    CHECK_FALSE(failed.ok);
    CHECK(failed.error.AsView().ContainsIgnoreCase(u8"could not be written"));
}
#endif
