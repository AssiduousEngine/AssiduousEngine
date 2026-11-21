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
        walk->property.componentType = String(propertyanimation::kTransformTarget);
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
        BinarySerializer writer(stream, SerializeMode::Write);
        asset.Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    SequenceAsset restored;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        restored.Serialize(reader);
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
