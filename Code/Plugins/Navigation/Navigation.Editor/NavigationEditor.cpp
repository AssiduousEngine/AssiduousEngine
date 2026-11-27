// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Navigation.Editor's entry, the part the editor alone loads. Load adds the Navigation preferences,
// the zone's gizmo and its inspector's Bake Navigation action and, where the editor serves MCP, the
// navigation_bake tool on a scene page; Unload takes each back, since they run this part's code (the
// tool goes with the plugin's recorded MCP contribution).

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import editor.core;
import editor.scene;
import editor.mcp; // McpPluginServices
import navigation.editor;

namespace
{
    bool g_loaded = false;

    void Load(foundation::runtime::PluginContext& context)
    {
        editor::EditorContext* editor = context.Find<editor::EditorContext>();
        if (editor == nullptr)
        {
            return;
        }
        navigation::RegisterNavigationEditorSettings(*editor);
        editor::GizmoRendererProviders::Get().Register(&navigation::AddNavigationGizmos);
        navigation::RegisterNavigationInspectorExtras();
        if (editor::mcp::McpPluginServices* mcp = context.Find<editor::mcp::McpPluginServices>())
        {
            (void)mcp->AddTools([editor](editor::mcp::PluginTools& tools)
                                { navigation::RegisterNavigationBakeTool(tools, *editor); });
        }
        g_loaded = true;
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        if (!g_loaded)
        {
            return;
        }
        navigation::RemoveNavigationInspectorExtras();
        editor::GizmoRendererProviders::Get().Unregister(&navigation::AddNavigationGizmos);
        if (editor::EditorContext* editor = context.Find<editor::EditorContext>())
        {
            navigation::RemoveNavigationEditorSettings(*editor);
        }
        g_loaded = false;
    }
}

PLUGIN_PART(&Load, &Unload)
