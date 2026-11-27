// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: the golden tool-call flow an agent runs.
//
// The flow crosses collections: an agent drives the MCP protocol (foundation.mcp) to scaffold,
// open and inspect a project on disk (editor.mcp over the headless EditorProject, which sits on
// the VFS and the content databases), all through JSON-RPC lines.

#include <doctest/doctest.h>


#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.mcp;
import foundation.mcp.reflection;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import foundation.mcp.script;
import foundation.runtime; // EnabledPlugin (the languages a created project enables)
#if defined(HAS_PLUGIN_ANGELSCRIPT)
import angelscript.foundation;
import angelscript.pipeline; // RegisterAngelScriptScriptCook: the angelscript plugin's Pipeline part
#endif
#if defined(HAS_PLUGIN_LUAU)
import luau.foundation;
import luau.pipeline; // RegisterLuauScriptCook: what the luau plugin's Pipeline part registers
#endif
import editor.mcp;
import editor.project;  // the seeded project, opened to check it
import foundation.settings; // the user's answers on a project's MCP folders
import fonts.pipeline;  // FontAsset (the seeded default UI font)

using namespace foundation::core;
using namespace foundation::mcp;
using foundation::json::JsonValue;
namespace json = foundation::json;

namespace
{
    // Register the first available backend and return its language id. pipeline.registration
    // also registers the enabled backends; this makes explicit the language the script_api
    // assertion expects.
    [[nodiscard, maybe_unused]] StringView RegisterSomeBackend()
    {
#if defined(HAS_PLUGIN_ANGELSCRIPT)
        angelscript::RegisterAngelScriptBackend();
        return StringView(u8"angelscript");
#elif defined(HAS_PLUGIN_LUAU)
        luau::RegisterLuauScriptBackend();
        return StringView(u8"luau");
#else
        return StringView{}; // no language built: the tests that need one are left out
#endif
    }

    JsonValue CallResponse(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = s.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        return json::Parse(line.response.AsView()).value;
    }

