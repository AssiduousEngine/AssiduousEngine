// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Terrain.Editor's entry, the part the editor alone loads. Load adds the terrain's and the vegetation
// mask's pages and previews, the terrain brushes and vegetation tools with their panels, and the
// splatmap thumbnails, where the editor offers its context and page services; Unload takes every one
// back, since they run this part's code.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import editor.core;
import editor.app;
import editor.terrain;
import editor.vegetation;

namespace
{
    foundation::core::u64 g_terrainPage = 0; // the page factories' keys while loaded
    foundation::core::u64 g_maskPage = 0;
    bool g_loaded = false;

    void Load(foundation::runtime::PluginContext& context)
    {
        editor::EditorContext* editor = context.Find<editor::EditorContext>();
        editor::app::AssetPageServicesSource* services = context.Find<editor::app::AssetPageServicesSource>();
        if (editor == nullptr || services == nullptr)
        {
            return;
        }
        editor::RegisterTerrainSceneTools(*editor); // sculpt, paint layers, cut holes; their panels; splatmap thumbnails
        editor::RegisterVegetationViewportTools();  // paint vegetation, paint props
        editor::RegisterVegetationToolPanels();
        g_terrainPage = editor::RegisterTerrainEditor(*editor, *services);
        g_maskPage = editor::RegisterVegetationMaskEditor(*editor, *services);
        g_loaded = true;
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        editor::EditorContext* editor = context.Find<editor::EditorContext>();
        if (editor == nullptr || !g_loaded)
        {
            return;
        }
        editor::UnregisterVegetationMaskEditor(*editor, g_maskPage);
        editor::UnregisterTerrainEditor(*editor, g_terrainPage);
        editor::UnregisterVegetationToolPanels();
        editor::UnregisterVegetationViewportTools();
        editor::UnregisterTerrainSceneTools(*editor);
        g_loaded = false;
    }
}

PLUGIN_PART(&Load, &Unload)
