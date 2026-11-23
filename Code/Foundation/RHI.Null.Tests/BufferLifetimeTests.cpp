// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// RHI.Null.Tests: buffers by handle, and when a destroyed one is really freed. A destroyed buffer's
// handle is stale at once; the buffer itself waits until every recording begun before the destroy
// has been submitted or discarded, and until those submissions complete (the tracker's rule, on
// its own and through the Null device, whose submissions complete as they are made).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;

using namespace foundation::core;
using namespace foundation::rhi;

extern "C" const char* DebugHandleLabel(const char* noun, foundation::core::u32 index,
                                            foundation::core::u32 generation);

namespace
{
    // An object the tracker frees: counts its frees.
    struct Freed
    {
        i32 count = 0;
    };
    void CountFree(void* context, void*) { ++static_cast<Freed*>(context)->count; }

    void CollectAt(SubmissionTracker& tracker, u64 completed)
    {
        const u64 values[1] = {completed};
        tracker.Collect(Span<const u64>(values, 1));
    }

    struct NullDeviceFixture
    {
        Backend* backend = nullptr;
        Device* device = nullptr;
        NullDeviceFixture()
        {
            REQUIRE(null::CreateNullBackend(backend).IsOk());
            REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
        }
        ~NullDeviceFixture()
        {
            device->Destroy();
            backend->Destroy();
        }
        [[nodiscard]] BufferHandle Make(u64 size = 256)
        {
            BufferDesc desc{};
            desc.size = size;
            desc.usage = BufferUsage::Uniform;
            BufferHandle buffer{};
            REQUIRE(device->CreateBuffer(desc, buffer).IsOk());
            return buffer;
        }
        [[nodiscard]] u64 BufferBytes() const { return device->Counters().bufferBytes; }
    };
}

TEST_CASE("buffer lifetime: a retired object waits for the submissions made before it, and no others")
{
    SubmissionTracker tracker(DefaultAllocator());
    const u32 queue = tracker.AddQueue();
    Freed freed;
    const u64 first = tracker.NextSubmission(queue);
    tracker.Retire(&freed, &CountFree, &freed);
    CollectAt(tracker, 0);
    CHECK(freed.count == 0); // the submission before it has not completed
    const u64 later = tracker.NextSubmission(queue);
    CollectAt(tracker, first);
    CHECK(freed.count == 1); // a submission after the retire is not waited for
    CHECK(later > first);
    CHECK(tracker.RetiredCount() == 0u);
}

TEST_CASE("buffer lifetime: one recorded before the destroy and submitted after it holds the object to that submission")
{
    SubmissionTracker tracker(DefaultAllocator());
    const u32 queue = tracker.AddQueue();
    Freed freed;
    // A frame's commands are recorded, then an upload destroys a buffer and submits, then the
    // frame's commands are submitted.
    const SubmissionTracker::Recording frame = tracker.Begin();
    tracker.Retire(&freed, &CountFree, &freed);
    const SubmissionTracker::Recording upload = tracker.Begin();
    const u64 uploadValue = tracker.NextSubmission(queue);
    tracker.End(upload, queue, uploadValue);
    CollectAt(tracker, uploadValue);
    CHECK(freed.count == 0); // the frame's commands are still open
    const u64 frameValue = tracker.NextSubmission(queue);
    tracker.End(frame, queue, frameValue);
    CollectAt(tracker, uploadValue);
    CHECK(freed.count == 0); // submitted, not complete
    CollectAt(tracker, frameValue);
    CHECK(freed.count == 1);
}

TEST_CASE("buffer lifetime: a recording discarded unsubmitted holds nothing back, and one begun after the destroy is not waited for")
{
    SubmissionTracker tracker(DefaultAllocator());
    const u32 queue = tracker.AddQueue();
    Freed freed;
    const SubmissionTracker::Recording abandoned = tracker.Begin();
    tracker.Retire(&freed, &CountFree, &freed);
    const SubmissionTracker::Recording after = tracker.Begin(); // cannot name the destroyed object
    CollectAt(tracker, 0);
    CHECK(freed.count == 0);
    tracker.End(abandoned); // discarded: never submitted
    CollectAt(tracker, 0);
    CHECK(freed.count == 1); // the open later recording did not hold it
    tracker.End(after, queue, tracker.NextSubmission(queue));
    tracker.End(after, queue, 99); // a second end does nothing
}