    JsonValue CallOk(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = CallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    JsonValue Obj() { return JsonValue::MakeObject(); }
    JsonValue With(JsonValue o, StringView k, StringView v)
    {
        o.Set(String(k), JsonValue::MakeString(String(v)));
        return o;
    }
}

TEST_CASE("integration.mcp: an agent creates, opens, and inspects a project via MCP tools")
{
    // Deterministic fixture: wipe any prior scratch project (.test-scratch is the CWD here).
    RemoveDirectoryRecursive(u8"mcp_fixture_project");

    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    RegisterReflectionTools(server); // the same host serves reflection and project tools

    // list advertises both tool families
    JsonValue tools = json::Parse(server.HandleLine(
                                        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}").response
                                       .AsView())
                          .value.Get(u8"result")
                          .Get(u8"tools");
    CHECK(tools.Count() >= 5); // project_create/open/info and type_list/type_info

    // Create, open, then read the info.
    JsonValue created = CallOk(server, u8"project_create",
                               With(With(Obj(), u8"directory", u8"mcp_fixture_project"), u8"name",
                                    u8"Fixture"));
    CHECK(created.Get(u8"created").AsBool() == true);

    JsonValue opened =
        CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_fixture_project"));
    CHECK(opened.Get(u8"name").AsString() == StringView(u8"Fixture"));

    JsonValue info = CallOk(server, u8"project_info", Obj());
    CHECK(info.Get(u8"name").AsString() == StringView(u8"Fixture"));
    CHECK(info.Get(u8"directory").AsString().Size() > 0u);
    CHECK(info.Get(u8"sourcesRoot").AsString().Size() > 0u);

    // The settings are grouped as the Project Settings dialog's tabs: every setting in exactly one
    // category, General first.
    const JsonValue categories = info.Get(u8"settingCategories");
    REQUIRE(categories.Count() > 1);
    CHECK(categories.KeyAt(0) == StringView(u8"General"));
    i64 grouped = 0;
    bool levelsOnQuality = false;
    for (i64 i = 0; i < categories.Count(); ++i)
    {
        const JsonValue names = categories.Get(categories.KeyAt(i));
        grouped += names.Count();
        for (i64 n = 0; n < names.Count(); ++n)
        {
            levelsOnQuality = levelsOnQuality ||
                              (categories.KeyAt(i) == StringView(u8"Quality") &&
                               names.At(n).AsString() == StringView(u8"qualityLevelIds"));
        }
    }
    CHECK(grouped == info.Get(u8"settings").Count());
    CHECK(levelsOnQuality);

    // The manifest is written to disk.
    CHECK(FileExists(u8"mcp_fixture_project/Project.xml"));

    // Opening a nonexistent project is a tool error with real text, not a protocol fault.
    JsonValue bad =
        CallResponse(server, u8"project_open", With(Obj(), u8"directory", u8"nope_not_a_project"));
    CHECK(bad.Get(u8"result").Get(u8"isError").AsBool() == true);
}

// project_create seeds the editor's starter content. A project made over MCP needs the default UI
// font as an asset: the engine's built-in font is not in a dist, so without it an exported game
// shows no text.
// A project lists folders outside it for its tools (a game's sources beside it). Opened by an agent
// through the headless host, which has no one to ask, it reaches only those the user approved in
// the editor, and says which it does not.
TEST_CASE("integration.mcp: project_open reaches the MCP folders the user approved, and names the rest")
{
    editor::RegisterMcpFolderTypes();
    const String answersDir = PathJoin(GetCurrentDirectory().AsView(), u8"mcp_open_folders_answers");
    (void)RemoveDirectoryRecursive(answersDir.AsView());
    REQUIRE(CreateDirectories(answersDir.AsView()));
    (void)RemoveDirectoryRecursive(u8"mcp_open_folders_project");
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"mcp_open_folders_project", u8"Folders").IsOk());
    Array<String> listed;
    {
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), u8"mcp_open_folders_project");
        REQUIRE(project);
        project->Settings().mcpFolders.PushBack(String(u8"../mcp_open_folders_game"));
        project->Settings().mcpFolders.PushBack(String(u8"../mcp_open_folders_art"));
        REQUIRE(project->SaveSettings().IsOk());
        listed = editor::ResolveMcpFolders(project->Directory(), project->Settings());
        foundation::settings::Settings store(DefaultAllocator());
        const String projectPath = PathNormalizeLexically(PathAbsolute(project->Directory()).AsView());
        store.Section<editor::McpFolderAnswers>().Record(projectPath.AsView(), listed[0].AsView(), true);
        foundation::vfs::NativeFileSystem root(answersDir.AsView(), DefaultAllocator());
        REQUIRE(editor::SaveMcpFolderAnswers(root, store).IsOk());
    }
    REQUIRE(listed.Size() == 2u);

    McpServer server;
    server.Paths().AddFolder(PathJoin(GetCurrentDirectory().AsView(), u8"mcp_open_folders_project").AsView());
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner, nullptr, {}, {}, answersDir.AsView());
    const JsonValue opened = CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_open_folders_project"));
    REQUIRE(opened.Get(u8"mcpFolders").Count() == 1u);
    CHECK(opened.Get(u8"mcpFolders").At(0).AsString() == listed[0]);
    REQUIRE(opened.Get(u8"mcpFoldersNotApproved").Count() == 1u);
    CHECK(opened.Get(u8"mcpFoldersNotApproved").At(0).AsString() == listed[1]);
    CHECK(server.Paths().Check(PathJoin(listed[0].AsView(), u8"generated/Hud.xml").AsView()).HasValue());
    CHECK_FALSE(server.Paths().Check(PathJoin(listed[1].AsView(), u8"Rock.glb").AsView()).HasValue());

    session.plugins = nullptr; // the project's plugins go before it
    owner.plugins.Reset();
    session.project = nullptr;
    owner.project.Reset();
    (void)RemoveDirectoryRecursive(u8"mcp_open_folders_project");
    (void)RemoveDirectoryRecursive(answersDir.AsView());
}

