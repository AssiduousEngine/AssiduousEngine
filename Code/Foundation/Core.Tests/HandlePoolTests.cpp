// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core.Tests: Handle and HandlePool. A handle names one value for good: freed, every copy is
// stale; from another pool of its type, it fails; a slot's count that would wrap retires the slot.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;

using namespace foundation::core;

namespace
{
    struct ThingTag;
    struct OtherTag;
    using ThingHandle = Handle<ThingTag>;
    using ThingPool = HandlePool<ThingTag, i32>;

    // Counts its live instances, to see the pool construct and destroy what it holds.
    struct Counted
    {
        static inline i32 live = 0;
        i32 value = 0;
        explicit Counted(i32 v) : value(v) { ++live; }
        Counted(Counted&& other) noexcept : value(other.value) { ++live; }
        Counted(const Counted&) = delete;
        Counted& operator=(const Counted&) = delete;
        Counted& operator=(Counted&&) = delete;
        ~Counted() { --live; }
    };
}

TEST_CASE("handles: a handle names its value until it is freed, and every copy goes stale with it")
{
    ThingPool pool(DefaultAllocator());
    const ThingHandle a = pool.Allocate(7);
    const ThingHandle b = pool.Allocate(9);
    REQUIRE(a.IsValid());
    REQUIRE(b.IsValid());
    CHECK(a != b);
    REQUIRE(pool.Get(a) != nullptr);
    CHECK(*pool.Get(a) == 7);
    CHECK(*pool.Get(b) == 9);
    CHECK(pool.Size() == 2u);

    const ThingHandle copy = a;
    CHECK(pool.Free(a));
    CHECK(pool.Get(a) == nullptr);
    CHECK(pool.Get(copy) == nullptr);
    CHECK_FALSE(pool.IsLive(copy));
    CHECK_FALSE(pool.Free(copy)); // freeing twice does nothing
    CHECK(pool.Size() == 1u);
    CHECK(*pool.Get(b) == 9); // the other untouched

    // The slot is reused, under a new count: the old handle still names nothing.
    const ThingHandle c = pool.Allocate(11);
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(pool.Get(a) == nullptr);
    CHECK(*pool.Get(c) == 11);

    CHECK(pool.Get(ThingHandle::Invalid()) == nullptr);
    CHECK(pool.Get(ThingHandle{}) == nullptr);
    CHECK(pool.Get(ThingHandle{c.index, 0}) == nullptr); // count 0 is never a live one
}

TEST_CASE("handles: a handle from another pool of its type fails there, though its slot and count match")
{
    ThingPool first(DefaultAllocator());
    ThingPool second(DefaultAllocator());
    CHECK(first.PoolId() != 0u);
    CHECK(second.PoolId() != 0u);
    CHECK(first.PoolId() != second.PoolId());

    const ThingHandle mine = first.Allocate(1);
    const ThingHandle theirs = second.Allocate(2);
    CHECK(mine.index == theirs.index);
    CHECK(mine.Count() == theirs.Count());
    CHECK(mine.PoolId() == first.PoolId());
    CHECK(second.Get(mine) == nullptr);
    CHECK(first.Get(theirs) == nullptr);
    CHECK_FALSE(second.Free(mine));
    CHECK(*second.Get(theirs) == 2);
}

TEST_CASE("handles: a pool's id is free again once it is gone, and each handle type has its own ids")
{
    u32 gone = 0;
    {
        ThingPool pool(DefaultAllocator());
        gone = pool.PoolId();
    }
    ThingPool again(DefaultAllocator());
    CHECK(again.PoolId() == gone); // the lowest free id

    HandlePool<OtherTag, i32> other(DefaultAllocator());
    CHECK(other.PoolId() == 1u); // another type's ids are its own
}

