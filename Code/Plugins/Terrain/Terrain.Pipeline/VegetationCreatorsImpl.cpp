// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vegetation.Pipeline - the vegetation domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module terrain.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;
using namespace pipeline; // its bases

namespace terrain
{
    void AddVegetationCreators(PipelinePluginServices& services)
    {
        // An empty 1024 x 1024 single-plane mask the Paint Vegetation brush fills.
        {
            AssetCreator creator;
            creator.label = String(u8"Vegetation Mask");
            creator.category = String(u8"Terrain");
            creator.type = &VegetationMaskAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                VegetationMaskAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"VegetationMask"),
                                             VegetationMaskAsset::StaticType(), asset);
            };
            (void)services.AddCreator(Move(creator));
        }
    }
}
