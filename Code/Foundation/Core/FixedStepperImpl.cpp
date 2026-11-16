// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The calling thread's running fixed step. Defined here, not inline, so one exists per process
// per thread: the scene that steps and the input that reads it live in different libraries.

module;
#include "Core/Prelude.h"

module foundation.core;

namespace foundation::core::detail
{
    const FixedStepFrame*& CurrentFixedStepSlot() noexcept
    {
        static thread_local const FixedStepFrame* slot = nullptr;
        return slot;
    }
}
