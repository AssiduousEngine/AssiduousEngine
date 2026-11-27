// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The C# cook: the classes a file declares (after any modifiers, comments ignored); MSBuild's
// problems read at their file, line and column, each once; the runtime's description read into
// classes (handlers, properties with defaults, tooltips, replication, a constructor's warning); a
// real project built with dotnet build, where one file's class uses another file's, described
// whole, and a broken file reported at its own line with nothing built.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <string>

import foundation.core;
import foundation.script;
import foundation.script.resource;
import foundation.script.facades;
import script.pipeline;
import csharp.pipeline;

using namespace foundation::core;
using namespace foundation::script;
using namespace pipeline;

namespace
{
    std::string Text(StringView text) { return std::string(reinterpret_cast<const char*>(text.Data()), text.Size()); }

    String Errors(const CookScriptErrorSink& sink)
    {
        String all;
        for (const CookScriptErrorSink::Entry& e : sink.errors)
            AppendFormat(all, u8"{}:{}: {}\n", e.module, e.line, e.message);
        return all;
    }
}

TEST_CASE("csharp cook: the classes a file declares, with the kind each names among its bases")
{
    const Array<DeclaredScriptClass> found = csharp::ListCSharpClasses(u8"using Engine;\n"
                                                               u8"// class Commented\n"
                                                               u8"/* public class Blocked */\n"
                                                               u8"public sealed partial class Player : EntityBehavior, IDamageable\n{\n"
                                                               u8"    class Nested { }\n"
                                                               u8"    int classic;\n}\n"
                                                               u8"internal static class Helpers<T> where T : class { }\n"
                                                               u8"public class Game : Engine.RunBehavior { }\n");
    REQUIRE(found.Size() == 4u);
    CHECK(found[0].name == u8"Player");
    CHECK(found[0].kind == BehaviorKind::Entity);
    CHECK(found[1].name == u8"Nested");
    CHECK(found[1].kind == BehaviorKind::None);
    CHECK(found[2].name == u8"Helpers");
    CHECK(found[2].kind == BehaviorKind::None); // a `where T : class` constraint is no base
    CHECK(found[3].name == u8"Game");
    CHECK(found[3].kind == BehaviorKind::Run);
}

TEST_CASE("csharp cook: a build's problems are read at their file, line and column, each once")
{
    CookScriptErrorSink sink;
    csharp::ReportBuildOutput(u8"/tmp/b/src/Game/Player.cs(12,9): error CS1002: ; expected [/tmp/b/src/Game/Game.csproj]\r\n"
                      u8"/tmp/b/src/Game/Helpers.cs(3,5): warning CS0168: The variable 'x' is declared but never used [/tmp/b/src/Game/Game.csproj]\n"
                      u8"Build FAILED.\n"
                      u8"/tmp/b/src/Game/Player.cs(12,9): error CS1002: ; expected [/tmp/b/src/Game/Game.csproj]\n",
                      u8"/tmp/b/src", sink);
    INFO(Text(Errors(sink).AsView()));
    REQUIRE(sink.errors.Size() == 2u);
    CHECK(sink.errors[0].kind == ScriptErrorKind::Compile);
    CHECK(sink.errors[0].module == u8"Game/Player.cs");
    CHECK(sink.errors[0].line == 12);
    CHECK(sink.errors[0].column == 9);
    CHECK(sink.errors[0].message == u8"CS1002: ; expected");
    CHECK(sink.errors[1].kind == ScriptErrorKind::Warning);
    CHECK(sink.errors[1].module == u8"Game/Helpers.cs");

    // Windows writes the path with backslashes; it is the same source path.
    CookScriptErrorSink windows;
    csharp::ReportBuildOutput(u8"C:\\Temp\\b\\src\\Code\\Player.cs(4,2): error CS1002: ; expected [C:\\Temp\\b\\src\\Code\\Game.csproj]\r\n",
                      u8"C:/Temp/b/src", windows);
    REQUIRE(windows.errors.Size() == 1u);
    CHECK(windows.errors[0].module == u8"Code/Player.cs");
    CHECK(windows.errors[0].line == 4);
}

TEST_CASE("csharp cook: the runtime's description reads into classes")
{
    ScriptAssemblySource assembly;
    CookScriptErrorSink sink;
    csharp::ParseAssemblyDescription(u8"class Mover\n"
                             u8"method onUpdate\n"
                             u8"field speed float 2.5\n"
                             u8"field label string two words\\nand a line\n"
                             u8"field heading vec3 1 2 3\n"
                             u8"field tint color 0.5 0.25 1 1\n"
                             u8"field target entity \n"
                             u8"tooltip speed units a second\n"
                             u8"replicated speed\n"
                             u8"class Broken\n"
                             u8"warning Broken's constructor threw at cook\n",
                             assembly, sink);
    REQUIRE(assembly.classes.Size() == 2u);
    const ScriptAssemblyClass* mover = assembly.FindClass(u8"Mover");
    REQUIRE(mover != nullptr);
    REQUIRE(mover->handlers.Size() == 1u);
    CHECK(mover->handlers[0] == u8"onUpdate");
    REQUIRE(mover->properties.Size() == 5u);
    CHECK(mover->properties[0].type == BehaviorPropertyType::Float);
    CHECK(mover->properties[0].defaultValue.number == doctest::Approx(2.5));
    CHECK(mover->properties[0].description == u8"units a second");
    CHECK(mover->properties[0].replicated);
    CHECK(mover->properties[0].hash == BehaviorPropertyNameHash(u8"speed"));
    CHECK(mover->properties[1].defaultValue.text == u8"two words\nand a line");
    CHECK(mover->properties[2].defaultValue.vector.z == doctest::Approx(3.0f));
    CHECK(mover->properties[3].defaultValue.color.g == doctest::Approx(0.25f));
    CHECK(mover->properties[4].type == BehaviorPropertyType::Entity);
    REQUIRE(sink.errors.Size() == 1u);
    CHECK(sink.errors[0].kind == ScriptErrorKind::Warning);
}

