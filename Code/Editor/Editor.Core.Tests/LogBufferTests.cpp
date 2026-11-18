// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// EditorLogBuffer tests: sink capture through the global logger, incremental CollectSince
// polling, bounded-ring overflow with drop counting, untruncated messages, cross-thread writes, the
// references a line carries, and the recognizers that find more in its text.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Log/Log.h"

import foundation.core;
import editor.core;

using namespace foundation::core;
using namespace editor;

TEST_CASE("editor-log: captures dispatches and collects incrementally")
{
    EditorLogBuffer buffer(DefaultAllocator(), 16);
    buffer.Write(LogLevel::Info, u8"Cat", u8"first");
    buffer.Write(LogLevel::Warning, u8"Cat", u8"second");
    CHECK(buffer.Count() == 2);

    Array<EditorLogEntry> out;
    u64 seq = buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 2);
    CHECK(out[0].message == u8"first");
    CHECK(out[0].category == u8"Cat");
    CHECK(out[0].level == LogLevel::Info);
    CHECK(out[1].message == u8"second");
    CHECK(out[1].sequence > out[0].sequence);
    // Each stamped with its time since the buffer was made, in order.
    CHECK(out[0].time.AsNanoseconds() >= 0);
    CHECK(out[1].time.AsNanoseconds() >= out[0].time.AsNanoseconds());

    // Incremental: nothing new since the returned high-water mark.
    Array<EditorLogEntry> more;
    const u64 seq2 = buffer.CollectSince(seq, more);
    CHECK(more.Size() == 0);
    CHECK(seq2 == seq);

    // A new entry shows up alone on the next poll.
    buffer.Write(LogLevel::Error, u8"Cat", u8"third");
    const u64 seq3 = buffer.CollectSince(seq2, more);
    REQUIRE(more.Size() == 1);
    CHECK(more[0].message == u8"third");
    CHECK(seq3 > seq2);
}

TEST_CASE("editor-log: bounded ring drops oldest and counts drops")
{
    EditorLogBuffer buffer(DefaultAllocator(), 4);
    for (i32 i = 0; i < 10; ++i)
    {
        String msg(u8"m");
        msg.PushBack(static_cast<utf8char>('0' + i));
        buffer.Write(LogLevel::Info, u8"Cat", msg.AsView());
    }
    CHECK(buffer.Count() == 4);
    CHECK(buffer.DroppedCount() == 6);

    Array<EditorLogEntry> out;
    (void)buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 4);
    CHECK(out[0].message == u8"m6"); // oldest surviving
    CHECK(out[3].message == u8"m9");
    // Sequences reveal the gap (entries 1..6 were dropped before collection).
    CHECK(out[0].sequence == 7);
}

TEST_CASE("editor-log: messages are not truncated")
{
    EditorLogBuffer buffer(DefaultAllocator(), 4);
    String longMessage;
    for (i32 i = 0; i < 100; ++i)
    {
        longMessage += u8"0123456789";
    } // 1000 chars
    buffer.Write(LogLevel::Error, u8"Build", longMessage.AsView());

    Array<EditorLogEntry> out;
    (void)buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 1);
    CHECK(out[0].message.Size() == longMessage.Size()); // core RingLogSink would cap at 192
}

TEST_CASE("editor-log: registered on the global logger it captures LOG output")
{
    EditorLogBuffer buffer(DefaultAllocator(), 16);
    GlobalLogger().AddSink(&buffer);
    LOG_WARNING(u8"EditorTest", u8"hello {}", 42);
    GlobalLogger().RemoveSink(&buffer);

    Array<EditorLogEntry> out;
    (void)buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 1);
    CHECK(out[0].level == LogLevel::Warning);
    CHECK(out[0].category == u8"EditorTest");
    CHECK(out[0].message == u8"hello 42");
}

TEST_CASE("editor-log: concurrent writers do not lose or corrupt entries")
{
    EditorLogBuffer buffer(DefaultAllocator(), 4096);
    constexpr i32 kThreads = 4;
    constexpr i32 kPerThread = 200;

    Thread threads[kThreads];
    for (i32 t = 0; t < kThreads; ++t)
    {
        threads[t] = Thread(
            [&buffer]()
            {
                for (i32 i = 0; i < kPerThread; ++i)
                {
                    buffer.Write(LogLevel::Info, u8"Thread", u8"message");
                }
            });
    }
    for (i32 t = 0; t < kThreads; ++t)
    {
        threads[t].Join();
    }

    CHECK(buffer.Count() == static_cast<usize>(kThreads * kPerThread));
    CHECK(buffer.DroppedCount() == 0);

    // All sequences unique and dense (1..N).
    Array<EditorLogEntry> out;
    const u64 high = buffer.CollectSince(0, out);
    CHECK(out.Size() == static_cast<usize>(kThreads * kPerThread));
    CHECK(high == static_cast<u64>(kThreads * kPerThread));
}

