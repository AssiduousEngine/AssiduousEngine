// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cook's checks over a parsed dialogue file.

module;

#include "Core/Prelude.h"

module dialogue.pipeline;

import foundation.core;
import dialogue.engine;

using namespace foundation::core;

namespace dialogue
{
    namespace
    {
        [[nodiscard]] bool Contains(Span<const String> list, StringView value) noexcept
        {
            for (const String& item : list)
            {
                if (item == value)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool Contains(const Array<String>& list, StringView value) noexcept
        {
            return Contains(Span<const String>(list.Data(), list.Size()), value);
        }

        [[nodiscard]] String Joined(const Array<String>& words)
        {
            String out;
            for (const String& word : words)
            {
                if (!out.IsEmpty())
                {
                    out.Append(u8", ");
                }
                out.Append(word.AsView());
            }
            return out;
        }

        class Checker
        {
        public:
            Checker(DialogueParse& parse, const DialogueCheckInputs& inputs) : m_parse(parse), m_inputs(inputs)
            {
                CollectDialogueFacts(parse, m_conversations, m_flagsSet, m_flagsRead);
            }

            void Run()
            {
                for (DialogueConversation& conversation : m_parse.conversations)
                {
                    const u32 header = conversation.steps.IsEmpty() ? 0u : conversation.steps.Back().sourceLine;
                    Level(conversation, header);
                    m_entitiesTold.Clear();
                    for (DialogueStep& step : conversation.steps)
                    {
                        Step(step);
                    }
                    Reachability(conversation);
                    m_entities = nullptr;
                }
            }

        private:
            void Problem(u32 line, String message, bool warning = false)
            {
                m_parse.problems.PushBack(DialogueProblem{line, warning, Move(message)});
            }

            // The speaker `id`, or null with the problem said (once, when there is no cast).
            const DialogueSpeaker* Speaker(StringView id, u32 line)
            {
                if (m_inputs.cast == nullptr)
                {
                    if (!m_noCastTold)
                    {
                        Problem(line, String(u8"the dialogue file names no cast"));
                        m_noCastTold = true;
                    }
                    return nullptr;
                }
                for (const DialogueSpeaker& speaker : *m_inputs.cast)
                {
                    if (speaker.id == id)
                    {
                        return &speaker;
                    }
                }
                Problem(line, Format(u8"no speaker `{}` in the cast", id));
                return nullptr;
            }

            // The dialogue asset `name`, when it is of one of `types`; nil, with the problem said,
            // otherwise.
            [[nodiscard]] Guid Asset(StringView name, Span<const StringView> types, StringView what, u32 line)
            {
                if (m_inputs.dialogueAssets == nullptr)
                {
                    if (!m_noAssetsTold)
                    {
                        Problem(line, String(u8"the dialogue file names no dialogue assets"));
                        m_noAssetsTold = true;
                    }
                    return {};
                }
                for (const DialogueAssetEntry& entry : *m_inputs.dialogueAssets)
                {
                    if (entry.name != name)
                    {
                        continue;
                    }
                    if (m_inputs.assets == nullptr)
                    {
                        return entry.asset;
                    }
                    const String type = m_inputs.assets->TypeOf(entry.asset);
                    if (type.IsEmpty())
                    {
                        Problem(line, Format(u8"the dialogue asset `{}` refers to no asset", name));
                        return {};
                    }
                    for (StringView accepted : types)
                    {
                        if (type == accepted)
                        {
                            return entry.asset;
                        }
                    }
                    Problem(line, Format(u8"the dialogue asset `{}` is of type {}, which is no {}", name, type, what));
                    return {};
                }
                Problem(line, Format(u8"no dialogue asset `{}`", name));
                return {};
            }

            // The level a conversation names: its entities, which later steps check their names against.
            void Level(const DialogueConversation& conversation, u32 header)
            {
                m_entities = nullptr;
                m_levelName = conversation.level.AsView();
                if (conversation.level.IsEmpty())
                {
                    return;
                }
                const Guid level = Asset(conversation.level.AsView(), Span<const StringView>(kLevelAssetTypes), u8"level", header);
                if (level != Guid{} && m_inputs.levels != nullptr)
                {
                    m_entities = m_inputs.levels->EntityNames(level);
                }
            }

            // A warning when the conversation's level has no entity `name`: one spawned at play is
            // not in the scene, so it may be there when the conversation plays.
            void Entity(StringView name, StringView whose, u32 line)
            {
                if (m_entities == nullptr || name.IsEmpty() || Contains(*m_entities, name) || Contains(m_entitiesTold, name))
                {
                    return;
                }
                m_entitiesTold.PushBack(String(name));
                Problem(line,
                        Format(u8"no entity `{}` ({}) in the level `{}`, unless it is spawned at play", name, whose, m_levelName),
                        true);
            }

            // The actions a line's or an answer's text names (`{Interact}`; `{{` is a brace).
            void Actions(StringView id, u32 line)
            {
                StringView text;
                for (const DialogueLineText& entry : m_parse.lines)
                {
                    if (entry.id == id)
                    {
                        text = entry.text.AsView();
                        break;
                    }
                }
                for (usize i = 0; i < text.Size(); ++i)
                {
                    if (text[i] != u8'{')
                    {
                        continue;
                    }
                    if (i + 1 < text.Size() && text[i + 1] == u8'{')
                    {
                        ++i;
                        continue;
                    }
                    const usize close = text.Find(u8'}', i + 1);
                    if (close == StringView::kNotFound)
                    {
                        Problem(line, String(u8"a `{` with no `}`: an action is `{Name}`, a brace `{{`"));
                        return;
                    }
                    const StringView action = text.SubStr(i + 1, close - i - 1);
                    if (m_inputs.actions == nullptr)
                    {
                        if (!m_noActionsTold)
                        {
                            String braced(u8"{");
                            braced.Append(action);
                            braced.Append(u8'}');
                            Problem(line, Format(u8"`{}` names an action, but the project has no input map", braced));
                            m_noActionsTold = true;
                        }
                    }
                    else if (!Contains(*m_inputs.actions, action))
                    {
                        Problem(line, Format(u8"no action `{}` in the project's input maps", action));
                    }
                    i = close;
                }
            }

            void Step(DialogueStep& step)
            {
                switch (step.kind)
                {
                case DialogueStepKind::Line:
                    if (const DialogueSpeaker* speaker = Speaker(step.speaker.AsView(), step.sourceLine))
                    {
                        Entity(speaker->entity.AsView(), Format(u8"the speaker `{}`'s", speaker->id).AsView(), step.sourceLine);
                        if (!step.mood.IsEmpty() && speaker->FindMood(step.mood.AsView()) == nullptr)
                        {
                            Array<String> moods;
                            for (const DialogueMood& mood : speaker->moods)
                            {
                                moods.PushBack(mood.name);
                            }
                            Problem(step.sourceLine, Format(u8"`{}` has no mood `{}` (has {})", step.speaker, step.mood,
                                                            moods.IsEmpty() ? String(u8"none") : Joined(moods)));
                        }
                    }
                    [[fallthrough]];
                case DialogueStepKind::Answer:
                    Actions(step.lineId.AsView(), step.sourceLine);
                    break;
                case DialogueStepKind::Play:
                    if (const DialogueSpeaker* speaker = Speaker(step.speaker.AsView(), step.sourceLine))
                    {
                        Clip(*speaker, step);
                    }
                    break;
                case DialogueStepKind::Sound:
                    if (!step.speaker.IsEmpty())
                    {
                        (void)Speaker(step.speaker.AsView(), step.sourceLine);
                    }
                    step.asset = Asset(step.name.AsView(), Span<const StringView>(kSoundAssetTypes), u8"sound",
                                       step.sourceLine);
                    break;
                case DialogueStepKind::Sequence:
                    step.asset = Asset(step.name.AsView(), Span<const StringView>(kSequenceAssetTypes), u8"sequence",
                                       step.sourceLine);
                    break;
                case DialogueStepKind::Jump: Jump(step); break;
                case DialogueStepKind::If:
                    if (!Contains(m_flagsSet, step.flag.AsView()) && !Contains(m_inputs.otherFlagsSet, step.flag.AsView()) &&
                        !IsProvidedFlag(step.flag.AsView(), m_inputs.providedFlags))
                    {
                        Problem(step.sourceLine, Format(u8"the flag `{}` is read but nothing sets it, and the settings do not "
                                                        u8"list it as provided",
                                                        step.flag));
                    }
                    break;
                case DialogueStepKind::Command: Command(step); break;
                default: break;
                }
            }

            // A jump within the file, or into the file its dialogue asset names (resolved to the
            // file's guid; nil when the asset is this file).
            void Jump(DialogueStep& step)
            {
                const Array<String>* conversations = &m_conversations;
                if (!step.file.IsEmpty())
                {
                    const Guid file = Asset(step.file.AsView(), Span<const StringView>(kDialogueFileAssetTypes),
                                            u8"dialogue file", step.sourceLine);
                    if (file == Guid{})
                    {
                        return;
                    }
                    if (file != m_inputs.self)
                    {
                        step.asset = file;
                        conversations = nullptr;
                        for (const DialogueFileConversations& other : m_inputs.otherFiles)
                        {
                            if (other.file == file)
                            {
                                conversations = &other.conversations;
                                break;
                            }
                        }
                        if (conversations == nullptr)
                        {
                            return; // a file that does not parse, which its own cook reports
                        }
                    }
                }
                if (!Contains(*conversations, step.name.AsView()))
                {
                    if (step.file.IsEmpty())
                    {
                        Problem(step.sourceLine, Format(u8"no conversation `{}` in this file to jump to", step.name));
                    }
                    else
                    {
                        Problem(step.sourceLine, Format(u8"no conversation `{}` in `{}` to jump to", step.name, step.file));
                    }
                }
            }

            void Clip(const DialogueSpeaker& speaker, DialogueStep& step)
            {
                if (m_inputs.assets == nullptr)
                {
                    return;
                }
                Array<String> names;
                for (const Guid& clip : speaker.clips)
                {
                    String name = m_inputs.assets->NameOf(clip);
                    if (name == step.name)
                    {
                        step.asset = clip;
                        return;
                    }
                    if (!name.IsEmpty())
                    {
                        names.PushBack(Move(name));
                    }
                }
                Problem(step.sourceLine, Format(u8"`{}` has no clip `{}` (has {})", step.speaker, step.name,
                                                names.IsEmpty() ? String(u8"none") : Joined(names)));
            }

            void Command(DialogueStep& step)
            {
                const DialogueCommandForm* form = nullptr;
                for (const DialogueCommandForm& candidate : m_inputs.commands)
                {
                    if (candidate.name == step.name)
                    {
                        form = &candidate;
                        break;
                    }
                }
                if (form == nullptr)
                {
                    return; // the parser matched it, so the file was parsed with other commands
                }
                for (DialogueArgument& argument : step.arguments)
                {
                    if (!argument.present)
                    {
                        continue;
                    }
                    for (const DialogueFormPart& part : form->parts)
                    {
                        if (part.literal || part.name != argument.name)
                        {
                            continue;
                        }
                        if (part.kind == DialogueArgumentKind::Speaker)
                        {
                            (void)Speaker(argument.text.AsView(), step.sourceLine);
                        }
                        else if (part.kind == DialogueArgumentKind::Entity)
                        {
                            Entity(argument.text.AsView(), Format(u8"`{}`'s {}", step.name, argument.name).AsView(),
                                   step.sourceLine);
                        }
                        else if (part.kind == DialogueArgumentKind::Asset)
                        {
                            const StringView type = part.assetType.AsView();
                            argument.asset = Asset(argument.text.AsView(), Span<const StringView>(&type, 1), type, step.sourceLine);
                        }
                    }
                }
            }

            // Statements nothing reaches: after a jump, or behind an answer's skip.
            void Reachability(const DialogueConversation& conversation)
            {
                const Array<DialogueStep>& steps = conversation.steps;
                Array<u8> reached;
                reached.Resize(steps.Size());
                Array<usize> pending;
                pending.PushBack(0);
                const auto visit = [&pending, &steps](i64 at)
                {
                    if (at >= 0 && static_cast<usize>(at) < steps.Size())
                    {
                        pending.PushBack(static_cast<usize>(at));
                    }
                };
                while (!pending.IsEmpty())
                {
                    const usize at = pending.Back();
                    pending.PopBack();
                    if (at >= steps.Size() || reached[at] != 0)
                    {
                        continue;
                    }
                    reached[at] = 1;
                    const DialogueStep& step = steps[at];
                    switch (step.kind)
                    {
                    case DialogueStepKind::Skip:
                    case DialogueStepKind::Answer: visit(step.next); break;
                    case DialogueStepKind::If:
                        visit(step.next);
                        visit(static_cast<i64>(at) + 1);
                        break;
                    case DialogueStepKind::Choice:
                        for (u32 a = 1; a <= step.count; ++a)
                        {
                            visit(static_cast<i64>(at + a));
                        }
                        break;
                    case DialogueStepKind::Jump:
                    case DialogueStepKind::End: break;
                    default: visit(static_cast<i64>(at) + 1); break;
                    }
                }
                u32 told = 0;
                for (usize i = 0; i < steps.Size(); ++i)
                {
                    const DialogueStepKind kind = steps[i].kind;
                    if (reached[i] != 0 || kind == DialogueStepKind::Skip || kind == DialogueStepKind::End ||
                        kind == DialogueStepKind::Answer || steps[i].sourceLine == told)
                    {
                        continue;
                    }
                    told = steps[i].sourceLine;
                    Problem(told, String(u8"nothing reaches this statement"), true);
                }
            }

            DialogueParse& m_parse;
            const DialogueCheckInputs& m_inputs;
            Array<String> m_conversations;
            Array<String> m_flagsSet;
            Array<String> m_flagsRead;
            bool m_noCastTold = false;
            bool m_noActionsTold = false;
            const Array<String>* m_entities = nullptr; // the conversation's level's, when it names one that loads
            StringView m_levelName;
            Array<String> m_entitiesTold;
            bool m_noAssetsTold = false;
        };
    }

    void Serialize(ISerializer& ar, DialogueAssetEntry& entry)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "name", entry.name);
        foundation::core::Serialize(ar, "asset", entry.asset);
        ar.EndObject();
    }

