// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.audio:sequence - the sound track's runner, serialization and registration.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.audio;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.audio;
import foundation.sequence;

using namespace foundation::core;
using namespace foundation::audio;

namespace engine::audio
{
    REFLECT_MEMBERS(SoundSequenceTrack, "rtti::audio")
    {
        builder.Attribute("displayName", String(u8"Sound"));
    }

    namespace
    {
        // Keeps one voice on the playhead while a section runs; stops it between sections and
        // when the play lets go.
        class SoundRunner final : public sequence::SequenceTrackRunner
        {
        public:
            SoundRunner(const SoundSequenceTrack& track, scene::Scene& scene, scene::EntityHandle entity)
                : m_track(&track), m_scene(&scene), m_entity(entity)
            {
            }

            void Evaluate(const sequence::SequenceStep& step) override
            {
                AudioSceneSystem* audio = m_scene->GetSystem<AudioSceneSystem>();
                AudioEngine* engine = audio != nullptr ? audio->Engine() : nullptr;
                if (engine == nullptr || audio->SceneGroup() == 0 || step.preview)
                {
                    StopVoice(); // no scene audio, or the editor posing: silence
                    return;
                }
                const SoundSection* section = m_track->SectionAt(step.time);
                AudioClip* clip = section != nullptr ? section->clip.Get() : nullptr;
                if (clip == nullptr)
                {
                    StopVoice();
                    m_section = nullptr;
                    return;
                }
                // Where in the clip the playhead says the voice is; past the end of a clip played
                // once, nothing more to hear.
                f32 expected = section->offset + (step.time - section->start);
                const f32 clipSeconds = clip->durationSeconds;
                if (section->loop && clipSeconds > 0.0f)
                {
                    expected -= Floor(expected / clipSeconds) * clipSeconds;
                }
                const bool audible = section->loop || expected < clipSeconds;
                m_engine = engine;
                if (section != m_section)
                {
                    StopVoice();
                    m_section = section;
                    if (audible)
                    {
                        Start(*audio, *clip, *section, expected);
                    }
                    return;
                }
                if (!engine->IsValidHandle(m_voice))
                {
                    // It played out, or was taken for another: a jump back into the clip brings
                    // it back; plain play does not repeat it.
                    if (step.jumped && audible)
                    {
                        Start(*audio, *clip, *section, expected);
                    }
                    return;
                }
                if (!audible)
                {
                    return; // ending on its own
                }
                VoiceStatus status;
                if (engine->GetVoiceStatus(m_voice, status) &&
                    (step.jumped || Abs(status.cursorSeconds - expected) > kSoundDriftSeconds))
                {
                    engine->SeekVoice(m_voice, expected);
                }
                if (m_entity.IsAssigned() && m_scene->IsValid(m_entity))
                {
                    engine->SetVoicePosition(m_voice, EntityPosition(), Float3{0.0f, 0.0f, 0.0f});
                }
            }

            void Release(bool restore) override
            {
                (void)restore; // a sound, nothing to put back
                StopVoice();
                m_section = nullptr;
            }

            void SetPaused(bool paused) override
            {
                if (m_engine != nullptr && m_engine->IsValidHandle(m_voice))
                {
                    m_engine->SetPaused(m_voice, paused);
                }
            }

        private:
            void Start(AudioSceneSystem& audio, AudioClip& clip, const SoundSection& section, f32 at)
            {
                AudioPlayParams params;
                params.bus = section.bus;
                params.volume = section.volume;
                params.loop = section.loop;
                params.sceneGroup = audio.SceneGroup();
                params.allowDedupe = false; // a timeline's sound is its own, never merged
                params.startSeconds = at;
                if (m_entity.IsAssigned() && m_scene->IsValid(m_entity))
                {
                    params.spatial = true;
                    params.position = EntityPosition();
                }
                m_voice = audio.Engine()->Play(RefPtr<AudioClip>(&clip), params);
            }

            void StopVoice()
            {
                if (m_engine != nullptr && m_engine->IsValidHandle(m_voice))
                {
                    m_engine->Stop(m_voice);
                }
                m_voice = VoiceHandle{};
            }

            [[nodiscard]] Float3 EntityPosition() const
            {
                return TransformPoint(Float3{0.0f, 0.0f, 0.0f}, m_scene->GetWorldMatrix(m_entity));
            }

            const SoundSequenceTrack* m_track;
            scene::Scene* m_scene;
            scene::EntityHandle m_entity;
            AudioEngine* m_engine = nullptr; // the engine the voice is on
            VoiceHandle m_voice;
            const SoundSection* m_section = nullptr; // whose voice it is
        };
    }

