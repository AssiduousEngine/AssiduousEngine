// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The code editor preference: which commands it names, how a command splits into a program and
// its arguments, and how a location fills its placeholders.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import editor.core;

using namespace foundation::core;
using namespace editor;

TEST_CASE("code-editor: a command splits at spaces outside double quotes")
{
    const Array<String> parts = SplitCommandLine(u8"  \"C:/Program Files/Ed/ed.exe\"  -g \"{file}\":{line}\t{folder} ");
    REQUIRE(parts.Size() == 4);
    CHECK(parts[0] == StringView(u8"C:/Program Files/Ed/ed.exe"));
    CHECK(parts[1] == StringView(u8"-g"));
    CHECK(parts[2] == StringView(u8"{file}:{line}"));
    CHECK(parts[3] == StringView(u8"{folder}"));
    CHECK(SplitCommandLine(u8"   ").IsEmpty());
    // An empty quoted argument is still an argument.
    const Array<String> empty = SplitCommandLine(u8"ed \"\"");
    REQUIRE(empty.Size() == 2);
    CHECK(empty[1].IsEmpty());
}

TEST_CASE("code-editor: placeholders fill each argument, a path with spaces staying one")
{
    CodeLocation at;
    at.file = u8"/home/me/My Game/Code/Scripts/Player.cs";
    at.line = 12;
    at.column = 5;
    at.folder = u8"/home/me/My Game/Code";
    const Array<String> parts = ExpandCodeEditorCommand(u8"code {folder} --goto {file}:{line}:{column}", at);
    REQUIRE(parts.Size() == 4);
    CHECK(parts[0] == StringView(u8"code"));
    CHECK(parts[1] == StringView(u8"/home/me/My Game/Code"));
    CHECK(parts[2] == StringView(u8"--goto"));
    CHECK(parts[3] == StringView(u8"/home/me/My Game/Code/Scripts/Player.cs:12:5"));

    // No line or column opens at the start; an unknown brace is left as written.
    CodeLocation start;
    start.file = u8"a.luau";
    const Array<String> plain = ExpandCodeEditorCommand(u8"ed +{line}:{column} {file} {other}", start);
    REQUIRE(plain.Size() == 4);
    CHECK(plain[1] == StringView(u8"+1:1"));
    CHECK(plain[2] == StringView(u8"a.luau"));
    CHECK(plain[3] == StringView(u8"{other}"));
}

TEST_CASE("code-editor: the preference names a preset's commands, the user's, or none")
{
    CHECK_FALSE(CodeEditorCommandsOf(nullptr).Outside());
    EditorCodeEditorSettings settings;
    CHECK_FALSE(CodeEditorCommandsOf(&settings).Outside()); // built-in by default

    settings.editor = String(u8"vscode");
    const CodeEditorCommands vscode = CodeEditorCommandsOf(&settings);
    CHECK(vscode.Outside());
    CHECK(vscode.fileCommand.AsView().StartsWith(u8"code "));
    CHECK(vscode.folderCommand == StringView(u8"code {folder}"));

    settings.editor = String(u8"command");
    settings.fileCommand = String(u8"  zed {file}:{line}  ");
    const CodeEditorCommands mine = CodeEditorCommandsOf(&settings);
    CHECK(mine.fileCommand == StringView(u8"zed {file}:{line}"));
    CHECK(mine.folderCommand.IsEmpty());

    // A command with nothing in it is the built-in page, as is an editor this build does not know.
    settings.fileCommand = String(u8"   ");
    CHECK_FALSE(CodeEditorCommandsOf(&settings).Outside());
    settings.editor = String(u8"someday");
    CHECK_FALSE(CodeEditorCommandsOf(&settings).Outside());
}
