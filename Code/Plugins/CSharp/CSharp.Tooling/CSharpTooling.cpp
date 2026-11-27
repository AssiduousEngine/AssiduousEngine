// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CSharp.Tooling's entry, the part the editor, the exporter and the MCP host load. Load adds the
// export's player hook where the host offers the export's services; the service records it, so the
// plugin's unload takes it out again.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import editor.project;
import csharp.tooling;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        editor::ExportPluginServices* services = context.Find<editor::ExportPluginServices>();
        if (services == nullptr)
        {
            return;
        }
        (void)services->AddPlayerHook(u8"csharp", [](const editor::PlayerHookContext& hook)
                                      { return csharp::PlayerNeedFor(hook, csharp::FindWebBuild()); });
    }
}

PLUGIN_PART(&Load, nullptr)
