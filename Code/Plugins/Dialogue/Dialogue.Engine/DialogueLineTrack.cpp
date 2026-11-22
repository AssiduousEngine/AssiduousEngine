// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The line track's runner, editing, serialization and registration.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.engine;

import foundation.core;
import foundation.resource;
import foundation.run;
import foundation.scene;
import foundation.sequence;

using namespace foundation::core;

namespace dialogue
{
    namespace sequence = foundation::sequence;

    REFLECT_VALUE(DialogueLineSection, "rtti::dialogue")
    {
        builder.Property<&DialogueLineSection::start>("start")
            .PropAttribute("displayName", String(u8"Start"))
            .Property<&DialogueLineSection::length>("length")
            .PropAttribute("displayName", String(u8"Length"))
            .PropAttribute("description", String(u8"Seconds on the timeline; the play holds at its end until the "
                                                  u8"line is advanced"))
            .Property<&DialogueLineSection::speaker>("speaker")
            .PropAttribute("displayName", String(u8"Speaker"))
            .PropAttribute("description", String(u8"The speaker's id in the track's cast"))
            .Property<&DialogueLineSection::mood>("mood")
            .PropAttribute("displayName", String(u8"Mood"))
            .Property<&DialogueLineSection::text>("text")
            .PropAttribute("displayName", String(u8"Text"))
            .Property<&DialogueLineSection::dialogue>("dialogue")
            .PropAttribute("displayName", String(u8"Dialogue File"))
            .PropAttribute("description", String(u8"A dialogue file to take the line from, by its line id, in place "
                                                  u8"of the text"))
            .Property<&DialogueLineSection::lineId>("lineId")
            .PropAttribute("displayName", String(u8"Line Id"));
    }

    REFLECT_MEMBERS(DialogueLineTrack, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Dialogue Lines"))
            .Property<&DialogueLineTrack::cast>("cast")
            .PropAttribute("displayName", String(u8"Cast"))
            .Property<&DialogueLineTrack::sections>("sections")
            .PropAttribute("displayName", String(u8"Lines"));
    }

    namespace
    {
        // Shows each section's line through the run's dialogue runner as the playhead enters it,
        // and holds the play at its end while the line is shown.
        class LineRunner final : public sequence::SequenceTrackRunner
        {
        public:
            LineRunner(const DialogueLineTrack& track, foundation::scene::Scene* scene) : m_track(&track), m_scene(scene) {}

            void Evaluate(const sequence::SequenceStep& step) override
            {
                if (step.preview)
                {
                    return; // the editor posing the scene shows no lines
                }
                const DialogueLineSection* section = m_track->SectionAt(step.time);
                if (section == m_section)
                {
                    return; // a line advanced inside its section is not shown again
                }
                EndLine();
                m_section = section;
                DialogueRunService* runner = Runner();
                if (section != nullptr && runner != nullptr)
                {
                    m_line = runner->ShowLine(m_track->cast.Get(), section->speaker.AsView(), section->mood.AsView(),
                                              section->Text(), section->lineId.AsView());
                }
            }

            void Release(bool restore) override
            {
                (void)restore;
                EndLine();
                m_section = nullptr;
            }

            [[nodiscard]] f32 HoldTime(f32 after, f32 upTo) const override
            {
                f32 first = -1.0f;
                for (const DialogueLineSection& section : m_track->sections)
                {
                    const f32 end = section.start + section.length;
                    if (end > after && end <= upTo && (first < 0.0f || end < first))
                    {
                        first = end;
                    }
                }
                return first;
            }

            [[nodiscard]] bool IsHolding() const override
            {
                const DialogueRunService* runner = Runner();
                return runner != nullptr && runner->IsShowing(m_line);
            }

        private:
            [[nodiscard]] DialogueRunService* Runner() const
            {
                foundation::run::IRun* run = m_scene != nullptr ? m_scene->Run() : nullptr;
                return run != nullptr ? run->Get<DialogueRunService>() : nullptr;
            }