TEST_CASE("integration.mcp: project_create seeds the default UI font, the sky and the primitives")
{
    pipeline::RegisterPipelineTypes();
    RemoveDirectoryRecursive(u8"mcp_seeded_project");
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    (void)pipeline::RegisterAllCreators(creators);
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());

    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, dataRoot.AsView());
    JsonValue created = CallOk(server, u8"project_create",
                               With(With(Obj(), u8"directory", u8"mcp_seeded_project"), u8"name", u8"Seeded"));
    CHECK(created.Get(u8"seeded").AsBool());

    UniquePtr<editor::EditorProject> project =
        editor::EditorProject::Open(DefaultAllocator(), u8"mcp_seeded_project");
    REQUIRE(static_cast<bool>(project));
    const Guid font = project->Settings().defaultUiFontId;
    REQUIRE_FALSE(font.IsNil());
    foundation::content::Instance* fontInstance = project->SourceDb().GetInstance(font);
    REQUIRE(fontInstance != nullptr);
    RefPtr<ISerializable> read = fontInstance->ReadObject();
    const pipeline::FontAsset* asset = Cast<pipeline::FontAsset>(read.Get());
    REQUIRE(asset != nullptr);
    CHECK(asset->family == StringView(u8"Roboto"));
    CHECK(asset->mode == pipeline::FontBakeMode::DistanceField); // one bake draws every size clean
    foundation::content::Group* root = project->SourceDb().RootGroup();
    REQUIRE(root->GetGroup(u8"Environment") != nullptr);
    CHECK(root->GetGroup(u8"Environment")->GetInstance(u8"Sky") != nullptr);
    CHECK(FileExists(u8"mcp_seeded_project/Sources/Environment/kloofendal_48d_partly_cloudy_puresky_1k.hdr"));
    CHECK_FALSE(FileExists(u8"mcp_seeded_project/Sources/BlueSky.hdr")); // the large sky is not seeded
    CHECK(FileExists(u8"mcp_seeded_project/Sources/Fonts/Roboto-Regular.ttf"));
}

TEST_CASE("integration.mcp: project_create enables the script languages asked for, AngelScript when none are named")
{
    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    const auto create = [&server](StringView dir, const JsonValue* languages)
    {
        RemoveDirectoryRecursive(dir);
        JsonValue args = With(With(Obj(), u8"directory", dir), u8"name", u8"Languages");
        if (languages != nullptr)
        {
            args.Set(u8"languages", *languages);
        }
        return CallResponse(server, u8"project_create", Move(args));
    };
    const auto enabled = [](StringView dir)
    {
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        String ids;
        for (const foundation::runtime::EnabledPlugin& entry : project->Settings().plugins)
        {
            ids.Append(ids.IsEmpty() ? StringView{} : StringView(u8","));
            ids.Append(entry.id.AsView());
        }
        return ids;
    };

    CHECK_FALSE(create(u8"mcp_languages_default", nullptr).Get(u8"result").Get(u8"isError").AsBool());
    CHECK(enabled(u8"mcp_languages_default") == StringView(u8"angelscript"));

    JsonValue luau = JsonValue::MakeArray();
    luau.Add(JsonValue::MakeString(u8"luau"));
    CHECK_FALSE(create(u8"mcp_languages_luau", &luau).Get(u8"result").Get(u8"isError").AsBool());
    CHECK(enabled(u8"mcp_languages_luau") == StringView(u8"luau"));

    const JsonValue none = JsonValue::MakeArray();
    CHECK_FALSE(create(u8"mcp_languages_none", &none).Get(u8"result").Get(u8"isError").AsBool());
    CHECK(enabled(u8"mcp_languages_none").IsEmpty());

    JsonValue unknown = JsonValue::MakeArray();
    unknown.Add(JsonValue::MakeString(u8"cobol"));
    const JsonValue refused = create(u8"mcp_languages_unknown", &unknown).Get(u8"result");
    CHECK(refused.Get(u8"isError").AsBool());
    CHECK(refused.Get(u8"content").At(0).Get(u8"text").AsString().AsView().Contains(u8"no script language 'cobol'"));
    CHECK_FALSE(FileExists(u8"mcp_languages_unknown/Project.xml")); // nothing was created

    const StringView made[] = {u8"mcp_languages_default", u8"mcp_languages_luau", u8"mcp_languages_none",
                               u8"mcp_languages_unknown"};
    for (StringView dir : made)
    {
        RemoveDirectoryRecursive(dir);
    }
}

