// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Pipeline's entry, the part the editor and the command-line tools load.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        // TODO(dialogue): the .dialogue asset type, its importer, its cook's checks and its creators.
        (void)context;
    }
}

PLUGIN_PART(&Load, nullptr)
