// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Project - ProjectSettings' reflection: the settings the Project Settings dialog edits,
// described once so the dialog and the MCP tools read the same list from the type. Kept out of
// the interface (a REFLECT_MEMBERS body there makes GCC emit a gcm cluster).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.project;

import foundation.core;
import foundation.settings; // the settings attributes (label, assetType, ...)

using namespace foundation::core;

namespace engine::project
{
    REFLECT_ENUM(WindowMode, "rtti::engine::project")
    {
        builder.Value("Windowed", WindowMode::Windowed)
            .Value("Fullscreen", WindowMode::Fullscreen)
            .Value("Borderless", WindowMode::Borderless);
    }

    REFLECT_ENUM(PresentMode, "rtti::engine::project")
    {
        builder.Value("Vsync", PresentMode::Vsync)
            .Value("Mailbox", PresentMode::Mailbox)
            .Value("Immediate", PresentMode::Immediate);
    }

    REFLECT_MEMBERS(ProjectSettings, "rtti::engine::project")
    {
        // 9 carried two settings of a removed UI library; 9 and 10 named the native game module by
        // its library path, which reads as the entry of the game's package; 9 to 11 carried the
        // scene-pass MSAA count, which is a quality level's now; 9 to 12 named the run behavior
        // startupScript and startupScriptId; 9 to 13 had particles in the engine and 9 to 14 terrain,
        // which a project read from them enables as plugins.
        builder.DataVersion(16).ReadsDataVersionsFrom(9);
        // The enums the display settings take (their enumerators name the dialog's and the MCP
        // tools' choices): FitMode is the core's.
        RegisterCoreTypes();
        RttiRegisterEnum_WindowMode();
        RttiRegisterEnum_PresentMode();
        const auto asset = [&builder](StringView label, StringView type, StringView emptyText,
                                      StringView category)
        {
            builder.PropAttribute(foundation::settings::kSettingLabelAttribute, String(label))
                .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(category))
                .PropAttribute(foundation::settings::kSettingAssetTypeAttribute, String(type))
                .PropAttribute(foundation::settings::kSettingEmptyTextAttribute, String(emptyText));
        };
        builder.Property<&ProjectSettings::name>("name")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Name"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"General"));
        // The scene the player and play-in-editor open.
        builder.Property<&ProjectSettings::defaultSceneId>("defaultSceneId");
        asset(u8"Default scene", u8"SceneDocument", u8"(none)", u8"Startup");
        builder.PropAttribute(foundation::settings::kSettingSummaryAttribute, String(u8"Default scene"));
        // The run behavior the player (and the Game tab) starts: a class deriving from RunBehavior,
        // named by what its asset cooks to, so a script's and a native behavior's asset both name one.
        builder.Property<&ProjectSettings::runBehaviorId>("runBehaviorId");
        asset(u8"Run behavior", u8"BehaviorClass", u8"(none)", u8"Startup");
        builder.PropAttribute(foundation::settings::kSettingSummaryAttribute, String(u8"Run behavior"));
        // The cooked map the player (and the Game tab) binds at startup.
        builder.Property<&ProjectSettings::defaultInputMapId>("defaultInputMapId");
        asset(u8"Default input map", u8"InputMapAsset", u8"(none)", u8"Startup");
        builder.PropAttribute(foundation::settings::kSettingSummaryAttribute, String(u8"Input map"));
        // The cooked mixer applied at startup; nil means the built-in neutral four-bus layout.
        builder.Property<&ProjectSettings::defaultBusLayoutId>("defaultBusLayoutId");
        asset(u8"Default bus layout", u8"AudioBusLayoutAsset", u8"(built-in)", u8"Audio");
        // The cooked font the game UI falls back to when a document names none.
        builder.Property<&ProjectSettings::defaultUiFontId>("defaultUiFontId");
        asset(u8"Default UI font", u8"FontAsset", u8"(built-in)", u8"UI");
        builder.PropAttribute(foundation::settings::kSettingSummaryAttribute, String(u8"UI font"));
        // Cooked fonts the game UI loads beside the default one, each a family a label picks.
        RegisterArrayType<Guid>(); // the list container (the dialog's list, the MCP tools)
        builder.Property<&ProjectSettings::uiFontIds>("uiFontIds");
        asset(u8"Other UI fonts", u8"FontAsset", u8"(none)", u8"UI");
        // The engine GUI's sheets, in order over its theme (its fonts are the UI's above).
        builder.Property<&ProjectSettings::guiStyleSheetIds>("guiStyleSheetIds");
        asset(u8"GUI sheets", u8"GuiStyleSheetAsset", u8"(none)", u8"UI");
        // The engine GUI's boot splash (its document binds `bootProgress`); nil means the engine's own.
        builder.Property<&ProjectSettings::guiLoadingDocumentId>("guiLoadingDocumentId");
        asset(u8"Loading screen", u8"GuiDocumentAsset", u8"(built-in)", u8"Startup");
        // Folders outside the project its MCP tools work in, relative to it; each reached only once
        // the user approves it for this project.
        RegisterArrayType<String>(); // the list container (the MCP tools)
        builder.Property<&ProjectSettings::mcpFolders>("mcpFolders")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"MCP folders"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"General"));
        // The display: the render resolution (0 x 0 is the output's size) and its fit, then the
        // player's window. An export preset may override either per platform.
        const Float4 renderSize{0.0f, 16384.0f, 1.0f, 0.0f};
        const Float4 windowSize{1.0f, 16384.0f, 1.0f, 0.0f};
        builder.Property<&ProjectSettings::renderWidth>("renderWidth")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Render width"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", renderSize);
        builder.Property<&ProjectSettings::renderHeight>("renderHeight")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Render height"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", renderSize);
        builder.Property<&ProjectSettings::renderFit>("renderFit")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Render fit"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"));
        builder.Property<&ProjectSettings::windowWidth>("windowWidth")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Window width"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", windowSize);
        builder.Property<&ProjectSettings::windowHeight>("windowHeight")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Window height"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", windowSize);
        builder.Property<&ProjectSettings::windowMode>("windowMode")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Window mode"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"));
        builder.Property<&ProjectSettings::windowResizable>("windowResizable")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Window resizable"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"));
        builder.Property<&ProjectSettings::presentMode>("presentMode")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Present mode"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"));
        builder.Property<&ProjectSettings::orientation>("orientation")
            .PropAttribute(foundation::settings::kSettingLabelAttribute, String(u8"Orientation"))
            .PropAttribute(foundation::settings::kSettingCategoryAttribute, String(u8"Display"));
        // The quality levels in an options menu's order, and the one the game starts at where no
        // export preset picks another.
        builder.Property<&ProjectSettings::qualityLevelIds>("qualityLevelIds");
        asset(u8"Quality levels", u8"RenderQualityAsset", u8"(none)", u8"Quality");
        builder.Property<&ProjectSettings::defaultQualityId>("defaultQualityId");
        asset(u8"Default quality", u8"RenderQualityAsset", u8"(the last level)", u8"Quality");
    }
}