TEST_CASE("integration.mcp: asset_list / asset_info read the open project's content DB")
{
    RemoveDirectoryRecursive(u8"mcp_asset_project");

    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterAssetTools(server, session);

    CallOk(server, u8"project_create",
           With(With(Obj(), u8"directory", u8"mcp_asset_project"), u8"name", u8"Assets"));
    CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_asset_project"));

    // A fresh project's source DB has no assets yet.
    CHECK(CallOk(server, u8"asset_list", Obj()).Get(u8"count").AsInt() == 0);

    // Seed one instance directly in the source DB, then read it back through the tools.
    Guid id;
    REQUIRE(Guid::TryParse(u8"12345678-1234-1234-1234-1234567890ab", id));
    session.project->SourceDb().RootGroup()->AddInstance(id, u8"hero", u8"rtti::test",
                                                         u8"TextureAsset");

    JsonValue list = CallOk(server, u8"asset_list", Obj());
    CHECK(list.Get(u8"count").AsInt() == 1);
    JsonValue first = list.Get(u8"assets").At(0);
    CHECK(first.Get(u8"name").AsString() == StringView(u8"hero"));
    CHECK(first.Get(u8"type").AsString() == StringView(u8"TextureAsset"));

    JsonValue seen = CallOk(server, u8"asset_info",
                            With(Obj(), u8"guid", u8"12345678-1234-1234-1234-1234567890ab"));
    CHECK(seen.Get(u8"name").AsString() == StringView(u8"hero"));
    CHECK(seen.Get(u8"typeNamespace").AsString() == StringView(u8"rtti::test"));

    // A well-formed but absent guid is a tool error with real text, not a protocol fault.
    JsonValue missing = CallResponse(server, u8"asset_info",
                                     With(Obj(), u8"guid", u8"00000000-0000-0000-0000-000000000000"));
    CHECK(missing.Get(u8"result").Get(u8"isError").AsBool() == true);

    // A malformed guid is likewise a tool error.
    JsonValue malformed =
        CallResponse(server, u8"asset_info", With(Obj(), u8"guid", u8"not-a-guid"));
    CHECK(malformed.Get(u8"result").Get(u8"isError").AsBool() == true);
}

