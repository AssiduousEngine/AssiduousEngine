// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.animation:sequence - the clip track's runner, serialization and registration.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.animation;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.animation;
import foundation.sequence;
import foundation.script.facades; // script::Entity, which SceneAnimationFacade::OverlayOf takes

using namespace foundation::core;

namespace engine::animation
{
    REFLECT_VALUE(ClipSection, "rtti::animation")
    {
        builder.Property<&ClipSection::clip>("clip")
            .PropAttribute("displayName", String(u8"Clip"))
            .Property<&ClipSection::start>("start")
            .PropAttribute("displayName", String(u8"Start"))
            .Property<&ClipSection::length>("length")
            .PropAttribute("displayName", String(u8"Length"))
            .PropAttribute("description", String(u8"Seconds on the timeline; 0 plays the clip once from its offset"))
            .Property<&ClipSection::offset>("offset")
            .PropAttribute("displayName", String(u8"Offset"))
            .Property<&ClipSection::speed>("speed")
            .PropAttribute("displayName", String(u8"Speed"))
            .Property<&ClipSection::blendIn>("blendIn")
            .PropAttribute("displayName", String(u8"Blend In"))
            .Property<&ClipSection::blendOut>("blendOut")
            .PropAttribute("displayName", String(u8"Blend Out"))
            .Property<&ClipSection::loop>("loop")
            .PropAttribute("displayName", String(u8"Loop"));
    }
    REFLECT_MEMBERS(ClipSequenceTrack, "rtti::animation")
    {
        builder.Attribute("displayName", String(u8"Clip"))
            .Property<&ClipSequenceTrack::sections>("sections")
            .PropAttribute("displayName", String(u8"Clips"));
    }

    namespace
    {
        // Drives the entity's overlay while a section runs; between sections, and when the play
        // lets go, the overlay blends back to the pose underneath.
        class ClipRunner final : public sequence::SequenceTrackRunner
        {
        public:
            ClipRunner(const ClipSequenceTrack& track, scene::Scene& scene, scene::EntityHandle entity)
                : m_track(&track), m_scene(&scene), m_entity(entity)
            {
            }

            void Evaluate(const sequence::SequenceStep& step) override
            {
                foundation::animation::PoseOverlay* overlay = EntityOverlay(*m_scene, m_entity);
                if (overlay == nullptr)
                {
                    return; // no player yet: nothing to show it on
                }
                const ClipSection* section = m_track->SectionAt(step.time);
                const foundation::animation::AnimationClip* clip = section != nullptr ? section->clip.Get() : nullptr;
                if (clip == nullptr)
                {
                    if (m_section != nullptr && overlay->IsDriven())
                    {
                        overlay->StopOneShot(0.0f); // the section blended itself out
                    }
                    m_section = nullptr;
                    return;
                }
                const f32 local = step.time - section->start;
                f32 clipTime = section->offset + local * section->speed;
                if (section->loop && clip->duration > 0.0f)
                {
                    clipTime = clipTime - Floor(clipTime / clip->duration) * clip->duration;
                }
                else
                {
                    clipTime = Min(clipTime, clip->duration);
                }
                f32 weight = 1.0f;
                if (section->blendIn > 0.0f)
                {
                    weight = Min(weight, local / section->blendIn);
                }
                if (section->blendOut > 0.0f)
                {
                    weight = Min(weight, (section->Length() - local) / section->blendOut);
                }
                overlay->DriveOneShot(*clip, clipTime, weight);
                m_section = section;
            }

            void Release(bool restore) override
            {
                (void)restore; // a pose laid over the entity, nothing to put back
                if (m_section == nullptr)
                {
                    return;
                }
                if (foundation::animation::PoseOverlay* overlay = EntityOverlay(*m_scene, m_entity);
                    overlay != nullptr && overlay->IsDriven())
                {
                    overlay->StopOneShot(m_section->blendOut); // stopped mid-section: ease back
                }
                m_section = nullptr;
            }

        private:
            const ClipSequenceTrack* m_track;
            scene::Scene* m_scene;
            scene::EntityHandle m_entity;
            const ClipSection* m_section = nullptr; // the one driving the overlay
        };
    }

    f32 ClipSection::Length() const noexcept
    {
        if (length > 0.0f)
        {
            return length;
        }
        const foundation::animation::AnimationClip* loaded = clip.Get();
        if (loaded == nullptr || speed <= 0.0f)
        {
            return 0.0f;
        }
        return Max(0.0f, loaded->duration - offset) / speed;
    }

    void ClipSequenceTrack::GetSection(usize index, f32& start, f32& length, String& label) const
    {
        if (index >= sections.Size())
            return;
        const ClipSection& section = sections[index];
        start = section.start;
        length = section.Length();
        const foundation::animation::AnimationClip* clip = section.clip.Get();
        label = clip != nullptr && !clip->Name().IsEmpty() ? clip->Name() : String(u8"Clip");
    }

    void ClipSequenceTrack::SetSection(usize index, f32 start, f32 length)
    {
        if (index >= sections.Size())
            return;
        ClipSection& section = sections[index];
        start = Max(0.0f, start);
        section.offset = Max(0.0f, section.offset + (start - section.start) * section.speed);
        section.start = start;
        section.length = Max(0.0f, length);
    }

