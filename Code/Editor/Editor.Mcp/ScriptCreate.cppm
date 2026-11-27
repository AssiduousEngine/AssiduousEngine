// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :script_create partition
//
// script_create: seed a fresh script asset from the chosen backend's own starter (the language
// cook's NewAssetTemplate, never hardcoded text), through the creation the editor's New Asset menu
// and asset_create run (pipeline::CreateScriptInstance). The starter source is written to
// Code/<name>.<ext> with the behavior's class named after the asset, and a ScriptClassAsset
// envelope records {fileName, language} in the source database. The agent then edits the file
// (the file is the truth), checks it with script_validate, and runs asset_cook to make the class
// attachable.

module;
#include "Core/Prelude.h"

export module editor.mcp:script_create;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.script;
import foundation.mcp;
import pipeline.core;
import script.pipeline;
import editor.project;
import :session;
import :script_validate; // NoScriptLanguage (the shared refusal)

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

namespace editor::mcp::detail
{
    inline Array<String> BehaviorKindChoices()
    {
        Array<String> choices;
        choices.PushBack(String(u8"entity"));
        choices.PushBack(String(u8"scene"));
        choices.PushBack(String(u8"run"));
        return choices;
    }

    inline foundation::script::BehaviorKind ParseBehaviorKind(StringView name)
    {
        if (name == StringView(u8"scene"))
        {
            return foundation::script::BehaviorKind::Scene;
        }
        if (name == StringView(u8"run"))
        {
            return foundation::script::BehaviorKind::Run;
        }
        return foundation::script::BehaviorKind::Entity;
    }
}

export namespace editor::mcp
{
    // Registers script_create against `server` (it writes into the open project's source DB and
    // Code/ folder).
    inline void RegisterScriptCreateTool(foundation::mcp::McpServer& server,
                                         ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"script_create",
            u8"Create a new script asset from the backend's starter: writes Code/<name>.<ext> "
            u8"with the starter of a behavior of the kind asked, its class named after the "
            u8"asset, and creates the script asset referencing it. Kinds: 'entity' (an "
            u8"EntityBehavior, set on an entity's Behavior component), 'scene' (a "
            u8"SceneBehavior, one per scene), 'run' (a RunBehavior, the project's run "
            u8"behavior). Returns the asset guid and the source file path: edit the FILE to "
            u8"write your gameplay code, use script_validate as the loop, then asset_cook to "
            u8"make the class attachable.",
            SchemaBuilder()
                .Str(u8"name", u8"the asset name (also the source file stem)", true)
                .Str(u8"language", u8"the script language: one whose plugin the project enables "
                                   u8"(angelscript, luau, csharp); an unknown one is refused with the list",
                     true)
                .Enum(u8"kind", detail::BehaviorKindChoices(),
                      u8"the kind of behavior to seed (default entity)")
                .Str(u8"group", u8"source-DB group path to place it in (slash-joined; "
                                u8"default root)")
                .Build(),
                foundation::mcp::ToolAnnotations::Creates(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String requestedName = args.Get(u8"name").AsString();
                if (requestedName.IsEmpty())
                {
                    return Err(String(u8"name must not be empty"));
                }
                const String language = args.Get(u8"language").AsString();
                const foundation::script::ScriptBackendDesc* backend =
                    foundation::script::ScriptBackendRegistry::Get().FindByLanguage(
                        language.AsView());
                pipeline::IScriptLanguageCook* cook =
                    pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(language.AsView());
                if (backend == nullptr || backend->fileExtensions.IsEmpty() || cook == nullptr)
                {
                    return Err(detail::NoScriptLanguage(language.AsView()));
                }
                const foundation::script::BehaviorKind kind =
                    detail::ParseBehaviorKind(args.Get(u8"kind").AsString().AsView());

                // The same creation File > New and asset_create run: the starter written to
                // Code/, the behavior's class named after the asset, the asset pointing at the
                // file.
                const String sourcesRoot = s->project->SourcesRoot();
                pipeline::AssetCreationContext creation;
                creation.root = s->project->SourceDb().RootGroup();
                creation.picked = detail::ResolveGroupPath(creation.root,
                                                           args.Get(u8"group").AsString().AsView());
                const String codeRoot = s->project->CodeRoot();
                creation.sourcesRoot = sourcesRoot.AsView();
                creation.codeRoot = codeRoot.AsView();
                creation.name = requestedName.AsView();
                content::Instance* instance = pipeline::CreateScriptInstance(
                    creation, language.AsView(), backend->fileExtensions[0].AsView(), kind,
                    pipeline::BehaviorBaseName(kind));
                if (instance == nullptr)
                {
                    return Err(Format(u8"could not create '{}': the source file under '{}' or the "
                                      u8"asset did not write (log_read says which)",
                                      requestedName.AsView(), codeRoot.AsView()));
                }
                const String name(instance->Name());
                // The file it wrote, as its asset names it (Code/<name>.<ext>, or .2 on a clash).
                RefPtr<ISerializable> written = instance->ReadObject();
                const auto* asset = Cast<pipeline::Asset>(written.Get());
                const String fileName(asset != nullptr ? asset->fileName.View() : StringView{});
                const String path = s->project->SourceFile(fileName.AsView());

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(instance->Id()));
                out.Set(u8"name", JsonValue::MakeString(name));
                out.Set(u8"language", JsonValue::MakeString(language));
                out.Set(u8"sourceFile", JsonValue::MakeString(path));
                out.Set(u8"fileName", JsonValue::MakeString(fileName));
                return out;
            });
    }
}
