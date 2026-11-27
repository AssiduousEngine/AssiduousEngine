// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// C# end to end, as an agent makes a game's C#: a project file and two sources imported through the
// MCP tools, where one source's behaviour uses a class the other declares; one cook builds the
// project's assembly and cooks each class from it; the cooked behaviour then runs in a scene, its
// inspector default (computed by the other file's class) applied, a component of its entity read
// through NetworkComponent.of (an object and an enum crossing), messages with a value it sends itself
// (text and a Float3 as the payload of entity.send), and its onUpdate moving its entity. Edited while the scene runs, the behaviour restarts on the new code once the cook rebuilds
// and the assembly reloads. A source that no longer builds fails the cook, and the class keeps its
// last good build.
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
#ifdef OPTION_HAS_DOTNET
import script.dotnet.pipeline; // RegisterDotNetScriptCook: what the csharp plugin's Pipeline part registers
#endif
import engine.behavior;
import editor.mcp;
import foundation.net.replication; // NetworkComponent: a component the behaviour reaches with of()
import particles.engine; // ParticleEffectComponent: a plugin's component, reached from C# as the engine's are

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
                                  u8"public class Mover : EntityBehavior\n{\n"
                                  u8"    readonly Entity m_entity;\n"
                                  u8"    public float speed = Speeds.Doubled(1.5f);\n\n"
                                  u8"    public Mover(Entity entity) { m_entity = entity; }\n\n"
                                  u8"    public void onStart()\n    {\n"
                                  u8"        NetworkComponent net = NetworkComponent.of(m_entity);\n"
                                  u8"        bool server = net.isValid() && net.authority == NetworkAuthority.Server;\n"
                                  u8"        m_entity.send(\"Named\", server ? \"started\" : \"no component\");\n"
                                  u8"        m_entity.send(\"Raised\", new Float3 { x = 0.0f, y = 2.0f, z = 0.0f });\n    }\n\n"
                                  u8"    public void onNamed(string name) => m_entity.setName(name);\n"
                                  u8"    public void onRaised(Float3 by) => m_entity.setPosition(m_entity.position().x, by.y, 0.0f);\n\n"
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
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    pipeline::RegisterDotNetScriptCook(DefaultAllocator()); // the csharp plugin's part does this as a project enabling it loads
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
        foundation::script::BehaviorClassFactory classes(DefaultAllocator());
        foundation::script::ScriptAssemblyFactory assemblies(DefaultAllocator());
        foundation::resource::ResourceManager resources(DefaultAllocator(), cookedDb);
        resources.AddFactory(&classes);
        resources.AddFactory(&assemblies);
        const Guid moverId(mover.AsView());
        REQUIRE_FALSE(moverId.IsNil());
        foundation::resource::Proxy<foundation::script::BehaviorClass> moverClass =
            resources.Bind<foundation::script::BehaviorClass>(moverId);
        REQUIRE(moverClass);
        CHECK(moverClass->language == u8"csharp");
        CHECK(moverClass->HasHandler(u8"onUpdate"));
        REQUIRE(moverClass->assembly.Get() != nullptr);
        CHECK_FALSE(Guid(project.AsView()).IsNil());
        const foundation::script::BehaviorProperty* speed =
            moverClass->FindProperty(foundation::script::BehaviorPropertyNameHash(u8"speed"));
        REQUIRE(speed != nullptr);
        CHECK(speed->defaultValue.number == doctest::Approx(3.0)); // computed by the other file's class

        // A headless scene runs it.
        foundation::run::RunBase run{DefaultAllocator(), u8"csharp-test"};
        scene::Scene world{DefaultAllocator(), u8"csharp-test"};
        engine::behavior::BehaviorRunHost host{DefaultAllocator()};
        RegisterCoreTypes();
        engine::behavior::RegisterBehaviorComponentReflection();
        foundation::script::RegisterScriptFacadeReflection();
        foundation::net::RegisterNetworkComponentScriptFacade();
        world.SetRun(&run);
        auto* net = world.AddSystem<foundation::net::NetworkComponentManager>();
        auto* components = world.AddSystem<engine::behavior::BehaviorComponentManager>();
        auto* scripts = world.AddSystem<engine::behavior::EntityBehaviorSystem>();
        components->SetScriptSystem(scripts);
        scripts->SetRunHost(&host);
        // The single-scene message route (a real run's BehaviorSubsystem installs a scene multiplexer).
        host.Binding().dispatchMessage =
            Function<void(scene::Scene*, scene::EntityHandle, StringView, Span<const Variant>)>{
                [scripts](scene::Scene*, scene::EntityHandle target, StringView message, Span<const Variant> args)
                { scripts->EnqueueMessage(target, message, args); }};
        const scene::EntityHandle walker = world.CreateEntity(u8"walker");
        engine::behavior::BehaviorComponent& component = components->Add(walker);
        engine::behavior::AttachedBehavior behavior;
        behavior.behaviorClass.SetProxy(moverClass); // as a scene binds it: a reload reaches it
        component.behaviors.PushBack(Move(behavior));
        net->Add(walker).authority = foundation::net::NetworkAuthority::Server;
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
        CHECK(world.GetLocalTransform(walker).position.y == doctest::Approx(2.0f)); // the Float3 its message carried
        CHECK(scripts->InstanceCount() == 1u);

        // Hot reload: the behaviour edited while the scene runs. The cook rebuilds the assembly and
        // recooks the classes; reloading the assembly (as the editor does for what a cook wrote)
        // rebuilds the classes bound to it, and the behaviour restarts on the edited code.
        String edited(kMover);
        const usize at = edited.AsView().Find(StringView(u8"\"started\""));
        REQUIRE(at != StringView::kNotFound);
        edited = Format(u8"{}\"restarted\"{}", edited.AsView().SubStr(0, at),
                        edited.AsView().SubStr(at + 9, edited.Size() - at - 9));
        Write(Format(u8"{}/Code/scripts/Mover.cs", projectDir).AsView(), edited.AsView());
        JsonValue recooked = CallOk(server, u8"asset_cook", Obj());
        CHECK(recooked.Get(u8"failed").AsInt() == 0);
        const foundation::script::BehaviorClass* before = moverClass.Get();
        CHECK(resources.Reload(Guid(project.AsView())));
        CHECK(moverClass.Get() != before); // the class rebuilt with its assembly
        host.Binding().deltaSeconds = 0.5f;
        run.Events().Drain();
        world.Update(0.5f);
        CHECK(world.GetEntityName(walker) == StringView(u8"restarted"));
        CHECK(scripts->InstanceCount() == 1u);
    }

    // A source that no longer builds fails the cook; the class keeps its last good build.
    Write(Format(u8"{}/Code/scripts/Speeds.cs", projectDir).AsView(), u8"public static class Speeds { broken }\n");
    JsonValue broken = CallOk(server, u8"asset_cook", Obj());
    CHECK(broken.Get(u8"failed").AsInt() >= 1);
    CHECK(FileExists(Format(u8"{}/Cooked/scripts/Mover.rasset", projectDir).AsView()));

    (void)RemoveDirectoryRecursive(projectDir);
}
namespace
{
    constexpr StringView kWidener = u8"using Engine;\n\n"
                                    u8"public class Widener : EntityBehavior\n{\n"
                                    u8"    readonly Entity m_entity;\n"
                                    u8"    public Widener(Entity entity) { m_entity = entity; }\n\n"
                                    u8"    public void onStart()\n    {\n"
                                    u8"        ParticleEffectComponent effect = ParticleEffectComponent.of(m_entity);\n"
                                    u8"        if (!effect.isValid())\n        {\n"
                                    u8"            m_entity.setName(\"no effect\");\n            return;\n        }\n"
                                    u8"        effect.meshScale = 3.0f;\n"
                                    u8"        m_entity.setName(\"widened\");\n    }\n}\n";
}

