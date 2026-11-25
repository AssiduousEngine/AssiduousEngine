// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// How a run's window presents: the mode it starts in (a command line's for one run, else the
// player's saved choice, else the project's setting), the modes a run offers its scripts, and a
// choice kept in the user's settings and read back as the next run's.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.rhi;
import foundation.settings;
import foundation.xml.serialization;
import engine.project;
import engine.render;
import engine.defaultapp;

using namespace foundation::core;
namespace render = engine::render;
using engine::runtime::KeepPresentMode;
using engine::runtime::OfferPresentModes;
using engine::runtime::SavedPresentMode;
using engine::runtime::StartupPresentMode;

TEST_CASE("display: the mode a run starts in, in order of who chose it")
{
    engine::project::ProjectSettings settings;
    CHECK(StartupPresentMode(settings, u8"") == StringView(u8"vsync")); // the project's default

    settings.presentMode = engine::project::PresentMode::Mailbox;
    CHECK(StartupPresentMode(settings, u8"") == StringView(u8"mailbox"));

    // The player's saved choice over the project's, and a command line's over both.
    CHECK(StartupPresentMode(settings, u8"immediate") == StringView(u8"immediate"));
    CHECK(StartupPresentMode(settings, u8"immediate", u8"vsync") == StringView(u8"vsync"));

    // A name no mode has is passed over.
    CHECK(StartupPresentMode(settings, u8"uncapped") == StringView(u8"mailbox"));
    CHECK(StartupPresentMode(settings, u8"immediate", u8"uncapped") == StringView(u8"immediate"));

    CHECK(engine::runtime::PresentModeOf(engine::project::PresentMode::Vsync) == foundation::rhi::PresentMode::Fifo);
    CHECK(engine::runtime::PresentModeOf(engine::project::PresentMode::Mailbox) == foundation::rhi::PresentMode::Mailbox);
    CHECK(engine::runtime::PresentModeOf(engine::project::PresentMode::Immediate) == foundation::rhi::PresentMode::Immediate);
}

TEST_CASE("display: the run offers every mode, and a choice is kept across runs")
{
    engine::runtime::RegisterDisplayUserSettingsType();
    engine::project::ProjectSettings settings;

    // The first run: no saved choice, so the project's.
    render::DisplayChoice choice;
    foundation::settings::Settings user(DefaultAllocator());
    OfferPresentModes(choice, StartupPresentMode(settings, SavedPresentMode(user)));
    REQUIRE(choice.presentModes.Size() == 3u);
    CHECK(choice.presentModes[0] == StringView(u8"vsync"));
    CHECK(choice.presentModes[1] == StringView(u8"mailbox"));
    CHECK(choice.presentModes[2] == StringView(u8"immediate"));
    CHECK(choice.presentMode == StringView(u8"vsync"));

    // The player's options menu picks immediate: kept in the user's settings, written to their file.
    choice.choosePresentMode = [&user](StringView name)
    {
        KeepPresentMode(user, name);
        return true;
    };
    CHECK(choice.ChoosePresentMode(u8"immediate"));
    CHECK(SavedPresentMode(user) == StringView(u8"immediate"));
    MemoryStream file;
    REQUIRE(user.Save(file, foundation::xml::XmlSerializerFactory()).IsOk());

    // The next run reads the file back and starts in the saved choice.
    foundation::settings::Settings read(DefaultAllocator());
    (void)file.Seek(0, SeekOrigin::Begin);
    REQUIRE(read.Load(file, foundation::xml::XmlSerializerFactory()).IsOk());
    render::DisplayChoice next;
    OfferPresentModes(next, StartupPresentMode(settings, SavedPresentMode(read)));
    CHECK(next.presentMode == StringView(u8"immediate"));
}
