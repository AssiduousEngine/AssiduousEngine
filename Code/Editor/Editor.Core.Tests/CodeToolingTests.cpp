// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The tooling files an outside code editor reads in the Code folder: a generated file is written
// when it differs; a file a user may edit is written when missing and again only while it is what
// was last written, never over an edit or a file that was there first. And with the languages
// registered, each one's declarations, and VS Code's two files only when asked for.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import editor.core;
#ifdef OPTION_HAS_LUAU
import foundation.script;
import foundation.script.luau;
import script.pipeline;
import script.luau.pipeline;
#endif

using namespace foundation::core;
using namespace editor;

namespace
{
    String ReadText(StringView path)
    {
        Result<Array<byte>> bytes = ReadFile(path);
        return bytes.HasValue() ? String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size())) : String{};
    }
    void WriteText(StringView path, StringView text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }
}

TEST_CASE("code-tooling: generated files follow, a user's edit is kept, a file there first is the user's")
{
    const StringView root = u8"code_tooling_scratch";
    (void)RemoveDirectoryRecursive(root);
    REQUIRE(CreateDirectories(root));
    const String code = PathJoin(root, u8"Code");
    const String ledger = PathJoin(root, u8".cache/code-tooling.txt");

    Array<CodeToolingFile> files;
    files.PushBack(CodeToolingFile{String(u8"engine.d.luau"), String(u8"declare A: number\n"), false});
    files.PushBack(CodeToolingFile{String(u8".vscode/settings.json"), String(u8"{ \"a\": 1 }\n"), true});
    files.PushBack(CodeToolingFile{String(u8".vscode/extensions.json"), String(u8"{ }\n"), true});
    // A user's extensions.json, there before anything was written.
    REQUIRE(CreateDirectories(PathJoin(code.AsView(), u8".vscode").AsView()));
    WriteText(PathJoin(code.AsView(), u8".vscode/extensions.json").AsView(), u8"{ \"mine\": true }\n");

    CHECK(KeepCodeToolingFiles(code.AsView(), ledger.AsView(), files.AsSpan()) == 2u);
    CHECK(ReadText(PathJoin(code.AsView(), u8"engine.d.luau").AsView()) == StringView(u8"declare A: number\n"));
    CHECK(ReadText(PathJoin(code.AsView(), u8".vscode/settings.json").AsView()) == StringView(u8"{ \"a\": 1 }\n"));
    CHECK(ReadText(PathJoin(code.AsView(), u8".vscode/extensions.json").AsView()) == StringView(u8"{ \"mine\": true }\n"));
    CHECK(KeepCodeToolingFiles(code.AsView(), ledger.AsView(), files.AsSpan()) == 0u); // nothing changed

    // The API changed: the declarations and the settings nobody edited are written again.
    files[0].text = String(u8"declare A: number\ndeclare B: string\n");
    files[1].text = String(u8"{ \"a\": 2 }\n");
    CHECK(KeepCodeToolingFiles(code.AsView(), ledger.AsView(), files.AsSpan()) == 2u);
    CHECK(ReadText(PathJoin(code.AsView(), u8".vscode/settings.json").AsView()) == StringView(u8"{ \"a\": 2 }\n"));

    // The user edits both; the declarations are generated and written again, the settings kept.
    WriteText(PathJoin(code.AsView(), u8"engine.d.luau").AsView(), u8"-- edited\n");
    WriteText(PathJoin(code.AsView(), u8".vscode/settings.json").AsView(), u8"{ \"a\": 2, \"mine\": 3 }\n");
    files[1].text = String(u8"{ \"a\": 3 }\n");
    CHECK(KeepCodeToolingFiles(code.AsView(), ledger.AsView(), files.AsSpan()) == 1u);
    CHECK(ReadText(PathJoin(code.AsView(), u8"engine.d.luau").AsView()) == files[0].text);
    CHECK(ReadText(PathJoin(code.AsView(), u8".vscode/settings.json").AsView()) == StringView(u8"{ \"a\": 2, \"mine\": 3 }\n"));

    // A file the user removed is written again.
    REQUIRE(FileDelete(PathJoin(code.AsView(), u8".vscode/settings.json").AsView()));
    CHECK(KeepCodeToolingFiles(code.AsView(), ledger.AsView(), files.AsSpan()) == 1u);
    CHECK(ReadText(PathJoin(code.AsView(), u8".vscode/settings.json").AsView()) == StringView(u8"{ \"a\": 3 }\n"));
    (void)RemoveDirectoryRecursive(root);
}

#ifdef OPTION_HAS_LUAU
TEST_CASE("code-tooling: Luau's declarations, and VS Code's extension and settings only when asked for")
{
    foundation::script::RegisterLuauScriptBackend();
    pipeline::RegisterLuauScriptCook();
    const Array<CodeToolingFile> plain = CodeToolingFiles(false);
    const CodeToolingFile* declarations = nullptr;
    for (const CodeToolingFile& file : plain)
    {
        CHECK_FALSE(file.path.AsView().StartsWith(u8".vscode/"));
        if (file.path == StringView(u8"engine.d.luau"))
            declarations = &file;
    }
    REQUIRE(declarations != nullptr);
    CHECK_FALSE(declarations->userMay);
    CHECK(declarations->text.AsView().Contains(u8"Engine API declarations"));

    const Array<CodeToolingFile> vscode = CodeToolingFiles(true);
    const CodeToolingFile* extensions = nullptr;
    const CodeToolingFile* settings = nullptr;
    for (const CodeToolingFile& file : vscode)
    {
        if (file.path == StringView(u8".vscode/extensions.json"))
            extensions = &file;
        if (file.path == StringView(u8".vscode/settings.json"))
            settings = &file;
    }
    REQUIRE(extensions != nullptr);
    REQUIRE(settings != nullptr);
    CHECK(extensions->userMay);
    CHECK(extensions->text.AsView().Contains(u8"\"JohnnyMorganz.luau-lsp\""));
    CHECK(settings->text.AsView().Contains(u8"\"luau-lsp.types.definitionFiles\": { \"@engine\": \"engine.d.luau\" }"));
    CHECK(settings->text.AsView().Contains(u8"\"luau-lsp.platform.type\": \"standard\""));
}
#endif
