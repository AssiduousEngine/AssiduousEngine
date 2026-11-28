// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// What a component lookup by entity costs: ComponentManager<T>::Get over 100,000 entities, with
// 10%, 50% and every one of them holding the component, asked for every entity (holders and the
// rest) in index order and in a shuffled order. Prints `[bench] scene get <percent>% <order> <ns>
// ns` lines, the median of the timed passes per Get. Skipped in a normal run; run it with
// --no-skip -tc="bench*" from an optimised build (build/clang-reldbg).

#include <doctest/doctest.h>
#include <cstdio>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

namespace
{
    struct Payload
    {
        f32 value = 1.0f;
        u32 pad[3] = {};
    };

    constexpr u32 kEntities = 100000;
    constexpr u32 kPasses = 21;

    // The median nanoseconds per Get of `kPasses` passes asking `manager` for each of `order`.
    f64 TimeGets(const ComponentManager<Payload>& manager, const Array<EntityHandle>& order, f32& sink)
    {
        Array<f64> perGet;
        for (u32 pass = 0; pass < kPasses; ++pass)
        {
            const Stopwatch watch = Stopwatch::StartNew();
            f32 sum = 0.0f;
            for (const EntityHandle entity : order)
            {
                if (const Payload* payload = manager.Get(entity))
                    sum += payload->value;
            }
            const f64 nanos = static_cast<f64>(watch.Elapsed().AsNanoseconds());
            sink += sum;
            perGet.PushBack(nanos / static_cast<f64>(order.Size()));
        }
        for (usize i = 1; i < perGet.Size(); ++i) // insertion sort: a handful of passes
        {
            for (usize j = i; j > 0 && perGet[j - 1] > perGet[j]; --j)
            {
                const f64 swapped = perGet[j];
                perGet[j] = perGet[j - 1];
                perGet[j - 1] = swapped;
            }
        }
        return perGet[perGet.Size() / 2];
    }
}

TEST_CASE("bench: a component lookup by entity, over 100,000 entities" * doctest::skip())
{
    f32 sink = 0.0f;
    constexpr u32 kPercents[] = {10u, 50u, 100u};
    for (const u32 percent : kPercents)
    {
        ComponentManager<Payload> manager;
        Random random;
        for (u32 index = 0; index < kEntities; ++index)
        {
            if (random.NextU32() % 100u < percent)
                (void)manager.Add(EntityHandle{index, 1});
        }

        Array<EntityHandle> inOrder;
        for (u32 index = 0; index < kEntities; ++index)
            inOrder.PushBack(EntityHandle{index, 1});
        Array<EntityHandle> shuffled = inOrder;
        for (usize i = shuffled.Size() - 1; i > 0; --i)
        {
            const usize j = static_cast<usize>(random.NextU32()) % (i + 1);
            const EntityHandle swapped = shuffled[i];
            shuffled[i] = shuffled[j];
            shuffled[j] = swapped;
        }

        std::printf("[bench] scene get %u%% index-order %.2f ns\n", percent, TimeGets(manager, inOrder, sink));
        std::printf("[bench] scene get %u%% shuffled %.2f ns\n", percent, TimeGets(manager, shuffled, sink));
    }
    CHECK(sink >= 0.0f); // keeps the sums alive
}