TEST_CASE("handles: a slot whose count would wrap is retired, never reused")
{
    ThingPool pool(DefaultAllocator());
    ThingHandle handle = pool.Allocate(0);
    const u32 index = handle.index;
    // Free and take the same slot until its count reaches the last one (about 16.7 million times;
    // one assertion at the end, not one per turn).
    bool sameSlot = true;
    while (handle.Count() != kHandleCountMask && sameSlot)
    {
        sameSlot = pool.Free(handle);
        handle = pool.Allocate(0);
        sameSlot = sameSlot && handle.index == index;
    }
    REQUIRE(sameSlot);
    CHECK(pool.Free(handle));
    CHECK(pool.Get(handle) == nullptr);
    const ThingHandle next = pool.Allocate(1);
    CHECK(next.index != index); // the worn slot is not handed out again
    CHECK(*pool.Get(next) == 1);
}

TEST_CASE("handles: values never move as the pool grows, and a full pool says so")
{
    ThingPool pool(DefaultAllocator(), 600); // three pages of 256
    const ThingHandle first = pool.Allocate(42);
    const i32* where = pool.Get(first);
    ThingHandle last{};
    for (i32 i = 0; i < 599; ++i)
    {
        last = pool.Allocate(i);
        REQUIRE(last.IsValid());
    }
    CHECK(pool.Get(first) == where);
    CHECK(*pool.Get(first) == 42);
    CHECK(pool.Size() == 600u);
    // 600 asked for, rounded up to whole pages: 768 in all.
    for (u32 i = 600; i < 768; ++i)
    {
        REQUIRE(pool.Allocate(0).IsValid());
    }
    CHECK_FALSE(pool.Allocate(0).IsValid());
    CHECK(pool.Free(last));
    CHECK(pool.Allocate(0).IsValid()); // a freed slot is room again
}

TEST_CASE("handles: ForEach visits each stored value with its handle, and the pool destroys what it holds")
{
    {
        HandlePool<ThingTag, Counted> pool(DefaultAllocator());
        const Handle<ThingTag> a = pool.Allocate(Counted(1));
        const Handle<ThingTag> b = pool.Allocate(Counted(2));
        const Handle<ThingTag> c = pool.Allocate(Counted(3));
        CHECK(Counted::live == 3);
        CHECK(pool.Free(b));
        CHECK(Counted::live == 2);

        i32 sum = 0;
        u32 visits = 0;
        pool.ForEach(
            [&](Handle<ThingTag> handle, Counted& value)
            {
                CHECK(pool.Get(handle) == &value);
                sum += value.value;
                ++visits;
            });
        CHECK(visits == 2u);
        CHECK(sum == 4);
        CHECK(pool.IsLive(a));
        CHECK(pool.IsLive(c));
    }
    CHECK(Counted::live == 0);
}

TEST_CASE("handles: the pool allocates from its owner's allocator and gives it all back")
{
    TrackingAllocator tracking(DefaultAllocator());
    {
        ThingPool pool(tracking);
        for (i32 i = 0; i < 300; ++i) // into a second page
        {
            (void)pool.Allocate(i);
        }
        CHECK(tracking.LiveBytes() > 0u);
    }
    CHECK(tracking.LiveBytes() == 0u);
}

TEST_CASE("handles: lookups on another thread see a stored value while the owner keeps allocating")
{
    ThingPool pool(DefaultAllocator(), 1u << 14);
    const ThingHandle first = pool.Allocate(5);
    Atomic<bool> done{false};
    Atomic<i32> misses{0};
    Thread reader(
        [&]()
        {
            while (!done.load(MemoryOrder::Acquire))
            {
                const i32* value = pool.Get(first);
                if (value == nullptr || *value != 5)
                {
                    misses.fetch_add(1, MemoryOrder::Relaxed);
                }
            }
        });
    for (i32 i = 0; i < (1 << 14) - 1; ++i) // every page added while the reader reads
    {
        REQUIRE(pool.Allocate(i).IsValid());
    }
    done.store(true, MemoryOrder::Release);
    reader.Join();
    CHECK(misses.load() == 0);
}
