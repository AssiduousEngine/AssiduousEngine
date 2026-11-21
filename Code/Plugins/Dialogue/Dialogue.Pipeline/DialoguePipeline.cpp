// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Pipeline's entry, the part the editor and the command-line tools load. Load registers the
// dialogue's asset types and, where the host offers its pipeline registries, adds the dialogue file's
// and the cast's builders, the `.dialogue` importer and File > New's creators, each recorded and taken
// out again when the plugin unloads. The builders read the plugin's settings, which the host holds
// while the plugin is loaded.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import pipeline.core;
import pipeline.importer; // PipelinePluginServices
import dialogue.engine;
import dialogue.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        dialogue::RegisterDialoguePipelineTypes(context.Types(), context.Serializables());
        const dialogue::DialogueSettings* settings = context.Settings<dialogue::DialogueSettings>();
        pipeline::PipelinePluginServices* pipeline = context.Find<pipeline::PipelinePluginServices>();
        if (pipeline == nullptr || settings == nullptr)
        {
            return;
        }
        (void)pipeline->AddBuilder(foundation::core::MakeUnique<dialogue::DialogueBuilder>(context.Allocator(), *settings));
        (void)pipeline->AddBuilder(foundation::core::MakeUnique<dialogue::DialogueCastBuilder>(context.Allocator()));
        (void)pipeline->AddImporter(foundation::core::MakeUnique<dialogue::DialogueFileImporter>(context.Allocator()));
        dialogue::AddDialogueCreators(*pipeline);
    }
}

PLUGIN_PART(&Load, nullptr)