TEST_CASE("integration.mcp: C# reaches an official plugin's component as it reaches the engine's")
{
    // The particles plugin's ParticleEffectComponent is in the bindings C# builds against, because
    // the bindings generator loads the official plugins before it reads the registry.
    const StringView projectDir = u8"mcp_csharp_plugin_project";
    (void)RemoveDirectoryRecursive(projectDir);

    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
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
    CallOk(server, u8"project_create", With(With(Obj(), u8"directory", projectDir), u8"name", u8"CSharpPlugin"));
    CallOk(server, u8"project_open", With(Obj(), u8"directory", projectDir));

    Write(u8"Game.csproj", csharp->NewProjectTemplate(u8"Game").AsView());
    Write(u8"Widener.cs", kWidener);
    (void)Import(server, u8"Game.csproj");
    const String widener = Import(server, u8"Widener.cs");
    JsonValue cooked = CallOk(server, u8"asset_cook", Obj());
    INFO(std::string(reinterpret_cast<const char*>(cooked.ToString().Data()), cooked.ToString().Size()));
    REQUIRE(cooked.Get(u8"failed").AsInt() == 0); // it compiled against the plugin's binding

    foundation::vfs::NativeFileSystem cookedMount(Format(u8"{}/Cooked", projectDir).AsView(), DefaultAllocator());
    foundation::content::ContentDatabase cookedDb(DefaultAllocator(), cookedMount, BinarySerializerFactory(), u8".rasset");
    foundation::script::BehaviorClassFactory classes(DefaultAllocator());
    foundation::script::ScriptAssemblyFactory assemblies(DefaultAllocator());
    foundation::resource::ResourceManager resources(DefaultAllocator(), cookedDb);
    resources.AddFactory(&classes);
    resources.AddFactory(&assemblies);
    foundation::resource::Proxy<foundation::script::BehaviorClass> widenerClass =
        resources.Bind<foundation::script::BehaviorClass>(Guid(widener.AsView()));
    REQUIRE(widenerClass);

    // A headless scene with the plugin's manager and facade, as a run with the plugin loaded has.
    foundation::run::RunBase run{DefaultAllocator(), u8"csharp-plugin-test"};
    scene::Scene world{DefaultAllocator(), u8"csharp-plugin-test"};
    engine::behavior::BehaviorRunHost host{DefaultAllocator()};
    RegisterCoreTypes();
    engine::behavior::RegisterBehaviorComponentReflection();
    foundation::script::RegisterScriptFacadeReflection();
    particles::RegisterParticleScriptFacade();
    world.SetRun(&run);
    auto* effects = world.AddSystem<particles::ParticleEffectComponentManager>();
    auto* components = world.AddSystem<engine::behavior::BehaviorComponentManager>();
    auto* scripts = world.AddSystem<engine::behavior::EntityBehaviorSystem>();
    components->SetScriptSystem(scripts);
    scripts->SetRunHost(&host);
    const scene::EntityHandle fountain = world.CreateEntity(u8"fountain");
    effects->Add(fountain).meshScale = 1.0f;
    engine::behavior::BehaviorComponent& component = components->Add(fountain);
    engine::behavior::AttachedBehavior behavior;
    behavior.behaviorClass.SetProxy(widenerClass);
    component.behaviors.PushBack(Move(behavior));
    world.Start();
    world.SetSimulationEnabled(true);
    host.Binding().deltaSeconds = 0.5f;
    run.Events().Drain();
    world.Update(0.5f);
    CHECK(world.GetEntityName(fountain) == StringView(u8"widened"));
    REQUIRE(effects->Get(fountain) != nullptr);
    CHECK(effects->Get(fountain)->meshScale == doctest::Approx(3.0f));
    particles::RemoveParticleScriptFacade();

    (void)RemoveDirectoryRecursive(projectDir);
}
#endif // OPTION_HAS_DOTNET
