// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Tooling's entry, the part the editor, the exporter and the MCP host load.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        // TODO(dialogue): dialogue_validate and dialogue_read, through McpPluginServices.
        (void)context;
    }
}

PLUGIN_PART(&Load, nullptr)
