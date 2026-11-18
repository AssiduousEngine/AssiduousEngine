// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :code_tooling partition.
//
// The files an outside code editor reads in a project's Code folder, so it completes and checks
// the scripts against the engine's API. Each language with declarations writes its file
// (AngelScript's as.predefined, Luau's engine.d.luau) from the API its checks use, so the file
// follows the engine and the plugins loaded. For VS Code, .vscode/extensions.json recommends each
// language's extension and .vscode/settings.json points them at the declarations.
//
// The declarations are generated, so they are written whenever they differ. A file a user may edit
// (the VS Code ones) is written when it is missing, and afterwards only while it is still what was
// last written: a ledger in the project's cache keeps the hash of each file written, and a file
// whose hash differs, or that was there before the ledger knew it, is the user's and is left.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.project:code_tooling;

import foundation.core;
import foundation.script;
import script.pipeline;

using namespace foundation::core;

export namespace editor
{
    /// One tooling file: its path under the Code folder (forward slashes), its text, and whether a
    /// user may edit it (then it is not written over their edit).
    struct CodeToolingFile
    {
        String path;
        String text;
        bool userMay = false;
    };

    /// The tooling files for the registered script languages: each one's declarations, and with
    /// `vscode` the two VS Code files. A language without a cook or declarations adds none.
    [[nodiscard]] inline Array<CodeToolingFile> CodeToolingFiles(bool vscode)
    {
        Array<CodeToolingFile> files;
        Array<String> extensions;
        Array<String> settings; // "key": value, one per entry
        for (const foundation::script::ScriptBackendDesc& backend : foundation::script::ScriptBackendRegistry::Get().All())
        {
            if (pipeline::IScriptLanguageCook* cook = pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(backend.languageId.AsView()))
            {
                foundation::script::ScriptDeclarations declarations = cook->Declarations();
                if (!declarations.fileName.IsEmpty())
                    files.PushBack(CodeToolingFile{Move(declarations.fileName), Move(declarations.text), false});
            }
            if (!backend.vscodeExtension.IsEmpty())
                extensions.PushBack(backend.vscodeExtension);
            for (const foundation::script::ScriptBackendDesc::EditorSetting& setting : backend.vscodeSettings)
                settings.PushBack(Format(u8"\"{}\": {}", setting.key, setting.json));
        }
        if (!vscode)
            return files;
        const auto list = [](const Array<String>& items, StringView open, StringView close, StringView indent, bool quoted)
        {
            String text(open);
            for (usize i = 0; i < items.Size(); ++i)
            {
                text.Append(i == 0 ? StringView(u8"\n") : StringView(u8",\n"));
                text.Append(indent);
                if (quoted)
                    text.Append(u8"\"");
                text.Append(items[i].AsView());
                if (quoted)
                    text.Append(u8"\"");
            }
            text.Append(items.IsEmpty() ? StringView{} : StringView(u8"\n"));
            text.Append(close);
            return text;
        };
        files.PushBack(CodeToolingFile{String(u8".vscode/extensions.json"),
                                       Format(u8"{{\n    \"recommendations\": {}\n}}\n", list(extensions, u8"[", u8"    ]", u8"        ", true)), true});
        files.PushBack(CodeToolingFile{String(u8".vscode/settings.json"), list(settings, u8"{", u8"}\n", u8"    ", false), true});
        return files;
    }

    /// Where KeepCodeToolingFiles keeps what it wrote, in a project's cache folder.
    inline constexpr StringView kCodeToolingLedger = u8"code-tooling.txt";

    /// Writes `files` under `codeRoot` as the partition's rules say, keeping their hashes in the
    /// ledger at `ledgerPath`. Returns how many it wrote.
    inline u32 KeepCodeToolingFiles(StringView codeRoot, StringView ledgerPath, Span<const CodeToolingFile> files)
    {
        // The ledger: a line per file written, its hash, a space, its path.
        struct Written
        {
            String path;
            String hash;
        };
        Array<Written> ledger;
        if (Result<Array<byte>> bytes = ReadFile(ledgerPath); bytes.HasValue())
        {
            const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
            {
                if (i < text.Size() && text[i] != utf8char('\n'))
                    continue;
                const StringView line = text.SubStr(start, i - start);
                start = i + 1;
                const usize space = line.Find(utf8char(' '));
                if (space != StringView::kNotFound && space > 0)
                    ledger.PushBack(Written{String(line.SubStr(space + 1, line.Size() - space - 1)), String(line.SubStr(0, space))});
            }
        }
        const auto hashOf = [](StringView text)
        { return Sha256::Of(Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).ToHex(); };

        u32 written = 0;
        bool changed = false;
        for (const CodeToolingFile& file : files)
        {
            const String onDisk = PathJoin(codeRoot, file.path.AsView());
            Result<Array<byte>> there = ReadFile(onDisk.AsView());
            const String wanted = hashOf(file.text.AsView());
            Written* known = nullptr;
            for (Written& entry : ledger)
                if (entry.path == file.path)
                    known = &entry;
            if (there.HasValue())
            {
                const String present = hashOf(StringView(reinterpret_cast<const utf8char*>(there.Value().Data()), there.Value().Size()));
                if (present == wanted)
                {
                    if (known == nullptr)
                    {
                        ledger.PushBack(Written{file.path, wanted}); // as it would have been written
                        changed = true;
                    }
                    continue;
                }
                if (file.userMay && (known == nullptr || known->hash != present))
                    continue; // the user's
            }
            (void)CreateDirectories(PathParent(onDisk.AsView()));
            if (!WriteFile(onDisk.AsView(), Span<const byte>(reinterpret_cast<const byte*>(file.text.Data()), file.text.Size())).IsOk())
            {
                LOG_WARNING(u8"Project", u8"{} could not be written", onDisk);
                continue;
            }
            ++written;
            changed = true;
            if (known != nullptr)
                known->hash = wanted;
            else
                ledger.PushBack(Written{file.path, wanted});
        }
        if (changed)
        {
            String text;
            for (const Written& entry : ledger)
                text.Append(Format(u8"{} {}\n", entry.hash, entry.path).AsView());
            (void)CreateDirectories(PathParent(ledgerPath));
            if (!WriteFile(ledgerPath, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk())
                LOG_WARNING(u8"Project", u8"{} could not be written", ledgerPath);
        }
        return written;
    }
}
