// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the dialogue sample (Data/SampleProjects/Millbrook) must stay readable and
// cookable with the dialogue plugin it enables. A scratch copy of its sources opens through the
// standalone MCP host, which loads the plugin; every conversation checks clean, the village scene
// loads with every component resolved, and the whole project cooks.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import foundation.scene;
import foundation.scene.resource;
import foundation.vfs;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
namespace runtime = foundation::runtime;
using foundation::json::JsonValue;

#if defined(HAS_PLUGIN_ANGELSCRIPT) // its scripts are AngelScript
namespace
{
    struct Called
    {
        JsonValue value;
        bool error = false;
        String text;
    };

    Called Call(McpServer& server, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = server.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        JsonValue response = json::Parse(line.response.AsView()).value;
        Called called;
        if (response.Has(u8"error"))
        {
            called.error = true;
            called.text = response.Get(u8"error").Get(u8"message").AsString();
            return called;
        }
        JsonValue result = response.Get(u8"result");
        called.error = result.Get(u8"isError").AsBool();
        called.text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (!called.error)
        {
            called.value = JsonValue::Parse(called.text.AsView());
        }
        return called;
    }
}

TEST_CASE("plugins: the Millbrook sample reads with the dialogue plugin, its conversations check clean, and it cooks")
{
    pipeline::RegisterPipelineTypes();
    engine::RegisterAllSceneComponentReflection();
    engine::RegisterAllScriptFacades(); // the scripts cook against the engine's script API
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String source = PathJoin(dataRoot.AsView(), u8"SampleProjects/Millbrook");
    REQUIRE(DirectoryExists(source.AsView()));

    // Its sources only: the cooked tree, the cache and the editor's state are regenerated.
    const StringView dir = u8"scratch_plugins_millbrook";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(CreateDirectory(dir));
    const FixedArray<StringView, 3> folders{{u8"Content", u8"Sources", u8"Code"}, 3};
    for (StringView folder : folders)
    {
        REQUIRE(CopyDirectoryRecursive(PathJoin(source.AsView(), folder).AsView(), PathJoin(dir, folder).AsView()));
    }
    Result<Array<byte>> settingsFile = ReadFile(PathJoin(source.AsView(), u8"Project.xml").AsView());
    REQUIRE(settingsFile.HasValue());
    REQUIRE(WriteFile(PathJoin(dir, u8"Project.xml").AsView(),
                      Span<const byte>(settingsFile.Value().Data(), settingsFile.Value().Size()))
                .IsOk());

    {
        pipeline::BuilderRegistry builders{DefaultAllocator()};
        pipeline::RegisterAllBuilders(builders, dataRoot.AsView());
        pipeline::ImporterRegistry importers{DefaultAllocator()};
        pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
        editor::EditorLogBuffer logBuffer{DefaultAllocator()};
        editor::mcp::McpPluginServices pluginTools{DefaultAllocator()};
        editor::mcp::ProjectSession session;
        editor::mcp::ProjectOwner owner;
        McpServer server;
        server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
        editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
        editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                         editor::mcp::EngineToolPaths{}, operations);
        pluginTools.AttachServer(server, session);
        editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, StringView{},
                                              editor::mcp::ProjectPlugins{runtime::LinkedPluginList(),
                                                                          pipeline::PipelineRegistries{&builders, &importers, &creators},
                                                                          &pluginTools});
        JsonValue open = JsonValue::MakeObject();
        open.Set(u8"directory", JsonValue::MakeString(String(dir)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(open)).error);
        REQUIRE(session.plugins != nullptr);
        CHECK(session.plugins->IsLoaded(u8"dialogue"));

        // The three dialogue files (the square, the bakery, the mill) check clean against the cast,
        // the dialogue assets and the settings' commands and flags.
        const Called validated = Call(server, u8"dialogue_validate", JsonValue::MakeObject());
        REQUIRE_FALSE(validated.error);
        CHECK(validated.value.Get(u8"files").Count() == 3);
        CHECK(validated.value.Get(u8"errors").AsNumber() == doctest::Approx(0.0));

        // The village loads with every component's type known.
        foundation::content::Instance* level = session.project->SourceDb().GetInstance(session.project->Settings().defaultSceneId);
        REQUIRE(level != nullptr);
        CHECK(level->Name() == StringView(u8"Millbrook"));
        {
            foundation::scene::Scene world(DefaultAllocator(), u8"Millbrook");
            engine::AddAllSceneManagers(world);
            REQUIRE(foundation::scene::LoadScene(*level, world).IsOk());
            CHECK(world.UnresolvedComponents().IsEmpty());
            CHECK(world.FindEntityByName(u8"Player").IsAssigned());
            CHECK(world.FindEntityByName(u8"EndingCamera").IsAssigned());
        }

        // Everything cooks: the models, the scene, the conversations, the sequences, the screens,
        // the scripts and the sounds.
        JsonValue force = JsonValue::MakeObject();
        force.Set(u8"force", JsonValue::MakeBool(true));
        const Called cooked = Call(server, u8"asset_cook", Move(force));
        REQUIRE_FALSE(cooked.error);
        CHECK(cooked.value.Get(u8"cooked").AsNumber() >= 600.0);
        CHECK(cooked.value.Get(u8"failed").AsNumber() == doctest::Approx(0.0));

        session.plugins = nullptr;
        owner.plugins.Reset();
        session.project = nullptr;
        owner.project.Reset();
        pluginTools.DetachServer(server);
    }
    (void)RemoveDirectoryRecursive(dir);
}
#endif

