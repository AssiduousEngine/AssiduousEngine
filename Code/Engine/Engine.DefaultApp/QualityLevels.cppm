// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - the `engine.defaultapp:quality` partition: the quality levels a game's run
// offers its scripts, read from the project, and the player's saved choice among them. The player
// and the editor's Game tab both fill their run's RenderQualityChoice here; the player also keeps
// a choice in the user's settings, a section of the file beside the game's other per-user
// settings, so the next run starts at it.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the user settings section (RTTI_OBJECT)

export module engine.defaultapp:quality;

import foundation.core;
import foundation.resource;
import foundation.settings;
import engine.project;
import engine.render;

using namespace foundation::core;

export namespace engine::runtime
{
    /// The player's quality choice, a section of the game's user settings: the chosen level's id.
    class RenderQualityUserSettings final : public ISerializable
    {
        RTTI_OBJECT(RenderQualityUserSettings, ISerializable)
    public:
        Guid level;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "level", level); }
    };

    /// Registers the user settings section, which loading the file instantiates by its name.
    /// Idempotent.
    void RegisterQualityUserSettingsType();

    /// Fills `choice` with the project's levels, in its order, each read through `resources`; a
    /// level that does not resolve is left out, with a warning. Returns the level the game starts
    /// at: `saved` (the player's choice) when the project lists it, else the preset's or the
    /// project's (ProjectSettings::StartupQuality); nil when that one was left out or the project
    /// lists none, for the engine's own level.
    [[nodiscard]] Guid OfferQualityLevels(engine::render::RenderQualityChoice& choice,
                                          const engine::project::ProjectSettings& settings,
                                          foundation::resource::ResourceManager& resources, const Guid& saved);

    /// The choice kept in `user` (nil: none).
    [[nodiscard]] Guid SavedQualityLevel(const foundation::settings::Settings& user);
    /// Keeps `level` as the choice in `user`.
    void KeepQualityLevel(foundation::settings::Settings& user, const Guid& level);
}
