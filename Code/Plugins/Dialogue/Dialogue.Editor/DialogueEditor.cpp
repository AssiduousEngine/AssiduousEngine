// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Editor's entry, the part the editor alone load.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        // TODO(dialogue): the dialogue page (the text, the cook's checks, the outline, the preview).
        (void)context;
    }
}

PLUGIN_PART(&Load, nullptr)