TEST_CASE("buffer lifetime: queues are waited for each on its own")
{
    SubmissionTracker tracker(DefaultAllocator());
    const u32 graphics = tracker.AddQueue();
    const u32 compute = tracker.AddQueue();
    Freed freed;
    const u64 g = tracker.NextSubmission(graphics);
    const u64 c = tracker.NextSubmission(compute);
    tracker.Retire(&freed, &CountFree, &freed);
    u64 completed[2] = {g, 0};
    tracker.Collect(Span<const u64>(completed, 2));
    CHECK(freed.count == 0); // compute's submission still running
    completed[1] = c;
    tracker.Collect(Span<const u64>(completed, 2));
    CHECK(freed.count == 1);
}

TEST_CASE("buffer lifetime: the tracker frees what it still holds when it goes")
{
    Freed freed;
    {
        SubmissionTracker tracker(DefaultAllocator());
        const u32 queue = tracker.AddQueue();
        (void)tracker.NextSubmission(queue);
        tracker.Retire(&freed, &CountFree, &freed);
        tracker.Retire(&freed, &CountFree, &freed);
    }
    CHECK(freed.count == 2);
}

TEST_CASE("buffer lifetime: a destroyed buffer's handle is stale at once, every copy of it, and every call on it does nothing")
{
    NullDeviceFixture f;
    BufferHandle buffer = f.Make();
    const BufferHandle copy = buffer;
    REQUIRE(f.device->Describe(buffer) != nullptr);
    CHECK(f.device->Describe(buffer)->size == 256u);
    REQUIRE(f.device->Map(buffer) != nullptr);
    f.device->Unmap(buffer);

    f.device->DestroyBuffer(buffer);
    CHECK_FALSE(buffer.IsValid()); // the caller's own copy is cleared
    CHECK(f.device->Describe(copy) == nullptr);
    CHECK(f.device->Map(copy) == nullptr);
    f.device->Unmap(copy);              // does nothing
    f.device->FlushRange(copy, 0, 16);  // does nothing
    BufferHandle again = copy;
    f.device->DestroyBuffer(again);     // destroying twice does nothing
    CHECK_FALSE(again.IsValid());

    // A new buffer in the freed slot is a different handle; the old one still names nothing.
    const BufferHandle next = f.Make(64);
    CHECK(next != copy);
    CHECK(f.device->Describe(copy) == nullptr);
    CHECK(f.device->Describe(next)->size == 64u);
}

TEST_CASE("buffer lifetime: on the Null device, a buffer destroyed while a recording made before it is open is freed at that recording's submit")
{
    NullDeviceFixture f;
    const u64 before = f.BufferBytes();
    BufferHandle buffer = f.Make(1024);
    CHECK(f.BufferBytes() == before + 1024u);

    CommandPool* pool = nullptr;
    REQUIRE(f.device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk()); // recording begins
    f.device->DestroyBuffer(buffer);
    CHECK(f.BufferBytes() == before + 1024u); // the open recording may name it
    CommandBuffer* cb = encoder->Finish();
    pool->DestroyEncoder(encoder); // finished: its command buffer still holds the recording
    CHECK(f.BufferBytes() == before + 1024u);
    CommandBuffer* cbs[1] = {cb};
    f.device->GetQueue(QueueType::Graphics)->Submit(Span<CommandBuffer* const>(cbs, 1));
    CHECK(f.BufferBytes() == before); // submitted and (on Null) complete: freed
    f.device->DestroyCommandPool(pool);
}

