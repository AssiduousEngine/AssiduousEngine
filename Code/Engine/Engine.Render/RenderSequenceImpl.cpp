// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.render:sequence - the camera cut and fade tracks' runners, serialization and
// registration.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module engine.render;

import foundation.core;
import foundation.scene;
import foundation.sequence;

using namespace foundation::core;

namespace engine::render
{
    REFLECT_VALUE(CameraCutSection, "rtti::render")
    {
        builder.Property<&CameraCutSection::start>("start")
            .PropAttribute("displayName", String(u8"Start"))
            .Property<&CameraCutSection::duration>("duration")
            .PropAttribute("displayName", String(u8"Duration"));
    }
    REFLECT_MEMBERS(CameraCutSequenceTrack, "rtti::render")
    {
        builder.Attribute("displayName", String(u8"Camera Cut"))
            .Property<&CameraCutSequenceTrack::sections>("sections")
            .PropAttribute("displayName", String(u8"Shots"));
    }
    REFLECT_MEMBERS(FadeSequenceTrack, "rtti::render")
    {
        builder.Attribute("displayName", String(u8"Fade"))
            .Property<&FadeSequenceTrack::color>("color")
            .PropAttribute("displayName", String(u8"Color"))
            .Property<&FadeSequenceTrack::amount>("amount")
            .PropAttribute("displayName", String(u8"Amount"));
    }

    namespace
    {
        // Holds the view while a section runs, and lets go between sections and at the end.
        class CameraCutRunner final : public sequence::SequenceTrackRunner
        {
        public:
            CameraCutRunner(const CameraCutSequenceTrack& track, CameraComponentManager& cameras,
                            scene::EntityHandle camera, const sequence::SequencePlay* play)
                : m_track(&track), m_cameras(&cameras), m_camera(camera), m_play(play)
            {
            }

            void Evaluate(const sequence::SequenceStep& step) override
            {
                if (m_track->CutsAt(step.time))
                {
                    if (!m_claiming)
                    {
                        m_cameras->ViewClaims().Claim(this, m_play != nullptr ? m_play->StartOrder() : 0u, m_camera);
                        m_claiming = true;
                    }
                }
                else if (m_claiming)
                {
                    m_cameras->ViewClaims().Release(this);
                    m_claiming = false;
                }
            }

            void Release(bool restore) override
            {
                (void)restore; // the view is a claim, nothing to put back
                if (m_claiming)
                {
                    m_cameras->ViewClaims().Release(this);
                    m_claiming = false;
                }
            }

        private:
            const CameraCutSequenceTrack* m_track;
            CameraComponentManager* m_cameras;
            scene::EntityHandle m_camera;
            const sequence::SequencePlay* m_play;
            bool m_claiming = false;
        };
    }

    namespace
    {
        // Holds the fade from the play's first step until it lets go, at the amount its keys give.
        class FadeRunner final : public sequence::SequenceTrackRunner
        {
        public:
            FadeRunner(const FadeSequenceTrack& track, PostProcessSystem& post, const sequence::SequencePlay* play)
                : m_track(&track), m_post(&post), m_play(play)
            {
            }

            void Evaluate(const sequence::SequenceStep& step) override
            {
                ScreenFade fade;
                fade.color = m_track->color;
                fade.amount = Clamp(m_track->amount.Evaluate(step.time), 0.0f, 1.0f);
                m_post->FadeClaims().Claim(this, m_play != nullptr ? m_play->StartOrder() : 0u, fade);
            }

            void Release(bool restore) override
            {
                (void)restore; // the fade is a claim, nothing to put back
                m_post->FadeClaims().Release(this);
            }

        private:
            const FadeSequenceTrack* m_track;
            PostProcessSystem* m_post;
            const sequence::SequencePlay* m_play;
        };
    }

    UniquePtr<sequence::SequenceTrackRunner> FadeSequenceTrack::Begin(const sequence::SequenceTrackContext& context) const
    {
        PostProcessSystem* post = context.scene->GetSystem<PostProcessSystem>();
        if (post == nullptr)
        {
            return {}; // a scene nothing renders: no view to fade
        }
        return MakeUnique<FadeRunner>(*context.allocator, *this, *post, context.play);
    }

