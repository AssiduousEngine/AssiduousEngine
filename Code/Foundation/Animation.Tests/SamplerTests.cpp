// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AnimationSampler: track and clip sampling, and pose blending.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;

using namespace foundation::core;
using namespace foundation::animation;

TEST_CASE("sampler: Float3 track linear interpolation")
{
    AnimationTrack<Float3> track;
    track.AddKeyframe(0.0f, Float3{0, 0, 0});
    track.AddKeyframe(1.0f, Float3{10, 0, 0});
    CHECK(NearlyEqual(SampleVec3(&track, 0.0f), Float3{0, 0, 0}));
    CHECK(NearlyEqual(SampleVec3(&track, 0.5f), Float3{5, 0, 0}));
    CHECK(NearlyEqual(SampleVec3(&track, 1.0f), Float3{10, 0, 0}));
    CHECK(NearlyEqual(SampleVec3(&track, 2.0f), Float3{10, 0, 0})); // clamps past last
    // empty / null returns the default
    CHECK(NearlyEqual(SampleVec3(nullptr, 0.5f, Float3{9, 9, 9}), Float3{9, 9, 9}));
}

TEST_CASE("sampler: Step interpolation holds previous value")
{
    AnimationTrack<Float3> track;
    track.interpolation = InterpolationMode::Step;
    track.AddKeyframe(0.0f, Float3{0, 0, 0});
    track.AddKeyframe(1.0f, Float3{10, 0, 0});
    CHECK(NearlyEqual(SampleVec3(&track, 0.5f), Float3{0, 0, 0})); // holds prev until next key
}

TEST_CASE("sampler: SampleClip animates targeted bones, bind pose otherwise")
{
    Skeleton skel{2};
    skel.Bones()[0].localBindPose.position = Float3{0, 0, 0};
    skel.Bones()[1].localBindPose.position = Float3{7, 7, 7};

    AnimationClip clip{u8"move", 1.0f};
    AnimationClip::Vec3Track* pos = clip.GetOrCreatePositionTrack(0);
    pos->AddKeyframe(0.0f, Float3{0, 0, 0});
    pos->AddKeyframe(1.0f, Float3{0, 10, 0});

    BoneTransform poses[2] = {};
    SampleClip(clip, skel, 0.5f, Span<BoneTransform>{poses, 2});
    CHECK(NearlyEqual(poses[0].position, Float3{0, 5, 0})); // animated
    CHECK(NearlyEqual(poses[1].position, Float3{7, 7, 7})); // untouched: bind pose
}

TEST_CASE("sampler: BlendPoses + AdditivePoses")
{
    BoneTransform a[1] = {};
    a[0].position = Float3{0, 0, 0};
    a[0].scale = Float3{1, 1, 1};
    BoneTransform b[1] = {};
    b[0].position = Float3{10, 0, 0};
    b[0].scale = Float3{3, 3, 3};
    BoneTransform out[1] = {};

    BlendPoses(Span<const BoneTransform>{a, 1}, Span<const BoneTransform>{b, 1}, 0.5f,
               Span<BoneTransform>{out, 1});
    CHECK(NearlyEqual(out[0].position, Float3{5, 0, 0}));
    CHECK(NearlyEqual(out[0].scale, Float3{2, 2, 2}));

    BoneTransform base[1] = {};
    base[0].position = Float3{1, 0, 0};
    base[0].scale = Float3{1, 1, 1};
    BoneTransform add[1] = {};
    add[0].position = Float3{0, 5, 0};
    add[0].scale = Float3{1, 1, 1};
    AdditivePoses(Span<const BoneTransform>{base, 1}, Span<const BoneTransform>{add, 1}, 1.0f,
                  Span<BoneTransform>{out, 1});
    CHECK(NearlyEqual(out[0].position, Float3{1, 5, 0})); // base + additive*weight
}

TEST_CASE("sampler: a cubic track follows its tangents, a rotation's included")
{
    // A rotation's cubic ignored its tangents, easing in and out of every key whatever they said.
    // Two keys of equal value with an out-tangent of one unit a second: the curve leaves the
    // first key at that slope.
    AnimationTrack<Float3> move;
    move.interpolation = InterpolationMode::CubicSpline;
    move.AddKeyframe(0.0f, Float3{0, 0, 0}, Float3{0, 0, 0}, Float3{1, 0, 0});
    move.AddKeyframe(1.0f, Float3{0, 0, 0}, Float3{0, 0, 0}, Float3{0, 0, 0});
    CHECK(SampleVec3(&move, 0.01f).x == doctest::Approx(0.01f).epsilon(0.05));

    AnimationTrack<Quaternion> turn;
    turn.interpolation = InterpolationMode::CubicSpline;
    turn.AddKeyframe(0.0f, Quaternion::Identity, Quaternion{0, 0, 0, 0}, Quaternion{0, 1, 0, 0});
    turn.AddKeyframe(1.0f, Quaternion::Identity, Quaternion{0, 0, 0, 0}, Quaternion{0, 0, 0, 0});
    const Quaternion early = SampleQuat(&turn, 0.1f);
    CHECK(early.y > 0.05f); // turning about y, as the tangent says (it stayed at identity)
    CHECK(early.y * early.y + early.w * early.w == doctest::Approx(1.0f)); // normalized
}
