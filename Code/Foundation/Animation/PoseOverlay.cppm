// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - the `:overlay` partition.
///
/// PoseOverlay: what plays over a player's own pose, whatever made it (a single clip, a state
/// machine's layers). Two things, both blended so nothing snaps:
///  - a one-shot: a clip played once (or a set number of times), whatever its own looping, that
///    blends in over the pose, plays, and blends out back to it (a wave, a flinch, a hop), then
///    says it ended. The pose underneath keeps moving all the while, so the character returns to
///    whatever it is doing by then, with no clip to return to.
///  - a fade from a held pose: the pose as it was shown, held and faded into the pose underneath
///    (a crossfade when a player's clip changes: the old clip's last pose eases into the new clip).
/// A player owns one and applies it after its own pose and before its pose modifiers, so inverse
/// kinematics still holds a foot to the ground during a one-shot. A one-shot moves no root and
/// fires no clip events: it is a look laid over the character, not its locomotion.

module;
#include "Core/Prelude.h"

export module foundation.animation:overlay;

import foundation.core;
import :skeleton; // BoneTransform, Skeleton
import :clip;     // AnimationClip
import :sampler;  // SampleClip

using namespace foundation::core;

export namespace foundation::animation
{
    /// How a one-shot plays: its speed, how long it takes to blend in over the pose and back out,
    /// and how many times it plays after the first (0: once; -1: until stopped).
    struct OneShotSettings
    {
        f32 speed = 1.0f;
        f32 blendIn = 0.2f;
        f32 blendOut = 0.2f;
        i32 loops = 0;
    };

    class PoseOverlay
    {
    public:
        /// Plays `clip` over the pose, borrowed (it must outlive the play). A one-shot already
        /// playing gives way at once: the pose it showed is held and faded from over this one's
        /// blend-in, so the change does not snap.
        void PlayOneShot(const AnimationClip& clip, const OneShotSettings& settings)
        {
            if (m_clip != nullptr && m_weight > 0.0f && !m_shown.IsEmpty())
            {
                FadeFrom(Span<const BoneTransform>{m_shown.Data(), m_shown.Size()}, settings.blendIn);
                m_weight = 1.0f; // the held pose carries the change; this one is in at once
            }
            else
            {
                m_weight = settings.blendIn > 0.0f ? 0.0f : 1.0f;
            }
            m_clip = &clip;
            m_settings = settings;
            m_settings.speed = settings.speed > 0.0f ? settings.speed : 1.0f;
            m_time = 0.0f;
            m_stopping = false;
        }

        /// Blends the one-shot out over `blendOut` seconds from where it is now (0: at once). It
        /// ends, and Update says so, once it is out.
        void StopOneShot(f32 blendOut)
        {
            if (m_clip == nullptr)
            {
                return;
            }
            if (blendOut <= 0.0f || m_weight <= 0.0f)
            {
                m_weight = 0.0f;
                m_endedNow = true;
                m_clip = nullptr;
                return;
            }
            m_stopping = true;
            m_stopFrom = m_weight;
            m_stopLeft = blendOut;
            m_stopSeconds = blendOut;
        }

        /// Holds `pose` and fades from it into the pose underneath over `seconds` (0: no fade).
        void FadeFrom(Span<const BoneTransform> pose, f32 seconds)
        {
            if (seconds <= 0.0f || pose.IsEmpty())
            {
                m_holdLeft = 0.0f;
                return;
            }
            m_held.Resize(pose.Size());
            for (usize i = 0; i < pose.Size(); ++i)
            {
                m_held[i] = pose[i];
            }
            m_holdLeft = seconds;
            m_holdSeconds = seconds;
        }

        /// From the play until it has blended out.
        [[nodiscard]] bool IsPlayingOneShot() const noexcept { return m_clip != nullptr; }
        [[nodiscard]] const AnimationClip* OneShotClip() const noexcept { return m_clip; }
        /// How much of the one-shot shows over the pose now (0 to 1).
        [[nodiscard]] f32 OneShotWeight() const noexcept { return m_clip != nullptr ? m_weight : 0.0f; }
        /// Whether its player must update it: a one-shot playing, a fade under way, or a stop at
        /// once whose end Update has still to report.
        [[nodiscard]] bool Active() const noexcept { return m_clip != nullptr || m_holdLeft > 0.0f || m_endedNow; }

