// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Navigation.Pipeline's entry, the part the editor and the command-line tools load. Load registers
// the navigation zone's asset type and, where the host offers its pipeline registries, adds its
// builder and File > New's creator, each recorded and taken out again when the plugin unloads.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import pipeline.core;
import pipeline.importer; // PipelinePluginServices
import navigation.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        navigation::RegisterNavigationZoneAsset();
        pipeline::PipelinePluginServices* services = context.Find<pipeline::PipelinePluginServices>();
        if (services == nullptr)
        {
            return;
        }
        (void)services->AddBuilder(foundation::core::MakeUnique<navigation::NavigationZoneAssetBuilder>(context.Allocator()));
        navigation::AddNavigationCreators(*services);
    }
}

PLUGIN_PART(&Load, nullptr)
