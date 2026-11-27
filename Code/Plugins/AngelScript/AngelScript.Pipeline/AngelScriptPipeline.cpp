// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AngelScript.Pipeline's entry, the part the editor and the command-line tools load. Load registers
// the AngelScript cook, so AngelScript scripts check, cook and start from a template; Unload takes
// it back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import script.pipeline;
import angelscript.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext&)
    {
        angelscript::RegisterAngelScriptScriptCook();
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)pipeline::ScriptLanguageCookRegistry::Get().Unregister(u8"angelscript");
    }
}

PLUGIN_PART(&Load, &Unload)
