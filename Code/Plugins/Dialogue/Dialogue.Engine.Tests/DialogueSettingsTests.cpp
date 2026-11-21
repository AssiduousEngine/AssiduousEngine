// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Engine: the plugin's settings, as the project's entry for the plugin holds them and as
// Project Settings shows them.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import engine.project;
import dialogue.engine;

using namespace foundation::core;
namespace runtime = foundation::runtime;
using dialogue::DialogueSettings;

TEST_CASE("dialogue settings: an entry with none gives the defaults")
{
    DialogueSettings settings;
    REQUIRE(runtime::ReadPluginSettings(Span<const u8>(), settings, DefaultAllocator()).IsOk());
    CHECK(settings.cast == Guid{});
    CHECK(settings.assets == Guid{});
    CHECK(settings.commands.IsEmpty());
    CHECK(settings.providedFlags.IsEmpty());
    CHECK(settings.flagPrefix == StringView(u8"flag."));
    CHECK(settings.fadeSeconds == doctest::Approx(0.5f));
    CHECK(settings.fadeColor.w == doctest::Approx(1.0f));
    CHECK_FALSE(settings.stockBox);
    CHECK(settings.typingSpeed == doctest::Approx(40.0f));
    CHECK(settings.advanceAction == StringView(u8"Interact"));
    CHECK_FALSE(settings.portraitOnRight);
}

TEST_CASE("dialogue settings: they read back from the entry as they were written")
{
    DialogueSettings written;
    written.commands.PushBack(String(u8"move <who:entity> to <target:entity> [running] [and wait]"));
    written.commands.PushBack(String(u8"fight <foe:list(foes)> leaves"));
    written.providedFlags.PushBack(String(u8"seen_*"));
    written.flagPrefix = String(u8"story.");
    written.fadeSeconds = 1.25f;
    written.fadeColor = Float4{1.0f, 1.0f, 1.0f, 1.0f};
    written.stockBox = true;
    written.typingSpeed = 60.0f;
    written.advanceAction = String(u8"Confirm");
    written.portraitOnRight = true;
    Array<u8> entry;
    REQUIRE(runtime::WritePluginSettings(written, entry, DefaultAllocator()).IsOk());

    DialogueSettings read;
    REQUIRE(runtime::ReadPluginSettings(Span<const u8>(entry.Data(), entry.Size()), read, DefaultAllocator()).IsOk());
    REQUIRE(read.commands.Size() == 2u);
    CHECK(read.commands[0] == written.commands[0]);
    CHECK(read.commands[1] == written.commands[1]);
    REQUIRE(read.providedFlags.Size() == 1u);
    CHECK(read.providedFlags[0] == StringView(u8"seen_*"));
    CHECK(read.flagPrefix == StringView(u8"story."));
    CHECK(read.fadeSeconds == doctest::Approx(1.25f));
    CHECK(read.fadeColor.x == doctest::Approx(1.0f));
    CHECK(read.stockBox);
    CHECK(read.typingSpeed == doctest::Approx(60.0f));
    CHECK(read.advanceAction == StringView(u8"Confirm"));
    CHECK(read.portraitOnRight);
}

TEST_CASE("dialogue settings: every setting has a label for Project Settings")
{
    const TypeInfo& type = DialogueSettings::StaticType();
    usize count = 0;
    for (const PropertyInfo& property : Properties(type))
    {
        ++count;
        const String* label = engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute);
        CHECK_MESSAGE(label != nullptr, property.name);
    }
    CHECK(count == 11u);
}
