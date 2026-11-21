// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A sequence's clip track: its sections play skeletal clips over the entity's own pose through
// the overlay, at the frame the playhead names (a seek lands on it), blended in and out, looped
// over a section, and let go when the play ends.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.sequence;
import engine.animation;
import engine.render;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;
namespace sequence = foundation::sequence;
using engine::animation::ClipSection;
using engine::animation::ClipSequenceTrack;

namespace
{
    // The bone climbs from 0 to 2 over the clip's second.
    RefPtr<animation::AnimationClip> Rise()
    {
        RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Rise", 1.0f, false);
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 2, 0});
        return clip;
    }

    struct Stage
    {
        scene::Scene level{DefaultAllocator(), u8"clips"};
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
        RefPtr<animation::AnimationClip> stand = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Stand", 1.0f, true);
        scene::EntityHandle hero;
        engine::animation::SkeletalAnimationComponent* animator = nullptr;

        Stage()
        {
            engine::animation::RegisterAnimationComponentReflection();
            skeleton->Bones()[0].index = 0;
            skeleton->Bones()[0].parentIndex = -1;
            skeleton->FindRootBones();
            skeleton->BuildChildIndices();
            skeleton->ComputeInverseBindPoses();
            stand->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
            stand->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 0, 0});
            level.AddSystem<engine::render::MeshComponentManager>();
            engine::animation::AddAnimationSceneManagers(level);
            hero = level.CreateEntity(u8"Hero");
            animator = &level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(hero);
            animator->skeleton.SetDirect(skeleton);
            animator->clip.SetDirect(stand);
            level.Update(0.01f); // the player is built
            REQUIRE(animator->player.Get() != nullptr);
        }

        [[nodiscard]] f32 Height()
        {
            (void)animator->player->GetSkinningMatrices();
            return animator->player->GetFinalPoses()[0].position.y;
        }
    };

    sequence::Sequence OneSection(const ClipSection& section)
    {
        RefPtr<ClipSequenceTrack> track = MakeRef<ClipSequenceTrack>(DefaultAllocator());
        track->binding = 0;
        track->sections.PushBack(section);
        sequence::Sequence sequence;
        sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"Hero")});
        sequence.tracks.PushBack(track);
        sequence.duration = track->Duration();
        return sequence;
    }

    ClipSection Section(RefPtr<animation::AnimationClip> clip, f32 start)
    {
        ClipSection section;
        section.clip.SetDirect(clip);
        section.start = start;
        section.blendIn = 0.0f;
        section.blendOut = 0.0f;
        return section;
    }
}

TEST_CASE("clip track: a section shows its clip at the playhead's frame, and the end lets go")
{
    Stage stage;
    sequence::Sequence sequence = OneSection(Section(Rise(), 1.0f));
    CHECK(sequence.duration == doctest::Approx(2.0f));
    sequence::SequencePlay play(DefaultAllocator(), stage.level, sequence);
    REQUIRE(play.Bind(u8"Hero", stage.hero));

    play.Play();
    play.Advance(0.5f); // before the section: the pose underneath
    CHECK(stage.Height() == doctest::Approx(0.0f));
    play.Seek(1.5f); // half way through the clip
    stage.level.Update(0.01f); // the player's own tick does not move a driven clip
    CHECK(stage.Height() == doctest::Approx(1.0f));
    play.Advance(0.25f);
    CHECK(stage.Height() == doctest::Approx(1.5f));
    play.Advance(1.0f); // the end: released
    CHECK(play.IsFinished());
    stage.level.Update(0.01f);
    CHECK(stage.Height() == doctest::Approx(0.0f));
}

TEST_CASE("clip track: a section blends in over the pose, and loops its clip over its length")
{
    Stage stage;
    ClipSection section = Section(Rise(), 0.0f);
    section.blendIn = 0.5f;
    section.length = 3.0f;
    section.loop = true;
    sequence::Sequence sequence = OneSection(section);
    sequence::SequencePlay play(DefaultAllocator(), stage.level, sequence);
    REQUIRE(play.Bind(u8"Hero", stage.hero));

    play.Play();
    play.Advance(0.25f); // clip at 0.25 (height 0.5), half blended in
    CHECK(stage.Height() == doctest::Approx(0.25f));
    play.Seek(2.5f); // the third time round, half way: in full
    CHECK(stage.Height() == doctest::Approx(1.0f));
}

TEST_CASE("clip track: stopping mid-section eases back to the pose underneath")
{
    Stage stage;
    ClipSection section = Section(Rise(), 0.0f);
    section.blendOut = 0.2f;
    section.length = 1.0f;
    sequence::Sequence sequence = OneSection(section);
    sequence::SequencePlay play(DefaultAllocator(), stage.level, sequence);
    REQUIRE(play.Bind(u8"Hero", stage.hero));

    play.Play();
    play.Advance(0.5f);
    CHECK(stage.Height() == doctest::Approx(1.0f));
    play.Stop();
    stage.level.Update(0.1f); // half way out
    CHECK(stage.Height() > 0.1f);
    CHECK(stage.Height() < 1.1f);
    stage.level.Update(0.2f);
    CHECK(stage.Height() == doctest::Approx(0.0f));
}

TEST_CASE("clip track: a sequence reads its clip sections back by their kind")
{
    engine::animation::RegisterAnimationComponentReflection();
    RefPtr<ClipSequenceTrack> track = MakeRef<ClipSequenceTrack>(DefaultAllocator());
    track->binding = 0;
    ClipSection section;
    section.clip.SetId(Guid{0x12, 0x34});
    section.start = 1.5f;
    section.length = 2.0f;
    section.offset = 0.25f;
    section.speed = 2.0f;
    section.blendIn = 0.1f;
    section.blendOut = 0.3f;
    section.loop = true;
    track->sections.PushBack(section);
    sequence::Sequence original;
    original.bindings.PushBack(sequence::SequenceBinding{String(u8"Hero")});
    original.tracks.PushBack(track);

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
    auto* clips = Cast<ClipSequenceTrack>(read.tracks[0].Get());
    REQUIRE(clips != nullptr);
    REQUIRE(clips->sections.Size() == 1u);
    const ClipSection& back = clips->sections[0];
    CHECK(back.clip.id == Guid{0x12, 0x34});
    CHECK(back.start == doctest::Approx(1.5f));
    CHECK(back.length == doctest::Approx(2.0f));
    CHECK(back.offset == doctest::Approx(0.25f));
    CHECK(back.speed == doctest::Approx(2.0f));
    CHECK(back.blendIn == doctest::Approx(0.1f));
    CHECK(back.blendOut == doctest::Approx(0.3f));
    CHECK(back.loop);
    CHECK(clips->Duration() == doctest::Approx(3.5f));
}
