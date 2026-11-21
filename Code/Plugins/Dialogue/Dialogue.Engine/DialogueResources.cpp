// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue plugin's types, registered each time the Engine part adds its domain.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.engine;

import foundation.core;

using namespace foundation::core;

namespace dialogue
{
    void RegisterCastValues(); // DialogueCast.cpp

    void RegisterDialogueTypes()
    {
        RegisterCastValues();
        GlobalTypeRegistry().Register(TypeOf<DialogueMood>());
        GlobalTypeRegistry().Register(TypeOf<DialogueSpeaker>());
        GlobalTypeRegistry().Register(DialogueResource::StaticType());
        GlobalTypeRegistry().Register(DialogueSource::StaticType());
        GlobalTypeRegistry().Register(DialogueCastResource::StaticType());
        GlobalTypeRegistry().Register(DialogueCastSource::StaticType());
        RegisterSerializable<DialogueSource>();
        RegisterSerializable<DialogueCastSource>();
    }
}
