// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Luau.Pipeline's entry, the part the editor and the command-line tools load. Load registers the Luau
// cook, so Luau scripts check, cook and start from a template, and adds File > New's Luau entries;
// Unload takes them back.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import script.pipeline;
import pipeline.core;     // AssetCreator
import pipeline.importer; // PipelinePluginServices
import luau.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        luau::RegisterLuauScriptCook();
        // File > New's entries for the language, where the host offers its pipeline registries; recorded,
        // so the unload takes them out again.
        if (pipeline::PipelinePluginServices* services = context.Find<pipeline::PipelinePluginServices>())
        {
            for (pipeline::AssetCreator& creator : pipeline::ScriptCreatorsFor(u8"luau"))
            {
                (void)services->AddCreator(foundation::core::Move(creator));
            }
        }
    }

    void Unload(foundation::runtime::PluginContext&)
    {
        (void)pipeline::ScriptLanguageCookRegistry::Get().Unregister(u8"luau");
    }
}

PLUGIN_PART(&Load, &Unload)
