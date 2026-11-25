// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - the `engine.defaultapp:display` partition: how a run's window presents. The
// player and the editor's Game tab both fill their run's DisplayChoice here; the player also keeps
// a choice in the user's settings, a section of the file beside the game's other per-user settings,
// so the next run starts in it.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the user settings section (RTTI_OBJECT)

export module engine.defaultapp:display;

import foundation.core;
import foundation.rhi;
import foundation.settings;
import engine.project;
import engine.render;

using namespace foundation::core;

export namespace engine::runtime
{
    /// The player's present mode, a section of the game's user settings: the mode's name.
    class DisplayUserSettings final : public ISerializable
    {
        RTTI_OBJECT(DisplayUserSettings, ISerializable)
    public:
        String presentMode;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "presentMode", presentMode); }
    };

    /// Registers the user settings section, which loading the file instantiates by its name.
    /// Idempotent.
    void RegisterDisplayUserSettingsType();

    /// The window's present mode a project's setting names.
    [[nodiscard]] foundation::rhi::PresentMode PresentModeOf(engine::project::PresentMode mode) noexcept;

    /// The name of the mode a run starts in: `asked` (a command line's, for this run) when a mode
    /// has that name, else `saved` (the player's choice) when one has, else the project's setting.
    [[nodiscard]] StringView StartupPresentMode(const engine::project::ProjectSettings& settings, StringView saved,
                                                StringView asked = {}) noexcept;

    /// Fills `choice` with every mode offered by name, presenting in `current`.
    void OfferPresentModes(engine::render::DisplayChoice& choice, StringView current);

    /// The choice kept in `user` (empty: none).
    [[nodiscard]] String SavedPresentMode(const foundation::settings::Settings& user);
    /// Keeps `name` as the choice in `user`.
    void KeepPresentMode(foundation::settings::Settings& user, StringView name);
}