#if defined(HAS_PLUGIN_ANGELSCRIPT) || defined(HAS_PLUGIN_LUAU)
namespace
{
    // The import and cook flow is the same for every backend: RegisterPipelineTypes registers
    // every enabled cook, the ScriptFileImporter accepts each backend's extension, and the cook
    // resolves by language. Only the source language and file extension differ, so the same flow
    // runs per backend.
    void DriveImportCookFlow(StringView projectDir, StringView sourceStem, StringView ext,
                             StringView sourceBody)
    {
        const String srcFile = Format(u8"{}.{}", sourceStem, ext);
        RemoveDirectoryRecursive(projectDir);
        FileDelete(srcFile.AsView());

        // The host assembles the pipeline registries once from the composition root, as Tools.Mcp
        // does; asset_cook and asset_import route through them.
        pipeline::RegisterPipelineTypes();
        pipeline::BuilderRegistry builders{DefaultAllocator()};
        pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
        pipeline::ImporterRegistry importers{DefaultAllocator()};
        pipeline::RegisterAllImporters(importers);

        McpServer server;
        server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
        editor::mcp::ProjectSession session;
        editor::mcp::ProjectOwner owner;
        editor::mcp::RegisterProjectOpenTools(server, session, owner);
        editor::mcp::RegisterProjectInfoTool(server, session);
        editor::mcp::RegisterAssetTools(server, session);
        editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
        editor::mcp::RegisterAssetWriteTools(server, session, importers, operations);

        CallOk(server, u8"project_create",
               With(With(Obj(), u8"directory", projectDir), u8"name", u8"Write"));
        CallOk(server, u8"project_open", With(Obj(), u8"directory", projectDir));

        // A real source file on disk (the working directory is .test-scratch). A trivial class
        // cooks clean.
        (void)WriteFile(srcFile.AsView(),
                        Span<const byte>(reinterpret_cast<const byte*>(sourceBody.Data()), sourceBody.Size()));
        const String absPath = PathAbsolute(srcFile.AsView());

        // Import routes by extension through the shared importer set into the source DB.
        JsonValue imported = CallOk(server, u8"asset_import",
                                    With(With(Obj(), u8"source", absPath.AsView()), u8"group",
                                         u8"scripts"));
        CHECK(imported.Get(u8"type").AsString() == StringView(u8"ScriptClassAsset"));
        CHECK(imported.Get(u8"name").AsString() == sourceStem);
        CHECK(imported.Get(u8"importer").AsString() == StringView(u8"Script"));
        const String guid = imported.Get(u8"guid").AsString();

        // It is written to the source DB, in the requested group.
        JsonValue list = CallOk(server, u8"asset_list", Obj());
        CHECK(list.Get(u8"count").AsInt() == 1);
        CHECK(list.Get(u8"assets").At(0).Get(u8"group").AsString() == StringView(u8"scripts"));

        // The raw source was copied into the project's Code/ (a script is code), in its group's
        // folder, and the envelope written under Content/.
        CHECK(FileExists(Format(u8"{}/Code/scripts/{}", projectDir, srcFile.AsView()).AsView())); // its group's folder
        CHECK_FALSE(FileExists(Format(u8"{}/Sources/{}", projectDir, srcFile.AsView()).AsView()));
        CHECK(FileExists(Format(u8"{}/Content/scripts/{}.xasset", projectDir, sourceStem).AsView()));

        // Cook builds it into the cooked DB, with the product guid equal to the source guid.
        JsonValue cooked = CallOk(server, u8"asset_cook", Obj());
        CHECK(cooked.Get(u8"planned").AsInt() == 1);
        CHECK(cooked.Get(u8"cooked").AsInt() == 1);
        CHECK(cooked.Get(u8"failed").AsInt() == 0);
        CHECK(FileExists(Format(u8"{}/Cooked/scripts/{}.rasset", projectDir, sourceStem).AsView()));

        // The cooked product is visible through asset_info on the cooked DB, under the source guid.
        JsonValue product =
            CallOk(server, u8"asset_info",
                   With(With(Obj(), u8"guid", guid.AsView()), u8"database", u8"cooked"));
        CHECK(product.Get(u8"guid").AsString() == guid);

        // A second cook does nothing, since nothing is dirty.
        JsonValue again = CallOk(server, u8"asset_cook", Obj());
        CHECK(again.Get(u8"planned").AsInt() == 0);
        CHECK(again.Get(u8"upToDate").AsInt() == 1);
    }
}

#ifdef HAS_PLUGIN_ANGELSCRIPT
TEST_CASE("integration.mcp: an agent imports + cooks an AngelScript source via MCP tools")
{
    angelscript::RegisterAngelScriptScriptCook(); // the angelscript plugin's part does this as a project enabling it loads
    DriveImportCookFlow(u8"mcp_write_as", u8"mcp_write_as_src", u8"as",
                        u8"class Hello {\n  Hello() {}\n  void greet() {}\n}\n");
}
#endif // HAS_PLUGIN_ANGELSCRIPT

#ifdef HAS_PLUGIN_LUAU
TEST_CASE("integration.mcp: an agent imports + cooks a Luau source via MCP tools")
{
    luau::RegisterLuauScriptCook(); // the luau plugin's part does this as a project enabling it loads
    DriveImportCookFlow(u8"mcp_write_luau", u8"mcp_write_luau_src", u8"luau",
                        u8"Hello = {}\nHello.__index = Hello\n"
                        u8"function Hello.new(entity)\n  return setmetatable({}, Hello)\nend\n"
                        u8"function Hello:greet() end\n");
}
#endif // HAS_PLUGIN_LUAU
#endif // HAS_PLUGIN_ANGELSCRIPT || HAS_PLUGIN_LUAU

