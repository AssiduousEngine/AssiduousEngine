// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles.Pipeline's entry, the part the editor and the command-line tools load. Load registers
// the particle effect's asset type and, where the host offers its pipeline registries, adds the
// effect's builder and File > New's creator, each recorded and taken out again when the plugin
// unloads.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import pipeline.core;
import pipeline.importer; // PipelinePluginServices
import particles.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        particles::RegisterParticleEffectAsset();
        pipeline::PipelinePluginServices* services = context.Find<pipeline::PipelinePluginServices>();
        if (services == nullptr)
        {
            return;
        }
        (void)services->AddBuilder(foundation::core::MakeUnique<particles::ParticleEffectAssetBuilder>(context.Allocator()));
        particles::AddParticleCreators(*services);
    }
}

PLUGIN_PART(&Load, nullptr)
