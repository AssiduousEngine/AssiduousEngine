// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - the `:sequence` partition: the track kind a sequence plays skeletal clips
/// with.
///
/// ClipSequenceTrack: sections of skeletal clips on the bound entity, each blended in and out
/// over the entity's own pose through its PoseOverlay. The sequence drives the overlay (its clip
/// time and weight each step), so a seek or a scrub lands on the frame; the animation graph or
/// clip underneath keeps running and shows again as a section blends out.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.animation:sequence;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.animation;
import foundation.sequence;

using namespace foundation::core;

export namespace engine::animation
{
    namespace sequence = foundation::sequence;

    /// After properties (an actor's transform keys), before cameras and events.
    inline constexpr i32 kClipTrackOrder = 100;

    /// One clip on a clip track's timeline.
    struct ClipSection
    {
        foundation::resource::Ref<foundation::animation::AnimationClip> clip;
        f32 start = 0.0f;    // on the sequence's timeline
        f32 length = 0.0f;   // 0: the clip once from its offset, at its speed
        f32 offset = 0.0f;   // into the clip, where the section starts it
        f32 speed = 1.0f;
        f32 blendIn = 0.2f;  // seconds over the pose underneath
        f32 blendOut = 0.2f; // seconds back to it before the section ends (0: holds to the end)
        bool loop = false;   // repeat the clip over the section; otherwise it stays on its last frame

        /// The section's length on the timeline: `length`, or the clip once from its offset.
        [[nodiscard]] f32 Length() const noexcept;
    };

    /// Plays skeletal clips on the bound entity over its own pose, a section at a time (of two
    /// overlapping, the later one).
    class ClipSequenceTrack final : public sequence::SequenceTrack
    {
        RTTI_OBJECT(ClipSequenceTrack, sequence::SequenceTrack)
    public:
        Array<ClipSection> sections; // by start

        [[nodiscard]] f32 Duration() const noexcept override;
        [[nodiscard]] i32 Order() const noexcept override { return kClipTrackOrder; }
        [[nodiscard]] UniquePtr<sequence::SequenceTrackRunner> Begin(const sequence::SequenceTrackContext& context) const override;
        void ResolveResources(foundation::resource::ResourceManager& manager) override;
        void Serialize(ISerializer& ar) override;
        /// The section running at `time` (its end included); null between sections.
        [[nodiscard]] const ClipSection* SectionAt(f32 time) const noexcept;
    };

    /// Registers the animation's track kinds. Idempotent.
    void RegisterAnimationSequenceTracks();
}