            void EndLine()
            {
                if (DialogueRunService* runner = Runner(); runner != nullptr && m_line != 0)
                {
                    runner->EndLine(m_line);
                }
                m_line = 0;
            }

            const DialogueLineTrack* m_track;
            foundation::scene::Scene* m_scene;
            const DialogueLineSection* m_section = nullptr; // whose line it is
            u32 m_line = 0;
        };
    }

    StringView DialogueLineSection::Text() const noexcept
    {
        if (!lineId.IsEmpty())
        {
            if (const DialogueResource* file = dialogue.Get())
            {
                const StringView found = file->Text(lineId.AsView());
                if (!found.IsEmpty())
                {
                    return found;
                }
            }
        }
        return text.AsView();
    }

    void Serialize(ISerializer& ar, DialogueLineSection& section)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "start", section.start);
        foundation::core::Serialize(ar, "length", section.length);
        foundation::core::Serialize(ar, "speaker", section.speaker);
        foundation::core::Serialize(ar, "mood", section.mood);
        foundation::core::Serialize(ar, "text", section.text);
        Guid dialogue = section.dialogue.id;
        foundation::core::Serialize(ar, "dialogue", dialogue);
        section.dialogue.SetId(dialogue);
        foundation::core::Serialize(ar, "lineId", section.lineId);
        ar.EndObject();
    }

    f32 DialogueLineTrack::Duration() const noexcept
    {
        f32 last = 0.0f;
        for (const DialogueLineSection& section : sections)
        {
            last = Max(last, section.start + section.length);
        }
        return last;
    }

    UniquePtr<sequence::SequenceTrackRunner> DialogueLineTrack::Begin(const sequence::SequenceTrackContext& context) const
    {
        return MakeUnique<LineRunner>(*context.allocator, *this, context.scene);
    }

    void DialogueLineTrack::ResolveResources(foundation::resource::ResourceManager& manager)
    {
        cast.Bind(manager);
        for (DialogueLineSection& section : sections)
        {
            if (!section.dialogue.id.IsNil())
            {
                section.dialogue.Bind(manager);
            }
        }
    }

    void DialogueLineTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        Guid castId = cast.id;
        foundation::core::Serialize(ar, "cast", castId);
        cast.SetId(castId);
        foundation::core::Serialize(ar, "sections", sections);
    }

    const DialogueLineSection* DialogueLineTrack::SectionAt(f32 time) const noexcept
    {
        const DialogueLineSection* found = nullptr;
        for (const DialogueLineSection& section : sections)
        {
            if (time >= section.start && time <= section.start + section.length &&
                (found == nullptr || section.start >= found->start))
            {
                found = &section;
            }
        }
        return found;
    }

    void DialogueLineTrack::GetSection(usize index, f32& start, f32& length, String& label) const
    {
        if (index >= sections.Size())
        {
            return;
        }
        const DialogueLineSection& section = sections[index];
        start = section.start;
        length = section.length;
        const StringView text = section.Text();
        label = section.speaker.IsEmpty() ? String(text) : Format(u8"{}: {}", section.speaker, text);
    }

    void DialogueLineTrack::SetSection(usize index, f32 start, f32 length)
    {
        if (index >= sections.Size())
        {
            return;
        }
        sections[index].start = Max(0.0f, start);
        sections[index].length = Max(0.0f, length);
    }

    void DialogueLineTrack::RemoveSection(usize index)
    {
        if (index < sections.Size())
        {
            sections.RemoveAt(index);
        }
    }

    i32 DialogueLineTrack::AddSection(f32 time)
    {
        usize at = 0;
        while (at < sections.Size() && sections[at].start <= time)
        {
            ++at;
        }
        DialogueLineSection section;
        section.start = Max(0.0f, time);
        sections.Insert(at, Move(section));
        return static_cast<i32>(at);
    }

    void RegisterDialogueLineTrack()
    {
        RttiRegisterValue_DialogueLineSection();
        RegisterArrayType<DialogueLineSection>();
        sequence::RegisterSequenceTrack<DialogueLineTrack>();
    }

}
