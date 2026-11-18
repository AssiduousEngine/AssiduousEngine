// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A file that is no asset as plain text: it shows what the file holds; an edit marks the page
// unsaved and Save writes it; Discard Changes reads it again; a line shown moves the cursor there;
// a file that does not read is read-only and never written.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.code;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::TextFilePage;
namespace gui = foundation::gui;

namespace
{
    String ReadText(StringView path)
    {
        Result<Array<byte>> bytes = ReadFile(path);
        return bytes.HasValue() ? String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size())) : String{};
    }
}

TEST_CASE("text file page: shows the file, saves an edit, reads it again, shows a line")
{
    const StringView file = u8"scratch_text_file_page.csproj";
    const char original[] = "<Project>\n  <PropertyGroup/>\n</Project>\n";
    REQUIRE(WriteFile(file, Span<const byte>(reinterpret_cast<const byte*>(original), sizeof(original) - 1)).IsOk());
    {
        TextFilePage page(DefaultAllocator(), file);
        CHECK(page.Title() == file);
        CHECK(page.Service<editor::ICodePage>() != nullptr);
        REQUIRE(page.Code() != nullptr);
        CHECK(page.Code()->GetText() == StringView(u8"<Project>\n  <PropertyGroup/>\n</Project>\n"));
        CHECK_FALSE(page.Code()->IsReadOnly());
        CHECK_FALSE(page.IsDirty());

        page.ShowLine(2, 3);
        REQUIRE(page.Code()->GetSelections().Size() == 1u);
        CHECK(page.Code()->GetSelections()[0].Head.Line == 1u);
        CHECK(page.Code()->GetSelections()[0].Head.Column == 2u);

        page.Code()->Type(u8"x");
        CHECK(page.IsDirty());
        CHECK(page.Save().IsOk());
        CHECK_FALSE(page.IsDirty());
        CHECK(ReadText(file) == page.Code()->GetText());
        CHECK(ReadText(file).AsView().Contains(u8"  x<PropertyGroup/>"));

        page.Code()->Type(u8"y");
        CHECK(page.DiscardChanges());
        CHECK_FALSE(page.IsDirty());
        CHECK(page.Code()->GetText() == ReadText(file));
    }
    (void)FileDelete(file);
}

TEST_CASE("text file page: a file that does not read is read-only and not written")
{
    const StringView file = u8"scratch_text_file_page_missing.luaurc";
    (void)FileDelete(file);
    TextFilePage page(DefaultAllocator(), file);
    CHECK(page.Code()->IsReadOnly());
    CHECK(page.Code()->GetText().IsEmpty());
    CHECK_FALSE(page.Save().IsOk());
    CHECK_FALSE(FileExists(file));
}
