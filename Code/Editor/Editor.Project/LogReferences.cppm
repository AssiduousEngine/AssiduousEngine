// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :log_references partition.
//
// Recognizers turn a log line's text into references for lines logged without them: most of the
// engine's lines, a compiler's output, a stack trace. Each is a matcher over the message that adds
// the references it finds, with the part of the message each stands for. A host keeps one set
// (LogReferenceRecognizers) and runs it when it shows or returns a line; the console then links
// the parts and log_read returns them. Two come with the engine: paths of files in the project,
// with a line and column when the text gives them (`file(12,5)`, `file:12:5`), and the ids of the
// project's assets.

module;
#include "Core/Prelude.h"

export module editor.project:log_references;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    class LogReferenceRecognizers
    {
    public:
        /// Adds what it finds in `message` to `out`.
        using Recognizer = Function<void(StringView message, Array<LogReference>& out)>;

        /// Adds a recognizer; the id removes it.
        u64 Add(Recognizer recognizer)
        {
            m_recognizers.PushBack(Entry{m_nextId, Move(recognizer)});
            return m_nextId++;
        }
        void Remove(u64 id)
        {
            for (usize i = 0; i < m_recognizers.Size(); ++i)
                if (m_recognizers[i].id == id)
                {
                    m_recognizers.RemoveAt(i);
                    return;
                }
        }

        /// What every recognizer finds in `message`, in the order of their parts; where two
        /// overlap, the one that starts first (else the longer) is kept.
        [[nodiscard]] Array<LogReference> Recognize(StringView message) const
        {
            Array<LogReference> found;
            for (const Entry& entry : m_recognizers)
                entry.recognizer(message, found);
            // Insertion sort: few per line.
            for (usize i = 1; i < found.Size(); ++i)
                for (usize k = i; k > 0 && Before(found[k], found[k - 1]); --k)
                {
                    LogReference moved = Move(found[k]);
                    found[k] = Move(found[k - 1]);
                    found[k - 1] = Move(moved);
                }
            Array<LogReference> kept;
            for (LogReference& reference : found)
                if (kept.IsEmpty() || reference.start >= kept.Back().start + kept.Back().length)
                    kept.PushBack(Move(reference));
            return kept;
        }

        /// A line's references: those its log call attached first, then those recognized in it
        /// that overlap none of the attached parts.
        [[nodiscard]] Array<LogReference> Of(StringView message, Span<const LogReference> attached) const
        {
            Array<LogReference> all;
            for (const LogReference& reference : attached)
                all.PushBack(reference);
            for (LogReference& reference : Recognize(message))
            {
                bool overlaps = false;
                for (const LogReference& given : attached)
                    overlaps = overlaps || (given.length > 0 && reference.start < given.start + given.length &&
                                            given.start < reference.start + reference.length);
                if (!overlaps)
                    all.PushBack(Move(reference));
            }
            return all;
        }

    private:
        static bool Before(const LogReference& a, const LogReference& b) noexcept
        {
            return a.start != b.start ? a.start < b.start : a.length > b.length;
        }

        struct Entry
        {
            u64 id = 0;
            Recognizer recognizer;
        };
        Array<Entry> m_recognizers;
        u64 m_nextId = 1;
    };

    namespace detail
    {
        [[nodiscard]] inline bool IsDigit(utf8char c) noexcept { return c >= utf8char('0') && c <= utf8char('9'); }

        // The number at `at`, how many digits it took in `length`; 0 when there is none.
        [[nodiscard]] inline u32 NumberAt(StringView text, usize at, usize& length) noexcept
        {
            u32 value = 0;
            length = 0;
            while (at + length < text.Size() && IsDigit(text[at + length]) && length < 9)
            {
                value = value * 10 + static_cast<u32>(text[at + length] - utf8char('0'));
                ++length;
            }
            return value;
        }

        // Where a path that starts at `start` ends: at whitespace, a quote, a bracket, a comma, or
        // a colon that a number follows (a line).
        [[nodiscard]] inline usize PathEnd(StringView text, usize start) noexcept
        {
            usize end = start;
            while (end < text.Size())
            {
                const utf8char c = text[end];
                if (c == utf8char(' ') || c == utf8char('\t') || c == utf8char('\n') || c == utf8char('"') || c == utf8char('\'') ||
                    c == utf8char('(') || c == utf8char(')') || c == utf8char(',') || c == utf8char('[') || c == utf8char(']') ||
                    c == utf8char('<') || c == utf8char('>'))
                    break;
                if (c == utf8char(':') && end + 1 < text.Size() && IsDigit(text[end + 1]))
                    break;
                ++end;
            }
            // A sentence's full stop is not the path's.
            while (end > start && (text[end - 1] == utf8char('.') || text[end - 1] == utf8char(':') || text[end - 1] == utf8char(';')))
                --end;
            return end;
        }
    }

    /// Paths of files under `projectRoot` (absolute, in either separator), and paths starting
    /// with the project's Code/ folder, which are read under it: each a source reference with the
    /// file's absolute path, and the line and column after it as `(12,5)`, `(12)`, `:12:5` or
    /// `:12` (its part takes them in).
    [[nodiscard]] inline LogReferenceRecognizers::Recognizer SourcePathRecognizer(StringView projectRoot, StringView codeFolder = u8"Code")
    {
        usize rootSize = projectRoot.Size();
        while (rootSize > 0 && (projectRoot[rootSize - 1] == utf8char('/') || projectRoot[rootSize - 1] == utf8char('\\')))
            --rootSize;
        const String root(projectRoot.SubStr(0, rootSize));
        return [root, code = String(codeFolder)](StringView message, Array<LogReference>& out)
        {
            if (root.IsEmpty())
                return;
            const auto sameSeparator = [](utf8char a, utf8char b)
            { return a == b || ((a == utf8char('/') || a == utf8char('\\')) && (b == utf8char('/') || b == utf8char('\\'))); };
            for (usize i = 0; i < message.Size(); ++i)
            {
                // A path starts at the project's root, or at "Code/" where a word starts.
                usize after = 0;
                bool relative = false;
                if (message.Size() - i > root.Size())
                {
                    bool matches = true;
                    for (usize k = 0; k < root.Size() && matches; ++k)
                        matches = sameSeparator(message[i + k], root[k]);
                    if (matches && (message[i + root.Size()] == utf8char('/') || message[i + root.Size()] == utf8char('\\')))
                        after = i + root.Size() + 1;
                }
                if (after == 0 && (i == 0 || message[i - 1] == utf8char(' ') || message[i - 1] == utf8char('\'') || message[i - 1] == utf8char('"') ||
                                   message[i - 1] == utf8char('(')))
                {
                    const StringView rest = message.SubStr(i, message.Size() - i);
                    if (rest.Size() > code.Size() + 1 && rest.StartsWith(code.AsView()) &&
                        (rest[code.Size()] == utf8char('/') || rest[code.Size()] == utf8char('\\')))
                    {
                        after = i + code.Size() + 1;
                        relative = true;
                    }
                }
                if (after == 0)
                    continue;
                const usize end = detail::PathEnd(message, after);
                if (end <= after)
                    continue;
                String file = relative ? PathJoin(root.AsView(), message.SubStr(i, end - i)) : String(message.SubStr(i, end - i));
                for (usize k = 0; k < file.Size(); ++k)
                    if (file[k] == utf8char('\\'))
                        file[k] = utf8char('/');
                // The line and column: (12,5), (12), :12:5 or :12.
                u32 line = 0;
                u32 column = 0;
                usize stop = end;
                usize digits = 0;
                if (end < message.Size() && message[end] == utf8char('('))
                {
                    line = detail::NumberAt(message, end + 1, digits);
                    usize at = end + 1 + digits;
                    if (digits > 0 && at < message.Size() && message[at] == utf8char(','))
                    {
                        usize more = 0;
                        column = detail::NumberAt(message, at + 1, more);
                        at += 1 + more;
                    }
                    if (digits > 0 && at < message.Size() && message[at] == utf8char(')'))
                        stop = at + 1;
                    else
                        line = column = 0;
                }
                else if (end < message.Size() && message[end] == utf8char(':'))
                {
                    line = detail::NumberAt(message, end + 1, digits);
                    stop = end + 1 + digits;
                    if (stop < message.Size() && message[stop] == utf8char(':'))
                    {
                        usize more = 0;
                        column = detail::NumberAt(message, stop + 1, more);
                        if (more > 0)
                            stop += 1 + more;
                    }
                }
                LogReference reference = SourceReference(file.AsView(), line, column);
                reference.start = i;
                reference.length = stop - i;
                out.PushBack(Move(reference));
                i = stop - 1;
            }
        };
    }

    /// Asset ids in their text form (with or without braces) that `known` says are the project's
    /// assets: each an asset reference.
    [[nodiscard]] inline LogReferenceRecognizers::Recognizer AssetIdRecognizer(Function<bool(const Guid&)> known)
    {
        return [known = Move(known)](StringView message, Array<LogReference>& out)
        {
            constexpr usize kLength = 36;
            for (usize i = 0; i + kLength <= message.Size(); ++i)
            {
                // A guid's dashes sit at 8, 13, 18 and 23; check those first to skip most text.
                if (message[i + 8] != utf8char('-') || message[i + 13] != utf8char('-') || message[i + 18] != utf8char('-') ||
                    message[i + 23] != utf8char('-'))
                    continue;
                Guid id;
                if (!Guid::TryParse(message.SubStr(i, kLength), id) || !known || !known(id))
                    continue;
                const bool braced = i > 0 && message[i - 1] == utf8char('{') && i + kLength < message.Size() && message[i + kLength] == utf8char('}');
                LogReference reference = AssetReference(id);
                reference.start = braced ? i - 1 : i;
                reference.length = braced ? kLength + 2 : kLength;
                out.PushBack(Move(reference));
                i += kLength - 1;
            }
        };
    }
}
