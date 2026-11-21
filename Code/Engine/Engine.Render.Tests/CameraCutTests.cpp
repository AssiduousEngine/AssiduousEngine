// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A sequence's camera cut: a claim on the view comes before the primary camera in the camera
// pick, the play begun last wins, and letting go passes the view back. The camera knows nothing
// of it.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.sequence;
import engine.render;

using namespace foundation::core;
using namespace engine::render;
using namespace foundation::render;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;

namespace
{
    // A gameplay camera (primary) and two cutscene cameras (not primary).
    struct CameraStage
    {
        scene::Scene scene{DefaultAllocator(), u8"cuts"};
        CameraComponentManager* cameras = nullptr;
        scene::EntityHandle gameplay;
        scene::EntityHandle wide;
        scene::EntityHandle close;

        CameraStage()
        {
            RegisterRenderComponentReflection();
            cameras = scene.AddSystem<CameraComponentManager>();
            gameplay = Camera(u8"gameplay", 0.0f, true);
            wide = Camera(u8"wide", 10.0f, false);
            close = Camera(u8"close", 20.0f, false);
            scene.UpdateTransforms();
        }

        scene::EntityHandle Camera(StringView name, f32 x, bool primary)
        {
            const scene::EntityHandle e = scene.CreateEntity(name);
            scene.SetLocalPosition(e, Float3{x, 0, 0});
            cameras->Add(e).primary = primary;
            return e;
        }

        // The camera the view is drawn from now.
        [[nodiscard]] scene::EntityHandle View()
        {
            ViewCamera camera;
            scene::EntityHandle picked = scene::EntityHandle::Invalid();
            (void)ExtractPrimaryCamera(scene, camera, nullptr, 0.0f, &picked);
            return picked;
        }
    };

    RefPtr<CameraCutSequenceTrack> Cut(i32 binding, f32 start, f32 duration)
    {
        RefPtr<CameraCutSequenceTrack> track = MakeRef<CameraCutSequenceTrack>(DefaultAllocator());
        track->binding = binding;
        track->sections.PushBack(CameraCutSection{start, duration});
        return track;
    }

    // "Wide" for the first second, "Close" for the second.
    sequence::Sequence TwoShots()
    {
        sequence::Sequence sequence;
        sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"Wide")});
        sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"Close")});
        sequence.tracks.PushBack(Cut(0, 0.0f, 1.0f));
        sequence.tracks.PushBack(Cut(1, 1.0f, 1.0f));
        sequence.duration = 2.0f;
        return sequence;
    }
}

TEST_CASE("camera cut: a claim on the view comes before the primary camera, the latest order winning")
{
    CameraStage stage;
    CHECK(stage.View() == stage.gameplay);

    int first = 0;
    int second = 0;
    stage.cameras->ClaimView(&first, 1, stage.wide);
    CHECK(stage.View() == stage.wide);
    stage.cameras->ClaimView(&second, 2, stage.close);
    CHECK(stage.View() == stage.close);
    stage.cameras->ReleaseView(&second);
    CHECK(stage.View() == stage.wide); // back to the claim under it
    stage.cameras->ReleaseView(&first);
    CHECK(stage.View() == stage.gameplay);

    // An inactive camera cannot be the view: the pick falls back to the primary.
    stage.cameras->ClaimView(&first, 1, stage.wide);
    stage.scene.SetActive(stage.wide, false);
    CHECK(stage.View() == stage.gameplay);
}

TEST_CASE("camera cut: a play cuts between its cameras and gives the view back when it finishes")
{
    CameraStage stage;
    sequence::Sequence sequence = TwoShots();
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    REQUIRE(play.Bind(u8"Wide", stage.wide));
    REQUIRE(play.Bind(u8"Close", stage.close));

    play.Play();
    CHECK(stage.View() == stage.wide);
    play.Advance(0.5f);
    CHECK(stage.View() == stage.wide);
    play.Advance(0.5f); // exactly on the cut: the shot cut to takes the view
    CHECK(stage.View() == stage.close);
    play.Advance(0.5f);
    CHECK(stage.View() == stage.close);
    play.Advance(1.0f); // the end, released
    CHECK(play.IsFinished());
    CHECK(stage.View() == stage.gameplay);
}

TEST_CASE("camera cut: a held play keeps its last shot until it stops")
{
    CameraStage stage;
    sequence::Sequence sequence = TwoShots();
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    REQUIRE(play.Bind(u8"Wide", stage.wide));
    REQUIRE(play.Bind(u8"Close", stage.close));
    play.finish = sequence::SequenceFinish::Hold;

    play.Play();
    play.Advance(3.0f);
    CHECK(play.IsFinished());
    CHECK(stage.View() == stage.close);
    play.Stop();
    CHECK(stage.View() == stage.gameplay);
}

TEST_CASE("camera cut: the play begun last holds the view, and gives it back to the one under it")
{
    CameraStage stage;
    sequence::Sequence longShot;
    longShot.bindings.PushBack(sequence::SequenceBinding{String(u8"Camera")});
    longShot.tracks.PushBack(Cut(0, 0.0f, 10.0f));
    longShot.duration = 10.0f;
    sequence::Sequence shortShot;
    shortShot.bindings.PushBack(sequence::SequenceBinding{String(u8"Camera")});
    shortShot.tracks.PushBack(Cut(0, 0.0f, 1.0f));
    shortShot.duration = 1.0f;

    sequence::SequencePlay under(DefaultAllocator(), stage.scene, longShot);
    REQUIRE(under.Bind(u8"Camera", stage.wide));
    sequence::SequencePlay over(DefaultAllocator(), stage.scene, shortShot);
    REQUIRE(over.Bind(u8"Camera", stage.close));

    under.Play();
    over.Play();
    CHECK(stage.View() == stage.close);
    under.Advance(0.5f); // the earlier play steps again: the later one keeps the view
    CHECK(stage.View() == stage.close);
    over.Advance(2.0f);
    CHECK(over.IsFinished());
    CHECK(stage.View() == stage.wide);
    under.Stop();
    CHECK(stage.View() == stage.gameplay);
}

TEST_CASE("camera cut: a track bound to an entity without a camera is skipped")
{
    CameraStage stage;
    const scene::EntityHandle prop = stage.scene.CreateEntity(u8"prop");
    sequence::Sequence sequence;
    sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"Camera")});
    sequence.tracks.PushBack(Cut(0, 0.0f, 1.0f));
    sequence.duration = 1.0f;
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    REQUIRE(play.Bind(u8"Camera", prop));
    play.Play();
    CHECK(stage.View() == stage.gameplay);
}

TEST_CASE("camera cut: a sequence reads its camera cuts back by their kind")
{
    RegisterRenderComponentReflection();
    sequence::Sequence original = TwoShots();
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        sequence::SerializeSequence(writer, original);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    sequence::Sequence read;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        sequence::SerializeSequence(reader, read);
    }
    REQUIRE(read.tracks.Size() == 2u);
    auto* close = Cast<CameraCutSequenceTrack>(read.tracks[1].Get());
    REQUIRE(close != nullptr);
    CHECK(close->binding == 1);
    REQUIRE(close->sections.Size() == 1u);
    CHECK(close->sections[0].start == doctest::Approx(1.0f));
    CHECK(close->sections[0].duration == doctest::Approx(1.0f));
    CHECK(close->Duration() == doctest::Approx(2.0f));

    // The kind is listed with the others, so the editor offers it.
    bool listed = false;
    for (const TypeInfo* kind : sequence::SequenceTrackKinds())
    {
        listed = listed || kind == &CameraCutSequenceTrack::StaticType();
    }
    CHECK(listed);
}
