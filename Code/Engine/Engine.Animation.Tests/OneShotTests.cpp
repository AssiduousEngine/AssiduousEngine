// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// One-shots and crossfades on the animated entities: a one-shot asked for on an animator starts
// over its clip, plays and ends, and its end reaches the animation subsystem's listeners with the
// entity and the clip; SceneAnimation says it plays and stops it; a clip changed with a fade time
// eases in; a graph's entity takes a one-shot too. (The script side, onAnimationFinished, is the
// bridge's: Engine.Integration.)

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.script.facades;
import engine.animation;
import engine.render;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;

namespace
{
    // A clip that holds the one bone at height `y`.
    RefPtr<animation::AnimationClip> Holding(StringView name, f32 y, f32 seconds = 1.0f, bool looping = true)
    {
        RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), name, seconds, looping);
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, y, 0});
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(seconds, Float3{0, y, 0});
        return clip;
    }

    struct Listener final : engine::animation::IAnimationListener
    {
        Array<engine::animation::AnimationOneShotEnded> ended;
        void OnOneShotEnded(const engine::animation::AnimationOneShotEnded& e) override { ended.PushBack(e); }
    };

    struct Stage
    {
        scene::Scene level{DefaultAllocator(), u8"oneshots"};
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
        scene::EntityHandle hero;

        Stage()
        {
            skeleton->Bones()[0].index = 0;
            skeleton->Bones()[0].parentIndex = -1;
            skeleton->FindRootBones();
            skeleton->BuildChildIndices();
            skeleton->ComputeInverseBindPoses();
            level.AddSystem<engine::render::MeshComponentManager>();
            engine::animation::AddAnimationSceneManagers(level);
            hero = level.CreateEntity(u8"Hero");
        }
        engine::animation::SkeletalAnimationComponent& Animator(RefPtr<animation::AnimationClip> clip)
        {
            auto& a = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(hero);
            a.skeleton.SetDirect(skeleton);
            a.clip.SetDirect(clip);
            return a;
        }
        void Run(f32 seconds)
        {
            for (f32 t = 0.0f; t < seconds - 1.0e-4f; t += 0.01f)
            {
                level.Update(0.01f);
            }
        }
        foundation::script::Entity Entity() const { return foundation::script::Entity{const_cast<scene::Scene*>(&level), hero.index, hero.generation}; }
    };

    f32 Height(engine::animation::SkeletalAnimationComponent& a)
    {
        (void)a.player->GetSkinningMatrices();
        return a.player->GetFinalPoses()[0].position.y;
    }
}

TEST_CASE("animation: a one-shot asked of an animator plays over its clip, ends, and its listeners hear it")
{
    Stage stage;
    auto& a = stage.Animator(Holding(u8"Stand", 0.0f));
    stage.Run(0.05f); // the player is built and the clip plays

    const Guid hopId{0x51, 0x7};
    a.oneShot.clip.SetDirect(Holding(u8"Hop", 1.0f, 0.5f));
    a.oneShot.clip.SetId(hopId);
    a.oneShot.settings = animation::OneShotSettings{1.0f, 0.1f, 0.1f, 0};
    a.oneShot.pending = true; // what SceneAnimation.playOneShot asks for
    const engine::animation::SceneAnimation animations{&stage.level};
    CHECK(animations.isPlayingOneShot(stage.Entity())); // asked for counts as playing

    engine::animation::AnimationSubsystem subsystem;
    Listener listener;
    subsystem.RegisterListener(&listener);
    subsystem.OnSystemsReady(stage.level);

    stage.Run(0.25f);
    CHECK(Height(a) == doctest::Approx(1.0f)); // in full over the clip
    CHECK(animations.isPlayingOneShot(stage.Entity()));
    subsystem.Update(0.0f);
    CHECK(listener.ended.IsEmpty());

    stage.Run(0.35f);
    CHECK_FALSE(animations.isPlayingOneShot(stage.Entity()));
    CHECK(Height(a) == doctest::Approx(0.0f)); // the clip's own pose again
    subsystem.Update(0.0f);
    REQUIRE(listener.ended.Size() == 1);
    CHECK(listener.ended[0].scene == &stage.level);
    CHECK(listener.ended[0].entity == stage.hero);
    CHECK(listener.ended[0].clip == hopId);
    subsystem.Update(0.0f);
    CHECK(listener.ended.Size() == 1); // told once

    // Stopped, it blends out and is reported too.
    a.oneShot.settings = animation::OneShotSettings{1.0f, 0.0f, 0.0f, -1};
    a.oneShot.pending = true;
    stage.Run(0.1f);
    CHECK(animations.isPlayingOneShot(stage.Entity()));
    animations.stopOneShot(stage.Entity(), 0.05f);
    stage.Run(0.1f);
    CHECK_FALSE(animations.isPlayingOneShot(stage.Entity()));
    subsystem.Update(0.0f);
    CHECK(listener.ended.Size() == 2);
    subsystem.UnregisterListener(&listener);
    subsystem.OnDestroying(stage.level);
}

TEST_CASE("animation: a clip changed with a fade time eases into the new clip")
{
    Stage stage;
    auto& a = stage.Animator(Holding(u8"High", 1.0f));
    stage.Run(0.1f);
    CHECK(Height(a) == doctest::Approx(1.0f));

    a.clipFade = 0.5f; // what SceneAnimation.setClip(entity, clip, 0.5) asks for
    a.clip.SetDirect(Holding(u8"Low", 0.0f));
    stage.Run(0.25f);
    CHECK(Height(a) == doctest::Approx(0.5f).epsilon(0.08));
    stage.Run(0.35f);
    CHECK(Height(a) == doctest::Approx(0.0f));
    CHECK(a.clipFade == 0.0f); // spent on that change

    a.clip.SetDirect(Holding(u8"High again", 1.0f)); // no fade asked: at once
    stage.Run(0.02f);
    CHECK(Height(a) == doctest::Approx(1.0f));
}

TEST_CASE("animation: a graph's entity takes a one-shot over its graph")
{
    Stage stage;
    RefPtr<animation::AnimationGraph> graph = MakeRef<animation::AnimationGraph>(DefaultAllocator());
    auto& g = stage.level.GetSystem<engine::animation::AnimationGraphComponentManager>()->Add(stage.hero);
    g.skeleton.SetDirect(stage.skeleton);
    g.graph.SetDirect(graph);
    stage.Run(0.02f);
    REQUIRE(g.player.Get() != nullptr);

    g.oneShot.clip.SetDirect(Holding(u8"Wave", 1.0f, 0.3f, false));
    g.oneShot.settings = animation::OneShotSettings{1.0f, 0.0f, 0.0f, 0};
    g.oneShot.pending = true;
    const engine::animation::SceneAnimation animations{&stage.level};
    stage.Run(0.1f);
    CHECK(animations.isPlayingOneShot(stage.Entity()));
    CHECK(g.player->GetLocalPoses()[0].position.y == doctest::Approx(1.0f));
    stage.Run(0.3f);
    CHECK_FALSE(animations.isPlayingOneShot(stage.Entity()));
    CHECK(g.player->GetLocalPoses()[0].position.y == doctest::Approx(0.0f));
    Array<engine::animation::OneShotEnded> ended;
    stage.level.GetSystem<engine::animation::AnimationGraphComponentManager>()->DrainEnded(ended);
    CHECK(ended.Size() == 1);
}
