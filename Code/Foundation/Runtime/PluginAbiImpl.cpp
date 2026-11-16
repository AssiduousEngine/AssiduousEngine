// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The plugin ABI stamp this engine was built with. The build gives it to this file alone (and to
// each part), never to a module interface, so a change to it recompiles these, not every user of
// foundation.runtime.

module;
#include "Core/Prelude.h"

module foundation.runtime;

import foundation.core;

#ifndef PLUGIN_ABI_STAMP
#error "PluginAbiImpl.cpp is compiled with PLUGIN_ABI_STAMP (cmake/Plugins.cmake)"
#endif

namespace foundation::runtime
{
    core::StringView PluginAbiStamp() noexcept
    {
        return core::StringView(PLUGIN_ABI_STAMP);
    }
}