TEST_CASE("csharp cook: a project builds whole, one file's class using another's, and a broken file is reported at its line")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    RegisterScriptResource();
    csharp::RegisterDotNetScriptCook(DefaultAllocator());
    IScriptAssemblyCook* cook = ScriptAssemblyCookRegistry::Get().FindByLanguage(u8"csharp");
    REQUIRE(cook != nullptr);
    CHECK(cook->ProjectExtension() == StringView(u8"csproj"));
    const String project = cook->NewProjectTemplate(u8"Game");
    CHECK(project.AsView().Contains(u8"<AssemblyName>Game</AssemblyName>"));
    // The project file names no machine's path: the engine's runtime comes from obj/Engine.props,
    // a local file the cook writes for this machine.
    CHECK_FALSE(project.AsView().Contains(csharp::EngineRuntimeAssemblyPath().AsView()));
    CHECK(project.AsView().Contains(u8"<Import Project=\"obj/Engine.props\""));
    const Array<ScriptAssemblyFile> local = cook->LocalFiles();
    REQUIRE(local.Size() == 1u);
    CHECK(local[0].path == StringView(u8"obj/Engine.props"));
    CHECK(local[0].text.AsView().Contains(csharp::EngineRuntimeAssemblyPath().AsView()));

    ScriptAssemblyFile helpers;
    helpers.path = String(u8"Game/Helpers.cs");
    // Actor: the project's own base for its entity behaviors (abstract: never made itself).
    helpers.text = String(u8"public static class Speeds\n{\n    public static float Double(float v) => v * 2.0f;\n}\n"
                          u8"public abstract class Actor : Engine.EntityBehavior { }\n");
    ScriptAssemblyFile player;
    player.path = String(u8"Game/Actors/Player.cs");
    player.text = String(u8"using Engine;\n\n"
                         u8"public class Player : Actor\n{\n"
                         u8"    readonly Entity m_entity;\n"
                         u8"    [Tooltip(\"units a second\")] public float speed = Speeds.Double(1.5f);\n"
                         u8"    [Replicated] public int lives = 3;\n"
                         u8"    public Player(Entity entity) { m_entity = entity; }\n"
                         u8"    public void onUpdate(float dt) { }\n"
                         u8"}\n");
    {
        const ScriptAssemblyFile files[] = {helpers, player};
        ScriptAssemblySource built;
        CookScriptErrorSink sink;
        const bool ok = cook->Build(u8"Game/Game.csproj", project.AsView(), Span<const ScriptAssemblyFile>{files, 2}, sink, built);
        INFO(Text(Errors(sink).AsView()));
        REQUIRE(ok);
        CHECK(built.image.Size() > 0u);
        CHECK(built.symbols.Size() > 0u);
        const ScriptAssemblyClass* described = built.FindClass(u8"Player");
        REQUIRE(described != nullptr);
        CHECK(described->kind == BehaviorKind::Entity); // through the project's own Actor
        bool hasUpdate = false;
        for (const String& h : described->handlers)
            hasUpdate = hasUpdate || h == u8"onUpdate";
        CHECK(hasUpdate);
        REQUIRE(described->properties.Size() == 2u);
        CHECK(described->properties[0].name == u8"speed");
        CHECK(described->properties[0].defaultValue.number == doctest::Approx(3.0)); // the helper ran
        CHECK(described->properties[0].description == u8"units a second");
        CHECK(described->properties[1].replicated);
        CHECK(built.FindClass(u8"Speeds") == nullptr); // static: no instance to make

        // The class cook takes the class from that description.
        IScriptLanguageCook* classes = ScriptLanguageCookRegistry::Get().FindByLanguage(u8"csharp");
        REQUIRE(classes != nullptr);
        CHECK(classes->BuildsInAssembly());
        // Its source names no kind (its base is Actor); the built assembly knows it is an entity's.
        BehaviorClassSource cooked;
        CookScriptErrorSink classSink;
        REQUIRE(classes->CookFromAssembly(player.text.AsView(), player.path.AsView(), built, classSink, cooked));
        CHECK(cooked.language == u8"csharp");
        CHECK(cooked.className == u8"Player");
        CHECK(cooked.kind == BehaviorKind::Entity);
        CHECK(cooked.properties.Size() == 2u);

        // The helper's file: a class no instance is made of, cooked as a helper with no class.
        BehaviorClassSource helper;
        REQUIRE(classes->CookFromAssembly(helpers.text.AsView(), helpers.path.AsView(), built, classSink, helper));
        CHECK(helper.className.IsEmpty());
        CHECK(helper.handlers.IsEmpty());
    }

    // A broken file: its error at its own line, and nothing built.
    player.text = String(u8"public class Player\n{\n    public void onUpdate(float dt) { int x = }\n}\n");
    const ScriptAssemblyFile files[] = {helpers, player};
    ScriptAssemblySource built;
    CookScriptErrorSink sink;
    CHECK_FALSE(cook->Build(u8"Game/Game.csproj", project.AsView(), Span<const ScriptAssemblyFile>{files, 2}, sink, built));
    INFO(Text(Errors(sink).AsView()));
    REQUIRE_FALSE(sink.errors.IsEmpty());
    CHECK(sink.errors[0].module == u8"Game/Actors/Player.cs");
    CHECK(sink.errors[0].line == 3);
    CHECK(built.image.IsEmpty());
}
