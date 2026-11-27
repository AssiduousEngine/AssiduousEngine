// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Terrain.Pipeline's entry, the part the editor and the command-line tools load. Load registers the
// terrain, splatmap and vegetation mask asset types and, where the host offers its pipeline
// registries, adds their builders, the splatmap and mask importers and File > New's creators, each
// recorded and taken out again when the plugin unloads.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.terrain.resource;
import foundation.vegetation.resource;
import pipeline.core;
import pipeline.importer; // PipelinePluginServices
import terrain.pipeline;
import vegetation.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        pipeline::RegisterTerrainAsset();
        foundation::terrain::RegisterTerrainResourceTypes();
        pipeline::RegisterSplatmapAsset();
        foundation::terrain::RegisterSplatmapResourceTypes();
        pipeline::RegisterVegetationMaskAsset();
        foundation::vegetation::RegisterVegetationMaskResourceTypes();
        pipeline::PipelinePluginServices* services = context.Find<pipeline::PipelinePluginServices>();
        if (services == nullptr)
        {
            return;
        }
        foundation::core::IAllocator& allocator = context.Allocator();
        (void)services->AddBuilder(foundation::core::MakeUnique<pipeline::TerrainAssetBuilder>(allocator));
        (void)services->AddBuilder(foundation::core::MakeUnique<pipeline::SplatmapAssetBuilder>(allocator));
        (void)services->AddBuilder(foundation::core::MakeUnique<pipeline::VegetationMaskAssetBuilder>(allocator));
        (void)services->AddImporter(foundation::core::MakeUnique<pipeline::SplatmapFileImporter>(allocator));
        (void)services->AddImporter(foundation::core::MakeUnique<pipeline::VegetationMaskFileImporter>(allocator));
        pipeline::AddTerrainCreators(*services);
        pipeline::AddVegetationCreators(*services);
    }
}

PLUGIN_PART(&Load, nullptr)