        /// Advances by `deltaTime`. True when a one-shot ended in this step (it played its last
        /// loop and blended out, or a stop finished blending it out).
        bool Update(f32 deltaTime)
        {
            bool ended = m_endedNow;
            m_endedNow = false;
            if (m_holdLeft > 0.0f)
            {
                m_holdLeft = Max(0.0f, m_holdLeft - deltaTime);
            }
            if (m_clip == nullptr)
            {
                return ended;
            }
            m_time += deltaTime * m_settings.speed;
            if (m_stopping)
            {
                m_stopLeft = Max(0.0f, m_stopLeft - deltaTime);
                m_weight = m_stopFrom * (m_stopLeft / m_stopSeconds);
                if (m_stopLeft <= 0.0f)
                {
                    m_clip = nullptr;
                    return true;
                }
                return ended;
            }
            const f32 total = TotalSeconds();
            if (total >= 0.0f && m_time >= total)
            {
                m_weight = 0.0f;
                m_clip = nullptr;
                return true;
            }
            // In over blendIn from the start; out over blendOut before the end (in real seconds,
            // so a fast one-shot blends as long as a slow one).
            const f32 elapsed = m_time / m_settings.speed;
            f32 weight = m_settings.blendIn > 0.0f ? Min(1.0f, elapsed / m_settings.blendIn) : 1.0f;
            if (total >= 0.0f && m_settings.blendOut > 0.0f)
            {
                const f32 left = (total - m_time) / m_settings.speed;
                weight = Min(weight, left / m_settings.blendOut);
            }
            m_weight = Clamp(weight, 0.0f, 1.0f);
            return ended;
        }

        /// Lays the overlay over `pose` (the player's own, in; what to show, out): the held pose
        /// fading into it, then the one-shot blended over that by its weight.
        void Apply(const Skeleton& skeleton, Span<BoneTransform> pose)
        {
            if (m_holdLeft > 0.0f && m_holdSeconds > 0.0f)
            {
                const f32 held = m_holdLeft / m_holdSeconds; // 1 just held, 0 faded out
                const usize n = Min(pose.Size(), m_held.Size());
                for (usize i = 0; i < n; ++i)
                {
                    pose[i] = BoneTransform::Lerp(pose[i], m_held[i], held);
                }
            }
            if (m_clip != nullptr && m_weight > 0.0f)
            {
                // The whole body (a bone the clip does not move takes its bind pose, as any clip's
                // sample does): a one-shot is a full-body look, not a layer of some bones.
                m_sampled.Resize(pose.Size());
                SampleClip(*m_clip, skeleton, ClipTime(), Span<BoneTransform>{m_sampled.Data(), m_sampled.Size()});
                for (usize i = 0; i < pose.Size(); ++i)
                {
                    pose[i] = BoneTransform::Lerp(pose[i], m_sampled[i], m_weight);
                }
            }
            // What was shown, for a one-shot that takes over from this one.
            if (m_clip != nullptr)
            {
                m_shown.Resize(pose.Size());
                for (usize i = 0; i < pose.Size(); ++i)
                {
                    m_shown[i] = pose[i];
                }
            }
        }

    private:
        // All its plays, in clip seconds; -1 while it loops until stopped.
        [[nodiscard]] f32 TotalSeconds() const noexcept
        {
            if (m_clip == nullptr)
            {
                return 0.0f;
            }
            if (m_settings.loops < 0)
            {
                return -1.0f;
            }
            return m_clip->duration * static_cast<f32>(m_settings.loops + 1);
        }

        // Where in the clip it is: each play from its start, the last held at its end.
        [[nodiscard]] f32 ClipTime() const noexcept
        {
            const f32 duration = m_clip->duration;
            if (duration <= 0.0f)
            {
                return 0.0f;
            }
            const f32 total = TotalSeconds();
            if (total >= 0.0f && m_time >= total)
            {
                return duration;
            }
            f32 t = m_time;
            while (t >= duration)
            {
                t -= duration;
            }
            return t;
        }

        const AnimationClip* m_clip = nullptr; // borrowed; null when none plays
        OneShotSettings m_settings;
        f32 m_time = 0.0f;   // into the one-shot, in clip seconds, over all its plays
        f32 m_weight = 0.0f; // how much of it shows
        bool m_stopping = false;
        f32 m_stopFrom = 0.0f;
        f32 m_stopLeft = 0.0f;
        f32 m_stopSeconds = 0.0f;
        bool m_endedNow = false; // stopped at once: Update reports it
        Array<BoneTransform> m_held; // the pose a fade starts from
        f32 m_holdLeft = 0.0f;
        f32 m_holdSeconds = 0.0f;
        Array<BoneTransform> m_sampled; // scratch: the one-shot's pose
        Array<BoneTransform> m_shown;   // what was shown last, while a one-shot plays
    };
} // namespace foundation::animation
