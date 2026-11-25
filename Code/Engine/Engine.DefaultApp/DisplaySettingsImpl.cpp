// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - how a run's window presents, and the player's saved choice (see
// DisplaySettings.cppm).
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // RTTI_DEFINE_OBJECT

module engine.defaultapp;

import foundation.core;
import foundation.graphics;
import foundation.rhi;
import foundation.settings;
import engine.project;
import engine.render;

using namespace foundation::core;

namespace engine::runtime
{
    RTTI_DEFINE_OBJECT(DisplayUserSettings, "rtti::engine::runtime")

    void RegisterDisplayUserSettingsType()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(DisplayUserSettings::StaticType());
            RegisterSerializable<DisplayUserSettings>();
            return true;
        }();
        (void)once;
    }

    foundation::rhi::PresentMode PresentModeOf(engine::project::PresentMode mode) noexcept
    {
        switch (mode)
        {
        case engine::project::PresentMode::Mailbox:
            return foundation::rhi::PresentMode::Mailbox;
        case engine::project::PresentMode::Immediate:
            return foundation::rhi::PresentMode::Immediate;
        case engine::project::PresentMode::Vsync:
            break;
        }
        return foundation::rhi::PresentMode::Fifo;
    }

    StringView StartupPresentMode(const engine::project::ProjectSettings& settings, StringView saved, StringView asked) noexcept
    {
        if (const foundation::graphics::NamedPresentMode* named = foundation::graphics::FindPresentMode(asked))
            return named->name;
        if (const foundation::graphics::NamedPresentMode* named = foundation::graphics::FindPresentMode(saved))
            return named->name;
        return foundation::graphics::PresentModeName(PresentModeOf(settings.presentMode));
    }

    void OfferPresentModes(engine::render::DisplayChoice& choice, StringView current)
    {
        choice.presentModes.Clear();
        for (const foundation::graphics::NamedPresentMode& entry : foundation::graphics::kPresentModes)
            choice.presentModes.PushBack(String(entry.name));
        choice.presentMode = String(current);
    }

    String SavedPresentMode(const foundation::settings::Settings& user)
    {
        const DisplayUserSettings* saved = user.Find<DisplayUserSettings>();
        return saved != nullptr ? saved->presentMode : String{};
    }

    void KeepPresentMode(foundation::settings::Settings& user, StringView name)
    {
        user.Section<DisplayUserSettings>().presentMode = String(name);
    }
}
