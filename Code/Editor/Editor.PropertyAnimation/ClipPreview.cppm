// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_preview` partition.
//
// What the panel does with the scene, none of which becomes a step in the scene's history or
// touches its unsaved mark:
//   - track seeds: an entity's animatable properties (its transform's position, rotation and
//     scale, and each reflected component's float, Float3, Color and Quaternion leaves that the
//     runtime can bind), which Add Track offers;
//   - the preview: the bound entity posed at a time, through the same binding the runtime's
//     animator writes with. The values it overwrites are saved first and restored when the preview
//     stops, and saved again when the tracks or the entity change;
//   - the transport's clock: playing, paused or stopped, looping or not, over the clip's length.

module;
#include "Core/Prelude.h"

export module editor.propertyanimation:clip_preview;

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;

using namespace foundation::core;

export namespace editor
{
    /// One animatable property of a target (a component, a scene's settings), from which a track is
    /// made: the track's source, target and path.
    struct AnimatablePropertyInfo
    {
        String source;
        String target;
        String propertyPath;
        foundation::propertyanimation::TrackValueKind kind = foundation::propertyanimation::TrackValueKind::Float;
    };

    /// A track kind for a reflected leaf type (f32, Float3, Color, Quaternion); empty for any other.
    [[nodiscard]] Optional<foundation::propertyanimation::TrackValueKind> InferTrackKind(const TypeInfo* leafType);
    /// The animatable leaves of `type` (nested structs by their dotted paths), each of which the
    /// runtime can resolve, as properties of `target` under the source `source`.
    void CollectAnimatableProperties(StringView source, StringView target, const TypeInfo& type,
                                     Array<AnimatablePropertyInfo>& out);

    class ClipPreview
    {
    public:
        explicit ClipPreview(foundation::scene::Scene& scene) : m_scene(&scene) {}
        ~ClipPreview() { Stop(); }
        ClipPreview(const ClipPreview&) = delete;
        ClipPreview& operator=(const ClipPreview&) = delete;

        /// What Add Track offers on `entity`: its transform's properties, then each component's, then
        /// what any other source that needs an entity offers on it.
        [[nodiscard]] Array<AnimatablePropertyInfo> TrackSeeds(const Guid& entity) const;
        /// What Add Track offers on the scene itself, from every source that needs no entity (the
        /// scene's settings).
        [[nodiscard]] Array<AnimatablePropertyInfo> SceneSeeds() const;
        /// A track target's value as the scene shows it (on `entity`, for a source that needs one;
        /// empty when there is no such entity, target or property).
        [[nodiscard]] Variant Read(const Guid& entity, StringView source, StringView target, StringView propertyPath) const;

        /// Poses `entity` with `clip` at `time`, first saving the values it overwrites so they can
        /// be restored.
        void Pose(const Guid& entity, const foundation::propertyanimation::PropertyAnimationClip& clip, f32 time);
        /// Restores what the preview wrote and ends the preview.
        void Stop();
        [[nodiscard]] bool IsPosing() const noexcept { return m_posing; }
        [[nodiscard]] const Guid& PosedEntity() const noexcept { return m_entity; }

    private:
        struct Kept
        {
            foundation::propertyanimation::TrackTarget target; // bound once: what it found lives as long as the scene
            Variant value;
            UniquePtr<foundation::propertyanimation::PropertyTargetWrites> writes; // told once the value is back
        };

        foundation::scene::Scene* m_scene;
        bool m_posing = false;
        Guid m_entity;
        Array<Kept> m_kept;      // each target as it was before the preview
        Array<String> m_tracks;  // the tracks the saved values belong to ("source|target|path" each)
    };

    /// The transport's clock over a clip's length.
    class ClipTransport
    {
    public:
        /// Plays from the current time (from the start when the time is at the end).
        void Play(f32 length)
        {
            if (m_time >= length - 1e-4f)
                m_time = 0.0f;
            m_playing = true;
            m_paused = false;
        }
        void Pause()
        {
            if (m_playing)
                m_paused = true;
        }
        /// Stops and returns to the start.
        void Stop()
        {
            m_playing = false;
            m_paused = false;
            m_time = 0.0f;
        }
        /// A scrub: sets the time shown, whether playing or not.
        void SetTime(f32 time) { m_time = Max(0.0f, time); }
        void SetLooping(bool looping) noexcept { m_looping = looping; }
        /// Moves on by `dt` while playing; true when the time moved. At the end it wraps when
        /// looping, else stops there.
        bool Advance(f32 dt, f32 length)
        {
            if (!m_playing || m_paused || dt <= 0.0f)
                return false;
            length = Max(length, 1e-3f);
            m_time += dt;
            if (m_time >= length)
            {
                if (m_looping)
                {
                    while (m_time >= length)
                        m_time -= length;
                }
                else
                {
                    m_time = length;
                    m_playing = false;
                }
            }
            return true;
        }
        [[nodiscard]] f32 Time() const noexcept { return m_time; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_playing && !m_paused; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_playing && m_paused; }
        [[nodiscard]] bool IsLooping() const noexcept { return m_looping; }

    private:
        f32 m_time = 0.0f;
        bool m_playing = false;
        bool m_paused = false;
        bool m_looping = true; // loops by default, as a new animator does
    };
}
