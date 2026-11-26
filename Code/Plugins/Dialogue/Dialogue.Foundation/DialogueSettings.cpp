// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DialogueSettings' reflection, which Project Settings shows, and its serialization, which the
// project's entry for the plugin holds.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.foundation;

import foundation.core;
import foundation.settings; // the settings attributes (label, assetType, ...)

using namespace foundation::core;

namespace dialogue
{
    REFLECT_MEMBERS(DialogueSettings, "rtti::dialogue")
    {
        builder.DataVersion(1);
        RegisterArrayType<String>(); // the two lists
        builder.Property<&DialogueSettings::cast>("cast")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Default cast"))
            .PropAttribute(foundation::settings::kSettingAssetTypeAttribute, String(u8"DialogueCastAsset"))
            .PropAttribute(foundation::settings::kSettingEmptyTextAttribute, String(u8"(none)"));
        builder.Property<&DialogueSettings::assets>("assets")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Default dialogue assets"))
            .PropAttribute(foundation::settings::kSettingAssetTypeAttribute, String(u8"DialogueAssetListAsset"))
            .PropAttribute(foundation::settings::kSettingEmptyTextAttribute, String(u8"(none)"));
        builder.Property<&DialogueSettings::commands>("commands").PropAttribute(foundation::settings::kSettingLabelAttribute,
                                                                               String(u8"Commands"));
        builder.Property<&DialogueSettings::providedFlags>("providedFlags")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Provided flags"));
        builder.Property<&DialogueSettings::flagPrefix>("flagPrefix").PropAttribute(foundation::settings::kSettingLabelAttribute,
                                                                                   String(u8"Flag prefix"));
        builder.Property<&DialogueSettings::fadeSeconds>("fadeSeconds")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Fade length"))
            .PropAttribute("range", Float4{0.0f, 10.0f, 0.1f, 0.0f});
        builder.Property<&DialogueSettings::fadeColor>("fadeColor")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Fade colour"))
            .PropAttribute("color", true);
        builder.Property<&DialogueSettings::stockBox>("stockBox").PropAttribute(foundation::settings::kSettingLabelAttribute,
                                                                               String(u8"Stock box"));
        builder.Property<&DialogueSettings::stockBoxSheet>("stockBoxSheet")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Stock box sheet"))
            .PropAttribute(foundation::settings::kSettingAssetTypeAttribute, String(u8"GuiStyleSheetAsset"))
            .PropAttribute(foundation::settings::kSettingEmptyTextAttribute, String(u8"(the plugin's own)"));
        builder.Property<&DialogueSettings::typingSpeed>("typingSpeed")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Typing speed"))
            .PropAttribute("range", Float4{0.0f, 200.0f, 1.0f, 0.0f});
        builder.Property<&DialogueSettings::advanceAction>("advanceAction")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Advance action"));
        builder.Property<&DialogueSettings::portraitOnRight>("portraitOnRight")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Portrait on the right"));
    }

    void DialogueSettings::Serialize(ISerializer& ar)
    {
        foundation::core::Serialize(ar, "cast", cast);
        foundation::core::Serialize(ar, "assets", assets);
        foundation::core::Serialize(ar, "commands", commands);
        foundation::core::Serialize(ar, "providedFlags", providedFlags);
        foundation::core::Serialize(ar, "flagPrefix", flagPrefix);
        foundation::core::Serialize(ar, "fadeSeconds", fadeSeconds);
        foundation::core::Serialize(ar, "fadeColor", fadeColor);
        foundation::core::Serialize(ar, "stockBox", stockBox);
        foundation::core::Serialize(ar, "stockBoxSheet", stockBoxSheet);
        foundation::core::Serialize(ar, "typingSpeed", typingSpeed);
        foundation::core::Serialize(ar, "advanceAction", advanceAction);
        foundation::core::Serialize(ar, "portraitOnRight", portraitOnRight);
    }
}
