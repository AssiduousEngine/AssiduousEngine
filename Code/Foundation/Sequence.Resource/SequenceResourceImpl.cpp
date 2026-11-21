// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.sequence.resource - the nested sequence track's runner and the registration.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module foundation.sequence.resource;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.sequence;

using namespace foundation::core;

namespace foundation::sequence
{
    REFLECT_MEMBERS(NestedSequenceTrack, "rtti::sequence")
    {
        builder.Attribute("displayName", String(u8"Sequence"))
            .Property<&NestedSequenceTrack::start>("start")
            .Property<&NestedSequenceTrack::length>("length")
            .Property<&NestedSequenceTrack::loop>("loop");
    }

    namespace
    {
        // Plays the nested sequence as a play of its own, stepped by the outer playhead mapped into
        // the section.
        class NestedRunner final : public SequenceTrackRunner
        {
        public:
            NestedRunner(IAllocator& allocator, scene::Scene& scene, const NestedSequenceTrack& track,
                         RefPtr<SequenceResource> nested, const SequencePlay& parent, ISequenceListener* listener)
                : m_track(&track), m_nested(Move(nested)), m_play(allocator, scene, m_nested->sequence)
            {
                m_play.SetParent(&parent);
                m_play.SetListener(listener);
                // The same cast: each name the nested sequence has is filled by the outer play's
                // entity of that name.
                for (const SequenceBinding& binding : m_nested->sequence.bindings)
                {
                    (void)m_play.Bind(binding.name.AsView(), parent.BoundEntity(binding.name.AsView()));
                }
            }

            void Evaluate(const SequenceStep& step) override
            {
                const f32 duration = m_nested->sequence.duration;
                const f32 length = m_track->SectionLength();
                const f32 now = Min(step.time - m_track->start, length);
                const f32 previous = Min(step.previousTime - m_track->start, length);
                if (now < 0.0f && previous < 0.0f)
                {
                    return; // the section is not reached
                }
                SequenceStep inner = step;
                if (!m_track->loop || duration <= 0.0f)
                {
                    if (m_applied && !step.jumped && previous >= Min(length, duration))
                    {
                        return; // past its end, already applied there
                    }
                    inner.time = Clamp(now, 0.0f, duration);
                    inner.previousTime = previous < 0.0f ? -1.0f : Min(previous, duration);
                    m_play.EvaluateStep(inner);
                    m_applied = true;
                    return;
                }
                // Looping: a step that crosses the nested sequence's end plays to it, then from its
                // start again.
                const f32 nowWrapped = Wrap(Max(now, 0.0f), duration);
                if (!step.jumped && previous >= 0.0f && Floor(previous / duration) != Floor(Max(now, 0.0f) / duration))
                {
                    SequenceStep toEnd = step;
                    toEnd.previousTime = Wrap(previous, duration);
                    toEnd.time = duration;
                    m_play.EvaluateStep(toEnd);
                    inner.previousTime = -1.0f;
                }
                else
                {
                    inner.previousTime = previous < 0.0f ? -1.0f : Wrap(previous, duration);
                }
                inner.time = nowWrapped;
                m_play.EvaluateStep(inner);
                m_applied = true;
            }

            // The nested play's runners pause with the outer play.
            void SetPaused(bool paused) override
            {
                if (paused)
                {
                    m_play.Pause();
                }
                else
                {
                    m_play.Resume();
                }
            }

            void Release(bool restore) override
            {
                m_play.finish = restore ? SequenceFinish::Restore : SequenceFinish::Release;
                m_play.Stop();
                m_applied = false;
            }

        private:
            [[nodiscard]] static f32 Wrap(f32 time, f32 duration) noexcept
            {
                return time - Floor(time / duration) * duration;
            }

            const NestedSequenceTrack* m_track;
            RefPtr<SequenceResource> m_nested; // kept alive while it plays, even across a reload
            SequencePlay m_play;
            bool m_applied = false;
        };
    }

    f32 NestedSequenceTrack::SectionLength() const noexcept
    {
        if (length > 0.0f)
        {
            return length;
        }
        const SequenceResource* nested = sequence.Get();
        return nested != nullptr ? nested->sequence.duration : 0.0f;
    }

    f32 NestedSequenceTrack::Duration() const noexcept { return start + SectionLength(); }

    void NestedSequenceTrack::GetSection(usize index, f32& at, f32& span, String& label) const
    {
        (void)index;
        at = start;
        span = SectionLength();
        label = name.IsEmpty() ? String(u8"Sequence") : name;
    }

    void NestedSequenceTrack::SetSection(usize index, f32 at, f32 span)
    {
        (void)index;
        start = Max(0.0f, at);
        length = Max(0.0f, span);
    }

    UniquePtr<SequenceTrackRunner> NestedSequenceTrack::Begin(const SequenceTrackContext& context) const
    {
        RefPtr<SequenceResource> nested(sequence.Get());
        if (!nested)
        {
            LOG_WARNING(u8"Sequence", u8"track '{}' has no sequence to play - skipped", name);
            return {};
        }
        // A sequence inside itself, directly or through others, would nest without end.
        if (context.play != nullptr && context.play->Plays(nested->sequence))
        {
            LOG_WARNING(u8"Sequence", u8"track '{}' plays a sequence it is already inside - skipped", name);
            return {};
        }
        return MakeUnique<NestedRunner>(*context.allocator, *context.allocator, *context.scene, *this, Move(nested),
                                        *context.play, context.listener);
    }

    void NestedSequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        foundation::core::Serialize(ar, "sequence", sequence);
        foundation::core::Serialize(ar, "start", start);
        foundation::core::Serialize(ar, "length", length);
        foundation::core::Serialize(ar, "loop", loop);
    }

    void RegisterSequenceResource()
    {
        static const bool once = []()
        {
            RegisterSequenceTypes();
            RegisterSequenceTrack<NestedSequenceTrack>();
            GlobalTypeRegistry().Register(SequenceResource::StaticType());
            GlobalTypeRegistry().Register(SequenceSource::StaticType());
            RegisterSerializable<SequenceSource>();
            return true;
        }();
        (void)once;
    }
}
