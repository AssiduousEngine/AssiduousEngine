// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Editor's entry, the part the editor alone loads. Load adds the dialogue file's, the
// cast's and the dialogue assets' pages to the editor's page registry, where the editor offers its context and its page
// services; Unload removes them, closing the pages they made, since those run this part's code.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import editor.core;
import editor.app;
import dialogue.engine;
import dialogue.editor;

namespace
{
    foundation::core::Array<foundation::core::u64> g_pages; // the page factories' keys while loaded

    void Load(foundation::runtime::PluginContext& context)
    {
        editor::EditorContext* editor = context.Find<editor::EditorContext>();
        editor::app::AssetPageServicesSource* services = context.Find<editor::app::AssetPageServicesSource>();
        const dialogue::DialogueSettings* settings = context.Settings<dialogue::DialogueSettings>();
        if (editor == nullptr || services == nullptr || settings == nullptr)
        {
            return;
        }
        g_pages = dialogue::RegisterDialoguePages(*editor, *services, *settings);
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        if (editor::EditorContext* editor = context.Find<editor::EditorContext>())
        {
            for (foundation::core::u64 key : g_pages)
            {
                editor->RemovePageFactory(key);
            }
        }
        g_pages.Clear();
    }
}

PLUGIN_PART(&Load, &Unload)
