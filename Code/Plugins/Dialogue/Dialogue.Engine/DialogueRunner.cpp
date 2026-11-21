// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The runner: a conversation's steps played, what waits held, the flags and the resume record.

module;

#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module dialogue.engine;

import foundation.core;
import foundation.run;

using namespace foundation::core;

namespace dialogue
{
    namespace
    {
        // The resume record's fields, in order, joined by `|` (none of them holds one: a guid, a
        // conversation id and a line id are words, the step and the hash numbers).
        enum ResumeField : usize
        {
            kResumeDialogue,
            kResumeConversation,
            kResumeStep,
            kResumeLine,
            kResumeHash,
            kResumeFieldCount,
        };

        [[nodiscard]] Array<StringView> SplitFields(StringView text)
        {
            Array<StringView> fields;
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
            {
                if (i == text.Size() || text[i] == u8'|')
                {
                    fields.PushBack(text.SubStr(start, i - start));
                    start = i + 1;
                }
            }
            return fields;
        }

        [[nodiscard]] u64 HashText(StringView text, u64 hash) noexcept
        {
            hash = HashBytes(text.Data(), text.Size(), hash);
            const u8 end = 0; // so "ab","c" and "a","bc" differ
            return HashBytes(&end, 1, hash);
        }

        template <typename T>
        [[nodiscard]] u64 HashValue(const T& value, u64 hash) noexcept
        {
            return HashBytes(&value, sizeof(value), hash);
        }
    }

    const DialogueArgument* DialogueCommandNotice::Find(StringView name) const noexcept
    {
        for (const DialogueArgument& argument : arguments)
        {
            if (argument.name == name)
            {
                return &argument;
            }
        }
        return nullptr;
    }

    bool DialogueRunner::Play(const Guid& dialogue, StringView conversation)
    {
        if (m_playing)
        {
            LOG_WARNING(u8"Dialogue", u8"play: the conversation `{}` is still playing", m_conversation->id);
            return false;
        }
        RefPtr<DialogueResource> file = m_host->LoadDialogue(dialogue);
        if (!file)
        {
            LOG_WARNING(u8"Dialogue", u8"play: no dialogue file {}", dialogue);
            return false;
        }
        const DialogueConversation* found = conversation.IsEmpty()
                                                ? (file->conversations.IsEmpty() ? nullptr : &file->conversations[0])
                                                : file->Find(conversation);
        if (found == nullptr)
        {
            LOG_WARNING(u8"Dialogue", u8"play: the dialogue file {} has no conversation `{}`", dialogue, conversation);
            return false;
        }
        Begin(Move(file), dialogue, *found, 0);
        Pump();
        return true;
    }

    bool DialogueRunner::Advance()
    {
        if (m_wait != DialogueWait::Line)
        {
            return false;
        }
        m_wait = DialogueWait::Nothing;
        ++m_step;
        Pump();
        return true;
    }

    bool DialogueRunner::Choose(i32 index)
    {
        if (m_wait != DialogueWait::Choice)
        {
            return false;
        }
        const DialogueStep& choice = m_conversation->steps[m_step];
        if (index < 0 || static_cast<u32>(index) >= choice.count)
        {
            return false;
        }
        const DialogueStep& answer = m_conversation->steps[m_step + 1 + static_cast<usize>(index)];
        m_wait = DialogueWait::Nothing;
        m_step = static_cast<usize>(answer.next);
        Pump();
        return true;
    }

    bool DialogueRunner::Done()
    {
        if (m_wait != DialogueWait::Command)
        {
            return false;
        }
        m_wait = DialogueWait::Nothing;
        Pump();
        return true;
    }

    void DialogueRunner::Stop()
    {
        if (m_playing)
        {
            Finish(true, false);
        }
    }