    void ClipSequenceTrack::RemoveSection(usize index)
    {
        if (index < sections.Size())
            sections.RemoveAt(index);
    }

    i32 ClipSequenceTrack::AddSection(f32 time)
    {
        usize at = 0;
        while (at < sections.Size() && sections[at].start <= time)
            ++at;
        ClipSection section;
        section.start = Max(0.0f, time);
        section.length = 1.0f;
        sections.Insert(at, Move(section));
        return static_cast<i32>(at);
    }

    f32 ClipSequenceTrack::Duration() const noexcept
    {
        f32 last = 0.0f;
        for (const ClipSection& section : sections)
        {
            last = Max(last, section.start + section.Length());
        }
        return last;
    }

    const ClipSection* ClipSequenceTrack::SectionAt(f32 time) const noexcept
    {
        const ClipSection* found = nullptr;
        for (const ClipSection& section : sections)
        {
            if (time >= section.start && time <= section.start + section.Length() &&
                (found == nullptr || section.start >= found->start))
            {
                found = &section;
            }
        }
        return found;
    }

    UniquePtr<sequence::SequenceTrackRunner> ClipSequenceTrack::Begin(const sequence::SequenceTrackContext& context) const
    {
        return MakeUnique<ClipRunner>(*context.allocator, *this, *context.scene, context.entity);
    }

    void ClipSequenceTrack::ResolveResources(foundation::resource::ResourceManager& manager)
    {
        for (ClipSection& section : sections)
        {
            section.clip.Bind(manager);
        }
    }

    void ClipSequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        const bool reading = ar.Mode() == SerializeMode::Read;
        Array<Guid> clips;
        Array<f32> starts;
        Array<f32> lengths;
        Array<f32> offsets;
        Array<f32> speeds;
        Array<f32> blendIns;
        Array<f32> blendOuts;
        Array<u8> loops;
        if (!reading)
        {
            for (const ClipSection& section : sections)
            {
                clips.PushBack(section.clip.id);
                starts.PushBack(section.start);
                lengths.PushBack(section.length);
                offsets.PushBack(section.offset);
                speeds.PushBack(section.speed);
                blendIns.PushBack(section.blendIn);
                blendOuts.PushBack(section.blendOut);
                loops.PushBack(section.loop ? 1u : 0u);
            }
        }
        foundation::core::Serialize(ar, "sectionClip", clips);
        foundation::core::Serialize(ar, "sectionStart", starts);
        foundation::core::Serialize(ar, "sectionLength", lengths);
        foundation::core::Serialize(ar, "sectionOffset", offsets);
        foundation::core::Serialize(ar, "sectionSpeed", speeds);
        foundation::core::Serialize(ar, "sectionBlendIn", blendIns);
        foundation::core::Serialize(ar, "sectionBlendOut", blendOuts);
        foundation::core::Serialize(ar, "sectionLoop", loops);
        if (reading)
        {
            sections.Clear();
            const usize count = clips.Size();
            if (starts.Size() < count || lengths.Size() < count || offsets.Size() < count || speeds.Size() < count ||
                blendIns.Size() < count || blendOuts.Size() < count || loops.Size() < count)
            {
                return; // a damaged record: no sections rather than wrong ones
            }
            for (usize i = 0; i < count; ++i)
            {
                ClipSection section;
                section.clip.SetId(clips[i]);
                section.start = starts[i];
                section.length = lengths[i];
                section.offset = offsets[i];
                section.speed = speeds[i];
                section.blendIn = blendIns[i];
                section.blendOut = blendOuts[i];
                section.loop = loops[i] != 0u;
                sections.PushBack(Move(section));
            }
        }
    }

    void RegisterAnimationSequenceTracks()
    {
        static const bool once = []()
        {
            sequence::RegisterSequenceTypes();
            RttiRegisterValue_ClipSection();
            RegisterArrayType<ClipSection>();
            sequence::RegisterSequenceTrack<ClipSequenceTrack>();
            return true;
        }();
        (void)once;
    }

    foundation::animation::PoseOverlay* EntityOverlay(scene::Scene& scene, scene::EntityHandle entity)
    {
        if (auto* graphs = scene.GetSystem<AnimationGraphComponentManager>())
        {
            if (AnimationGraphComponent* c = graphs->Get(entity); c != nullptr && c->player.Get() != nullptr)
            {
                return &c->player->Overlay();
            }
        }
        if (auto* skeletal = scene.GetSystem<SkeletalAnimationComponentManager>())
        {
            if (SkeletalAnimationComponent* c = skeletal->Get(entity); c != nullptr && c->player.Get() != nullptr)
            {
                return &c->player->Overlay();
            }
        }
        return nullptr;
    }

    foundation::animation::PoseOverlay* SceneAnimationFacade::OverlayOf(foundation::script::Entity entity) const
    {
        scene::Scene* owner = scene.Get();
        return owner != nullptr ? EntityOverlay(*owner, entity.Handle()) : nullptr;
    }
}
