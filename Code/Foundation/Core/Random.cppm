// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include "Core/System/SystemBackend.h" // sys::FillEntropy, sys::GetTicks (FromSystemEntropy)

export module foundation.core:random;

import :base;

export namespace foundation::core
{
    // =======================================================================
    // Random - PCG32. Deterministic and seedable (good for replays/tests).
    // =======================================================================
    class Random
    {
    public:
        /// A generator no other process or run shares: seeded from the OS's entropy, so two
        /// that mint ids (two branches adding to one scene, two editors making assets) do not
        /// mint the same ones. Construct with a seed instead where a sequence must repeat (a
        /// test pinning ids, a replay). Should the OS refuse entropy, the clock and a
        /// process-wide count seed it, which still differ from run to run and generator to
        /// generator.
        [[nodiscard]] static Random FromSystemEntropy() noexcept
        {
            u64 words[2];
            if (!sys::FillEntropy(words, sizeof(words)))
            {
                static u64 s_fallbacks = 0;
                words[0] = sys::GetTicks() ^ 0x9e3779b97f4a7c15ull;
                words[1] = static_cast<u64>(sys::GetUnixTime()) + (++s_fallbacks) * 0xbf58476d1ce4e5b9ull;
            }
            return Random(words[0], words[1]);
        }

        explicit Random(u64 seed = 0x853c49e6748fea9bull,
                        u64 sequence = 0xda3e39cb94b95bdbull) noexcept
        {
            m_state = 0;
            m_inc = (sequence << 1u) | 1u;
            NextU32();
            m_state += seed;
            NextU32();
        }

        u32 NextU32() noexcept
        {
            const u64 old = m_state;
            m_state = old * 6364136223846793005ull + m_inc;
            const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
            const u32 rot = static_cast<u32>(old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        }

        [[nodiscard]] u64 NextU64() noexcept
        {
            const u64 hi = NextU32();
            const u64 lo = NextU32();
            return (hi << 32u) | lo;
        }

        // Uniform float in [0, 1).
        [[nodiscard]] f32 NextFloat() noexcept
        {
            return static_cast<f32>(NextU32() >> 8u) * (1.0f / 16777216.0f);
        }

        [[nodiscard]] f32 NextFloat(f32 min, f32 max) noexcept
        {
            return min + NextFloat() * (max - min);
        }

        // Uniform integer in [min, max] inclusive.
        [[nodiscard]] i32 NextInt(i32 min, i32 max) noexcept
        {
            const u32 range = static_cast<u32>(max - min) + 1u;
            return min + static_cast<i32>(NextU32() % range);
        }

        [[nodiscard]] bool NextBool() noexcept { return (NextU32() & 1u) != 0u; }

    private:
        u64 m_state;
        u64 m_inc;
    };
}