    f32 SoundSection::Length() const noexcept
    {
        if (length > 0.0f)
        {
            return length;
        }
        const AudioClip* loaded = clip.Get();
        return loaded != nullptr ? Max(0.0f, loaded->durationSeconds - offset) : 0.0f;
    }

    void SoundSequenceTrack::GetSection(usize index, f32& start, f32& length, String& label) const
    {
        if (index >= sections.Size())
            return;
        start = sections[index].start;
        length = sections[index].Length();
        label = name.IsEmpty() ? String(u8"Sound") : name;
    }

    void SoundSequenceTrack::SetSection(usize index, f32 start, f32 length)
    {
        if (index >= sections.Size())
            return;
        SoundSection& section = sections[index];
        start = Max(0.0f, start);
        section.offset = Max(0.0f, section.offset + (start - section.start));
        section.start = start;
        section.length = Max(0.0f, length);
    }

    void SoundSequenceTrack::RemoveSection(usize index)
    {
        if (index < sections.Size())
            sections.RemoveAt(index);
    }

    i32 SoundSequenceTrack::AddSection(f32 time)
    {
        usize at = 0;
        while (at < sections.Size() && sections[at].start <= time)
            ++at;
        SoundSection section;
        section.start = Max(0.0f, time);
        section.length = 1.0f;
        sections.Insert(at, Move(section));
        return static_cast<i32>(at);
    }

    f32 SoundSequenceTrack::Duration() const noexcept
    {
        f32 last = 0.0f;
        for (const SoundSection& section : sections)
        {
            last = Max(last, section.start + section.Length());
        }
        return last;
    }

    const SoundSection* SoundSequenceTrack::SectionAt(f32 time) const noexcept
    {
        const SoundSection* found = nullptr;
        for (const SoundSection& section : sections)
        {
            if (time >= section.start && time <= section.start + section.Length() &&
                (found == nullptr || section.start >= found->start))
            {
                found = &section;
            }
        }
        return found;
    }

    UniquePtr<sequence::SequenceTrackRunner> SoundSequenceTrack::Begin(const sequence::SequenceTrackContext& context) const
    {
        return MakeUnique<SoundRunner>(*context.allocator, *this, *context.scene, context.entity);
    }

    void SoundSequenceTrack::ResolveResources(foundation::resource::ResourceManager& manager)
    {
        for (SoundSection& section : sections)
        {
            section.clip.Bind(manager);
        }
    }

    void SoundSequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        const bool reading = ar.Mode() == SerializeMode::Read;
        Array<Guid> clips;
        Array<f32> starts;
        Array<f32> lengths;
        Array<f32> offsets;
        Array<f32> volumes;
        Array<u8> buses;
        Array<u8> loops;
        if (!reading)
        {
            for (const SoundSection& section : sections)
            {
                clips.PushBack(section.clip.id);
                starts.PushBack(section.start);
                lengths.PushBack(section.length);
                offsets.PushBack(section.offset);
                volumes.PushBack(section.volume);
                buses.PushBack(static_cast<u8>(section.bus));
                loops.PushBack(section.loop ? 1u : 0u);
            }
        }
        foundation::core::Serialize(ar, "sectionClip", clips);
        foundation::core::Serialize(ar, "sectionStart", starts);
        foundation::core::Serialize(ar, "sectionLength", lengths);
        foundation::core::Serialize(ar, "sectionOffset", offsets);
        foundation::core::Serialize(ar, "sectionVolume", volumes);
        foundation::core::Serialize(ar, "sectionBus", buses);
        foundation::core::Serialize(ar, "sectionLoop", loops);
        if (reading)
        {
            sections.Clear();
            const usize count = clips.Size();
            if (starts.Size() < count || lengths.Size() < count || offsets.Size() < count || volumes.Size() < count ||
                buses.Size() < count || loops.Size() < count)
            {
                return; // a damaged record: no sections rather than wrong ones
            }
            for (usize i = 0; i < count; ++i)
            {
                SoundSection section;
                section.clip.SetId(clips[i]);
                section.start = starts[i];
                section.length = lengths[i];
                section.offset = offsets[i];
                section.volume = volumes[i];
                section.bus = buses[i] < static_cast<u8>(AudioBus::Count) ? static_cast<AudioBus>(buses[i]) : AudioBus::Effects;
                section.loop = loops[i] != 0u;
                sections.PushBack(Move(section));
            }
        }
    }

    void RegisterAudioSequenceTracks()
    {
        static const bool once = []()
        {
            sequence::RegisterSequenceTypes();
            sequence::RegisterSequenceTrack<SoundSequenceTrack>();
            return true;
        }();
        (void)once;
    }
}
