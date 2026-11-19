// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the page tools over a real EditorContext with a test page factory. Covers the
// list, opening by guid (and focusing a page that is already open), reloading in place through the
// page's reload hook, the refusals of reload and close over unsaved changes and the arguments that
// override them, and the identity every tool returns.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
namespace content = foundation::content;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    class PagedAsset : public ISerializable
    {
        RTTI_OBJECT(PagedAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    class TestPage final : public EditorPage
    {
    public:
        explicit TestPage(StringView title) : EditorPage(DefaultAllocator()), m_title(title) {}
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override
        {
            ClearDirty();
            return Status{};
        }
        void OnAssetExternallyModified() override { ++reloads; }
        // Discard reads the file again for a change made outside the history.
        bool ReloadFromSource() override { return readable; }
        u32 reloads = 0;
        bool readable = true;

    private:
        String m_title;
    };

    class TestPageFactory final : public IEditorPageFactory
    {
    public:
        [[nodiscard]] const TypeInfo* PrimaryType() const override
        {
            return &PagedAsset::StaticType();
        }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext&,
                                                       content::Instance& instance) override
        {
            ++created;
            return UniquePtr<EditorPage>(DefaultAllocator().New<TestPage>(instance.Name()),
                                         DefaultAllocator());
        }
        u32 created = 0;
    };

    // A tools/call line with its answer parsed: the tool's payload, or the error text.
    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
    };
    Answer Call(McpServer& server, StringView tool, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}",
                                   tool, argumentsJson);
        LineOutcome outcome = server.HandleLine(line.AsView());
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        Answer answer;
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
        {
            answer.payload = json::Parse(text.AsView()).value;
        }
        else
        {
            answer.error = text;
        }
        return answer;
    }

    String GuidText(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return String(StringView(text, 36));
    }
}

RTTI_DEFINE_OBJECT(PagedAsset, "rtti::editor::app::test")

