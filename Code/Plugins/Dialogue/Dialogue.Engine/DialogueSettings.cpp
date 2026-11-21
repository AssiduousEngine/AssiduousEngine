// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DialogueSettings' reflection, which Project Settings shows, and its serialization, which the
// project's entry for the plugin holds.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.engine;

import foundation.core;
import engine.project; // the settings' attributes

using namespace foundation::core;

namespace dialogue
{
    REFLECT_MEMBERS(DialogueSettings, "rtti::dialogue")
    {
        using engine::project::kSettingLabelAttribute;
        builder.DataVersion(1);
        RegisterArrayType<String>(); // the two lists
        builder.Property<&DialogueSettings::commands>("commands").PropAttribute(kSettingLabelAttribute,
                                                                               String(u8"Commands"));
        builder.Property<&DialogueSettings::providedFlags>("providedFlags")
            .PropAttribute(kSettingLabelAttribute, String(u8"Provided flags"));
        builder.Property<&DialogueSettings::flagPrefix>("flagPrefix").PropAttribute(kSettingLabelAttribute,
                                                                                   String(u8"Flag prefix"));
        builder.Property<&DialogueSettings::stockBox>("stockBox").PropAttribute(kSettingLabelAttribute,
                                                                               String(u8"Stock box"));
        builder.Property<&DialogueSettings::typingSpeed>("typingSpeed")
            .PropAttribute(kSettingLabelAttribute, String(u8"Typing speed"))
            .PropAttribute("range", Float4{0.0f, 200.0f, 1.0f, 0.0f});
        builder.Property<&DialogueSettings::advanceAction>("advanceAction")
            .PropAttribute(kSettingLabelAttribute, String(u8"Advance action"));
        builder.Property<&DialogueSettings::portraitOnRight>("portraitOnRight")
            .PropAttribute(kSettingLabelAttribute, String(u8"Portrait on the right"));
    }

    void DialogueSettings::Serialize(ISerializer& ar)
    {
        foundation::core::Serialize(ar, "commands", commands);
        foundation::core::Serialize(ar, "providedFlags", providedFlags);
        foundation::core::Serialize(ar, "flagPrefix", flagPrefix);
        foundation::core::Serialize(ar, "stockBox", stockBox);
        foundation::core::Serialize(ar, "typingSpeed", typingSpeed);
        foundation::core::Serialize(ar, "advanceAction", advanceAction);
        foundation::core::Serialize(ar, "portraitOnRight", portraitOnRight);
    }
}
