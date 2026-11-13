// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The render profile assets: a profile asset round-trips its values through its source envelope,
// cooks to its record by a copy, loads through the runtime factory, and declares its texture
// reference so the cook knows the profile needs it. A Render Quality (a quality level) does the
// same, with nothing to reference.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.render;
import pipeline.core;
import engine.render;
import render.pipeline;

using namespace foundation::core;
using namespace pipeline;
namespace content = foundation::content;
namespace render = engine::render;

namespace
{
    void RemoveTree(StringView root)
    {
        foundation::vfs::NativeFileSystem fs(root, DefaultAllocator());
        Array<foundation::vfs::DirEntry> entries;
        if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            for (const auto& e : entries)
            {
                if (!e.isDirectory)
                {
                    (void)fs.AsWritable()->Delete(e.name.AsView());
                }
            }
        }
        (void)RemoveDirectory(root);
    }
}

TEST_CASE("render.pipeline: an Environment Profile cooks and loads with its values")
{
    RegisterRenderProfileAssets();
    render::RegisterRenderProfileResources();
    const StringView dir = u8"scratch_render_pipeline_db";
    RemoveTree(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");

        // The source asset round-trips its values (the block's own serializer).
        EnvironmentProfileAsset asset;
        asset.values.ambientIntensity = 0.08f;
        asset.values.skyMode = foundation::render::SkyMode::Analytic;
        asset.values.shadowDistance = 60.0f;
        asset.values.skyTexture.id = Guid{0x5u, 0x6u};
        auto* source = db.RootGroup()->CreateInstance(u8"dusk", EnvironmentProfileAsset::StaticType());
        REQUIRE(source->WriteObject(asset).IsOk());
        RefPtr<ISerializable> back = source->ReadObject();
        auto* read = Cast<EnvironmentProfileAsset>(back.Get());
        REQUIRE(read != nullptr);
        CHECK(read->values.shadowDistance == doctest::Approx(60.0f));
        CHECK(read->values.skyTexture.id == Guid{0x5u, 0x6u});

        // The cook copies the values; the sky texture is a reference the product needs.
        EnvironmentProfileAssetBuilder builder;
        AssetDependencies dependencies;
        AssetBuildContext ctx{DefaultAllocator()};
        builder.ScanDependencies(asset, ctx, dependencies);
        REQUIRE(dependencies.references.Size() == 1u);
        CHECK(dependencies.references[0] == Guid{0x5u, 0x6u});
        asset.values.skyTexture.id = Guid{}; // no texture product in this database
        auto* cooked =
            db.RootGroup()->CreateInstance(u8"dusk.cooked", render::EnvironmentProfileSource::StaticType());
        ctx.output = cooked;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        render::EnvironmentProfileFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<render::EnvironmentProfile> profile =
            manager.Bind<render::EnvironmentProfile>(cooked->Id());
        REQUIRE(profile);
        CHECK(profile->values.ambientIntensity == doctest::Approx(0.08f));
        CHECK(profile->values.skyMode == foundation::render::SkyMode::Analytic);
        CHECK(profile->values.shadowDistance == doctest::Approx(60.0f));
    }
    RemoveTree(dir);
}

TEST_CASE("render.pipeline: a Post Process Profile cooks and loads with its values")
{
    RegisterRenderProfileAssets();
    render::RegisterRenderProfileResources();
    const StringView dir = u8"scratch_render_pipeline_post_db";
    RemoveTree(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        PostProcessProfileAsset asset;
        asset.values.exposureEV = 1.0f;
        asset.values.fxaaSubpixel = 0.5f;
        asset.values.aoMode = foundation::render::AoMode::GTAO;
        auto* cooked =
            db.RootGroup()->CreateInstance(u8"look", render::PostProcessProfileSource::StaticType());
        PostProcessProfileAssetBuilder builder;
        AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = cooked;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        render::PostProcessProfileFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<render::PostProcessProfile> profile =
            manager.Bind<render::PostProcessProfile>(cooked->Id());
        REQUIRE(profile);
        CHECK(profile->values.exposureEV == doctest::Approx(1.0f));
        CHECK(profile->values.fxaaSubpixel == doctest::Approx(0.5f));
        CHECK(profile->values.aoMode == foundation::render::AoMode::GTAO);
    }
    RemoveTree(dir);
}