TEST_CASE("page-tools: list, open, focus, the dirty refusals of reload and close, and the "
          "arguments that override them")
{
    (void)RemoveDirectoryRecursive(u8"mcp_page_tools_project");
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"mcp_page_tools_project", u8"Pages").IsOk());
    UniquePtr<EditorProject> project =
        EditorProject::Open(DefaultAllocator(), u8"mcp_page_tools_project");
    REQUIRE(project);
    GlobalTypeRegistry().Register(PagedAsset::StaticType());
    content::Instance* one =
        project->SourceDb().RootGroup()->CreateInstance(u8"One", PagedAsset::StaticType());
    content::Instance* two =
        project->SourceDb().RootGroup()->CreateInstance(u8"Two", PagedAsset::StaticType());
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);

    EditorContext context{DefaultAllocator()};
    auto factory = UniquePtr<IEditorPageFactory>(DefaultAllocator().New<TestPageFactory>(),
                                                 DefaultAllocator());
    TestPageFactory* factoryPtr = static_cast<TestPageFactory*>(factory.Get());
    context.Pages().Register(Move(factory));

    // The test supplies the context's open and close hooks, since there are no panels here.
    u32 closes = 0;
    app::PageToolSeams seams;
    seams.context = &context;
    seams.openPage = [&](const Guid& id) -> EditorPage*
    {
        content::Instance* instance = project->SourceDb().GetInstance(id);
        return instance != nullptr ? context.OpenPage(*instance) : nullptr;
    };
    seams.closePage = [&](EditorPage* page)
    {
        ++closes;
        context.ClosePage(page);
    };
    McpServer server;
    app::RegisterPageTools(server, Move(seams));
    CHECK(server.ToolCount() == app::kPageToolCount);

    // Nothing open yet.
    Answer listed = Call(server, u8"page_list", u8"{}");
    REQUIRE(listed.ok);
    CHECK(listed.payload.Get(u8"pages").Count() == 0);

    // With no project in the editor (the project manager), page_open refuses.
    const String oneGuid = GuidText(one->Id());
    CHECK_FALSE(Call(server, u8"page_open", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView()).ok);
    context.SetProject(project.Get()); // as the editor does once it opens one

    // Open by guid: created once, active, clean; opening again focuses instead of recreating.
    Answer opened = Call(server, u8"page_open", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView());
    REQUIRE(opened.ok);
    CHECK(opened.payload.Get(u8"title").AsString() == StringView(u8"One"));
    CHECK(opened.payload.Get(u8"active").AsBool());
    CHECK_FALSE(opened.payload.Get(u8"dirty").AsBool());
    CHECK(factoryPtr->created == 1u);
    const String twoGuid = GuidText(two->Id());
    REQUIRE(Call(server, u8"page_open", Format(u8"{{\"guid\":\"{}\"}}", twoGuid.AsView()).AsView()).ok);
    CHECK(context.ActivePage()->InstanceId() == two->Id());
    EditorPage* revealed = nullptr;
    context.OnRevealPage = [&revealed](EditorPage* shown) { revealed = shown; };
    Answer again = Call(server, u8"page_open", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView());
    REQUIRE(again.ok);
    CHECK(factoryPtr->created == 2u); // focused, not recreated
    CHECK(context.ActivePage()->InstanceId() == one->Id());
    CHECK(revealed == context.ActivePage()); // its tab is raised too, or a background tab would stay hidden
    listed = Call(server, u8"page_list", u8"{}");
    REQUIRE(listed.ok);
    CHECK(listed.payload.Get(u8"pages").Count() == 2);

    // Unknown guids and a malformed one are refusals, not pages.
    Answer unknown = Call(server, u8"page_open", u8"{\"guid\":\"00000000-0000-0000-0000-000000000000\"}");
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.error.AsView().StartsWith(u8"no asset with guid"));
    Answer malformed = Call(server, u8"page_open", u8"{\"guid\":\"nope\"}");
    CHECK_FALSE(malformed.ok);
    CHECK(malformed.error.AsView().StartsWith(u8"invalid guid"));

    // A clean page reloads in place: the same page object, refreshed through its hook, never
    // closed or recreated.
    auto* pageOne = static_cast<TestPage*>(context.ActivePage());
    Answer reload = Call(server, u8"page_reload", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView());
    REQUIRE(reload.ok);
    CHECK(pageOne->reloads == 1u);
    CHECK(closes == 0u);
    CHECK(factoryPtr->created == 2u);
    CHECK(context.ActivePage() == pageOne);
    // A dirty page refuses reload and close. Reload with force discards the changes and then
    // refreshes; close with discard drops them.
    pageOne->MarkDirty();
    reload = Call(server, u8"page_reload", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView());
    CHECK_FALSE(reload.ok);
    CHECK(reload.error.AsView().StartsWith(u8"page 'One' has unsaved changes"));
    CHECK(pageOne->reloads == 1u);
    reload = Call(server, u8"page_reload",
                  Format(u8"{{\"guid\":\"{}\",\"force\":true}}", oneGuid.AsView()).AsView());
    REQUIRE(reload.ok);
    CHECK(pageOne->reloads == 2u);
    CHECK_FALSE(pageOne->IsDirty());
    CHECK(closes == 0u);
    CHECK(factoryPtr->created == 2u);
    CHECK_FALSE(reload.payload.Get(u8"dirty").AsBool());
    CHECK(reload.payload.Get(u8"active").AsBool());
    // A file that does not read cannot be gone back to: the changes stay, and the call says so.
    pageOne->readable = false;
    pageOne->MarkDirty();
    reload = Call(server, u8"page_reload",
                  Format(u8"{{\"guid\":\"{}\",\"force\":true}}", oneGuid.AsView()).AsView());
    CHECK_FALSE(reload.ok);
    CHECK(reload.error.AsView().Contains(u8"could not be discarded"));
    CHECK(pageOne->IsDirty());
    CHECK(pageOne->reloads == 2u);
    pageOne->readable = true;
    CHECK(context.OpenPages().Size() == 2u);

    pageOne->MarkDirty();
    Answer close = Call(server, u8"page_close", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView());
    CHECK_FALSE(close.ok);
    CHECK(close.error.AsView().StartsWith(u8"page 'One' has unsaved changes"));
    close = Call(server, u8"page_close",
                 Format(u8"{{\"guid\":\"{}\",\"discard\":true}}", oneGuid.AsView()).AsView());
    REQUIRE(close.ok);
    CHECK(close.payload.Get(u8"closed").AsBool());
    CHECK(closes == 1u);
    CHECK(context.OpenPages().Size() == 1u);
    // Reloading a page that is not open is a refusal that points at the tools to use.
    reload = Call(server, u8"page_reload", Format(u8"{{\"guid\":\"{}\"}}", oneGuid.AsView()).AsView());
    CHECK_FALSE(reload.ok);
    CHECK(reload.error.AsView().StartsWith(u8"no open page for guid"));

    context.ClosePage(context.ActivePage());
    context.SetProject(nullptr); // before the project goes
    project = nullptr;
    (void)RemoveDirectoryRecursive(u8"mcp_page_tools_project");
}

