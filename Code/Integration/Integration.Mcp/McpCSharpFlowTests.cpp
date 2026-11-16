// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// C# end to end, as an agent makes a game's C#: a project file and two sources imported through the
// MCP tools, where one source's behaviour uses a class the other declares; one cook builds the
// project's assembly and cooks each class from it; the cooked behaviour then runs in a scene, its
// inspector default (computed by the other file's class) applied and its onUpdate moving its
// entity. A source that no longer builds fails the cook, and the class keeps its last good build.
#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <string>

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.mcp;
import foundation.resource;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import script.pipeline;
import engine.script;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
using foundation::json::JsonValue;
namespace json = foundation::json;
namespace scene = foundation::scene;

#ifdef OPTION_HAS_DOTNET
namespace
{
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
        INFO(std::string(reinterpret_cast<const char*>(resp.ToString().Data()), resp.ToString().Size()));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    JsonValue Obj() { return JsonValue::MakeObject(); }
    JsonValue With(JsonValue o, StringView k, StringView v)
    {
        o.Set(String(k), JsonValue::MakeString(String(v)));
        return o;
    }

    void Write(StringView path, StringView text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }

    String Import(McpServer& server, StringView file)
    {
        JsonValue imported = CallOk(server, u8"asset_import",
                                    With(With(Obj(), u8"source", PathAbsolute(file).AsView()), u8"group", u8"scripts"));
        return imported.Get(u8"guid").AsString();
    }

    constexpr StringView kMover = u8"using Engine;\n\n"
                                  u8"public class Mover\n{\n"
                                  u8"    readonly Entity m_entity;\n"
                                  u8"    public float speed = Speeds.Doubled(1.5f);\n\n"
                                  u8"    public Mover(Entity entity) { m_entity = entity; }\n\n"
                                  u8"    public void onStart() { m_entity.setName(\"started\"); }\n\n"
                                  u8"    public void onUpdate(float dt)\n    {\n"
                                  u8"        Float3 p = m_entity.position();\n"
                                  u8"        m_entity.setPosition(p.x + speed * dt, p.y, p.z);\n    }\n}\n";
}

TEST_CASE("integration.mcp: a C# project cooks whole through the MCP tools, and its behaviour runs in a scene")
{
    const StringView projectDir = u8"mcp_csharp_project";
    (void)RemoveDirectoryRecursive(projectDir);

    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot()); // registers C#'s cooks
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::RegisterAllImporters(importers);
    pipeline::IScriptAssemblyCook* csharp = pipeline::ScriptAssemblyCookRegistry::Get().FindByLanguage(u8"csharp");
    REQUIRE(csharp != nullptr);

    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView());
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterAssetTools(server, session);
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterAssetWriteTools(server, session, importers, operations);
    CallOk(server, u8"project_create", With(With(Obj(), u8"directory", projectDir), u8"name", u8"CSharp"));
    CallOk(server, u8"project_open", With(Obj(), u8"directory", projectDir));

    // The project file, a helper and a behaviour that uses it, imported as an agent would.
    Write(u8"Game.csproj", csharp->NewProjectTemplate(u8"Game").AsView());
    Write(u8"Speeds.cs", u8"public static class Speeds\n{\n    public static float Doubled(float v) => v * 2.0f;\n}\n");
    Write(u8"Mover.cs", kMover);
    const String project = Import(server, u8"Game.csproj");
    (void)Import(server, u8"Speeds.cs");
    const String mover = Import(server, u8"Mover.cs");

    JsonValue cooked = CallOk(server, u8"asset_cook", Obj());
    INFO(std::string(reinterpret_cast<const char*>(cooked.ToString().Data()), cooked.ToString().Size()));
    CHECK(cooked.Get(u8"failed").AsInt() == 0);
    CHECK(cooked.Get(u8"cooked").AsInt() == 3);

    // The cooked class, through the factories a game loads it with.
    {
        foundation::vfs::NativeFileSystem cookedMount(Format(u8"{}/Cooked", projectDir).AsView(), DefaultAllocator());
        foundation::content::ContentDatabase cookedDb(DefaultAllocator(), cookedMount, BinarySerializerFactory(), u8".rasset");
        foundation::script::ScriptClassFactory classes(DefaultAllocator());
        foundation::script::ScriptAssemblyFactory assemblies(DefaultAllocator());
        foundation::resource::ResourceManager resources(DefaultAllocator(), cookedDb);
        resources.AddFactory(&classes);
        resources.AddFactory(&assemblies);
        const Guid moverId(mover.AsView());
        REQUIRE_FALSE(moverId.IsNil());
        foundation::resource::Proxy<foundation::script::ScriptClass> moverClass =
            resources.Bind<foundation::script::ScriptClass>(moverId);
        REQUIRE(moverClass);
        CHECK(moverClass->language == u8"csharp");
        CHECK(moverClass->HasHandler(u8"onUpdate"));
        REQUIRE(moverClass->assembly.Get() != nullptr);
        CHECK_FALSE(Guid(project.AsView()).IsNil());
        const foundation::script::ScriptPropertyDesc* speed =
            moverClass->FindProperty(foundation::script::ScriptPropertyNameHash(u8"speed"));
        REQUIRE(speed != nullptr);
        CHECK(speed->defaultValue.number == doctest::Approx(3.0)); // computed by the other file's class

        // A headless scene runs it.
        foundation::run::RunBase run{DefaultAllocator(), u8"csharp-test"};
        scene::Scene world{DefaultAllocator(), u8"csharp-test"};
        engine::script::ScriptRunHost host{DefaultAllocator()};
        RegisterCoreTypes();
        engine::script::RegisterScriptComponentReflection();
        foundation::script::RegisterScriptFacadeReflection();
        world.SetRun(&run);
        auto* components = world.AddSystem<engine::script::ScriptComponentManager>();
        auto* scripts = world.AddSystem<engine::script::ScriptSceneSystem>();
        components->SetScriptSystem(scripts);
        scripts->SetRunHost(&host);
        const scene::EntityHandle walker = world.CreateEntity(u8"walker");
        engine::script::ScriptComponent& component = components->Add(walker);
        engine::script::ScriptBehavior behavior;
        behavior.script = RefPtr<foundation::script::ScriptClass>(moverClass.Get());
        component.behaviors.PushBack(Move(behavior));
        world.Start();
        world.SetSimulationEnabled(true);
        for (i32 frame = 0; frame < 2; ++frame)
        {
            host.Binding().deltaSeconds = 0.5f;
            run.Events().Drain();
            world.Update(0.5f);
        }
        CHECK(world.GetEntityName(walker) == StringView(u8"started"));
        CHECK(world.GetLocalTransform(walker).position.x == doctest::Approx(3.0f)); // speed 3 for two half seconds
        CHECK(scripts->InstanceCount() == 1u);
    }

    // A source that no longer builds fails the cook; the class keeps its last good build.
    Write(Format(u8"{}/Sources/Speeds.cs", projectDir).AsView(), u8"public static class Speeds { broken }\n");
    JsonValue broken = CallOk(server, u8"asset_cook", Obj());
    CHECK(broken.Get(u8"failed").AsInt() >= 1);
    CHECK(FileExists(Format(u8"{}/Cooked/scripts/Mover.rasset", projectDir).AsView()));

    (void)RemoveDirectoryRecursive(projectDir);
}
#endif // OPTION_HAS_DOTNET
