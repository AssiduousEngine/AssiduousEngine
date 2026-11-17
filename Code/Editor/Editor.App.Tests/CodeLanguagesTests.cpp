// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the code languages. Each shared definition (Data/Editor/Code) is read by its id
// the first time it is asked for and then kept; an id with no file finds none and is not read again.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui.code;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace
{
    // The data root's files, as the editor's resources read them; `reads` counts the asks.
    Function<bool(StringView, String&)> ReadData(u32& reads)
    {
        return [&reads](StringView path, String& out)
        {
            ++reads;
            const String full = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(TEST_SOURCE_DIR)), u8"Data").AsView(), path);
            Result<Array<byte>> bytes = ReadFile(full.AsView());
            if (!bytes.HasValue())
                return false;
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            return true;
        };
    }
}

TEST_CASE("code languages: each shared definition read by its id once, an unknown one none")
{
    u32 reads = 0;
    editor::app::CodeLanguages languages(DefaultAllocator(), ReadData(reads));
    constexpr StringView kIds[] = {u8"angelscript", u8"luau", u8"csharp", u8"hlsl", u8"xml", u8"css"};
    for (const StringView id : kIds)
    {
        const gui::CodeLanguage* language = languages.Find(id);
        REQUIRE(language != nullptr);
        CHECK_FALSE(language->Contexts.IsEmpty());
        CHECK(languages.Find(id) == language); // kept: the same object, not read again
    }
    CHECK(reads == 6u);
    CHECK(languages.Find(u8"cobol") == nullptr);
    CHECK(languages.Find(u8"cobol") == nullptr);
    CHECK(reads == 7u); // a miss is kept too
    CHECK(languages.Find(u8"") == nullptr);
}

// C# colours as C# does: its keywords and types, a verbatim string whose backslash is no escape,
// an interpolated string, and a comment.
TEST_CASE("code languages: C# colours its keywords, types, strings and comments")
{
    u32 reads = 0;
    editor::app::CodeLanguages languages(DefaultAllocator(), ReadData(reads));
    const gui::CodeLanguage* csharp = languages.Find(u8"csharp");
    REQUIRE(csharp != nullptr);

    const StringView line = u8"public static string Path = @\"a\\b\" + $\"{x}\"; // done";
    gui::CodeLexState state;
    Array<gui::CodeSpan> spans;
    csharp->Tokenize(line, state, spans);
    const auto tokenAt = [&](StringView text)
    {
        const usize at = line.Find(text);
        REQUIRE(at != StringView::kNotFound);
        for (const gui::CodeSpan& span : spans)
            if (span.Start <= at && at < span.End)
                return span.Token;
        return gui::CodeToken::Text;
    };
    CHECK(tokenAt(u8"public") == gui::CodeToken::Keyword);
    CHECK(tokenAt(u8"static") == gui::CodeToken::Keyword);
    CHECK(tokenAt(u8"string") == gui::CodeToken::Type);
    CHECK(tokenAt(u8"Path") == gui::CodeToken::Text);
    CHECK(tokenAt(u8"\\b") == gui::CodeToken::String); // inside @"...": no escape
    CHECK(tokenAt(u8"{x}") == gui::CodeToken::String);
    CHECK(tokenAt(u8"done") == gui::CodeToken::Comment);
}
