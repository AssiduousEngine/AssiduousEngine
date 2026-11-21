// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue format's parser and the command forms' notation.

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
        constexpr StringView kBuiltIn[] = {u8"conversation", u8"choice", u8"end", u8"if",       u8"else",
                                           u8"set",          u8"clear",  u8"wait", u8"play",    u8"sound",
                                           u8"fade",         u8"sequence", u8"jump"};

        [[nodiscard]] bool IsWordChar(char8_t c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_';
        }

        [[nodiscard]] bool IsWord(StringView text) noexcept
        {
            if (text.IsEmpty())
            {
                return false;
            }
            for (char8_t c : text)
            {
                if (!IsWordChar(c))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool IsSpace(char8_t c) noexcept { return c == u8' ' || c == u8'\t' || c == u8'\r'; }

        // ---- the forms' notation -------------------------------------------------------------

        // A form's tokens: `[`, `]`, an argument `<...>` whole, and words.
        [[nodiscard]] Array<String> FormTokens(StringView text)
        {
            Array<String> tokens;
            usize i = 0;
            while (i < text.Size())
            {
                const char8_t c = text[i];
                if (IsSpace(c))
                {
                    ++i;
                }
                else if (c == u8'[' || c == u8']')
                {
                    tokens.PushBack(String(text.SubStr(i, 1)));
                    ++i;
                }
                else if (c == u8'<')
                {
                    const usize close = text.Find(u8'>', i);
                    const usize end = close == StringView::kNotFound ? text.Size() : close + 1;
                    tokens.PushBack(String(text.SubStr(i, end - i)));
                    i = end;
                }
                else
                {
                    usize end = i;
                    while (end < text.Size() && !IsSpace(text[end]) && text[end] != u8'[' && text[end] != u8']' &&
                           text[end] != u8'<')
                    {
                        ++end;
                    }
                    tokens.PushBack(String(text.SubStr(i, end - i)));
                    i = end;
                }
            }
            return tokens;
        }

        // Between the parentheses of `asset(...)` or `list(...)`; empty when `text` is not `prefix(...)`.
        [[nodiscard]] StringView Parenthesized(StringView text, StringView prefix) noexcept
        {
            if (!text.StartsWith(prefix) || !text.EndsWith(u8")") || text.Size() < prefix.Size() + 2)
            {
                return {};
            }
            return text.SubStr(prefix.Size() + 1, text.Size() - prefix.Size() - 2);
        }

        [[nodiscard]] bool ParseArgument(StringView token, DialogueFormPart& part, String& error)
        {
            if (!token.EndsWith(u8">"))
            {
                error = Format(u8"`{}` is not closed by `>`", token);
                return false;
            }
            const StringView inner = token.SubStr(1, token.Size() - 2);
            const usize colon = inner.Find(u8':');
            if (colon == StringView::kNotFound)
            {
                error = Format(u8"`{}`: an argument is `<name:kind>`", token);
                return false;
            }
            const StringView name = inner.SubStr(0, colon);
            if (!IsWord(name))
            {
                error = Format(u8"`{}`: an argument's name is one word", token);
                return false;
            }
            part.literal = false;
            part.name = String(name);
            StringView kind = inner.SubStr(colon + 1, inner.Size() - colon - 1);
            const usize equals = kind.Find(u8'=');
            if (equals != StringView::kNotFound)
            {
                part.hasDefault = true;
                part.defaultValue = String(kind.SubStr(equals + 1, kind.Size() - equals - 1));
                kind = kind.SubStr(0, equals);
            }
            if (kind == StringView(u8"text"))
            {
                part.kind = DialogueArgumentKind::Text;
            }
            else if (kind == StringView(u8"word"))
            {
                part.kind = DialogueArgumentKind::Word;
            }
            else if (kind == StringView(u8"number"))
            {
                part.kind = DialogueArgumentKind::Number;
            }
            else if (kind == StringView(u8"entity"))
            {
                part.kind = DialogueArgumentKind::Entity;
            }
            else if (kind == StringView(u8"speaker"))
            {
                part.kind = DialogueArgumentKind::Speaker;
            }
            else if (kind == StringView(u8"flag"))
            {
                part.kind = DialogueArgumentKind::Flag;
            }
            else if (const StringView type = Parenthesized(kind, u8"asset"); !type.IsEmpty())
            {
                part.kind = DialogueArgumentKind::Asset;
                part.assetType = String(type);
            }
            else if (const StringView list = Parenthesized(kind, u8"list"); !list.IsEmpty())
            {
                part.kind = DialogueArgumentKind::List;
                part.list = String(list);
            }
            else if (kind.Find(u8'|') != StringView::kNotFound)
            {
                part.kind = DialogueArgumentKind::OneOf;
                usize start = 0;
                while (start <= kind.Size())
                {
                    usize bar = kind.Find(u8'|', start);
                    if (bar == StringView::kNotFound)
                    {
                        bar = kind.Size();
                    }
                    const StringView choice = kind.SubStr(start, bar - start);
                    if (!IsWord(choice))
                    {
                        error = Format(u8"`{}`: each choice is one word", token);
                        return false;
                    }
                    part.choices.PushBack(String(choice));
                    start = bar + 1;
                }
            }
            else
            {
                error = Format(u8"`{}`: no kind `{}` (text, word, number, a|b, entity, speaker, flag, asset(Type), "
                               u8"list(name))",
                               token, kind);
                return false;
            }
            if (part.hasDefault && part.kind == DialogueArgumentKind::Number && !ParseFloat(part.defaultValue.AsView()).HasValue())
            {
                error = Format(u8"`{}`: the default is not a number", token);
                return false;
            }
            return true;
        }

        // A form, built in or declared; a declared one may not take a built-in statement's name.
        [[nodiscard]] bool ParseForm(StringView text, DialogueCommandForm& out, String& error, bool builtIn)
        {
            out = DialogueCommandForm{};
            out.text = String(Trim(text));
            Array<String> tokens = FormTokens(text);
            if (tokens.IsEmpty())
            {
                error = String(u8"a form is empty");
                return false;
            }
            if (tokens.Size() > 1 && (tokens.Back() == StringView(u8"waits") || tokens.Back() == StringView(u8"leaves")))
            {
                out.wait = tokens.Back() == StringView(u8"waits") ? DialogueCommandWait::Always : DialogueCommandWait::Leaves;
                tokens.PopBack();
            }
            u32 group = 0;
            for (const String& token : tokens)
            {
                if (token == StringView(u8"["))
                {
                    if (group != 0)
                    {
                        error = String(u8"an optional part holds no other");
                        return false;
                    }
                    group = ++out.groupCount;
                }
                else if (token == StringView(u8"]"))
                {
                    if (group == 0)
                    {
                        error = String(u8"a `]` closes nothing");
                        return false;
                    }
                    if (out.parts.IsEmpty() || out.parts.Back().group != group)
                    {
                        error = String(u8"an optional part is empty");
                        return false;
                    }
                    group = 0;
                }
                else if (token.AsView().StartsWith(u8"<"))
                {
                    DialogueFormPart part;
                    if (!ParseArgument(token.AsView(), part, error))
                    {
                        return false;
                    }
                    part.group = group;
                    if (part.hasDefault && group == 0)
                    {
                        error = Format(u8"`{}`: only an optional part's argument has a default", token);
                        return false;
                    }
                    for (const DialogueFormPart& other : out.parts)
                    {
                        if (!other.literal && other.name == part.name)
                        {
                            error = Format(u8"two arguments are named `{}`", part.name);
                            return false;
                        }
                    }
                    out.parts.PushBack(Move(part));
                }
                else
                {
                    DialogueFormPart part;
                    part.literal = true;
                    part.word = token;
                    part.group = group;
                    out.parts.PushBack(Move(part));
                }
            }
            if (group != 0)
            {
                error = String(u8"a `[` is not closed");
                return false;
            }
            for (const DialogueFormPart& part : out.parts)
            {
                if (!part.literal || part.group != 0)
                {
                    break;
                }
                if (!out.name.IsEmpty())
                {
                    out.name.Append(u8' ');
                }
                out.name.Append(part.word.AsView());
            }
            if (out.name.IsEmpty())
            {
                error = String(u8"a form starts with its name, a word");
                return false;
            }
            const StringView first = out.parts[0].word.AsView();
            if (!builtIn && (IsBuiltInStatement(first) || first.StartsWith(u8"@") || first.StartsWith(u8">") ||
                             first.StartsWith(u8"#")))
            {
                error = Format(u8"`{}` starts a statement of the format's own", first);
                return false;
            }
            for (u32 g = 1; g <= out.groupCount; ++g)
            {
                Array<const DialogueFormPart*> inGroup;
                for (const DialogueFormPart& part : out.parts)
                {
                    if (part.group == g)
                    {
                        inGroup.PushBack(&part);
                    }
                }
                if (inGroup.Size() == 2 && inGroup[0]->literal && inGroup[1]->literal &&
                    inGroup[0]->word == StringView(u8"and") && inGroup[1]->word == StringView(u8"wait"))
                {
                    out.waitGroup = g;
                }
            }
            if (out.waitGroup != 0 && out.wait != DialogueCommandWait::Never)
            {
                error = String(u8"a command that always waits or leaves takes no `[and wait]`");
                return false;
            }
            return true;
        }

        // ---- matching a statement to a form ---------------------------------------------------

        struct Word
        {
            String text;
            bool quoted = false;
        };

        // A statement's words; a "quoted name" is one word. False for an unclosed quote.
        [[nodiscard]] bool StatementWords(StringView text, Array<Word>& out)
        {
            usize i = 0;
            while (i < text.Size())
            {
                if (IsSpace(text[i]))
                {
                    ++i;
                    continue;
                }
                if (text[i] == u8'"')
                {
                    const usize close = text.Find(u8'"', i + 1);
                    if (close == StringView::kNotFound)
                    {
                        return false;
                    }
                    out.PushBack(Word{String(text.SubStr(i + 1, close - i - 1)), true});
                    i = close + 1;
                    continue;
                }
                usize end = i;
                while (end < text.Size() && !IsSpace(text[end]))
                {
                    ++end;
                }
                out.PushBack(Word{String(text.SubStr(i, end - i)), false});
                i = end;
            }
            return true;
        }

        [[nodiscard]] bool SingleWord(DialogueArgumentKind kind) noexcept
        {
            return kind == DialogueArgumentKind::Word || kind == DialogueArgumentKind::Number ||
                   kind == DialogueArgumentKind::OneOf || kind == DialogueArgumentKind::Speaker ||
                   kind == DialogueArgumentKind::Flag || kind == DialogueArgumentKind::Asset;
        }

        struct MatchState
        {
            Array<DialogueArgument> arguments; // one per argument part, in order
            Array<u8> taken;                   // per optional group (index = group)
        };

        // Matches a statement's words against a form: an argument takes the fewest words that let
        // the rest match, so it runs to the next literal word; an optional part is taken when it
        // can be.
        class FormMatcher
        {
        public:
            FormMatcher(const DialogueCommandForm& form, const Array<Word>& words) : m_form(form), m_words(words)
            {
                for (usize p = 0; p < form.parts.Size(); ++p)
                {
                    m_slot.PushBack(form.parts[p].literal ? -1 : static_cast<i32>(m_argumentCount++));
                }
            }

            [[nodiscard]] bool Run(MatchState& state)
            {
                state.arguments.Clear();
                state.arguments.Resize(m_argumentCount);
                state.taken.Clear();
                state.taken.Resize(m_form.groupCount + 1);
                for (usize p = 0; p < m_form.parts.Size(); ++p)
                {
                    if (m_slot[p] >= 0)
                    {
                        state.arguments[static_cast<usize>(m_slot[p])].name = m_form.parts[p].name;
                    }
                }
                return Match(1, state); // the first word, the name's first, matched already
            }

        private:
            [[nodiscard]] bool Match(usize at, MatchState& state) { return MatchFrom(1, at, state); }

            [[nodiscard]] bool MatchFrom(usize part, usize at, MatchState& state)
            {
                const Array<DialogueFormPart>& parts = m_form.parts;
                if (part == parts.Size())
                {
                    return at == m_words.Size();
                }
                const DialogueFormPart& here = parts[part];
                if (here.group != 0 && parts[part - 1].group != here.group)
                {
                    const MatchState saved = state;
                    state.taken[here.group] = 1;
                    if (MatchPart(part, at, state))
                    {
                        return true;
                    }
                    state = saved;
                    usize after = part;
                    while (after < parts.Size() && parts[after].group == here.group)
                    {
                        ++after;
                    }
                    return MatchFrom(after, at, state);
                }
                return MatchPart(part, at, state);
            }

            [[nodiscard]] bool MatchPart(usize part, usize at, MatchState& state)
            {
                const DialogueFormPart& here = m_form.parts[part];
                if (at >= m_words.Size())
                {
                    return false;
                }
                if (here.literal)
                {
                    return !m_words[at].quoted && m_words[at].text == here.word && MatchFrom(part + 1, at + 1, state);
                }
                usize most = 1;
                if (!SingleWord(here.kind) && !m_words[at].quoted)
                {
                    while (at + most < m_words.Size() && !m_words[at + most].quoted)
                    {
                        ++most;
                    }
                }
                for (usize count = 1; count <= most; ++count)
                {
                    String text;
                    for (usize w = at; w < at + count; ++w)
                    {
                        if (w > at)
                        {
                            text.Append(u8' ');
                        }
                        text.Append(m_words[w].text.AsView());
                    }
                    f32 number = 0.0f;
                    if (!Accepts(here, text.AsView(), number))
                    {
                        continue;
                    }
                    const MatchState saved = state;
                    DialogueArgument& argument = state.arguments[static_cast<usize>(m_slot[part])];
                    argument.text = Move(text);
                    argument.number = number;
                    argument.present = true;
                    if (MatchFrom(part + 1, at + count, state))
                    {
                        return true;
                    }
                    state = saved;
                }
                return false;
            }

            [[nodiscard]] static bool Accepts(const DialogueFormPart& part, StringView text, f32& number)
            {
                switch (part.kind)
                {
                case DialogueArgumentKind::Number:
                {
                    const Optional<f64> value = ParseFloat(text);
                    if (!value.HasValue())
                    {
                        return false;
                    }
                    number = static_cast<f32>(value.Value());
                    return true;
                }
                case DialogueArgumentKind::OneOf:
                    for (const String& choice : part.choices)
                    {
                        if (choice == text)
                        {
                            return true;
                        }
                    }
                    return false;
                case DialogueArgumentKind::Speaker:
                case DialogueArgumentKind::Flag:
                case DialogueArgumentKind::Asset: return IsWord(text);
                default: return true;
                }
            }

            const DialogueCommandForm& m_form;
            const Array<Word>& m_words;
            Array<i32> m_slot; // per part: its argument's index, -1 for a literal
            usize m_argumentCount = 0;
        };

        // A matched statement's arguments: the defaults of what was left out, and one present-or-not
        // argument per optional part of literal words alone (`[running]`), named by its words.
        [[nodiscard]] Array<DialogueArgument> Arguments(const DialogueCommandForm& form, MatchState& state)
        {
            Array<DialogueArgument> out;
            usize slot = 0;
            for (const DialogueFormPart& part : form.parts)
            {
                if (part.literal)
                {
                    continue;
                }
                DialogueArgument argument = Move(state.arguments[slot++]);
                if (!argument.present && part.hasDefault)
                {
                    argument.text = part.defaultValue;
                    const Optional<f64> value = ParseFloat(part.defaultValue.AsView());
                    argument.number = value.HasValue() ? static_cast<f32>(value.Value()) : 0.0f;
                }
                out.PushBack(Move(argument));
            }
            for (u32 g = 1; g <= form.groupCount; ++g)
            {
                if (g == form.waitGroup)
                {
                    continue;
                }
                bool literalsOnly = true;
                String name;
                for (const DialogueFormPart& part : form.parts)
                {
                    if (part.group != g)
                    {
                        continue;
                    }
                    literalsOnly = literalsOnly && part.literal;
                    if (!name.IsEmpty())
                    {
                        name.Append(u8' ');
                    }
                    name.Append(part.word.AsView());
                }
                if (literalsOnly)
                {
                    DialogueArgument presence;
                    presence.name = Move(name);
                    presence.present = state.taken[g] != 0;
                    out.PushBack(Move(presence));
                }
            }
            return out;
        }

        [[nodiscard]] const DialogueArgument* Find(const Array<DialogueArgument>& arguments, StringView name)
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

        // `#RRGGBB` or `#RRGGBBAA`.
        [[nodiscard]] bool ParseColor(StringView text, Float4& out)
        {
            if (!text.StartsWith(u8"#") || (text.Size() != 7 && text.Size() != 9))
            {
                return false;
            }
            f32 channels[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (usize c = 0; c < (text.Size() - 1) / 2; ++c)
            {
                u32 value = 0;
                for (usize d = 0; d < 2; ++d)
                {
                    const char8_t h = text[1 + c * 2 + d];
                    u32 digit = 0;
                    if (h >= u8'0' && h <= u8'9')
                    {
                        digit = static_cast<u32>(h - u8'0');
                    }
                    else if (h >= u8'a' && h <= u8'f')
                    {
                        digit = static_cast<u32>(h - u8'a' + 10);
                    }
                    else if (h >= u8'A' && h <= u8'F')
                    {
                        digit = static_cast<u32>(h - u8'A' + 10);
                    }
                    else
                    {
                        return false;
                    }
                    value = value * 16 + digit;
                }
                channels[c] = static_cast<f32>(value) / 255.0f;
            }
            out = Float4{channels[0], channels[1], channels[2], channels[3]};
            return true;
        }

        // The built-in statements that take arguments, as forms.
        struct BuiltInForms
        {
            DialogueCommandForm wait, set, clear, play, sound, fade, sequence, jump;

            BuiltInForms()
            {
                String error;
                (void)ParseForm(u8"wait <seconds:number>", wait, error, true);
                (void)ParseForm(u8"set <flag:flag>", set, error, true);
                (void)ParseForm(u8"clear <flag:flag>", clear, error, true);
                (void)ParseForm(u8"play <speaker:speaker> <clip:text> [and wait]", play, error, true);
                (void)ParseForm(u8"sound <sound:asset(Sound)> [at <speaker:speaker>]", sound, error, true);
                (void)ParseForm(u8"fade <direction:in|out> [for <seconds:number>] [<color:word>]", fade, error, true);
                (void)ParseForm(u8"sequence <sequence:asset(Sequence)> [and wait]", sequence, error, true);
                (void)ParseForm(u8"jump <conversation:word>", jump, error, true);
            }

            [[nodiscard]] const DialogueCommandForm* Find(StringView word) const noexcept
            {
                const DialogueCommandForm* const all[] = {&wait, &set, &clear, &play, &sound, &fade, &sequence, &jump};
                for (const DialogueCommandForm* form : all)
                {
                    if (form->name == word)
                    {
                        return form;
                    }
                }
                return nullptr;
            }
        };

        [[nodiscard]] const BuiltInForms& BuiltIns()
        {
            static const BuiltInForms forms;
            return forms;
        }

        // ---- the structure ----------------------------------------------------------------------

        struct SourceLine
        {
            u32 number = 0;
            StringView text; // trimmed
        };

        [[nodiscard]] StringView FirstWord(StringView text) noexcept
        {
            usize end = 0;
            while (end < text.Size() && !IsSpace(text[end]))
            {
                ++end;
            }
            return text.SubStr(0, end);
        }

        // A line's or an answer's text and its explicit `#line:<id>`, when it ends with one.
        void SplitLineId(StringView text, StringView& body, StringView& id)
        {
            body = text;
            id = {};
            constexpr StringView kTag = u8"#line:";
            usize at = StringView::kNotFound;
            for (usize from = 0;;)
            {
                const usize found = text.Find(kTag, from);
                if (found == StringView::kNotFound)
                {
                    break;
                }
                at = found;
                from = found + 1;
            }
            if (at == StringView::kNotFound)
            {
                return;
            }
            const StringView candidate = text.SubStr(at + kTag.Size(), text.Size() - at - kTag.Size());
            if (candidate.IsEmpty())
            {
                return;
            }
            for (char8_t c : candidate)
            {
                if (!IsWordChar(c) && c != u8'.')
                {
                    return; // not an id at the very end: text
                }
            }
            id = candidate;
            body = Trim(text.SubStr(0, at));
        }

        class Parser
        {
        public:
            Parser(StringView text, Span<const DialogueCommandForm> commands, DialogueParse& out)
                : m_commands(commands), m_out(out)
            {
                u32 number = 0;
                usize start = 0;
                while (start <= text.Size())
                {
                    usize end = text.Find(u8'\n', start);
                    if (end == StringView::kNotFound)
                    {
                        end = text.Size();
                    }
                    ++number;
                    const StringView line = Trim(text.SubStr(start, end - start));
                    if (!line.IsEmpty() && !line.StartsWith(u8"#"))
                    {
                        m_lines.PushBack(SourceLine{number, line});
                    }
                    start = end + 1;
                }
            }

            void Run()
            {
                bool strayTold = false;
                while (m_at < m_lines.Size())
                {
                    const SourceLine& line = m_lines[m_at];
                    if (FirstWord(line.text) != StringView(u8"conversation"))
                    {
                        if (!strayTold)
                        {
                            Problem(line.number, String(u8"a statement before any `conversation <id>`"));
                            strayTold = true;
                        }
                        ++m_at;
                        continue;
                    }
                    strayTold = false;
                    Conversation(line);
                }
            }

        private:
            void Problem(u32 line, String message, bool warning = false)
            {
                m_out.problems.PushBack(DialogueProblem{line, warning, Move(message)});
            }

            void Conversation(const SourceLine& header)
            {
                ++m_at;
                Array<Word> words;
                (void)StatementWords(header.text, words);
                DialogueConversation conversation;
                if (words.Size() < 2 || !IsWord(words[1].text.AsView()) ||
                    (words.Size() > 2 && (words[2].text != StringView(u8"in") || words.Size() < 4)))
                {
                    Problem(header.number, String(u8"a conversation is `conversation <id> [in <level>]`"));
                }
                else
                {
                    conversation.id = words[1].text;
                    for (usize w = 3; w < words.Size(); ++w)
                    {
                        if (w > 3)
                        {
                            conversation.level.Append(u8' ');
                        }
                        conversation.level.Append(words[w].text.AsView());
                    }
                    for (const DialogueConversation& other : m_out.conversations)
                    {
                        if (other.id == conversation.id)
                        {
                            Problem(header.number, Format(u8"a second conversation `{}`", conversation.id));
                        }
                    }
                }
                m_conversation = &conversation;
                m_ordinal = 0;
                (void)Block(conversation.steps, false, false, false);
                DialogueStep end;
                end.kind = DialogueStepKind::End;
                end.sourceLine = header.number;
                conversation.steps.PushBack(Move(end));
                m_conversation = nullptr;
                m_out.conversations.PushBack(Move(conversation));
            }

            // Statements into `steps` until a word the caller waits for (`>`, `else`, `end`), the
            // next conversation or the end. Returns the word it stopped at, empty for neither.
            StringView Block(Array<DialogueStep>& steps, bool stopAtAnswer, bool stopAtElse, bool stopAtEnd)
            {
                while (m_at < m_lines.Size())
                {
                    const SourceLine& line = m_lines[m_at];
                    const StringView first = FirstWord(line.text);
                    if (first == StringView(u8"conversation"))
                    {
                        return {};
                    }
                    if (line.text.StartsWith(u8">"))
                    {
                        if (stopAtAnswer)
                        {
                            return u8">";
                        }
                        Problem(line.number, String(u8"an answer `>` outside a choice"));
                        ++m_at;
                        continue;
                    }
                    if (first == StringView(u8"else") || first == StringView(u8"end"))
                    {
                        if (line.text != first)
                        {
                            Problem(line.number, Format(u8"`{}` takes nothing after it", first));
                        }
                        if ((first == StringView(u8"else") && stopAtElse) || (first == StringView(u8"end") && stopAtEnd))
                        {
                            return first;
                        }
                        Problem(line.number, first == StringView(u8"else") ? String(u8"an `else` outside an `if`")
                                                                           : String(u8"an `end` with nothing open"));
                        ++m_at;
                        continue;
                    }
                    Statement(line, steps);
                }
                return {};
            }

            void Statement(const SourceLine& line, Array<DialogueStep>& steps)
            {
                if (line.text.StartsWith(u8"@"))
                {
                    LineStatement(line, steps);
                    ++m_at;
                    return;
                }
                const StringView first = FirstWord(line.text);
                if (first == StringView(u8"choice"))
                {
                    Choice(line, steps);
                    return;
                }
                if (first == StringView(u8"if"))
                {
                    If(line, steps);
                    return;
                }
                ++m_at;
                Array<Word> words;
                if (!StatementWords(line.text, words))
                {
                    Problem(line.number, String(u8"a quote `\"` is not closed"));
                    return;
                }
                if (const DialogueCommandForm* form = BuiltIns().Find(first))
                {
                    BuiltIn(line, *form, words, steps);
                    return;
                }
                if (first == StringView(u8"choice") || IsBuiltInStatement(first))
                {
                    Problem(line.number, Format(u8"`{}` takes nothing after it", first));
                    return;
                }
                Command(line, words, steps);
            }

            [[nodiscard]] String NextLineId(StringView explicitId, u32 line)
            {
                ++m_ordinal;
                String id = explicitId.IsEmpty()
                                ? Format(u8"{}.{}", m_conversation->id, m_ordinal)
                                : String(explicitId);
                for (const DialogueLineText& other : m_out.lines)
                {
                    if (other.id == id)
                    {
                        Problem(line, Format(u8"a second line `#line:{}`", id));
                        break;
                    }
                }
                return id;
            }

            void LineStatement(const SourceLine& line, Array<DialogueStep>& steps)
            {
                const StringView rest = Trim(line.text.SubStr(1, line.text.Size() - 1));
                const usize colon = rest.Find(u8':');
                if (colon == StringView::kNotFound)
                {
                    Problem(line.number, String(u8"a line is `@ <speaker> [mood]: <text>`"));
                    return;
                }
                Array<Word> head;
                (void)StatementWords(rest.SubStr(0, colon), head);
                StringView body;
                StringView id;
                SplitLineId(Trim(rest.SubStr(colon + 1, rest.Size() - colon - 1)), body, id);
                if (head.IsEmpty() || head.Size() > 2 || !IsWord(head[0].text.AsView()) ||
                    (head.Size() == 2 && !IsWord(head[1].text.AsView())))
                {
                    Problem(line.number, String(u8"a line is `@ <speaker> [mood]: <text>`: a speaker and a mood are words"));
                    return;
                }
                if (body.IsEmpty())
                {
                    Problem(line.number, String(u8"a line says nothing"));
                    return;
                }
                DialogueStep step;
                step.kind = DialogueStepKind::Line;
                step.speaker = head[0].text;
                if (head.Size() == 2)
                {
                    step.mood = head[1].text;
                }
                step.lineId = NextLineId(id, line.number);
                step.sourceLine = line.number;
                m_out.lines.PushBack(DialogueLineText{step.lineId, String(body)});
                steps.PushBack(Move(step));
            }

            struct Answer
            {
                DialogueStep step;
                Array<DialogueStep> body;
            };

            // Appends `body` to `steps`, its jumps moved to where it now starts.
            static void Append(Array<DialogueStep>& steps, Array<DialogueStep>& body)
            {
                const i32 offset = static_cast<i32>(steps.Size());
                for (DialogueStep& step : body)
                {
                    if (step.next >= 0)
                    {
                        step.next += offset;
                    }
                    steps.PushBack(Move(step));
                }
            }

            void Choice(const SourceLine& line, Array<DialogueStep>& steps)
            {
                if (line.text != StringView(u8"choice"))
                {
                    Problem(line.number, String(u8"`choice` takes nothing after it"));
                }
                ++m_at;
                Array<Answer> answers;
                StringView stopped;
                while (m_at < m_lines.Size() && m_lines[m_at].text.StartsWith(u8">"))
                {
                    const SourceLine& answerLine = m_lines[m_at];
                    StringView body;
                    StringView id;
                    SplitLineId(Trim(answerLine.text.SubStr(1, answerLine.text.Size() - 1)), body, id);
                    Answer answer;
                    answer.step.kind = DialogueStepKind::Answer;
                    answer.step.sourceLine = answerLine.number;
                    if (body.IsEmpty())
                    {
                        Problem(answerLine.number, String(u8"an answer says nothing"));
                    }
                    answer.step.lineId = NextLineId(id, answerLine.number);
                    m_out.lines.PushBack(DialogueLineText{answer.step.lineId, String(body)});
                    ++m_at;
                    stopped = Block(answer.body, true, false, true);
                    answers.PushBack(Move(answer));
                    if (stopped != StringView(u8">"))
                    {
                        break;
                    }
                }
                if (answers.IsEmpty())
                {
                    Problem(line.number, String(u8"a choice needs `> <answer>` lines"));
                    Array<DialogueStep> skipped; // read on to its `end`, kept out of the conversation
                    stopped = Block(skipped, false, false, true);
                }
                if (stopped == StringView(u8"end"))
                {
                    ++m_at;
                }
                else
                {
                    Problem(line.number, String(u8"the choice opened here is not closed by `end`"));
                }
                DialogueStep choice;
                choice.kind = DialogueStepKind::Choice;
                choice.count = static_cast<u32>(answers.Size());
                choice.sourceLine = line.number;
                steps.PushBack(Move(choice));
                const usize firstAnswer = steps.Size();
                for (Answer& answer : answers)
                {
                    steps.PushBack(answer.step);
                }
                Array<usize> skips;
                for (usize a = 0; a < answers.Size(); ++a)
                {
                    steps[firstAnswer + a].next = static_cast<i32>(steps.Size());
                    Append(steps, answers[a].body);
                    DialogueStep skip;
                    skip.kind = DialogueStepKind::Skip;
                    skip.sourceLine = line.number;
                    skips.PushBack(steps.Size());
                    steps.PushBack(Move(skip));
                }
                for (usize skip : skips)
                {
                    steps[skip].next = static_cast<i32>(steps.Size());
                }
            }

            void If(const SourceLine& line, Array<DialogueStep>& steps)
            {
                ++m_at;
                Array<Word> words;
                (void)StatementWords(line.text, words);
                DialogueStep test;
                test.kind = DialogueStepKind::If;
                test.sourceLine = line.number;
                const bool negate = words.Size() == 3 && words[1].text == StringView(u8"not");
                if ((words.Size() != 2 && !negate) || !IsWord(words.Back().text.AsView()))
                {
                    Problem(line.number, String(u8"an if is `if [not] <flag>`"));
                }
                test.negate = negate;
                test.flag = words.Back().text;
                const usize at = steps.Size();
                steps.PushBack(Move(test));
                StringView stopped = Block(steps, false, true, true);
                if (stopped == StringView(u8"else"))
                {
                    ++m_at;
                    DialogueStep skip;
                    skip.kind = DialogueStepKind::Skip;
                    skip.sourceLine = line.number;
                    const usize skipAt = steps.Size();
                    steps.PushBack(Move(skip));
                    steps[at].next = static_cast<i32>(steps.Size());
                    stopped = Block(steps, false, false, true);
                    steps[skipAt].next = static_cast<i32>(steps.Size());
                }
                else
                {
                    steps[at].next = static_cast<i32>(steps.Size());
                }
                if (stopped == StringView(u8"end"))
                {
                    ++m_at;
                }
                else
                {
                    Problem(line.number, String(u8"the `if` opened here is not closed by `end`"));
                }
            }

            void BuiltIn(const SourceLine& line, const DialogueCommandForm& form, const Array<Word>& words,
                         Array<DialogueStep>& steps)
            {
                MatchState state;
                FormMatcher matcher(form, words);
                if (!matcher.Run(state))
                {
                    Problem(line.number, Format(u8"`{}` is `{}`", form.name, form.text));
                    return;
                }
                const Array<DialogueArgument> arguments = Arguments(form, state);
                DialogueStep step;
                step.sourceLine = line.number;
                const StringView name = form.name.AsView();
                const auto text = [&arguments](StringView argument) -> String
                {
                    const DialogueArgument* found = Find(arguments, argument);
                    return found != nullptr ? found->text : String();
                };
                const auto number = [&arguments](StringView argument) -> f32
                {
                    const DialogueArgument* found = Find(arguments, argument);
                    return found != nullptr ? found->number : 0.0f;
                };
                const bool waits = form.waitGroup != 0 && state.taken[form.waitGroup] != 0;
                if (name == StringView(u8"wait"))
                {
                    step.kind = DialogueStepKind::Wait;
                    step.seconds = number(u8"seconds");
                }
                else if (name == StringView(u8"set") || name == StringView(u8"clear"))
                {
                    step.kind = name == StringView(u8"set") ? DialogueStepKind::Set : DialogueStepKind::Clear;
                    step.flag = text(u8"flag");
                }
                else if (name == StringView(u8"play"))
                {
                    step.kind = DialogueStepKind::Play;
                    step.speaker = text(u8"speaker");
                    step.name = text(u8"clip");
                    step.waits = waits;
                }
                else if (name == StringView(u8"sound"))
                {
                    step.kind = DialogueStepKind::Sound;
                    step.name = text(u8"sound");
                    step.speaker = text(u8"speaker");
                }
                else if (name == StringView(u8"fade"))
                {
                    step.kind = DialogueStepKind::Fade;
                    step.fadeIn = text(u8"direction") == StringView(u8"in");
                    const DialogueArgument* seconds = Find(arguments, u8"seconds");
                    step.seconds = seconds != nullptr && seconds->present ? seconds->number : -1.0f;
                    const DialogueArgument* color = Find(arguments, u8"color");
                    if (color != nullptr && color->present)
                    {
                        if (!ParseColor(color->text.AsView(), step.color))
                        {
                            Problem(line.number, Format(u8"`{}`: a colour is `#RRGGBB` or `#RRGGBBAA`", color->text));
                            return;
                        }
                        step.hasColor = true;
                    }
                }
                else if (name == StringView(u8"sequence"))
                {
                    step.kind = DialogueStepKind::Sequence;
                    step.name = text(u8"sequence");
                    step.waits = waits;
                }
                else if (name == StringView(u8"jump"))
                {
                    step.kind = DialogueStepKind::Jump;
                    step.name = text(u8"conversation");
                    if (!IsWord(step.name.AsView()))
                    {
                        Problem(line.number, String(u8"`jump` names a conversation by its id, a word"));
                        return;
                    }
                }
                steps.PushBack(Move(step));
            }

            void Command(const SourceLine& line, const Array<Word>& words, Array<DialogueStep>& steps)
            {
                const StringView first = words[0].text.AsView();
                const DialogueCommandForm* nearest = nullptr;
                for (const DialogueCommandForm& form : m_commands)
                {
                    if (form.parts.IsEmpty() || !(form.parts[0].word == first) || words[0].quoted)
                    {
                        continue;
                    }
                    if (nearest == nullptr)
                    {
                        nearest = &form;
                    }
                    MatchState state;
                    FormMatcher matcher(form, words);
                    if (!matcher.Run(state))
                    {
                        continue;
                    }
                    DialogueStep step;
                    step.kind = DialogueStepKind::Command;
                    step.sourceLine = line.number;
                    step.name = form.name;
                    step.arguments = Arguments(form, state);
                    step.waits = form.wait == DialogueCommandWait::Always ||
                                 (form.waitGroup != 0 && state.taken[form.waitGroup] != 0);
                    step.leaves = form.wait == DialogueCommandWait::Leaves;
                    steps.PushBack(Move(step));
                    return;
                }
                if (nearest != nullptr)
                {
                    Problem(line.number, Format(u8"no declared command reads this: the nearest is `{}` (a name holding one "
                                                u8"of its words is written in quotes)",
                                                nearest->text));
                }
                else
                {
                    Problem(line.number, Format(u8"`{}` is no statement and no declared command", first));
                }
            }

            Span<const DialogueCommandForm> m_commands;
            DialogueParse& m_out;
            Array<SourceLine> m_lines;
            usize m_at = 0;
            DialogueConversation* m_conversation = nullptr;
            u32 m_ordinal = 0; // the lines and answers of the conversation so far
        };
    }

    bool IsBuiltInStatement(StringView word) noexcept
    {
        for (StringView builtIn : kBuiltIn)
        {
            if (builtIn == word)
            {
                return true;
            }
        }
        return false;
    }

    bool ParseCommandForm(StringView text, DialogueCommandForm& out, String& error)
    {
        return ParseForm(text, out, error, false);
    }

    bool DialogueParse::Ok() const noexcept
    {
        for (const DialogueProblem& problem : problems)
        {
            if (!problem.warning)
            {
                return false;
            }
        }
        return true;
    }

    DialogueParse ParseDialogue(StringView text, Span<const DialogueCommandForm> commands)
    {
        DialogueParse out;
        Parser parser(text, commands, out);
        parser.Run();
        // By line, keeping the order of problems on one line.
        for (usize i = 1; i < out.problems.Size(); ++i)
        {
            for (usize j = i; j > 0 && out.problems[j - 1].line > out.problems[j].line; --j)
            {
                DialogueProblem swap = Move(out.problems[j]);
                out.problems[j] = Move(out.problems[j - 1]);
                out.problems[j - 1] = Move(swap);
            }
        }
        return out;
    }
}
