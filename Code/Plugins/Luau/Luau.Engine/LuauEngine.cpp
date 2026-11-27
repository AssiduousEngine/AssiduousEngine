// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Luau.Engine's entry. Load registers the Luau backend, so a run whose scripts are Luau starts a
// Luau context; Unload takes it back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.script;
import luau.foundation;

namespace
{
    void Load(foundation::runtime::PluginContext&)
    {
        luau::RegisterLuauScriptBackend();
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)foundation::script::ScriptBackendRegistry::Get().Unregister(u8"luau");
    }
}

PLUGIN_PART(&Load, &Unload)
