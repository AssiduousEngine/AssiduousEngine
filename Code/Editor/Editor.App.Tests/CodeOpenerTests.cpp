// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Where code opens, over a scratch project with one script asset in Code/: the built-in page at the
// line by default; an outside editor's command with the location put in when the preference names
// one; the built-in page, with a warning, when that command does not start; the Code folder in the
// folder command or the file manager; and Open in Built-in Editor whatever the preference.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.settings;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::EditorCodeOpener;
namespace content = foundation::content;

namespace
{
    class OpenerAsset : public ISerializable
    {
        RTTI_OBJECT(OpenerAsset, ISerializable)
    public:
        int value = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "value", value); }
    };

    // A code page that records where it was asked to show.
    class LinePage final : public editor::EditorPage, public editor::ICodePage
    {
    public:
        LinePage() : editor::EditorPage(DefaultAllocator()) { Provide<editor::ICodePage>(*this); }
        [[nodiscard]] StringView Title() const override { return u8"Player"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        void ShowLine(u32 atLine, u32 atColumn) override
        {
            line = atLine;
            column = atColumn;
        }
        u32 line = 0;
        u32 column = 0;
    };

    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        foundation::settings::Settings user{DefaultAllocator()};
        UniquePtr<EditorCodeOpener> opener;
        content::Instance* script = nullptr;
        LinePage page;
        Array<content::Instance*> opened;
        Array<Array<String>> launched; // program, then arguments
        bool launchStarts = true;
        Array<String> shownFolders;
        Array<String> warnings;

        Bed()
        {
            (void)RemoveDirectoryRecursive(u8"code_opener_project");
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"code_opener_project", u8"Opener").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"code_opener_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(OpenerAsset::StaticType());
            RegisterSerializable<OpenerAsset>();
            editor::RegisterEditorSettingsTypes();
            script = project->SourceDb().RootGroup()->CreateInstance(u8"Player", OpenerAsset::StaticType());
            REQUIRE(script != nullptr);
            OpenerAsset object;
            REQUIRE(script->WriteObject(object).IsOk());
            script->SetSourceName(u8"Code/Scripts/Player.cs");
            context.SetProject(project.Get());
            context.SetUserEditorSettings(&user);
            Bed* self = this;
            context.OnNotice = [self](editor::NoticeKind kind, StringView message)
            {
                if (kind == editor::NoticeKind::Warning)
                    self->warnings.PushBack(String(message));
            };
            EditorCodeOpener::Services services;
            services.context = &context;
            services.openPage = [self](content::Instance& instance) -> editor::EditorPage*
            {
                self->opened.PushBack(&instance);
                return &self->page;
            };
            services.launch = [self](StringView program, Span<const StringView> args)
            {
                Array<String> call;
                call.PushBack(String(program));
                for (const StringView arg : args)
                    call.PushBack(String(arg));
                self->launched.PushBack(Move(call));
                return self->launchStarts;
            };
            services.showFolder = [self](StringView folder) { self->shownFolders.PushBack(String(folder)); };
            opener = MakeUnique<EditorCodeOpener>(DefaultAllocator(), Move(services));
        }
        ~Bed()
        {
            opener.Reset();
            context.SetUserEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(u8"code_opener_project");
        }
        [[nodiscard]] String ScriptFile() const { return project->SourceFile(u8"Code/Scripts/Player.cs"); }
        void Prefer(StringView editorId, StringView fileCommand = {}, StringView folderCommand = {})
        {
            editor::EditorCodeEditorSettings& code = user.Section<editor::EditorCodeEditorSettings>();
            code.editor = String(editorId);
            code.fileCommand = String(fileCommand);
            code.folderCommand = String(folderCommand);
        }
    };
}

RTTI_DEFINE_OBJECT(OpenerAsset, "rtti::tests::codeopener")

TEST_CASE("code opener: with no preference a file opens its asset's page at the line")
{
    Bed bed;
    CHECK(bed.opener->Open(bed.ScriptFile().AsView(), 12, 4));
    REQUIRE(bed.opened.Size() == 1u);
    CHECK(bed.opened[0] == bed.script);
    CHECK(bed.page.line == 12u);
    CHECK(bed.page.column == 4u);
    CHECK(bed.launched.IsEmpty());

    // A file no asset names opens nothing, and says so.
    CHECK_FALSE(bed.opener->Open(PathJoin(bed.project->CodeRoot().AsView(), u8"Holdout.csproj").AsView(), 0, 0));
    CHECK(bed.warnings.Size() == 1u);
}

TEST_CASE("code opener: VS Code gets the folder and the file at its line; a code asset opens there too")
{
    Bed bed;
    bed.Prefer(u8"vscode");
    CHECK(bed.opener->Open(bed.ScriptFile().AsView(), 7, 0));
    REQUIRE(bed.launched.Size() == 1u);
    const Array<String>& call = bed.launched[0];
    REQUIRE(call.Size() == 4u);
    CHECK(call[0] == StringView(u8"code"));
    CHECK(call[1] == bed.project->CodeRoot());
    CHECK(call[2] == StringView(u8"--goto"));
    CHECK(call[3] == Format(u8"{}:7:1", bed.ScriptFile()));
    CHECK(bed.opened.IsEmpty());

    CHECK(bed.opener->OpenAsset(*bed.script));
    CHECK(bed.launched.Size() == 2u);
    CHECK(bed.opened.IsEmpty());

    // Open in Built-in Editor takes the page whatever is preferred.
    CHECK(bed.opener->OpenAsset(*bed.script, true));
    CHECK(bed.launched.Size() == 2u);
    CHECK(bed.opened.Size() == 1u);
}

TEST_CASE("code opener: a command that does not start opens the page instead, with a warning")
{
    Bed bed;
    bed.Prefer(u8"command", u8"\"my editor\" {file} -l {line}");
    bed.launchStarts = false;
    CHECK(bed.opener->Open(bed.ScriptFile().AsView(), 3, 0));
    REQUIRE(bed.launched.Size() == 1u);
    CHECK(bed.launched[0][0] == StringView(u8"my editor"));
    CHECK(bed.launched[0][3] == StringView(u8"3"));
    CHECK(bed.opened.Size() == 1u);
    REQUIRE(bed.warnings.Size() == 1u);
    CHECK(bed.warnings[0].AsView().StartsWith(u8"my editor"));
}

TEST_CASE("code opener: the Code folder opens in the folder command, else the file manager")
{
    Bed bed;
    CHECK(bed.opener->Open(StringView{}, 0, 0));
    REQUIRE(bed.shownFolders.Size() == 1u);
    CHECK(bed.shownFolders[0] == bed.project->CodeRoot());

    bed.Prefer(u8"vscode");
    CHECK(bed.opener->Open(StringView{}, 0, 0));
    REQUIRE(bed.launched.Size() == 1u);
    CHECK(bed.launched[0].Size() == 2u);
    CHECK(bed.launched[0][1] == bed.project->CodeRoot());
    CHECK(bed.shownFolders.Size() == 1u);

    // A command with no folder command shows the folder in the file manager.
    bed.Prefer(u8"command", u8"ed {file}");
    CHECK(bed.opener->Open(StringView{}, 0, 0));
    CHECK(bed.launched.Size() == 1u);
    CHECK(bed.shownFolders.Size() == 2u);
}
