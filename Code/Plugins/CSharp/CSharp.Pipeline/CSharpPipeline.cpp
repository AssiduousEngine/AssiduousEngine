// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CSharp.Pipeline's entry, the part the editor and the command-line tools load. Load registers the C#
// cooks, the class cook and the project build, so C# classes check, build and cook; Unload takes
// them back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import script.pipeline;
import csharp.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        csharp::RegisterDotNetScriptCook(context.Allocator());
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)pipeline::ScriptLanguageCookRegistry::Get().Unregister(u8"csharp");
        (void)pipeline::ScriptAssemblyCookRegistry::Get().Unregister(u8"csharp");
    }
}

PLUGIN_PART(&Load, &Unload)
