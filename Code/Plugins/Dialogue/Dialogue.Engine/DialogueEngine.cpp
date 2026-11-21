// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Engine's entry. Load registers the plugin's types, reads the project's settings for the
// plugin (the host holds them while the plugin is loaded; Project Settings edits that object) and,
// in a host that offers the engine's service, adds the dialogue's domain (the factories that load
// a cooked dialogue file and a cooked cast) and a runner to every run. What it registers and adds is recorded and taken out
// again when the plugin unloads.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.resource;
import engine.domain;
import engine.composition; // EnginePluginServices
import dialogue.engine;

namespace
{
    const foundation::resource::ResourceModule* const kResources[] = {&dialogue::kDialogueResourceModule};
    const engine::DomainModule kDialogueDomain{
        .id = u8"dialogue", .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{kResources, 1}};

    void Load(foundation::runtime::PluginContext& context)
    {
        context.Types().Register(dialogue::DialogueSettings::StaticType());
        const dialogue::DialogueSettings* settings = context.Settings<dialogue::DialogueSettings>();
        dialogue::RegisterDialogueTypes();
        if (engine::EnginePluginServices* services = context.Find<engine::EnginePluginServices>())
        {
            (void)services->AddDomain(kDialogueDomain);
            if (settings != nullptr)
            {
                (void)services->AddRunService(dialogue::DialogueRunContribution(*settings));
            }
        }
    }
}

PLUGIN_PART(&Load, nullptr)
