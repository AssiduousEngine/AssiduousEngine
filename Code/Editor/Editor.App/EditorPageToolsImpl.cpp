// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - the page tools' bodies.
module;
#include "Core/Prelude.h"

module editor.app;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.vfs;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolResult;

namespace editor::app
{
    namespace
    {
        // The handlers share a reference to this state, so it outlives the server (Function is
        // move-only, so the handlers cannot each hold a copy).
        class PageToolState final : public RefCounted
        {
        public:
            explicit PageToolState(PageToolSeams seams) : seams(Move(seams)) {}
            PageToolSeams seams;
        };

        JsonValue PageIdentity(const editor::EditorContext& context, const editor::EditorPage& page)
        {
            utf8char guid[37];
            page.InstanceId().ToChars(guid);
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"guid", JsonValue::MakeString(StringView(guid, 36)));
            out.Set(u8"title", JsonValue::MakeString(String(page.Title())));
            out.Set(u8"dirty", JsonValue::MakeBool(page.IsDirty()));
            out.Set(u8"active", JsonValue::MakeBool(context.ActivePage() == &page));
            return out;
        }

        editor::EditorPage* FindOpenPage(const editor::EditorContext& context, const Guid& id)
        {
            for (const UniquePtr<editor::EditorPage>& page : context.OpenPages())
            {
                if (page->InstanceId() == id)
                {
                    return page.Get();
                }
            }
            return nullptr;
        }

        Result<Guid, String> ParseGuidArgument(const JsonValue& args)
        {
            const String text = args.Get(u8"guid").AsString();
            Guid id;
            if (!Guid::TryParse(text.AsView(), id))
            {
                return Err(Format(u8"invalid guid '{}'", text.AsView()));
            }
            return id;
        }

