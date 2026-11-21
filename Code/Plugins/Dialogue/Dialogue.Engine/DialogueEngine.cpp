// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Engine's entry. Load registers the settings type and reads the project's settings for
// the plugin into it, so the host holds them while the plugin is loaded (Project Settings edits
// that object). What it registers is recorded and taken out again when the plugin unloads.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import dialogue.engine;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        context.Types().Register(dialogue::DialogueSettings::StaticType());
        (void)context.Settings<dialogue::DialogueSettings>();
    }
}

PLUGIN_PART(&Load, nullptr)