    bool IsProvidedFlag(StringView flag, Span<const String> provided) noexcept
    {
        for (const String& entry : provided)
        {
            const StringView name = entry.AsView();
            if (name.EndsWith(u8"*") ? flag.StartsWith(name.SubStr(0, name.Size() - 1)) : flag == name)
            {
                return true;
            }
        }
        return false;
    }

    void CollectDialogueFacts(const DialogueParse& parse, Array<String>& conversations, Array<String>& flagsSet,
                              Array<String>& flagsRead)
    {
        for (const DialogueConversation& conversation : parse.conversations)
        {
            if (!conversation.id.IsEmpty())
            {
                conversations.PushBack(conversation.id);
            }
            for (const DialogueStep& step : conversation.steps)
            {
                if ((step.kind == DialogueStepKind::Set || step.kind == DialogueStepKind::Clear) &&
                         !Contains(flagsSet, step.flag.AsView()))
                {
                    flagsSet.PushBack(step.flag);
                }
                else if (step.kind == DialogueStepKind::If && !Contains(flagsRead, step.flag.AsView()))
                {
                    flagsRead.PushBack(step.flag);
                }
            }
        }
    }

    void CheckDialogue(DialogueParse& parse, const DialogueCheckInputs& inputs)
    {
        Checker checker(parse, inputs);
        checker.Run();
        for (usize i = 1; i < parse.problems.Size(); ++i)
        {
            for (usize j = i; j > 0 && parse.problems[j - 1].line > parse.problems[j].line; --j)
            {
                DialogueProblem swap = Move(parse.problems[j]);
                parse.problems[j] = Move(parse.problems[j - 1]);
                parse.problems[j - 1] = Move(swap);
            }
        }
    }
}
