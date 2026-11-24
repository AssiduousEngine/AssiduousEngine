// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// sequence.pipeline - the cook end to end (author a sequence asset, cook it into a content DB, load
// the product through the runtime factory), the asset's own round trip, and File > New's creator.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import sequence.pipeline;
import foundation.xml.serialization; // a sequence asset as a project stores it, at data version 0

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::sequence;
using namespace pipeline;
namespace propertyanimation = foundation::propertyanimation;

namespace
{
    Sequence Authored()
    {
        Sequence sequence;
        sequence.frameRate = 24.0f;
        sequence.bindings.PushBack(SequenceBinding{String(u8"Mentor")});
        RefPtr<PropertySequenceTrack> walk = MakeRef<PropertySequenceTrack>(DefaultAllocator());
        walk->name = String(u8"walk");
        walk->binding = 0;
        walk->property.target = String(propertyanimation::kTransformTarget);
        walk->property.propertyPath = String(u8"position");
        walk->property.kind = propertyanimation::TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 2.0f;
        b.value = 4.0f;
        walk->property.channels[0].AddKey(a);
        walk->property.channels[0].AddKey(b);
        sequence.tracks.PushBack(walk);
        RefPtr<EventSequenceTrack> cues = MakeRef<EventSequenceTrack>(DefaultAllocator());
        cues->keys.PushBack(SequenceEventKey{1.0f, String(u8"cue"), String(u8"gate")});
        sequence.tracks.PushBack(cues);
        sequence.markers.PushBack(SequenceMarker{1.5f, String(u8"line"), true});
        sequence.duration = sequence.ComputeDuration();
        return sequence;
    }
}