    void FadeSequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        foundation::core::Serialize(ar, "color", color);
        const bool reading = ar.Mode() == SerializeMode::Read;
        Array<f32> times;
        Array<f32> values;
        Array<f32> tangentsIn;
        Array<f32> tangentsOut;
        Array<u8> interpolations;
        Array<u8> tangentModes;
        if (!reading)
        {
            for (const CurveKey& key : amount.Keys())
            {
                times.PushBack(key.time);
                values.PushBack(key.value);
                tangentsIn.PushBack(key.tangentIn);
                tangentsOut.PushBack(key.tangentOut);
                interpolations.PushBack(static_cast<u8>(key.interpolation));
                tangentModes.PushBack(static_cast<u8>(key.tangentMode));
            }
        }
        foundation::core::Serialize(ar, "keyTime", times);
        foundation::core::Serialize(ar, "keyValue", values);
        foundation::core::Serialize(ar, "keyTangentIn", tangentsIn);
        foundation::core::Serialize(ar, "keyTangentOut", tangentsOut);
        foundation::core::Serialize(ar, "keyInterpolation", interpolations);
        foundation::core::Serialize(ar, "keyTangentMode", tangentModes);
        if (reading)
        {
            amount.Clear();
            const usize count = Min(Min(times.Size(), values.Size()), Min(tangentsIn.Size(), tangentsOut.Size()));
            for (usize i = 0; i < count; ++i)
            {
                CurveKey key;
                key.time = times[i];
                key.value = values[i];
                key.tangentIn = tangentsIn[i];
                key.tangentOut = tangentsOut[i];
                if (i < interpolations.Size())
                {
                    key.interpolation = static_cast<decltype(key.interpolation)>(interpolations[i]);
                }
                if (i < tangentModes.Size())
                {
                    key.tangentMode = static_cast<decltype(key.tangentMode)>(tangentModes[i]);
                }
                amount.AddKey(key);
            }
        }
    }

    // === Editing ===

    void CameraCutSequenceTrack::GetSection(usize index, f32& start, f32& length, String& label) const
    {
        start = index < sections.Size() ? sections[index].start : 0.0f;
        length = index < sections.Size() ? sections[index].duration : 0.0f;
        label = name.IsEmpty() ? String(u8"Shot") : name;
    }

    void CameraCutSequenceTrack::SetSection(usize index, f32 start, f32 length)
    {
        if (index < sections.Size())
            sections[index] = CameraCutSection{Max(0.0f, start), Max(0.0f, length)};
    }

    void CameraCutSequenceTrack::RemoveSection(usize index)
    {
        if (index < sections.Size())
            sections.RemoveAt(index);
    }

    i32 CameraCutSequenceTrack::AddSection(f32 time)
    {
        usize at = 0;
        while (at < sections.Size() && sections[at].start <= time)
            ++at;
        sections.Insert(at, CameraCutSection{Max(0.0f, time), 1.0f});
        return static_cast<i32>(at);
    }

    String FadeSequenceTrack::KeyChannelName(usize channel) const
    {
        (void)channel;
        return String(u8"amount");
    }

    void FadeSequenceTrack::GetKeyTimes(usize channel, Array<f32>& out) const
    {
        (void)channel;
        out.Clear();
        for (const CurveKey& key : amount.Keys())
            out.PushBack(key.time);
    }

    void FadeSequenceTrack::MoveKeys(usize channel, Span<const usize> keys, f32 delta, Array<usize>& moved)
    {
        (void)channel;
        sequence::MoveKeysInTime(amount.Keys(), &CurveKey::time, keys, delta, moved);
        amount.ApplyTangentModes();
    }

    void FadeSequenceTrack::DeleteKeys(usize channel, Span<const usize> keys)
    {
        (void)channel;
        sequence::DeleteKeysAt(amount.Keys(), keys);
        amount.ApplyTangentModes();
    }

    i32 FadeSequenceTrack::InsertKey(usize channel, f32 time)
    {
        (void)channel;
        time = Max(0.0f, time);
        for (usize i = 0; i < amount.Keys().Size(); ++i)
            if (Abs(amount.Keys()[i].time - time) < 1e-5f)
                return static_cast<i32>(i);
        const f32 value = amount.IsEmpty() ? 0.0f : Clamp(amount.Evaluate(time), 0.0f, 1.0f);
        amount.AddKey(CurveKey{time, value, 0.0f, 0.0f, CurveKeyInterpolation::Linear, CurveTangentMode::Auto});
        amount.ApplyTangentModes();
        for (usize i = 0; i < amount.Keys().Size(); ++i)
            if (Abs(amount.Keys()[i].time - time) < 1e-5f)
                return static_cast<i32>(i);
        return -1;
    }

    Curve* FadeSequenceTrack::KeyCurve(usize channel)
    {
        (void)channel;
        return &amount;
    }

    f32 CameraCutSequenceTrack::Duration() const noexcept
    {
        f32 last = 0.0f;
        for (const CameraCutSection& section : sections)
        {
            last = Max(last, section.start + section.duration);
        }
        return last;
    }

    bool CameraCutSequenceTrack::CutsAt(f32 time) const noexcept
    {
        for (const CameraCutSection& section : sections)
        {
            if (time >= section.start && time <= section.start + section.duration)
            {
                return true;
            }
        }
        return false;
    }

    UniquePtr<sequence::SequenceTrackRunner> CameraCutSequenceTrack::Begin(const sequence::SequenceTrackContext& context) const
    {
        CameraComponentManager* cameras = context.scene->GetSystem<CameraComponentManager>();
        if (cameras == nullptr || !cameras->Has(context.entity))
        {
            LOG_WARNING(u8"Sequence", u8"camera cut track '{}' is bound to an entity with no camera - skipped", name);
            return {};
        }
        return MakeUnique<CameraCutRunner>(*context.allocator, *this, *cameras, context.entity, context.play);
    }

    void CameraCutSequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        const bool reading = ar.Mode() == SerializeMode::Read;
        Array<f32> starts;
        Array<f32> durations;
        if (!reading)
        {
            for (const CameraCutSection& section : sections)
            {
                starts.PushBack(section.start);
                durations.PushBack(section.duration);
            }
        }
        foundation::core::Serialize(ar, "sectionStart", starts);
        foundation::core::Serialize(ar, "sectionDuration", durations);
        if (reading)
        {
            sections.Clear();
            const usize count = Min(starts.Size(), durations.Size());
            for (usize i = 0; i < count; ++i)
            {
                sections.PushBack(CameraCutSection{starts[i], durations[i]});
            }
        }
    }

    void RegisterRenderSequenceTracks()
    {
        static const bool once = []()
        {
            sequence::RegisterSequenceTypes();
            RttiRegisterValue_CameraCutSection();
            RegisterArrayType<CameraCutSection>();
            sequence::RegisterSequenceTrack<CameraCutSequenceTrack>();
            sequence::RegisterSequenceTrack<FadeSequenceTrack>();
            return true;
        }();
        (void)once;
    }
}
