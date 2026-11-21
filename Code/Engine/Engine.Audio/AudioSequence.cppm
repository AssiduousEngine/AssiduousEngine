// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Audio - the `:sequence` partition: the track kind a sequence plays sounds with.
///
/// SoundSequenceTrack: sections of audio clips, each played as a voice in the scene's group (at
/// the bound entity, spatial, when the track is bound; flat when not). A section entered part way
/// starts its voice at that point, a voice that drifts from the playhead is moved back to it, and
/// a jump moves it at once; the voice pauses with the play, and plays on at a hold (a line of
/// dialogue finishes while the game waits).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.audio:sequence;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.audio;
import foundation.sequence;

using namespace foundation::core;

export namespace engine::audio
{
    namespace sequence = foundation::sequence;

    /// With properties and animation, before cameras and events.
    inline constexpr i32 kSoundTrackOrder = 200;
    /// How far a voice may wander from the playhead before it is moved back (a streamed clip
    /// seeks only to a block, so it is wider than a frame).
    inline constexpr f32 kSoundDriftSeconds = 0.1f;

    /// One sound on a sound track's timeline.
    struct SoundSection
    {
        foundation::resource::Ref<foundation::audio::AudioClip> clip;
        f32 start = 0.0f;  // on the sequence's timeline
        f32 length = 0.0f; // 0: the clip once from its offset
        f32 offset = 0.0f; // into the clip, where the section starts it
        f32 volume = 1.0f;
        foundation::audio::AudioBus bus = foundation::audio::AudioBus::Effects;
        bool loop = false; // repeat the clip over the section

        /// The section's length on the timeline: `length`, or the clip once from its offset.
        [[nodiscard]] f32 Length() const noexcept;
    };

    /// Plays sounds over the sequence, a section at a time (of two overlapping, the later one); at
    /// the bound entity when bound.
    class SoundSequenceTrack final : public sequence::SequenceTrack
    {
        RTTI_OBJECT(SoundSequenceTrack, sequence::SequenceTrack)
    public:
        Array<SoundSection> sections; // by start

        [[nodiscard]] f32 Duration() const noexcept override;
        [[nodiscard]] i32 Order() const noexcept override { return kSoundTrackOrder; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<sequence::SequenceTrackRunner> Begin(const sequence::SequenceTrackContext& context) const override;
        void ResolveResources(foundation::resource::ResourceManager& manager) override;
        void Serialize(ISerializer& ar) override;
        /// The section running at `time` (its end included); null between sections.
        [[nodiscard]] const SoundSection* SectionAt(f32 time) const noexcept;
    };

    /// Registers the audio's track kinds. Idempotent.
    void RegisterAudioSequenceTracks();
}