TEST_CASE("sequence.pipeline: an asset cooks to the content DB and the factory loads it")
{
    RegisterSequenceAssets();

    FileDelete(u8"scratch_sequence_pipe_db/seq.rasset");
    RemoveDirectory(u8"scratch_sequence_pipe_db");
    NativeFileSystem mount(u8"scratch_sequence_pipe_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        auto* instance = db.RootGroup()->CreateInstance(u8"seq", SequenceSource::StaticType());
        id = instance->Id();

        SequenceAsset asset;
        asset.sequence = Authored();
        SequenceAssetBuilder builder;
        REQUIRE(builder.AssetType() == &SequenceAsset::StaticType());
        REQUIRE(builder.ProductType() == &SequenceSource::StaticType());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = instance;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    SequenceFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<SequenceResource> loaded = manager.Bind<SequenceResource>(id);
    REQUIRE(loaded);
    const Sequence& sequence = loaded->sequence;
    CHECK(sequence.duration == doctest::Approx(2.0f));
    CHECK(sequence.frameRate == doctest::Approx(24.0f));
    CHECK(sequence.FindBinding(u8"Mentor") == 0);
    REQUIRE(sequence.markers.Size() == 1u);
    CHECK(sequence.markers[0].hold);
    REQUIRE(sequence.tracks.Size() == 2u);
    auto* walk = Cast<PropertySequenceTrack>(sequence.tracks[0].Get());
    REQUIRE(walk != nullptr);
    CHECK(walk->property.Sample(1.0f).Get<Float3>().x == doctest::Approx(2.0f));
    CHECK(Cast<EventSequenceTrack>(sequence.tracks[1].Get()) != nullptr);

    FileDelete(u8"scratch_sequence_pipe_db/seq.rasset");
    FileDelete(u8"scratch_sequence_pipe_db/seq.data.bin");
    RemoveDirectory(u8"scratch_sequence_pipe_db");
}

TEST_CASE("sequence.pipeline: the asset keeps its file name and sequence through a save")
{
    RegisterSequenceAssets();
    SequenceAsset asset;
    asset.fileName = SourcePath(u8"Cutscenes/gate.rasset");
    asset.sequence = Authored();

    MemoryStream stream;
    {
        // As the content system stores it: inside the type's versioned payload.
        BinarySerializer writer(stream, SerializeMode::Write);
        BeginVersionedPayload(writer, *asset.GetType());
        asset.Serialize(writer);
        EndVersionedPayload(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    SequenceAsset restored;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        BeginVersionedPayload(reader, *restored.GetType());
        restored.Serialize(reader);
        EndVersionedPayload(reader);
    }
    CHECK(restored.fileName.View() == StringView(u8"Cutscenes/gate.rasset"));
    CHECK(restored.sequence.tracks.Size() == 2u);
    CHECK(restored.sequence.markers.Size() == 1u);
}

TEST_CASE("sequence.pipeline: File > New makes an empty sequence")
{
    RegisterSequenceAssets();
    AssetCreatorRegistry registry(DefaultAllocator());
    RegisterSequenceCreators(registry);
    bool found = false;
    for (usize i = 0; i < registry.Count(); ++i)
    {
        const AssetCreator& creator = registry.At(i);
        if (creator.type == &SequenceAsset::StaticType())
        {
            found = true;
            CHECK(creator.label.AsView() == StringView(u8"Sequence"));
            CHECK(creator.category.AsView() == StringView(u8"Animation"));
        }
    }
    CHECK(found);
}

// A sequence asset stored before a property track's target had a source (data version 0: each
// track's `component`) still reads, its targets components; written again, it is version 1 with
// `source` and `target`.
TEST_CASE("sequence.pipeline: a version 0 sequence asset reads its targets as components, and is written as version 1")
{
    RegisterSequenceAssets();
    const String root = String(u8"scratch_sequence_pipe_v0");
    FileDelete(u8"scratch_sequence_pipe_v0/Roll.xasset");
    RemoveDirectory(root.AsView());
    REQUIRE(CreateDirectories(root.AsView()));
    const String text = Format(u8"<root>\n"
                               u8"\t<string name=\"guid\">6c2d3e4f-5061-4728-93a4-b5c6d7e8f90a</string>\n"
                               u8"\t<string name=\"typeNamespace\">rtti::pipeline::sequence</string>\n"
                               u8"\t<string name=\"typeName\">SequenceAsset</string>\n"
                               u8"\t<array name=\"dataVersions\" count=\"1\">\n"
                               u8"\t\t<u64 name=\"type\">{}</u64>\n"
                               u8"\t\t<u32 name=\"version\">0</u32>\n"
                               u8"\t</array>\n"
                               u8"\t<object name=\"payload\">\n"
                               u8"\t\t<string name=\"fileName\"/>\n"
                               u8"\t\t<f32 name=\"duration\">2</f32>\n"
                               u8"\t\t<f32 name=\"frameRate\">30</f32>\n"
                               u8"\t\t<array name=\"bindings\" count=\"1\"><string>Cart</string></array>\n"
                               u8"\t\t<array name=\"markerTime\" count=\"0\"/>\n"
                               u8"\t\t<array name=\"markerName\" count=\"0\"/>\n"
                               u8"\t\t<array name=\"markerHold\" count=\"0\"/>\n"
                               u8"\t\t<array name=\"tracks\" count=\"1\">\n"
                               u8"\t\t\t<object>\n"
                               u8"\t\t\t\t<u64 name=\"type\">{}</u64>\n"
                               u8"\t\t\t\t<string name=\"name\"/>\n"
                               u8"\t\t\t\t<i32 name=\"binding\">0</i32>\n"
                               u8"\t\t\t\t<bool name=\"muted\">false</bool>\n"
                               u8"\t\t\t\t<string name=\"component\">Transform</string>\n"
                               u8"\t\t\t\t<string name=\"path\">position</string>\n"
                               u8"\t\t\t\t<u8 name=\"kind\">1</u8>\n"
                               u8"\t\t\t\t<array name=\"keyCount\" count=\"4\"><u32>1</u32><u32>0</u32><u32>0</u32><u32>0</u32></array>\n"
                               u8"\t\t\t\t<array name=\"keyTime\" count=\"1\"><f32>0</f32></array>\n"
                               u8"\t\t\t\t<array name=\"keyValue\" count=\"1\"><f32>6</f32></array>\n"
                               u8"\t\t\t\t<array name=\"keyTangentIn\" count=\"1\"><f32>0</f32></array>\n"
                               u8"\t\t\t\t<array name=\"keyTangentOut\" count=\"1\"><f32>0</f32></array>\n"
                               u8"\t\t\t\t<array name=\"keyInterpolation\" count=\"1\"><u8>1</u8></array>\n"
                               u8"\t\t\t\t<array name=\"keyTangentMode\" count=\"1\"><u8>1</u8></array>\n"
                               u8"\t\t\t\t<array name=\"quatTime\" count=\"0\"/>\n"
                               u8"\t\t\t\t<array name=\"quatValue\" count=\"0\"/>\n"
                               u8"\t\t\t</object>\n"
                               u8"\t\t</array>\n"
                               u8"\t</object>\n"
                               u8"</root>\n",
                               SequenceAsset::StaticType().id, PropertySequenceTrack::StaticType().id);
    const String file = PathJoin(root.AsView(), u8"Roll.xasset");
    REQUIRE(WriteFile(file.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    {
        NativeFileSystem mount(root.AsView(), DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        foundation::content::Instance* instance = db.RootGroup()->GetInstance(u8"Roll");
        REQUIRE(instance != nullptr);
        RefPtr<ISerializable> object = instance->ReadObject();
        auto* asset = Cast<SequenceAsset>(object.Get());
        REQUIRE(asset != nullptr);
        REQUIRE(asset->sequence.tracks.Size() == 1u);
        const auto* property = Cast<PropertySequenceTrack>(asset->sequence.tracks[0].Get());
        REQUIRE(property != nullptr);
        CHECK(property->property.source.AsView() == propertyanimation::kComponentSource);
        CHECK(property->property.target.AsView() == propertyanimation::kTransformTarget);
        CHECK(property->property.channels[0].KeyCount() == 1u);
        CHECK(property->NeedsBinding());
        REQUIRE(instance->WriteObject(*asset).IsOk());
    }
    Result<Array<byte>> written = ReadFile(file.AsView());
    REQUIRE(written.HasValue());
    const StringView saved(reinterpret_cast<const utf8char*>(written.Value().Data()), written.Value().Size());
    CHECK(saved.Contains(u8"<u32 name=\"version\">1</u32>"));
    CHECK(saved.Contains(u8"<string name=\"target\">Transform</string>"));
    CHECK_FALSE(saved.Contains(u8"name=\"component\""));
    FileDelete(file.AsView());
    RemoveDirectory(root.AsView());
}
