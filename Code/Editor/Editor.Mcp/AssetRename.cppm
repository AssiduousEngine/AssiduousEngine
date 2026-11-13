// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :asset_rename partition.
//
// asset_rename: the Assets browser's Rename for an agent. The asset keeps its guid, so everything
// that references it still does; its cooked product and the default scene's path mirror follow
// (EditorProject::RenameAsset). The host runs it through IProjectOperations::Rename (the editor
// waits out a cook).

module;
#include "Core/Prelude.h"

export module editor.mcp:asset_rename;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.mcp;
import editor.project;
import :session;
import :operations;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

export namespace editor::mcp
{
    inline void RegisterAssetRenameTool(foundation::mcp::McpServer& server, ProjectSession& session,
                                        IProjectOperations& operations)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolOutcome;
        ProjectSession* s = &session;
        IProjectOperations* ops = &operations;
        server.RegisterTool(
            u8"asset_rename",
            u8"Rename a source asset within its group, as the editor's Assets browser does. It keeps "
            u8"its guid, so whatever references it still does; its cooked product and the project's "
            u8"default scene path follow. Refused for a name a file cannot take (empty, any of "
            u8"/\\:*?\"<>|, a space at an end, a dot at its end) or one another asset in the group "
            u8"has. In the editor a running cook is waited out. Returns {guid, name, path}.",
            SchemaBuilder()
                .Str(u8"guid", u8"the source asset to rename", true)
                .Str(u8"name", u8"its new name, within the same group", true)
                .Build(),
            foundation::mcp::ToolAnnotations::Adjusts(),
            [s, ops](foundation::mcp::ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String guidText = args.Get(u8"guid").AsString();
                Guid id;
                if (!Guid::TryParse(guidText.AsView(), id))
                {
                    return Err(Format(u8"'{}' is not a valid guid", guidText.AsView()));
                }
                const String name = args.Get(u8"name").AsString();
                OperationStep<bool> step = ops->Rename(call, id, name.AsView());
                if (!step.HasValue())
                {
                    return Err(Move(step.Error()));
                }
                if (!step.Value().HasValue())
                {
                    return ToolOutcome::NotFinished(); // held while a cook reads the databases
                }
                const content::Instance* instance = s->project->SourceDb().GetInstance(id);
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(id));
                out.Set(u8"name", JsonValue::MakeString(String(instance != nullptr ? instance->Name() : name.AsView())));
                out.Set(u8"path", JsonValue::MakeString(instance != nullptr ? instance->Path() : String{}));
                return out;
            });
    }
}