#if defined(HAS_PLUGIN_ANGELSCRIPT) || defined(HAS_PLUGIN_LUAU)
TEST_CASE("integration.mcp: script_api reports the COMPLETE engine surface, headless, no device")
{
    // The composition root that describes the surface: every subsystem facade, as metadata only,
    // with no device and no subsystem instantiated. This lets the headless MCP host report the
    // engine's full script surface (RigidBody, Audio, gui, run and the rest), not just the core.
    engine::RegisterAllScriptFacades();
    const StringView backend = RegisterSomeBackend();

    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    RegisterScriptTools(server);

    JsonValue api = CallOk(server, u8"script_api", With(Obj(), u8"language", String(backend)));
    JsonValue reported = api.Get(u8"languages").At(0);
    CHECK(reported.Get(u8"language").AsString() == backend);

    // Collect the bound script names.
    JsonValue types = reported.Get(u8"types");
    Array<String> names;
    for (i64 i = 0; i < types.Count(); ++i)
    {
        names.PushBack(types.At(i).Get(u8"scriptName").AsString());
    }
    const auto has = [&names](StringView n)
    {
        for (const String& s : names)
        {
            if (s.AsView() == n)
            {
                return true;
            }
        }
        return false;
    };

    // The core facades and the gameplay subsystem facades must all be present.
    CHECK(has(u8"Entity"));
    CHECK(has(u8"RigidBodyComponent")); // physics
    CHECK(has(u8"Audio"));              // audio
    CHECK(has(u8"gui"));                // game GUI (scriptName alias, bound lowercase)
    CHECK(has(u8"run"));                // run/scene-load facade (ApplicationRun)
    CHECK(has(u8"Net"));                // networking
    // The full surface, with physics, audio, input, gui and the rest, is much larger than the
    // core facades alone.
    CHECK(reported.Get(u8"typeCount").AsInt() > 30);
}

#if defined(HAS_PLUGIN_ANGELSCRIPT) || defined(HAS_PLUGIN_LUAU)
namespace
{
    // One property as `script_api` lists it for `language`, or null when it is not listed.
    JsonValue ListedMember(StringView language, StringView typeName, StringView memberName)
    {
        McpServer server;
        server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
        RegisterScriptTools(server);
        JsonValue types = CallOk(server, u8"script_api", With(Obj(), u8"language", language))
                              .Get(u8"languages")
                              .At(0)
                              .Get(u8"types");
        for (i64 i = 0; i < types.Count(); ++i)
        {
            JsonValue type = types.At(i);
            if (type.Get(u8"scriptName").AsString().AsView() != typeName)
            {
                continue;
            }
            JsonValue members = type.Get(u8"members");
            for (i64 m = 0; m < members.Count(); ++m)
            {
                if (members.At(m).Get(u8"name").AsString().AsView() == memberName &&
                    members.At(m).Get(u8"kind").AsString().AsView() == u8"property")
                {
                    return members.At(m);
                }
            }
        }
        return JsonValue();
    }

    // What every backend's listing promises: the properties a script can use, its bases' included,
    // and not one it cannot. A skeletal animation's clip is an asset reference, which no backend
    // has a script form for (a script changes it through SceneAnimation's setClip), and its mesh
    // entities are a container, which binds as owner methods rather than a property; its speed is
    // a plain value. A Button's text is its base's property, used on the Button as well.
    void CheckListedProperties(StringView language)
    {
        CHECK(ListedMember(language, u8"SkeletalAnimationComponent", u8"clip").IsNull());
        CHECK(ListedMember(language, u8"SkeletalAnimationComponent", u8"meshEntities").IsNull());
        JsonValue speed = ListedMember(language, u8"SkeletalAnimationComponent", u8"speed");
        REQUIRE_FALSE(speed.IsNull());
        CHECK_FALSE(speed.Get(u8"readOnly").AsBool());
        JsonValue text = ListedMember(language, u8"Button", u8"text");
        REQUIRE_FALSE(text.IsNull());
        CHECK_FALSE(text.Get(u8"readOnly").AsBool());
    }
}
#endif

#if defined(HAS_PLUGIN_ANGELSCRIPT)
#endif // a language to report on

TEST_CASE("integration.mcp: script_api lists the AngelScript properties a script can use, inherited "
          "ones too, and not an asset reference it cannot")
{
    engine::RegisterAllScriptFacades();
    angelscript::RegisterAngelScriptBackend();
    CheckListedProperties(u8"angelscript");
}
#endif

