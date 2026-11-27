// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Luau.Pipeline's entry, the part the editor and the command-line tools load. Load registers the Luau
// cook, so Luau scripts check, cook and start from a template; Unload takes it back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import script.pipeline;
import luau.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext&)
    {
        luau::RegisterLuauScriptCook();
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)pipeline::ScriptLanguageCookRegistry::Get().Unregister(u8"luau");
    }
}

PLUGIN_PART(&Load, &Unload)
