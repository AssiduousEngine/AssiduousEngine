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
import terrain.foundation;
import pipeline.core;
import pipeline.importer; // PipelinePluginServices
import terrain.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        terrain::RegisterTerrainAsset();
        terrain::RegisterTerrainResourceTypes();
        terrain::RegisterSplatmapAsset();
        terrain::RegisterSplatmapResourceTypes();
        terrain::RegisterVegetationMaskAsset();
        terrain::RegisterVegetationMaskResourceTypes();
        pipeline::PipelinePluginServices* services = context.Find<pipeline::PipelinePluginServices>();
        if (services == nullptr)
        {
            return;
        }
        foundation::core::IAllocator& allocator = context.Allocator();
        (void)services->AddBuilder(foundation::core::MakeUnique<terrain::TerrainAssetBuilder>(allocator));
        (void)services->AddBuilder(foundation::core::MakeUnique<terrain::SplatmapAssetBuilder>(allocator));
        (void)services->AddBuilder(foundation::core::MakeUnique<terrain::VegetationMaskAssetBuilder>(allocator));
        (void)services->AddImporter(foundation::core::MakeUnique<terrain::SplatmapFileImporter>(allocator));
        (void)services->AddImporter(foundation::core::MakeUnique<terrain::VegetationMaskFileImporter>(allocator));
        terrain::AddTerrainCreators(*services);
        terrain::AddVegetationCreators(*services);
    }
}

PLUGIN_PART(&Load, nullptr)