        String UnsavedChangesRefusal(const editor::EditorPage& page, StringView argument)
        {
            return Format(u8"page '{}' has unsaved changes - ask the user to save or discard them, "
                          u8"or pass {}:true to discard them yourself",
                          page.Title(), argument);
        }
    }

    void RegisterPageTools(foundation::mcp::McpServer& server, PageToolSeams seams)
    {
        RefPtr<PageToolState> state = MakeRef<PageToolState>(editor::EditorRootAllocator(), Move(seams));

        server.RegisterTool(
            u8"page_list",
            u8"The pages the editor has open: each one's asset guid, title, whether it has unsaved "
            u8"changes, and which is active. A page is the live, editable view of an asset; the "
            u8"scene and prefab tools read and write the asset's SOURCE, which a page reloads "
            u8"from disk only through page_reload.",
            SchemaBuilder().Build(), ToolAnnotations::ReadOnly(),
            [state](const JsonValue&) -> ToolResult
            {
                JsonValue pages = JsonValue::MakeArray();
                for (const UniquePtr<editor::EditorPage>& page : state->seams.context->OpenPages())
                {
                    pages.Add(PageIdentity(*state->seams.context, *page));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"pages", Move(pages));
                return out;
            });

        server.RegisterTool(
            u8"page_open",
            u8"Open the page for a source asset (by guid) and make it the active one, or focus it "
            u8"when it is already open. Returns the page's identity. Fails when no asset has that "
            u8"guid or its type has no page.",
            SchemaBuilder().Str(u8"guid", u8"the source asset to open", true).Build(),
            ToolAnnotations::Adjusts(),
            [state](const JsonValue& args) -> ToolResult
            {
                Result<Guid, String> id = ParseGuidArgument(args);
                if (!id.HasValue())
                {
                    return Err(Move(id.Error()));
                }
                editor::EditorContext& context = *state->seams.context;
                if (context.Project() == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                editor::EditorPage* page = FindOpenPage(context, id.Value());
                if (page == nullptr)
                {
                    page = state->seams.openPage(id.Value());
                    if (page == nullptr)
                    {
                        return Err(Format(u8"no asset with guid '{}', or its type has no page "
                                          u8"(see log_read, category Editor)",
                                          args.Get(u8"guid").AsString().AsView()));
                    }
                }
                context.RevealPage(page); // bring it to the front: a background tab's viewport never renders
                return PageIdentity(context, *page);
            });

        server.RegisterTool(
            u8"page_reload",
            u8"Reload an open page from its asset's source on disk - after scene_write or "
            u8"prefab_write changed what the page shows. The page stays open and refreshes in "
            u8"place (its undo history goes). REFUSED while the page has unsaved changes unless "
            u8"`force` is true, which discards them first. Returns the page's identity.",
            SchemaBuilder()
                .Str(u8"guid", u8"the open page's asset", true)
                .Boolean(u8"force", u8"discard the page's unsaved changes (default false)")
                .Build(),
            ToolAnnotations::Overwrites(),
            [state](const JsonValue& args) -> ToolResult
            {
                Result<Guid, String> id = ParseGuidArgument(args);
                if (!id.HasValue())
                {
                    return Err(Move(id.Error()));
                }
                editor::EditorContext& context = *state->seams.context;
                editor::EditorPage* page = FindOpenPage(context, id.Value());
                if (page == nullptr)
                {
                    return Err(Format(u8"no open page for guid '{}' (page_list shows the open ones; "
                                      u8"page_open opens one)",
                                      args.Get(u8"guid").AsString().AsView()));
                }
                if (page->IsDirty())
                {
                    if (!args.Get(u8"force").AsBool())
                    {
                        return Err(UnsavedChangesRefusal(*page, u8"force"));
                    }
                    // Discard through the page itself, then refresh. A page refreshes only when
                    // clean (an apply-to-prefab or a re-import leaves a dirty page alone with a
                    // warning), so the discard must come first.
                    if (!context.DiscardPage(*page))
                    {
                        return Err(String(u8"the page's unsaved changes could not be discarded: its file "
                                          u8"did not read (the Console says why), so they are kept"));
                    }
                }
                page->OnAssetExternallyModified();
                return PageIdentity(context, *page);
            });

        server.RegisterTool(
            u8"page_close",
            u8"Close an open page. REFUSED while it has unsaved changes unless `discard` is true, "
            u8"which drops them. Returns {closed, guid}.",
            SchemaBuilder()
                .Str(u8"guid", u8"the open page's asset", true)
                .Boolean(u8"discard", u8"drop the page's unsaved changes (default false)")
                .Build(),
            ToolAnnotations::Overwrites(),
            [state](const JsonValue& args) -> ToolResult
            {
                Result<Guid, String> id = ParseGuidArgument(args);
                if (!id.HasValue())
                {
                    return Err(Move(id.Error()));
                }
                editor::EditorPage* page = FindOpenPage(*state->seams.context, id.Value());
                if (page == nullptr)
                {
                    return Err(Format(u8"no open page for guid '{}'",
                                      args.Get(u8"guid").AsString().AsView()));
                }
                if (page->IsDirty() && !args.Get(u8"discard").AsBool())
                {
                    return Err(UnsavedChangesRefusal(*page, u8"discard"));
                }
                state->seams.closePage(page);
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"closed", JsonValue::MakeBool(true));
                out.Set(u8"guid", args.Get(u8"guid"));
                return out;
            });

        server.RegisterTool(
            u8"code_open",
            u8"Open code where the user's Code Editor preference says (an outside editor such as VS "
            u8"Code, or the built-in code page), as the Assets panel's Open does: a code asset by "
            u8"`guid`, or a file by `path` in the project's Code folder (as `Code/...`), at `line` "
            u8"and `column` (1-based; omitted: the start). With neither, opens the Code folder. Use "
            u8"page_open to look at a script's page yourself. Returns {opened, file}.",
            SchemaBuilder()
                .Str(u8"guid", u8"a code asset")
                .Str(u8"path", u8"a file in the project's Code folder, as Code/...")
                .Integer(u8"line", u8"the line to open at, 1-based")
                .Integer(u8"column", u8"the column to open at, 1-based")
                .Build(),
            ToolAnnotations::Adjusts(),
            [state](const JsonValue& args) -> ToolResult
            {
                editor::EditorContext& context = *state->seams.context;
                editor::EditorProject* project = context.Project();
                if (project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const bool byGuid = !args.Get(u8"guid").AsString().IsEmpty();
                const bool byPath = !args.Get(u8"path").AsString().IsEmpty();
                if (byGuid && byPath)
                {
                    return Err(String(u8"give guid or path, not both"));
                }
                const i64 line = args.Get(u8"line").AsInt(0);
                const i64 column = args.Get(u8"column").AsInt(0);
                if (line < 0 || column < 0)
                {
                    return Err(String(u8"line and column are 1-based"));
                }
                // The source path (Code/...) the file has, which must be in the Code folder.
                String source;
                if (byGuid)
                {
                    Result<Guid, String> id = ParseGuidArgument(args);
                    if (!id.HasValue())
                    {
                        return Err(Move(id.Error()));
                    }
                    const foundation::content::Instance* instance = project->SourceDb().GetInstance(id.Value());
                    if (instance == nullptr)
                    {
                        return Err(Format(u8"no asset with guid '{}'", args.Get(u8"guid").AsString().AsView()));
                    }
                    source = String(instance->SourceName());
                }
                else if (byPath)
                {
                    source = args.Get(u8"path").AsString();
                    (void)source.Replace(u8'\\', u8'/');
                }
                String file;
                if (byGuid || byPath)
                {
                    StringView rest;
                    if (!foundation::vfs::AfterFolder(source.AsView(), foundation::vfs::kSourcesCodeFolder, rest) || rest.IsEmpty() ||
                        source.AsView().Contains(u8".."))
                    {
                        return Err(Format(u8"'{}' is not in the project's Code folder", source.AsView()));
                    }
                    file = project->SourceFile(source.AsView());
                    if (!FileExists(file.AsView()))
                    {
                        return Err(Format(u8"no file '{}'", source.AsView()));
                    }
                }
                if (!state->seams.openCode || !state->seams.openCode(file.AsView(), static_cast<u32>(line), static_cast<u32>(column)))
                {
                    return Err(String(u8"nothing opened (see log_read, category Editor)"));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"opened", JsonValue::MakeBool(true));
                out.Set(u8"file", JsonValue::MakeString(byGuid || byPath ? Move(source) : String(foundation::vfs::kSourcesCodeFolder)));
                return out;
            });
    }
}
