// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :fixed_stepper partition.
//
// FixedStepper: the fixed-timestep accumulator (pure, no clock: callers feed dt).
// Advance() drains whole steps from the accumulator, clamped to maxSteps per call
// as a spiral-of-death guard: the excess time is dropped, not deferred. Alpha() is the
// leftover fraction of a step, the interpolation weight for consumers blending
// fixed-rate state (physics poses). Each scene owns a stepper and its time scale, and
// the application host uses one for its app-level lane.

module;
#include "Core/Prelude.h"

export module foundation.core:fixed_stepper;

import :base;
import :math; // Floor, IsFinite

export namespace foundation::core
{
    struct FixedStepper
    {
        f32 step = 1.0f / 60.0f;
        u32 maxSteps = 4;
        f32 accumulator = 0.0f;

        [[nodiscard]] u32 Advance(f32 deltaTime)
        {
            if (step <= 0.0f)
            {
                accumulator = 0.0f;
                return 0;
            } // a zero step must not spin
            // A delta that is not a number of seconds (infinite, NaN) is no time at all: added, it
            // would make the accumulator infinite and the drain below never end.
            if (deltaTime > 0.0f && IsFinite(deltaTime))
            {
                accumulator += deltaTime;
            }
            // More whole steps due than one call runs (a long stall, a huge delta): the excess is
            // dropped at once, keeping the fraction, rather than drained a step at a time (which
            // never ends once a step is below the accumulator's precision).
            const f32 due = accumulator / step;
            if (due >= static_cast<f32>(maxSteps) + 1.0f)
            {
                accumulator -= Floor(due) * step;
                if (!(accumulator >= 0.0f && accumulator < step))
                {
                    accumulator = 0.0f; // what is left of a huge value is no fraction worth keeping
                }
                return maxSteps;
            }
            u32 steps = 0;
            while (accumulator >= step) // at most maxSteps + 1 times, from the guard above
            {
                accumulator -= step;
                ++steps;
            }
            if (steps > maxSteps)
            {
                steps = maxSteps;
            } // the excess was already drained: dropped
            return steps;
        }

        [[nodiscard]] f32 Alpha() const noexcept
        {
            return (step > 0.0f) ? (accumulator / step) : 0.0f;
        }
    };

    // The fixed step running on this thread: which of its frame's steps it is (0 for the first) and
    // how many frames have gone by since its stepper last ran one (1 when it stepped the frame
    // before). What happens once a frame (an action pressed) belongs to the first step of a frame
    // only, and what happened in frames that ran no step belongs to the next one that does: so a
    // fixed step sees each such edge once, never twice in a frame of two steps and never not at
    // all in a frame of none.
    struct FixedStepFrame
    {
        u32 index = 0;
        u64 framesSinceLastStep = 1;
    };

    namespace detail
    {
        // Defined in FixedStepperImpl.cpp, so it is one per process: defined in this interface, each
        // shared library would have its own.
        [[nodiscard]] const FixedStepFrame*& CurrentFixedStepSlot() noexcept;
    }

    // Null outside every fixed step.
    [[nodiscard]] inline const FixedStepFrame* CurrentFixedStep() noexcept
    {
        return detail::CurrentFixedStepSlot();
    }

    // Says a fixed step is running for its lifetime; nests (a step that steps another restores it).
    class FixedStepScope
    {
    public:
        explicit FixedStepScope(const FixedStepFrame& frame) noexcept
            : m_previous(detail::CurrentFixedStepSlot())
        {
            detail::CurrentFixedStepSlot() = &frame;
        }
        ~FixedStepScope() { detail::CurrentFixedStepSlot() = m_previous; }
        FixedStepScope(const FixedStepScope&) = delete;
        FixedStepScope& operator=(const FixedStepScope&) = delete;

    private:
        const FixedStepFrame* m_previous;
    };
}
