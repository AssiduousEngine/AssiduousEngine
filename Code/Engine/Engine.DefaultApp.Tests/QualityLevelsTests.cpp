// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The quality levels a run offers its scripts and the player's saved choice: the project's levels in
// its order (one that does not resolve left out), the starting level (the saved choice while the
// project lists it, else the project's), and a choice kept in the user's settings and read back
// as the next run's starting level, as the player does across runs.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.vfs;
import foundation.settings;
import foundation.xml.serialization;
import engine.project;
import engine.render;
import engine.defaultapp;

using namespace foundation::core;
namespace render = engine::render;
using engine::runtime::KeepQualityLevel;
using engine::runtime::OfferQualityLevels;
using engine::runtime::SavedQualityLevel;

TEST_CASE("quality levels: the run offers the project's levels and starts at the saved choice, kept across runs")
{
    render::RegisterRenderProfileResources();
    engine::runtime::RegisterQualityUserSettingsType();
    const StringView dir = u8"scratch_defaultapp_quality_db";
    (void)RemoveDirectoryRecursive(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        const auto write = [&db](StringView name, u32 msaa) -> Guid
        {
            render::RenderQualitySource source;
            source.values.label = String(name);
            source.values.msaaSamples = msaa;
            foundation::content::Instance* instance = db.RootGroup()->CreateInstance(name, render::RenderQualitySource::StaticType());
            REQUIRE(instance != nullptr);
            REQUIRE(instance->WriteObject(source).IsOk());
            return instance->Id();
        };
        const Guid low = write(u8"Low", 1);
        const Guid high = write(u8"High", 4);
        render::RenderQualityFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager resources(DefaultAllocator(), db);
        resources.AddFactory(&factory);

        engine::project::ProjectSettings settings;
        const Guid missing{0x71, 0x72};
        settings.qualityLevelIds.PushBack(low);
        settings.qualityLevelIds.PushBack(missing); // listed, but nothing resolves it
        settings.qualityLevelIds.PushBack(high);
        settings.defaultQualityId = high;

        // The first run: no saved choice, so the project's default; the missing level not offered.
        render::RenderQualityChoice choice;
        CHECK(OfferQualityLevels(choice, settings, resources, Guid{}) == high);
        REQUIRE(choice.levels.Size() == 2u);
        CHECK(choice.levels[0].values.label == StringView(u8"Low"));
        CHECK(choice.levels[1].values.label == StringView(u8"High"));
        CHECK(choice.current == high);

        // The player's script picks Low: the choice plays it and keeps it in the user's settings,
        // which are written to their file.
        foundation::settings::Settings user(DefaultAllocator());
        render::RenderQualitySettings played;
        choice.choose = [&](const render::OfferedQualityLevel& level)
        {
            played = level.values;
            KeepQualityLevel(user, level.id);
        };
        CHECK(choice.Choose(u8"Low"));
        CHECK(played.msaaSamples == 1u);
        CHECK(SavedQualityLevel(user) == low);
        MemoryStream file;
        REQUIRE(user.Save(file, foundation::xml::XmlSerializerFactory()).IsOk());

        // The next run reads the file back and starts at the saved choice.
        foundation::settings::Settings read(DefaultAllocator());
        (void)file.Seek(0, SeekOrigin::Begin);
        REQUIRE(read.Load(file, foundation::xml::XmlSerializerFactory()).IsOk());
        render::RenderQualityChoice next;
        CHECK(OfferQualityLevels(next, settings, resources, SavedQualityLevel(read)) == low);
        CHECK(next.current == low);

        // A saved level the project no longer lists is ignored for the project's default; a
        // project whose starting level does not resolve starts at the engine's own (nil).
        settings.qualityLevelIds.Clear();
        settings.qualityLevelIds.PushBack(high);
        CHECK(OfferQualityLevels(next, settings, resources, low) == high);
        settings.qualityLevelIds[0] = missing;
        settings.defaultQualityId = missing;
        CHECK(OfferQualityLevels(next, settings, resources, Guid{}).IsNil());
        CHECK(next.levels.IsEmpty());
    }
    (void)RemoveDirectoryRecursive(dir);
}
