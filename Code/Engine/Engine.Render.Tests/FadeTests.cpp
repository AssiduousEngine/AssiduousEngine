// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A sequence's fade: its keys give the amount over the play, the play begun last holds the fade,
// and letting go clears it (or hands it to the play under it).

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.sequence;
import engine.render;

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;

namespace
{
    struct FadeStage
    {
        scene::Scene scene{DefaultAllocator(), u8"fades"};
        PostProcessSystem* post = nullptr;

        FadeStage()
        {
            RegisterRenderComponentReflection();
            post = scene.AddSystem<PostProcessSystem>();
        }
    };

    // Black from nothing to all over a second; `color` when given.
    sequence::Sequence FadeOut(f32 seconds = 1.0f, Color color = Color{0.0f, 0.0f, 0.0f, 1.0f})
    {
        RefPtr<FadeSequenceTrack> track = MakeRef<FadeSequenceTrack>(DefaultAllocator());
        track->color = color;
        CurveKey from;
        from.time = 0.0f;
        from.value = 0.0f;
        from.interpolation = CurveKeyInterpolation::Linear;
        CurveKey to;
        to.time = seconds;
        to.value = 1.0f;
        to.interpolation = CurveKeyInterpolation::Linear;
        track->amount.AddKey(from);
        track->amount.AddKey(to);
        sequence::Sequence sequence;
        sequence.tracks.PushBack(track);
        sequence.duration = seconds;
        return sequence;
    }
}

TEST_CASE("fade: its keys give the amount, and the play's end clears it")
{
    FadeStage stage;
    CHECK(stage.post->Fade().amount == 0.0f);
    sequence::Sequence sequence = FadeOut(1.0f, Color{1.0f, 0.5f, 0.25f, 1.0f});
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);

    play.Play();
    CHECK(stage.post->Fade().amount == doctest::Approx(0.0f));
    play.Advance(0.5f);
    CHECK(stage.post->Fade().amount == doctest::Approx(0.5f));
    CHECK(stage.post->Fade().color.g == doctest::Approx(0.5f));
    play.Advance(1.0f); // the end: released
    CHECK(play.IsFinished());
    CHECK(stage.post->Fade().amount == 0.0f);
}

TEST_CASE("fade: a held play stays faded until it stops")
{
    FadeStage stage;
    sequence::Sequence sequence = FadeOut();
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    play.finish = sequence::SequenceFinish::Hold;
    play.Play();
    play.Advance(2.0f);
    CHECK(play.IsFinished());
    CHECK(stage.post->Fade().amount == doctest::Approx(1.0f));
    play.Stop();
    CHECK(stage.post->Fade().amount == 0.0f);
}

TEST_CASE("fade: the play begun last holds the fade, and gives it back to the one under it")
{
    FadeStage stage;
    sequence::Sequence black = FadeOut(10.0f);
    sequence::Sequence white = FadeOut(1.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    sequence::SequencePlay under(DefaultAllocator(), stage.scene, black);
    sequence::SequencePlay over(DefaultAllocator(), stage.scene, white);

    under.Play();
    over.Play();
    under.Advance(5.0f); // the earlier play steps on: the later one keeps the fade
    over.Advance(0.5f);
    CHECK(stage.post->Fade().color.r == doctest::Approx(1.0f));
    CHECK(stage.post->Fade().amount == doctest::Approx(0.5f));
    over.Advance(1.0f);
    CHECK(over.IsFinished());
    CHECK(stage.post->Fade().color.r == doctest::Approx(0.0f));
    CHECK(stage.post->Fade().amount == doctest::Approx(0.5f));
    under.Stop();
    CHECK(stage.post->Fade().amount == 0.0f);
}

TEST_CASE("fade: a scene nothing renders has no view to fade")
{
    scene::Scene bare(DefaultAllocator(), u8"bare");
    RegisterRenderComponentReflection();
    sequence::Sequence sequence = FadeOut();
    sequence::SequencePlay play(DefaultAllocator(), bare, sequence);
    play.Play();
    play.Advance(0.5f);
    CHECK(play.IsPlaying());
}

TEST_CASE("fade: a sequence reads its fade back by its kind")
{
    RegisterRenderComponentReflection();
    sequence::Sequence original = FadeOut(2.0f, Color{0.2f, 0.4f, 0.6f, 1.0f});
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
    REQUIRE(read.tracks.Size() == 1u);
    auto* fade = Cast<FadeSequenceTrack>(read.tracks[0].Get());
    REQUIRE(fade != nullptr);
    CHECK(fade->color.b == doctest::Approx(0.6f));
    REQUIRE(fade->amount.KeyCount() == 2u);
    CHECK(fade->amount.Evaluate(1.0f) == doctest::Approx(0.5f));
    CHECK(fade->Duration() == doctest::Approx(2.0f));
}

TEST_CASE("fade and camera cut editing: the fade's amount is one curve channel; a shot is a section")
{
    FadeSequenceTrack fade;
    CHECK(fade.KeyChannelCount() == 1u);
    CHECK(fade.KeyCurve(0) == &fade.amount);
    CHECK(fade.InsertKey(0, 0.5f) == 0);
    CHECK(fade.amount.Keys()[0].value == doctest::Approx(0.0f));

    CameraCutSequenceTrack cuts;
    CHECK(cuts.AddSection(2.0f) == 0);
    cuts.SetSection(0, 1.0f, 3.0f);
    CHECK(cuts.Duration() == doctest::Approx(4.0f));
    cuts.RemoveSection(0);
    CHECK(cuts.SectionCount() == 0u);
}
