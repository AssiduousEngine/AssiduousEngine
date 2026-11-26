// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue plugin's resource module: its cooked types and their factories, which the Engine
// part adds to the engine's composition as a domain when it loads.

module;

#include "Core/Prelude.h"

export module dialogue.foundation:resources;

import foundation.core;
import foundation.resource;
import :conversation;
import :cast;

using namespace foundation::core;

export namespace dialogue
{
    /// Registers the dialogue's and the cast's types: their values, sources and resources. Called
    /// on each load, so a plugin loaded again after an unload registers them again.
    void RegisterDialogueTypes();

    inline constexpr foundation::resource::ResourceFactoryDesc kDialogueResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<DialogueResource, DialogueSource, DialogueFactory>(),
        foundation::resource::FactoryWithAllocator<DialogueCastResource, DialogueCastSource, DialogueCastFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kDialogueResourceModule{
        u8"dialogue", &RegisterDialogueTypes, kDialogueResourceFactories,
        sizeof(kDialogueResourceFactories) / sizeof(kDialogueResourceFactories[0])};
}
