// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.render:sequence - the camera cut track's runner, serialization and registration.

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
    REFLECT_MEMBERS(CameraCutSequenceTrack, "rtti::render")
    {
        builder.Attribute("displayName", String(u8"Camera cut"));
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
                        m_cameras->ClaimView(this, m_play != nullptr ? m_play->StartOrder() : 0u, m_camera);
                        m_claiming = true;
                    }
                }
                else if (m_claiming)
                {
                    m_cameras->ReleaseView(this);
                    m_claiming = false;
                }
            }

            void Release(bool restore) override
            {
                (void)restore; // the view is a claim, nothing to put back
                if (m_claiming)
                {
                    m_cameras->ReleaseView(this);
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
            sequence::RegisterSequenceTrack<CameraCutSequenceTrack>();
            return true;
        }();
        (void)once;
    }
}
