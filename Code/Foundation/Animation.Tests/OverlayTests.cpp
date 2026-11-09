// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PoseOverlay on the players: a one-shot blends in over the clip's pose, plays once whatever its
// own looping, blends out and says it ended; it plays its loops, or until stopped; a stop blends it
// out; a crossfade eases from the old clip's pose into the new; a graph's pose takes one too.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;

using namespace foundation::core;
using namespace foundation::animation;

namespace
{
    void OneBone(Skeleton& s)
    {
        s.Bones()[0].index = 0;
        s.Bones()[0].parentIndex = -1;
        s.FindRootBones();
        s.BuildChildIndices();
        s.ComputeInverseBindPoses();
    }

    // A clip that holds the bone at height `y` for `seconds`.
    void Holding(AnimationClip& clip, f32 y)
    {
        clip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, y, 0});
        clip.GetOrCreatePositionTrack(0)->AddKeyframe(clip.duration, Float3{0, y, 0});
    }

    // The bone's height as the player last showed it.
    f32 Height(AnimationPlayer& player)
    {
        (void)player.GetSkinningMatrices(); // evaluates
        return player.GetFinalPoses()[0].position.y;
    }

    // Runs the player `seconds` in 1/100 s steps; how many one-shots ended meanwhile.
    i32 Run(AnimationPlayer& player, f32 seconds)
    {
        i32 ended = 0;
        for (f32 t = 0.0f; t < seconds - 1.0e-4f; t += 0.01f)
        {
            player.Update(0.01f);
            ended += player.ConsumeOneShotEnded() ? 1 : 0;
        }
        return ended;
    }
}

TEST_CASE("overlay: a one-shot blends in over the clip, plays once though it loops, blends out and ends")
{
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationClip stand{u8"Stand", 1.0f, true};
    Holding(stand, 0.0f);
    AnimationClip hop{u8"Hop", 1.0f, true}; // a looping clip: the one-shot still plays it once
    Holding(hop, 1.0f);
    AnimationPlayer player{skeleton};
    player.Play(&stand);
    player.Overlay().PlayOneShot(hop, OneShotSettings{1.0f, 0.2f, 0.2f, 0});
    CHECK(player.Overlay().IsPlayingOneShot());

    CHECK(Run(player, 0.1f) == 0);
    CHECK(Height(player) == doctest::Approx(0.5f).epsilon(0.06)); // halfway in
    CHECK(Run(player, 0.4f) == 0);
    CHECK(Height(player) == doctest::Approx(1.0f)); // in full
    CHECK(Run(player, 0.4f) == 0);
    CHECK(Height(player) < 0.6f); // on its way out
    CHECK(Run(player, 0.2f) == 1); // ended once
    CHECK_FALSE(player.Overlay().IsPlayingOneShot());
    CHECK(Height(player) == doctest::Approx(0.0f)); // the clip's own pose again
    CHECK(player.State() == PlaybackState::Playing); // the clip played on underneath
    CHECK(Run(player, 1.0f) == 0);
}

TEST_CASE("overlay: a one-shot plays its loops, or until stopped, and a stop blends it out")
{
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationClip stand{u8"Stand", 1.0f, true};
    Holding(stand, 0.0f);
    AnimationClip wave{u8"Wave", 0.5f, false};
    Holding(wave, 1.0f);
    AnimationPlayer player{skeleton};
    player.Play(&stand);

    // Once more after the first: two plays, a second in all.
    player.Overlay().PlayOneShot(wave, OneShotSettings{1.0f, 0.0f, 0.0f, 1});
    CHECK(Run(player, 0.9f) == 0);
    CHECK(Height(player) == doctest::Approx(1.0f));
    CHECK(Run(player, 0.2f) == 1);

    // Until stopped: still playing long after its length; a stop takes it out over 0.2 s.
    player.Overlay().PlayOneShot(wave, OneShotSettings{1.0f, 0.0f, 0.0f, -1});
    CHECK(Run(player, 3.0f) == 0);
    CHECK(player.Overlay().IsPlayingOneShot());
    player.Overlay().StopOneShot(0.2f);
    CHECK(Run(player, 0.1f) == 0);
    CHECK(Height(player) == doctest::Approx(0.5f).epsilon(0.06));
    CHECK(Run(player, 0.15f) == 1);
    CHECK(Height(player) == doctest::Approx(0.0f));

    // A stop with no blend ends it at once.
    player.Overlay().PlayOneShot(wave, OneShotSettings{1.0f, 0.0f, 0.0f, -1});
    CHECK(Run(player, 0.1f) == 0);
    player.Overlay().StopOneShot(0.0f);
    CHECK(Run(player, 0.01f) == 1);
}

TEST_CASE("overlay: a crossfade eases from the old clip's pose into the new one")
{
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationClip high{u8"High", 1.0f, true};
    Holding(high, 1.0f);
    AnimationClip low{u8"Low", 1.0f, true};
    Holding(low, 0.0f);
    AnimationPlayer player{skeleton};
    player.Play(&high);
    (void)Run(player, 0.1f);
    CHECK(Height(player) == doctest::Approx(1.0f));

    player.CrossFadeTo(&low, 0.5f);
    CHECK(Run(player, 0.25f) == 0); // a fade is no one-shot: nothing ends
    CHECK(Height(player) == doctest::Approx(0.5f).epsilon(0.06));
    (void)Run(player, 0.3f);
    CHECK(Height(player) == doctest::Approx(0.0f));
    CHECK(player.CurrentClip() == &low);

    // No fade time: a plain switch.
    player.CrossFadeTo(&high, 0.0f);
    (void)Run(player, 0.01f);
    CHECK(Height(player) == doctest::Approx(1.0f));
}

TEST_CASE("overlay: a graph's pose takes a one-shot the same way")
{
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationClip hop{u8"Hop", 1.0f, false};
    Holding(hop, 1.0f);
    AnimationGraph graph; // no layers: its pose is the bind pose
    AnimationGraphPlayer player{graph, skeleton};
    player.Overlay().PlayOneShot(hop, OneShotSettings{1.0f, 0.0f, 0.0f, 0});
    player.Update(0.5f);
    CHECK(player.GetLocalPoses()[0].position.y == doctest::Approx(1.0f));
    CHECK_FALSE(player.ConsumeOneShotEnded());
    player.Update(0.6f);
    CHECK(player.ConsumeOneShotEnded());
    CHECK(player.GetLocalPoses()[0].position.y == doctest::Approx(0.0f));
}
