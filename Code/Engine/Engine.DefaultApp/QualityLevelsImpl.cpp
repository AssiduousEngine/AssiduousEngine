// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - the quality levels a run offers and the player's saved choice (see
// QualityLevels.cppm).
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // RTTI_DEFINE_OBJECT

module engine.defaultapp;

import foundation.core;
import foundation.resource;
import foundation.settings;
import engine.project;
import engine.render;

using namespace foundation::core;

namespace engine::runtime
{
    RTTI_DEFINE_OBJECT(RenderQualityUserSettings, "rtti::engine::runtime")

    void RegisterQualityUserSettingsType()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(RenderQualityUserSettings::StaticType());
            RegisterSerializable<RenderQualityUserSettings>();
            return true;
        }();
        (void)once;
    }

    Guid OfferQualityLevels(engine::render::RenderQualityChoice& choice, const engine::project::ProjectSettings& settings,
                            foundation::resource::ResourceManager& resources, const Guid& saved)
    {
        choice.levels.Clear();
        for (const Guid& id : settings.qualityLevelIds)
        {
            if (auto level = resources.Bind<engine::render::RenderQualityLevel>(id))
                choice.levels.PushBack(engine::render::OfferedQualityLevel{id, level->values});
            else
                LOG_WARNING(u8"Quality", u8"the quality level {} did not resolve; it is not offered", id);
        }
        const Guid start = settings.StartupQuality(saved);
        choice.current = choice.Find(start) != nullptr ? start : Guid{};
        return choice.current;
    }

    Guid SavedQualityLevel(const foundation::settings::Settings& user)
    {
        const RenderQualityUserSettings* saved = user.Find<RenderQualityUserSettings>();
        return saved != nullptr ? saved->level : Guid{};
    }

    void KeepQualityLevel(foundation::settings::Settings& user, const Guid& level)
    {
        user.Section<RenderQualityUserSettings>().level = level;
    }
}
