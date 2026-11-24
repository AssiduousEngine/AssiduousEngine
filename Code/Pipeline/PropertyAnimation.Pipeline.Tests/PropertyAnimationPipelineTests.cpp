// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// propertyanimation.pipeline - the cook path end to end: author a PropertyAnimationClipAsset, cook it
// with the builder into a content DB, then load the cooked product through the runtime factory and
// sample it, covering the asset, source, product and resource chain.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;
import propertyanimation.pipeline;
import foundation.xml.serialization; // a clip asset as a project stores it, at data version 0

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::propertyanimation;
using namespace pipeline;

TEST_CASE("propanim.pipeline: asset cooks to the content DB, factory loads + samples it")
{
    RegisterPropertyAnimationAssets(); // asset, source and resource types

    FileDelete(u8"scratch_propanim_pipe_db/clip.rasset");
    RemoveDirectory(u8"scratch_propanim_pipe_db");
    NativeFileSystem mount(u8"scratch_propanim_pipe_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"clip", PropertyAnimationClipSource::StaticType());
        id = inst->Id();

        // Author a clip: a Float3 position track 0..(10,0,0) over 1s.
        PropertyAnimationClip clip;
        PropertyTrack t;
        t.target = String(u8"Transform");
        t.propertyPath = String(u8"position");
        t.kind = TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 1.0f;
        b.value = 10.0f;
        t.channels[0].AddKey(a);
        t.channels[0].AddKey(b);
        clip.tracks.PushBack(Move(t));
        clip.duration = clip.ComputeDuration();

        PropertyAnimationClipAsset asset;
        PropertyAnimationClipSource::FromClip(clip, asset.source);

        PropertyAnimationClipAssetBuilder builder;
        REQUIRE(builder.AssetType() == &PropertyAnimationClipAsset::StaticType());
        REQUIRE(builder.ProductType() == &PropertyAnimationClipSource::StaticType());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Load the cooked product through the runtime factory.
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    PropertyAnimationClipFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<PropertyAnimationClipResource> res = manager.Bind<PropertyAnimationClipResource>(id);
    REQUIRE(res);
    CHECK(res->clip.duration == doctest::Approx(1.0f));
    REQUIRE(res->clip.tracks.Size() == 1u);
    CHECK(res->clip.tracks[0].propertyPath.AsView() == StringView(u8"position"));
    CHECK(res->clip.tracks[0].Sample(0.5f).Get<Float3>().x == doctest::Approx(5.0f));

    FileDelete(u8"scratch_propanim_pipe_db/clip.rasset");
    FileDelete(u8"scratch_propanim_pipe_db/clip.data.bin");
    RemoveDirectory(u8"scratch_propanim_pipe_db");
}

TEST_CASE("propanim.pipeline: asset embeds + serializes its source (undo/save round-trip shape)")
{
    RegisterPropertyAnimationAssets();

    PropertyAnimationClipAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"Clips/pulse.rasset");
    asset.source.duration = 2.5f;
    asset.source.trackKind.PushBack(static_cast<u8>(TrackValueKind::Float));
    asset.source.trackSource.PushBack(String(kComponentSource));
    asset.source.trackTarget.PushBack(String(u8"Light"));
    asset.source.trackPath.PushBack(String(u8"intensity"));

    MemoryStream stream;
    {
        // As the content system stores it: inside the type's versioned payload.
        BinarySerializer writer(stream, SerializeMode::Write);
        BeginVersionedPayload(writer, *asset.GetType());
        asset.Serialize(writer);
        EndVersionedPayload(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    PropertyAnimationClipAsset restored;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        BeginVersionedPayload(reader, *restored.GetType());
        restored.Serialize(reader);
        EndVersionedPayload(reader);
    }
    CHECK(restored.fileName.View() == StringView(u8"Clips/pulse.rasset"));
    CHECK(restored.source.duration == doctest::Approx(2.5f));
    REQUIRE(restored.source.trackPath.Size() == 1u);
    CHECK(restored.source.trackPath[0].AsView() == StringView(u8"intensity"));
}

// A clip asset stored before a track's target had a source (data version 0: `trackComponent`) still
// reads, each target a component's; written again, it is version 1 with `trackSource` and
// `trackTarget`.
TEST_CASE("propanim.pipeline: a version 0 clip asset reads its targets as components, and is written as version 1")
{
    RegisterPropertyAnimationAssets();
    const String root = String(u8"scratch_propanim_pipe_v0");
    FileDelete(u8"scratch_propanim_pipe_v0/Bob.xasset");
    RemoveDirectory(root.AsView());
    REQUIRE(CreateDirectories(root.AsView()));
    const String text = Format(u8"<root>\n"
                               u8"\t<string name=\"guid\">5b1c2d3e-4f50-4617-8293-a4b5c6d7e8f9</string>\n"
                               u8"\t<string name=\"typeNamespace\">rtti::pipeline::propertyanimation</string>\n"
                               u8"\t<string name=\"typeName\">PropertyAnimationClipAsset</string>\n"
                               u8"\t<array name=\"dataVersions\" count=\"1\">\n"
                               u8"\t\t<u64 name=\"type\">{}</u64>\n"
                               u8"\t\t<u32 name=\"version\">0</u32>\n"
                               u8"\t</array>\n"
                               u8"\t<object name=\"payload\">\n"
                               u8"\t\t<string name=\"fileName\"/>\n"
                               u8"\t\t<f32 name=\"duration\">1</f32>\n"
                               u8"\t\t<array name=\"trackComponent\" count=\"1\"><string>Light</string></array>\n"
                               u8"\t\t<array name=\"trackPath\" count=\"1\"><string>intensity</string></array>\n"
                               u8"\t\t<array name=\"trackKind\" count=\"1\"><u8>0</u8></array>\n"
                               u8"\t\t<array name=\"channelKeyStart\" count=\"1\"><u32>0</u32></array>\n"
                               u8"\t\t<array name=\"channelKeyCount\" count=\"1\"><u32>1</u32></array>\n"
                               u8"\t\t<array name=\"keyTime\" count=\"1\"><f32>0</f32></array>\n"
                               u8"\t\t<array name=\"keyValue\" count=\"1\"><f32>2</f32></array>\n"
                               u8"\t\t<array name=\"keyTangentIn\" count=\"1\"><f32>0</f32></array>\n"
                               u8"\t\t<array name=\"keyTangentOut\" count=\"1\"><f32>0</f32></array>\n"
                               u8"\t\t<array name=\"keyInterp\" count=\"1\"><u8>1</u8></array>\n"
                               u8"\t\t<array name=\"trackQuatStart\" count=\"1\"><u32>0</u32></array>\n"
                               u8"\t\t<array name=\"trackQuatCount\" count=\"1\"><u32>0</u32></array>\n"
                               u8"\t\t<array name=\"quatTime\" count=\"0\"/>\n"
                               u8"\t\t<array name=\"quatValue\" count=\"0\"/>\n"
                               u8"\t</object>\n"
                               u8"</root>\n",
                               PropertyAnimationClipAsset::StaticType().id);
    const String file = PathJoin(root.AsView(), u8"Bob.xasset");
    REQUIRE(WriteFile(file.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    {
        NativeFileSystem mount(root.AsView(), DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        foundation::content::Instance* instance = db.RootGroup()->GetInstance(u8"Bob");
        REQUIRE(instance != nullptr);
        RefPtr<ISerializable> object = instance->ReadObject();
        auto* asset = Cast<PropertyAnimationClipAsset>(object.Get());
        REQUIRE(asset != nullptr);
        PropertyAnimationClip clip;
        asset->source.FillClip(clip);
        REQUIRE(clip.tracks.Size() == 1u);
        CHECK(clip.tracks[0].source.AsView() == kComponentSource);
        CHECK(clip.tracks[0].target.AsView() == StringView(u8"Light"));
        CHECK(clip.tracks[0].propertyPath.AsView() == StringView(u8"intensity"));
        REQUIRE(instance->WriteObject(*asset).IsOk());
    }
    Result<Array<byte>> written = ReadFile(file.AsView());
    REQUIRE(written.HasValue());
    const StringView saved(reinterpret_cast<const utf8char*>(written.Value().Data()), written.Value().Size());
    CHECK(saved.Contains(u8"<u32 name=\"version\">1</u32>"));
    CHECK(saved.Contains(u8"name=\"trackTarget\""));
    CHECK_FALSE(saved.Contains(u8"name=\"trackComponent\""));
    FileDelete(file.AsView());
    RemoveDirectory(root.AsView());
}
