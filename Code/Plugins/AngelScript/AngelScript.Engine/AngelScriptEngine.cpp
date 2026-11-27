// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AngelScript.Engine's entry. Load registers the AngelScript backend, so a run whose scripts are
// AngelScript starts an AngelScript context; Unload takes it back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.script;
import foundation.script.angelscript;

namespace
{
    void Load(foundation::runtime::PluginContext&)
    {
        foundation::script::angelscript::RegisterAngelScriptBackend();
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)foundation::script::ScriptBackendRegistry::Get().Unregister(u8"angelscript");
    }
}

PLUGIN_PART(&Load, &Unload)