#if defined(HAS_PLUGIN_LUAU)
TEST_CASE("integration.mcp: script_api lists the Luau properties a script can use, inherited ones "
          "too, and not an asset reference or a container it cannot")
{
    engine::RegisterAllScriptFacades();
    luau::RegisterLuauScriptBackend();
    CheckListedProperties(u8"luau");
}
#endif

TEST_CASE("integration.mcp: project_info before any project is open is a tool error")
{
    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);

    JsonValue resp = CallResponse(server, u8"project_info", JsonValue::MakeObject());
    REQUIRE(resp.Has(u8"result"));
    CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == true);
}


TEST_CASE("integration.mcp: asset_import names the other claimants of an extension, and a hint "
          "picks one")
{
    RemoveDirectoryRecursive(u8"mcp_import_claimants");
    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::RegisterAllImporters(importers);

    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    server.Paths().AddFolder(foundation::vfs::FindDataRoot().AsView()); // the engine's data, as a host allows
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterAssetWriteTools(server, session, importers, operations);
    CallOk(server, u8"project_create",
           With(With(Obj(), u8"directory", u8"mcp_import_claimants"), u8"name", u8"Claims"));
    CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_import_claimants"));

    // A real PNG from the engine's data, an extension several importers claim (texture, image,
    // heightfield and the terrain masks). The test uses two copies, since a re-import of the same
    // source lands on the asset it already made.
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String png = PathJoin(dataRoot.AsView(), u8"Samples/Images/logo.png");
    REQUIRE(FileExists(png.AsView()));
    REQUIRE(FileCopyPreserving(png.AsView(), u8"mcp_import_claimants_second.png"));
    const String second = PathAbsolute(u8"mcp_import_claimants_second.png");

    // No hint: the first claimant imports, and the result names every other one.
    JsonValue unhinted = CallOk(server, u8"asset_import", With(Obj(), u8"source", png.AsView()));
    CHECK(unhinted.Get(u8"importer").AsString() == StringView(u8"Texture"));
    CHECK(unhinted.Get(u8"type").AsString() == StringView(u8"TextureAsset"));
    const JsonValue others = unhinted.Get(u8"alsoClaimableBy");
    const usize claimants = static_cast<usize>(importers.FindAllFor(u8"png").Size());
    REQUIRE(claimants >= 3u);
    REQUIRE(static_cast<usize>(others.Count()) == claimants - 1);
    bool image = false;
    bool heightfield = false;
    bool texture = false;
    for (usize i = 0; i < static_cast<usize>(others.Count()); ++i)
    {
        const String label = others.At(i).AsString();
        image = image || label == u8"Image";
        heightfield = heightfield || label == u8"Heightfield";
        texture = texture || label == u8"Texture";
    }
    CHECK(image);
    CHECK(heightfield);
    CHECK_FALSE(texture); // the one that imported is not an alternative to itself

    // A hint that claims the extension picks that importer; the result names the rest.
    JsonValue hinted = CallOk(server, u8"asset_import",
                              With(With(Obj(), u8"source", second.AsView()), u8"importer", u8"Image"));
    CHECK(hinted.Get(u8"importer").AsString() == StringView(u8"Image"));
    CHECK(hinted.Get(u8"type").AsString() == StringView(u8"ImageAsset"));
    CHECK(static_cast<usize>(hinted.Get(u8"alsoClaimableBy").Count()) == claimants - 1);

    // A hint nothing answers to is refused before any import, naming the claimants in
    // registration order (the unhinted default first).
    JsonValue refused = CallResponse(server, u8"asset_import",
                                     With(With(Obj(), u8"source", png.AsView()), u8"importer",
                                          u8"Sculpture"));
    REQUIRE(refused.Get(u8"result").Get(u8"isError").AsBool());
    const String reason = refused.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString();
    CHECK(reason.AsView().StartsWith(
        u8"no importer 'Sculpture' claims '.png'; the claimants are: Texture Image Heightfield"));

    FileDelete(u8"mcp_import_claimants_second.png");
    owner.project = nullptr;
    RemoveDirectoryRecursive(u8"mcp_import_claimants");
}