TEST_CASE("page-tools: code_open hands a code asset, a Code/ file or the Code folder to the code "
          "opener at the line asked, and refuses what is not the project's code")
{
    (void)RemoveDirectoryRecursive(u8"mcp_code_open_project");
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"mcp_code_open_project", u8"Code").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), u8"mcp_code_open_project");
    REQUIRE(project);
    GlobalTypeRegistry().Register(PagedAsset::StaticType());
    content::Instance* script = project->SourceDb().RootGroup()->CreateInstance(u8"Player", PagedAsset::StaticType());
    content::Instance* level = project->SourceDb().RootGroup()->CreateInstance(u8"Level", PagedAsset::StaticType());
    REQUIRE(script != nullptr);
    REQUIRE(level != nullptr);
    script->SetSourceName(u8"Code/Scripts/Player.as");
    level->SetSourceName(u8"Levels/Level.scene");
    const String file = project->SourceFile(u8"Code/Scripts/Player.as");
    REQUIRE(CreateDirectories(PathParent(file.AsView())));
    REQUIRE(WriteFile(file.AsView(), Span<const byte>(reinterpret_cast<const byte*>("void Main() {}"), 14)).IsOk());

    struct Opened
    {
        String file;
        u32 line = 0;
        u32 column = 0;
    };
    Array<Opened> opened;
    EditorContext context{DefaultAllocator()};
    app::PageToolSeams seams;
    seams.context = &context;
    seams.openCode = [&](StringView path, u32 line, u32 column)
    {
        opened.PushBack(Opened{String(path), line, column});
        return true;
    };
    McpServer server;
    app::RegisterPageTools(server, Move(seams));

    // With no project in the editor, nothing opens.
    CHECK_FALSE(Call(server, u8"code_open", u8"{}").ok);
    context.SetProject(project.Get());

    // A code asset at a line and column: the opener gets its file on disk.
    Answer answer = Call(server, u8"code_open",
                         Format(u8"{{\"guid\":\"{}\",\"line\":12,\"column\":3}}", GuidText(script->Id()).AsView()).AsView());
    REQUIRE(answer.ok);
    CHECK(answer.payload.Get(u8"file").AsString() == StringView(u8"Code/Scripts/Player.as"));
    REQUIRE(opened.Size() == 1u);
    CHECK(opened[0].file == file);
    CHECK(opened[0].line == 12u);
    CHECK(opened[0].column == 3u);

    // The same file by its path, at the start.
    answer = Call(server, u8"code_open", u8"{\"path\":\"Code/Scripts/Player.as\"}");
    REQUIRE(answer.ok);
    REQUIRE(opened.Size() == 2u);
    CHECK(opened[1].file == file);
    CHECK(opened[1].line == 0u);

    // Neither: the Code folder, as an empty file.
    answer = Call(server, u8"code_open", u8"{}");
    REQUIRE(answer.ok);
    CHECK(answer.payload.Get(u8"file").AsString() == StringView(u8"Code"));
    REQUIRE(opened.Size() == 3u);
    CHECK(opened[2].file.IsEmpty());

    // Refused, nothing opened: an asset that is not code, a path out of the Code folder or up
    // from it, a file not there, an unknown guid, both at once, a negative line.
    CHECK_FALSE(Call(server, u8"code_open", Format(u8"{{\"guid\":\"{}\"}}", GuidText(level->Id()).AsView()).AsView()).ok);
    CHECK_FALSE(Call(server, u8"code_open", u8"{\"path\":\"Levels/Level.scene\"}").ok);
    CHECK_FALSE(Call(server, u8"code_open", u8"{\"path\":\"Code/../project.json\"}").ok);
    CHECK_FALSE(Call(server, u8"code_open", u8"{\"path\":\"Code/Missing.as\"}").ok);
    CHECK_FALSE(Call(server, u8"code_open", Format(u8"{{\"guid\":\"{}\"}}", u8"00000000-0000-0000-0000-00000000c0de").AsView()).ok);
    CHECK_FALSE(Call(server, u8"code_open",
                     Format(u8"{{\"guid\":\"{}\",\"path\":\"Code/Scripts/Player.as\"}}", GuidText(script->Id()).AsView()).AsView())
                    .ok);
    CHECK_FALSE(Call(server, u8"code_open", u8"{\"path\":\"Code/Scripts/Player.as\",\"line\":-1}").ok);
    CHECK(opened.Size() == 3u);

    context.SetProject(nullptr); // before the project goes
    project = nullptr;
    (void)RemoveDirectoryRecursive(u8"mcp_code_open_project");
}