TEST_CASE("render.pipeline: a Render Quality round-trips, cooks and loads with its values")
{
    RegisterRenderProfileAssets();
    render::RegisterRenderProfileResources();
    const StringView dir = u8"scratch_render_pipeline_quality_db";
    RemoveTree(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");

        // A phone's level: every field away from the engine's defaults, so each one is seen to travel.
        RenderQualityAsset asset;
        asset.values.label = String(u8"Low");
        asset.values.msaaSamples = 1;
        asset.values.antialiasing = render::AaMode::FXAA;
        asset.values.shadows = render::QualityTier::Low;
        asset.values.shadowDistanceMax = 60.0f;
        asset.values.ambientOcclusion = render::QualityTier::Off;
        asset.values.reflections = render::QualityTier::Off;
        asset.values.globalIllumination = render::QualityTier::Off;
        asset.values.bloom = render::QualityTier::Medium;
        asset.values.environmentResolution = render::ResolutionTier::Low;

        auto* source = db.RootGroup()->CreateInstance(u8"low", RenderQualityAsset::StaticType());
        REQUIRE(source->WriteObject(asset).IsOk());
        RefPtr<ISerializable> back = source->ReadObject();
        auto* read = Cast<RenderQualityAsset>(back.Get());
        REQUIRE(read != nullptr);
        CHECK(read->values.label == StringView(u8"Low"));
        CHECK(read->values.msaaSamples == 1u);
        CHECK(read->values.antialiasing == render::AaMode::FXAA);
        CHECK(read->values.shadows == render::QualityTier::Low);
        CHECK(read->values.shadowDistanceMax == doctest::Approx(60.0f));
        CHECK(read->values.ambientOcclusion == render::QualityTier::Off);
        CHECK(read->values.reflections == render::QualityTier::Off);
        CHECK(read->values.globalIllumination == render::QualityTier::Off);
        CHECK(read->values.bloom == render::QualityTier::Medium);
        CHECK(read->values.environmentResolution == render::ResolutionTier::Low);

        // It references nothing, and cooks to its record by a copy that the factory loads.
        RenderQualityAssetBuilder builder;
        AssetDependencies dependencies;
        AssetBuildContext ctx{DefaultAllocator()};
        builder.ScanDependencies(asset, ctx, dependencies);
        CHECK(dependencies.references.IsEmpty());
        auto* cooked = db.RootGroup()->CreateInstance(u8"low.cooked", render::RenderQualitySource::StaticType());
        ctx.output = cooked;
        REQUIRE(builder.Build(asset, ctx).IsOk());
        render::RenderQualityFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<render::RenderQualityLevel> quality = manager.Bind<render::RenderQualityLevel>(cooked->Id());
        REQUIRE(quality);
        CHECK(quality->values.label == StringView(u8"Low"));
        CHECK(quality->values.msaaSamples == 1u);
        CHECK(quality->values.shadows == render::QualityTier::Low);
        CHECK(quality->values.ambientOcclusion == render::QualityTier::Off);
        CHECK(quality->values.environmentResolution == render::ResolutionTier::Low);
    }
    RemoveTree(dir);
}

TEST_CASE("render.pipeline: a new Render Quality is the engine's highest level")
{
    // The defaults are what a level made from File > New starts at: every effect at High, 4x MSAA.
    const render::RenderQualitySettings values;
    CHECK(values.label == StringView(u8"High"));
    CHECK(values.msaaSamples == 4u);
    CHECK(values.antialiasing == render::AaMode::Off);
    CHECK(values.shadows == render::QualityTier::High);
    CHECK(values.shadowDistanceMax == doctest::Approx(0.0f));
    CHECK(values.ambientOcclusion == render::QualityTier::High);
    CHECK(values.reflections == render::QualityTier::High);
    CHECK(values.globalIllumination == render::QualityTier::High);
    CHECK(values.bloom == render::QualityTier::High);
    CHECK(values.environmentResolution == render::ResolutionTier::High);
}
