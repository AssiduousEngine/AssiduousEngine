// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CSharp.Engine's entry. Load registers the C# backend, so a run whose scripts are C# starts a C#
// context; Unload takes it back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.script;
import foundation.script.dotnet;

namespace
{
    void Load(foundation::runtime::PluginContext&)
    {
        foundation::script::dotnet::RegisterDotNetScriptBackend();
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)foundation::script::ScriptBackendRegistry::Get().Unregister(u8"csharp");
    }
}

PLUGIN_PART(&Load, &Unload)