TEST_CASE("editor-log: an entry keeps the references its log call attached")
{
    EditorLogBuffer buffer(DefaultAllocator(), 8);
    Logger& logger = GlobalLogger();
    const LogLevel previous = logger.MinLevel();
    logger.SetMinLevel(LogLevel::Info);
    logger.AddSink(&buffer);
    LOG_WARNING_AT(u8"Script", SourceReference(u8"/p/Code/A.luau", 3), u8"a problem");
    LOG_WARNING(u8"Script", u8"no reference");
    logger.RemoveSink(&buffer);
    logger.SetMinLevel(previous);

    Array<EditorLogEntry> entries;
    (void)buffer.CollectSince(0, entries);
    REQUIRE(entries.Size() == 2u);
    REQUIRE(entries[0].references.Size() == 1u);
    CHECK(entries[0].references[0].Field(0) == StringView(u8"/p/Code/A.luau"));
    CHECK(entries[1].references.IsEmpty());
}

TEST_CASE("editor-log: paths in the project become source references, with their line and column")
{
    LogReferenceRecognizers recognizers;
    (void)recognizers.Add(SourcePathRecognizer(u8"/home/me/Game/"));

    // A compiler's file(line,col), with the part taking in the numbers.
    const StringView build = u8"/home/me/Game/Code/Player.cs(12,5): error CS0103: no 'speed'";
    Array<LogReference> found = recognizers.Recognize(build);
    REQUIRE(found.Size() == 1u);
    CHECK(found[0].kind == kLogSourceReference);
    CHECK(found[0].Field(0) == StringView(u8"/home/me/Game/Code/Player.cs"));
    CHECK(found[0].Field(1) == StringView(u8"12"));
    CHECK(found[0].Field(2) == StringView(u8"5"));
    CHECK(build.SubStr(found[0].start, found[0].length) == StringView(u8"/home/me/Game/Code/Player.cs(12,5)"));

    // file:line:col, a path in the Code folder read under the project, a Windows path, and a
    // path that ends a sentence.
    found = recognizers.Recognize(u8"see Code/Enemies/Boss.luau:7 and \\home\\me\\Game\\Sources\\a.png. Also /home/me/Game/x.as:3:9.");
    REQUIRE(found.Size() == 3u);
    CHECK(found[0].Field(0) == StringView(u8"/home/me/Game/Code/Enemies/Boss.luau"));
    CHECK(found[0].Field(1) == StringView(u8"7"));
    CHECK(found[0].Field(2).IsEmpty());
    CHECK(found[1].Field(0) == StringView(u8"/home/me/Game/Sources/a.png"));
    CHECK(found[1].Field(1).IsEmpty());
    CHECK(found[2].Field(0) == StringView(u8"/home/me/Game/x.as"));
    CHECK(found[2].Field(2) == StringView(u8"9"));

    // A path outside the project, and a word that only contains "Code/", are not.
    CHECK(recognizers.Recognize(u8"/home/other/Game/Code/A.luau:1 and MyCode/x.luau").IsEmpty());
}

TEST_CASE("editor-log: the ids of known assets become asset references; overlaps keep the first")
{
    Guid known;
    REQUIRE(Guid::TryParse(u8"0e9b3a52-1c1f-4c2e-9d3b-5a7f1e2d3c4b", known));
    LogReferenceRecognizers recognizers;
    (void)recognizers.Add(AssetIdRecognizer([known](const Guid& id) { return id == known; }));
    (void)recognizers.Add(SourcePathRecognizer(u8"/p"));

    const StringView message = u8"{0e9b3a52-1c1f-4c2e-9d3b-5a7f1e2d3c4b} reads /p/Code/a.luau; 11111111-2222-3333-4444-555555555555 is unknown";
    const Array<LogReference> found = recognizers.Recognize(message);
    REQUIRE(found.Size() == 2u);
    CHECK(found[0].kind == kLogAssetReference);
    CHECK(found[0].start == 0u);
    CHECK(found[0].length == 38u); // the braces too
    CHECK(found[1].kind == kLogSourceReference);

    // A line's references: the attached first, then the recognized.
    const LogReference attached = SourceReference(u8"/p/Code/b.luau");
    const Array<LogReference> all = recognizers.Of(message, Span<const LogReference>(&attached, 1));
    REQUIRE(all.Size() == 3u);
    CHECK(all[0].Field(0) == StringView(u8"/p/Code/b.luau"));
    CHECK(all[1].kind == kLogAssetReference);
}