    bool DialogueRunner::Resume()
    {
        if (m_playing)
        {
            return false;
        }
        const String record = m_host->ReadText(kDialogueResumeKey);
        if (record.IsEmpty())
        {
            return false;
        }
        const Array<StringView> fields = SplitFields(record.AsView());
        Guid dialogue;
        if (fields.Size() != kResumeFieldCount || !Guid::TryParse(fields[kResumeDialogue], dialogue))
        {
            LOG_WARNING(u8"Dialogue", u8"resume: the record `{}` is not one", record);
            m_host->WriteText(kDialogueResumeKey, {});
            return false;
        }
        RefPtr<DialogueResource> file = m_host->LoadDialogue(dialogue);
        const DialogueConversation* conversation = file ? file->Find(fields[kResumeConversation]) : nullptr;
        if (conversation == nullptr)
        {
            LOG_WARNING(u8"Dialogue", u8"resume: the dialogue file {} has no conversation `{}` now", dialogue,
                        fields[kResumeConversation]);
            m_host->WriteText(kDialogueResumeKey, {});
            return false;
        }
        const StringView line = fields[kResumeLine];
        const Optional<i64> saved = ParseInt(fields[kResumeStep]);
        usize step = 0;
        if (fields[kResumeHash] == Format(u8"{}", HashSteps(*conversation)).AsView() && saved.HasValue() &&
            saved.Value() >= 0 && static_cast<usize>(saved.Value()) < conversation->steps.Size())
        {
            step = static_cast<usize>(saved.Value()); // the conversation is as it was saved
        }
        else
        {
            bool found = false;
            for (usize i = 0; i < conversation->steps.Size() && !line.IsEmpty(); ++i)
            {
                const DialogueStep& candidate = conversation->steps[i];
                if (candidate.kind == DialogueStepKind::Line && candidate.lineId == line)
                {
                    step = i;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                LOG_WARNING(u8"Dialogue", u8"resume: the conversation `{}` changed since the save; it starts again",
                            conversation->id);
            }
        }
        Begin(Move(file), dialogue, *conversation, step);
        m_lastLine = String(line);
        Pump();
        return true;
    }

    bool DialogueRunner::Flag(StringView name) { return m_host->ReadFlag(FlagKey(name).AsView()); }

    void DialogueRunner::SetFlag(StringView name, bool on) { m_host->WriteFlag(FlagKey(name).AsView(), on); }

    void DialogueRunner::Update(f32 deltaTime)
    {
        if (!m_playing)
        {
            return;
        }
        if (m_wait == DialogueWait::Time)
        {
            m_timer -= deltaTime;
            if (m_timer > 0.0f)
            {
                return;
            }
        }
        else if (m_wait == DialogueWait::Staging)
        {
            if (m_host->IsStaging(m_staging))
            {
                return;
            }
            m_staging = 0;
        }
        else
        {
            return;
        }
        m_wait = DialogueWait::Nothing;
        Pump();
    }

    void DialogueRunner::OnRunStopping(foundation::run::IRun&)
    {
        // The run is ending: nothing is left to tell, and the resume record stays for the next.
        m_playing = false;
        m_wait = DialogueWait::Nothing;
        m_conversation = nullptr;
        m_file = {};
        m_cast = {};
    }

    u64 DialogueRunner::HashSteps(const DialogueConversation& conversation) noexcept
    {
        u64 hash = kFnv1a64OffsetBasis;
        for (const DialogueStep& step : conversation.steps)
        {
            hash = HashValue(static_cast<u8>(step.kind), hash);
            hash = HashText(step.lineId.AsView(), hash);
            hash = HashText(step.name.AsView(), hash);
            hash = HashText(step.file.AsView(), hash);
            hash = HashText(step.flag.AsView(), hash);
            hash = HashValue(step.negate, hash);
            hash = HashValue(step.next, hash);
            hash = HashValue(step.count, hash);
        }
        return hash;
    }

    void DialogueRunner::Begin(RefPtr<DialogueResource> file, const Guid& dialogue, const DialogueConversation& conversation,
                               usize step)
    {
        if (!file->cast.IsNil() && (!m_file || m_file->cast != file->cast || !m_cast))
        {
            m_cast = m_host->LoadCast(file->cast);
        }
        else if (file->cast.IsNil())
        {
            m_cast = {};
        }
        m_file = Move(file);
        m_dialogue = dialogue;
        m_conversation = &conversation;
        m_step = step;
        m_wait = DialogueWait::Nothing;
        m_staging = 0;
        m_lastLine = {};
        m_playing = true;
    }

    void DialogueRunner::Pump()
    {
        // A host may answer a notice at once (advance a line from inside OnLine): that call moves
        // the runner on and this loop goes on from there, rather than a second loop inside it.
        if (m_pumping)
        {
            return;
        }
        m_pumping = true;
        while (m_playing && m_wait == DialogueWait::Nothing)
        {
            const Array<DialogueStep>& steps = m_conversation->steps;
            if (m_step >= steps.Size())
            {
                Finish(false, false);
                break;
            }
            const DialogueStep& step = steps[m_step];
            switch (step.kind)
            {
            case DialogueStepKind::Line:
            {
                DialogueLineNotice notice;
                notice.dialogue = m_dialogue;
                notice.conversation = m_conversation->id;
                notice.speaker = step.speaker;
                notice.mood = step.mood;
                notice.lineId = step.lineId;
                notice.text = Fill(Text(step.lineId.AsView()).AsView());
                if (const DialogueSpeaker* speaker = m_cast ? m_cast->Find(step.speaker.AsView()) : nullptr)
                {
                    notice.shownName = speaker->shownName;
                    if (notice.mood.IsEmpty() && !speaker->moods.IsEmpty())
                    {
                        notice.mood = speaker->moods[0].name; // the first is the default
                    }
                    const DialogueMood* mood = speaker->FindMood(notice.mood.AsView());
                    notice.portrait = mood != nullptr && !mood->portrait.IsNil() ? mood->portrait : speaker->portrait;
                }
                m_lastLine = step.lineId;
                WriteResume(m_step);
                m_wait = DialogueWait::Line;
                m_host->OnLine(notice);
                break;
            }
            case DialogueStepKind::Choice:
            {
                DialogueChoiceNotice notice;
                notice.dialogue = m_dialogue;
                notice.conversation = m_conversation->id;
                for (u32 a = 1; a <= step.count && m_step + a < steps.Size(); ++a)
                {
                    const DialogueStep& answer = steps[m_step + a];
                    notice.answers.PushBack(Fill(Text(answer.lineId.AsView()).AsView()));
                    notice.lineIds.PushBack(answer.lineId);
                }
                m_wait = DialogueWait::Choice;
                m_host->OnChoice(notice);
                break;
            }
            case DialogueStepKind::Answer: // only a choice reaches its answers' statements
            case DialogueStepKind::Skip:
                if (step.next < 0)
                {
                    Finish(false, false);
                    break;
                }
                m_step = static_cast<usize>(step.next);
                break;
            case DialogueStepKind::If:
                if (Flag(step.flag.AsView()) != step.negate)
                {
                    ++m_step;
                }
                else if (step.next < 0)
                {
                    Finish(false, false);
                }
                else
                {
                    m_step = static_cast<usize>(step.next);
                }
                break;
            case DialogueStepKind::Set:
            case DialogueStepKind::Clear:
                SetFlag(step.flag.AsView(), step.kind == DialogueStepKind::Set);
                ++m_step;
                break;
            case DialogueStepKind::Wait:
                ++m_step;
                if (step.seconds > 0.0f)
                {
                    m_timer = step.seconds;
                    m_wait = DialogueWait::Time;
                }
                break;
            case DialogueStepKind::Play:
            case DialogueStepKind::Sound:
            case DialogueStepKind::Fade:
            case DialogueStepKind::Sequence:
            {
                const u32 token = m_host->Stage(step, m_cast.Get());
                ++m_step;
                // A fade always waits; the others when told to (`and wait`).
                if (token != 0 && (step.waits || step.kind == DialogueStepKind::Fade))
                {
                    m_staging = token;
                    m_wait = DialogueWait::Staging;
                }
                break;
            }
            case DialogueStepKind::Jump:
            {
                RefPtr<DialogueResource> file = step.asset.IsNil() ? m_file : m_host->LoadDialogue(step.asset);
                const DialogueConversation* next = file ? file->Find(step.name.AsView()) : nullptr;
                if (next == nullptr)
                {
                    LOG_WARNING(u8"Dialogue", u8"jump: no conversation `{}` to jump to", step.name);
                    Finish(false, false);
                    break;
                }
                const Guid dialogue = step.asset.IsNil() ? m_dialogue : step.asset;
                Begin(Move(file), dialogue, *next, 0);
                break;
            }
            case DialogueStepKind::Command:
            {
                DialogueCommandNotice notice;
                notice.dialogue = m_dialogue;
                notice.conversation = m_conversation->id;
                notice.name = step.name;
                notice.arguments = step.arguments;
                notice.waits = step.waits && !step.leaves;
                notice.leaves = step.leaves;
                ++m_step;
                if (step.leaves)
                {
                    WriteResume(m_step);
                    m_host->OnCommand(notice);
                    Finish(false, true);
                    break;
                }
                if (notice.waits)
                {
                    m_wait = DialogueWait::Command;
                }
                m_host->OnCommand(notice);
                break;
            }
            case DialogueStepKind::End: Finish(false, false); break;
            }
        }
        m_pumping = false;
    }

    void DialogueRunner::Finish(bool stopped, bool left)
    {
        DialogueEndedNotice notice;
        notice.dialogue = m_dialogue;
        notice.conversation = m_conversation != nullptr ? m_conversation->id : String();
        notice.stopped = stopped;
        notice.left = left;
        // Cleared before the game hears it, so it may play another from its handler.
        m_playing = false;
        m_wait = DialogueWait::Nothing;
        m_conversation = nullptr;
        m_file = {};
        m_staging = 0;
        if (!left)
        {
            m_host->WriteText(kDialogueResumeKey, {});
        }
        m_host->OnEnded(notice);
    }

    void DialogueRunner::WriteResume(usize step)
    {
        const String record = Format(u8"{}|{}|{}|{}|{}", m_dialogue, m_conversation->id, step, m_lastLine,
                                     HashSteps(*m_conversation));
        m_host->WriteText(kDialogueResumeKey, record.AsView());
    }

    String DialogueRunner::FlagKey(StringView name) const
    {
        String key(m_settings->flagPrefix.AsView());
        key.Append(name);
        return key;
    }

    String DialogueRunner::Text(StringView lineId) { return m_file ? String(m_file->Text(lineId)) : String(); }

    String DialogueRunner::Fill(StringView text)
    {
        String out;
        for (usize i = 0; i < text.Size(); ++i)
        {
            const char8_t c = text[i];
            if ((c == u8'{' || c == u8'}') && i + 1 < text.Size() && text[i + 1] == c)
            {
                out.Append(c); // `{{` and `}}` are braces
                ++i;
                continue;
            }
            if (c != u8'{')
            {
                out.Append(c);
                continue;
            }
            const usize close = text.Find(u8'}', i + 1);
            if (close == StringView::kNotFound)
            {
                out.Append(text.SubStr(i, text.Size() - i));
                break;
            }
            const StringView action = text.SubStr(i + 1, close - i - 1);
            const String label = m_host->ActionLabel(action);
            out.Append(label.IsEmpty() ? text.SubStr(i, close - i + 1) : label.AsView());
            i = close;
        }
        return out;
    }
}