TEST_CASE("buffer lifetime: on the Null device, a pool's reset discards what it never submitted, and a buffer it held back goes")
{
    NullDeviceFixture f;
    const u64 before = f.BufferBytes();
    BufferHandle buffer = f.Make(512);
    CommandPool* pool = nullptr;
    REQUIRE(f.device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    (void)encoder->Finish(); // never submitted
    f.device->DestroyBuffer(buffer);
    CHECK(f.BufferBytes() == before + 512u);
    pool->Reset();
    f.device->DestroyBuffer(buffer); // nothing: a collect happens on the next destroy or submit
    BufferHandle other = f.Make(16);
    f.device->DestroyBuffer(other); // collects
    CHECK(f.BufferBytes() == before);
    f.device->DestroyCommandPool(pool);
}

TEST_CASE("buffer lifetime: a buffer handle from one device is refused by another")
{
    NullDeviceFixture a;
    NullDeviceFixture b;
    const BufferHandle fromA = a.Make(32);
    const BufferHandle fromB = b.Make(48);
    CHECK(fromA.index == fromB.index); // the same slot in each device's table
    CHECK(b.device->Describe(fromA) == nullptr);
    CHECK(a.device->Describe(fromB) == nullptr);
    CHECK(a.device->Describe(fromA)->size == 32u);
}

namespace
{
    struct StaleLines
    {
        int count = 0;
        const char* needle = "";
        static bool Sink(void* context, bool error, const char* utf8)
        {
            auto* self = static_cast<StaleLines*>(context);
            if (error && std::strstr(utf8, "[rhi] stale") != nullptr && std::strstr(utf8, self->needle) != nullptr)
                ++self->count;
            return true; // swallowed: the test reads them
        }
    };
}

TEST_CASE("buffer lifetime: a resource's label is the device's to tell, and a stale use is reported once, by it")
{
    NullDeviceFixture f;
    BufferDesc desc{};
    desc.size = 64;
    desc.usage = BufferUsage::Uniform;
    desc.label = u8"label.probe.buffer";
    BufferHandle buffer{};
    REQUIRE(f.device->CreateBuffer(desc, buffer).IsOk());
    CHECK(f.device->Label(buffer) == StringView(u8"label.probe.buffer"));
    const BufferHandle kept = buffer;
    f.device->DestroyBuffer(buffer);
    CHECK(f.device->Label(kept) == StringView(u8"label.probe.buffer")); // remembered after the destroy

    StaleLines lines;
    lines.needle = "label.probe.buffer";
    SetLogSink(&StaleLines::Sink, &lines);
    CHECK(f.device->Describe(kept) == nullptr);
    CHECK(f.device->Map(kept) == nullptr);
    BufferHandle again = kept;
    f.device->DestroyBuffer(again); // a second destroy is a stale use too
    SetLogSink(nullptr, nullptr);
    CHECK(lines.count == 1); // once per handle, named

    // A label is copied at creation: the caller's string may go.
    String transient(u8"label.probe.copied");
    desc.label = transient.AsView();
    BufferHandle copied{};
    REQUIRE(f.device->CreateBuffer(desc, copied).IsOk());
    transient = String(u8"something else");
    CHECK(f.device->Label(copied) == StringView(u8"label.probe.copied"));
    f.device->DestroyBuffer(copied);
}

TEST_CASE("buffer lifetime: a debugger reads a handle's label through Core, by its noun and pool")
{
    NullDeviceFixture f;
    BufferDesc desc{};
    desc.size = 64;
    desc.usage = BufferUsage::Uniform;
    desc.label = u8"debugger.probe";
    BufferHandle buffer{};
    REQUIRE(f.device->CreateBuffer(desc, buffer).IsOk());
    CHECK(StringView(reinterpret_cast<const utf8char*>(DebugHandleLabel("Buffer", buffer.index, buffer.generation))) ==
          StringView(u8"'debugger.probe'"));
    const BufferHandle kept = buffer;
    f.device->DestroyBuffer(buffer);
    CHECK(StringView(reinterpret_cast<const utf8char*>(DebugHandleLabel("Buffer", kept.index, kept.generation))) ==
          StringView(u8"stale (was 'debugger.probe')"));
    // Another type's pool of the same id knows nothing of it.
    CHECK(StringView(reinterpret_cast<const utf8char*>(DebugHandleLabel("Texture", kept.index, kept.generation))) ==
          StringView(u8"stale"));
    // A pool nothing registered answers empty.
    CHECK(StringView(reinterpret_cast<const utf8char*>(DebugHandleLabel("NoSuchThing", 0, 1u << 24))).IsEmpty());
}
