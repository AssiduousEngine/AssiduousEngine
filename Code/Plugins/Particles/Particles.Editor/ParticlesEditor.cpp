// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles.Editor's entry, the part the editor alone loads. Load adds the particle effect's page
// to the editor's page registry and its thumbnails to the thumbnail service, where the editor offers
// its context and page services; Unload removes both, since they run this part's code.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import editor.core;
import editor.app;
import particles.editor;

namespace
{
    foundation::core::u64 g_page = 0;                                        // the page factory's key while loaded
    const editor::ISceneThumbnailGenerator* g_thumbnails = nullptr; // the generator, while loaded

    void Load(foundation::runtime::PluginContext& context)
    {
        editor::EditorContext* editor = context.Find<editor::EditorContext>();
        editor::app::AssetPageServicesSource* services = context.Find<editor::app::AssetPageServicesSource>();
        if (editor == nullptr || services == nullptr)
        {
            return;
        }
        g_page = particles::RegisterParticleEditor(*editor, *services);
        if (editor::ThumbnailService* thumbnails = editor->Thumbnails())
        {
            foundation::core::UniquePtr<editor::ISceneThumbnailGenerator> generator = particles::MakeParticleThumbnailGenerator();
            g_thumbnails = generator.Get();
            thumbnails->RegisterSceneGenerator(foundation::core::Move(generator));
        }
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        if (editor::EditorContext* editor = context.Find<editor::EditorContext>())
        {
            if (g_page != 0)
            {
                editor->RemovePageFactory(g_page);
            }
            if (g_thumbnails != nullptr && editor->Thumbnails() != nullptr)
            {
                editor->Thumbnails()->RemoveSceneGenerator(g_thumbnails);
            }
        }
        g_page = 0;
        g_thumbnails = nullptr;
    }
}

PLUGIN_PART(&Load, &Unload)
