// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Animation types: interpolation, channels, keyframes, and animation clips.

module;
#include "Core/Prelude.h"

#include <string>
#include <vector>

export module foundation.model:model_animation;

import foundation.core;

using namespace foundation::core;

export namespace foundation::model
{

    /// Animation interpolation type.
    enum class AnimationInterpolation : u32
    {
        Linear,
        Step,
        CubicSpline,
    };

    /// Animation channel target path.
    enum class AnimationPath : u32
    {
        Translation,
        Rotation,
        Scale,
        Weights, // Morph target weights.
    };

    /// Keyframe data for an animation.
    struct AnimationKeyframe
    {
        f32 time = 0.0f;
        Float4 value{}; // Translation (xyz), Rotation (xyzw), Scale (xyz), or single Weight.
        // A cubic spline's tangents (per second, as glTF stores them); zero otherwise.
        Float4 inTangent{};
        Float4 outTangent{};

        constexpr AnimationKeyframe() = default;
        constexpr AnimationKeyframe(f32 t, Float4 v) : time(t), value(v) {}
        constexpr AnimationKeyframe(f32 t, Float4 v, Float4 in, Float4 out)
            : time(t), value(v), inTangent(in), outTangent(out)
        {
        }
    };

    /// An animation channel targeting a specific bone/node property.
    class AnimationChannel
    {
    public:
        AnimationChannel() = default;
        ~AnimationChannel() = default;

        /// Add a keyframe.
        void addKeyframe(f32 time, Float4 value)
        {
            m_keyframes.PushBack(AnimationKeyframe(time, value));
        }
        /// Add a cubic spline keyframe with its tangents.
        void addKeyframe(f32 time, Float4 value, Float4 inTangent, Float4 outTangent)
        {
            m_keyframes.PushBack(AnimationKeyframe(time, value, inTangent, outTangent));
        }

        [[nodiscard]] Span<const AnimationKeyframe> keyframes() const
        {
            return Span<const AnimationKeyframe>(m_keyframes.Data(), m_keyframes.Size());
        }
        [[nodiscard]] Span<AnimationKeyframe> keyframes()
        {
            return Span<AnimationKeyframe>(m_keyframes.Data(), m_keyframes.Size());
        }

        /// Sample the animation at a given time.
        [[nodiscard]] Float4 sample(f32 time) const
        {
            if (m_keyframes.IsEmpty())
                return {};
            if (m_keyframes.Size() == 1)
                return m_keyframes[0].value;

            // Clamp to animation bounds.
            if (time <= m_keyframes[0].time)
                return m_keyframes[0].value;
            if (time >= m_keyframes.Back().time)
                return m_keyframes.Back().value;

            // Find surrounding keyframes.
            usize i = 0;
            while (i < m_keyframes.Size() - 1 && m_keyframes[i + 1].time < time)
                ++i;

            auto& k0 = m_keyframes[i];
            auto& k1 = m_keyframes[i + 1];

            f32 t = (time - k0.time) / (k1.time - k0.time);

            switch (interpolation)
            {
            case AnimationInterpolation::Step:
                return k0.value;
            case AnimationInterpolation::Linear:
                if (path == AnimationPath::Rotation)
                {
                    // Quaternion slerp.
                    Quaternion q0(k0.value.x, k0.value.y, k0.value.z, k0.value.w);
                    Quaternion q1(k1.value.x, k1.value.y, k1.value.z, k1.value.w);
                    Quaternion result = Slerp(q0, q1, t);
                    return Float4(result.x, result.y, result.z, result.w);
                }
                else
                {
                    return Lerp(k0.value, k1.value, t);
                }
            case AnimationInterpolation::CubicSpline:
            {
                // glTF's Hermite: the tangents are per second, so scaled by the keys' gap; a
                // rotation is the same blend of its components, normalized.
                const f32 dt = k1.time - k0.time;
                const f32 t2 = t * t, t3 = t2 * t;
                const Float4 v = k0.value * (2.0f * t3 - 3.0f * t2 + 1.0f) +
                                 k0.outTangent * (dt * (t3 - 2.0f * t2 + t)) +
                                 k1.value * (-2.0f * t3 + 3.0f * t2) + k1.inTangent * (dt * (t3 - t2));
                if (path == AnimationPath::Rotation)
                {
                    const Quaternion q = Normalized(Quaternion(v.x, v.y, v.z, v.w));
                    return Float4(q.x, q.y, q.z, q.w);
                }
                return v;
            }
            }
            return {};
        }

        // -- Public fields --

        /// Target bone index.
        i32 targetBone = 0;

        /// Property being animated.
        AnimationPath path = AnimationPath::Translation;

        /// Interpolation method.
        AnimationInterpolation interpolation = AnimationInterpolation::Linear;

    private:
        Array<AnimationKeyframe> m_keyframes;
    };

    /// A complete animation clip.
    class ModelAnimation
    {
    public:
        ModelAnimation() = default;

        ~ModelAnimation()
        {
            for (auto* ch : m_channels)
                delete ch;
        }

        // Non-copyable, movable.
        ModelAnimation(const ModelAnimation&) = delete;
        ModelAnimation& operator=(const ModelAnimation&) = delete;
        ModelAnimation(ModelAnimation&& other) noexcept
            : duration(other.duration), m_name(static_cast<String&&>(other.m_name)),
              m_channels(static_cast<Array<AnimationChannel*>&&>(other.m_channels))
        {
            other.m_channels.Clear();
        }
        ModelAnimation& operator=(ModelAnimation&& other) noexcept
        {
            if (this != &other)
            {
                for (auto* ch : m_channels)
                    delete ch;
                m_name = static_cast<String&&>(other.m_name);
                m_channels = static_cast<Array<AnimationChannel*>&&>(other.m_channels);
                duration = other.duration;
                other.m_channels.Clear();
            }
            return *this;
        }

        [[nodiscard]] StringView name() const { return StringView(m_name.Data(), m_name.Size()); }
        void setName(StringView n) { m_name = String(n); }

        /// Add a channel (takes ownership of raw pointer).
        void addChannel(AnimationChannel* channel) { m_channels.PushBack(channel); }

        [[nodiscard]] Span<AnimationChannel* const> channels() const
        {
            return Span<AnimationChannel* const>(m_channels.Data(), m_channels.Size());
        }
        [[nodiscard]] Span<AnimationChannel*> channels()
        {
            return Span<AnimationChannel*>(m_channels.Data(), m_channels.Size());
        }

        /// Calculate duration from keyframes.
        void calculateDuration()
        {
            duration = 0.0f;
            for (auto* channel : m_channels)
            {
                for (auto& kf : channel->keyframes())
                {
                    if (kf.time > duration)
                        duration = kf.time;
                }
            }
        }

        // -- Public fields --

        /// Duration of the animation in seconds.
        f32 duration = 0.0f;

    private:
        String m_name;
        Array<AnimationChannel*> m_channels;
    };

} // namespace foundation::model
