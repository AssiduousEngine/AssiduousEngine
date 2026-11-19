// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// C# as a script backend, through the registry as a run host reaches it: the class's built assembly
// loads as its bytecode, the class is made with its entity, a property is set by its setter, a
// handler runs, a field reads back; a handler the class lacks is NotFound and reports nothing; a C#
// exception is a runtime error with its message, at its file and line from the assembly's symbols;
// source never loads at run time, and a class with
// no assembly is refused.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <string>

import foundation.core;
import foundation.script;
import foundation.script.dotnet;
import foundation.script.facades; // Entity

using namespace foundation::core;
using namespace foundation::script;

namespace
{
    struct Errors final : IScriptErrorHandler
    {
        Array<ScriptErrorKind> kinds;
        Array<String> messages;
        void OnError(const ScriptError& error) override
        {
            kinds.PushBack(error.kind);
            messages.PushBack(String(error.message));
        }
    };
}

TEST_CASE("dotnet backend: a behaviour runs through the script interfaces")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    dotnet::RegisterDotNetScriptBackend();
    REQUIRE(ScriptBackendRegistry::Get().FindByExtension(u8"cs") != nullptr);

    RefPtr<IScriptManager> manager = CreateScriptManagerForLanguage(dotnet::kCSharpLanguage, DefaultAllocator());
    REQUIRE(manager.Get() != nullptr);
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context.Get() != nullptr);
    Errors errors;
    context->SetErrorHandler(&errors);

    const Result<Array<byte>> image =
        ReadFile(PathJoin(StringView(reinterpret_cast<const utf8char*>(TESTGAME_DIR)), u8"TestGame.dll").AsView());
    REQUIRE(image.HasValue());
    const BehaviorModuleClass mover{u8"Mover.cs", u8"", {}, Span<const byte>{image.Value().Data(), image.Value().Size()}};
    REQUIRE(context->LoadBehaviorModule(Span<const BehaviorModuleClass>{&mover, 1}, u8"behaviors#1").IsOk());
    // A second class from the same assembly: loaded once, nothing more to do.
    REQUIRE(context->LoadBehaviorModule(Span<const BehaviorModuleClass>{&mover, 1}, u8"behaviors#2").IsOk());

    Variant entity[1] = {Variant::From(Entity{})};
    RefPtr<ScriptObject> instance = context->CreateInstance(u8"Mover", Span<Variant>{entity, 1});
    REQUIRE(instance.Get() != nullptr);

    Variant speed[1] = {Variant::From<f64>(3.0)};
    CHECK(instance->Invoke(u8"speed=", Span<Variant>{speed, 1}).HasValue());
    Variant dt[1] = {Variant::From<f32>(2.0f)};
    CHECK(instance->Invoke(u8"onUpdate", Span<Variant>{dt, 1}).HasValue());
    const Result<Variant> travelled = instance->GetProperty(u8"travelled");
    REQUIRE(travelled.HasValue());
    REQUIRE(travelled.Value().TryGet<f32>() != nullptr);
    CHECK(*travelled.Value().TryGet<f32>() == doctest::Approx(6.0f));

    // A handler C# gives the engine: held as a script delegate, called from here inside the context,
    // and let go when the engine drops it.
    {
        const Result<Variant> given = instance->Invoke(u8"tripler", Span<Variant>{});
        REQUIRE(given.HasValue());
        IScriptDelegate* handler = given.Value().AsObject<IScriptDelegate>();
        REQUIRE(handler != nullptr);
        Variant two[1] = {Variant::From<f32>(2.0f)};
        const Result<Variant> tripled = handler->Invoke(Span<Variant>{two, 1});
        REQUIRE(tripled.HasValue());
        REQUIRE(tripled.Value().TryGet<f32>() != nullptr);
        CHECK(*tripled.Value().TryGet<f32>() == doctest::Approx(6.0f));
        const Result<Variant> handled = instance->GetProperty(u8"handled");
        REQUIRE(handled.HasValue());
        CHECK(*handled.Value().TryGet<i32>() == 1);
    }

    // A handler the class does not declare: NotFound, nothing reported.
    const Result<Variant> missing = instance->Invoke(u8"onStart", Span<Variant>{});
    REQUIRE_FALSE(missing.HasValue());
    CHECK(missing.Error() == ErrorCode::NotFound);
    CHECK(errors.kinds.IsEmpty());

    // A method bound once and called by its handle: the same as by name, and the same handle for
    // the same name and count; a handle called with another count, or none, is NotFound.
    {
        const ScriptMethod update = instance->Bind(u8"onUpdate", 1);
        REQUIRE(update.IsValid());
        CHECK(instance->Bind(u8"onUpdate", 1).id == update.id);
        CHECK(instance->Invoke(update, Span<Variant>{dt, 1}).HasValue());
        CHECK(*instance->GetProperty(u8"travelled").Value().TryGet<f32>() == doctest::Approx(12.0f));
        CHECK(instance->Invoke(update, Span<Variant>{}).Error() == ErrorCode::NotFound);
        CHECK(instance->Invoke(ScriptMethod{}, Span<Variant>{}).Error() == ErrorCode::NotFound);
        CHECK(instance->Invoke(instance->Bind(u8"onStart", 0), Span<Variant>{}).Error() == ErrorCode::NotFound);
        CHECK(errors.kinds.IsEmpty());
    }

    // A C# exception: a runtime error carrying its message, through a handle as by name.
    CHECK_FALSE(instance->Invoke(instance->Bind(u8"fail", 0), Span<Variant>{}).HasValue());
    REQUIRE(errors.kinds.Size() == 1);
    CHECK(errors.kinds[0] == ScriptErrorKind::Runtime);
    CHECK(errors.messages[0].AsView().Contains(u8"failed on purpose"));

    // Source does not load at run time, and a class without its assembly is refused.
    CHECK(context->Load(u8"class X {}", u8"X.cs").Code() == ErrorCode::NotSupported);
    const BehaviorModuleClass bare{u8"Bare.cs", u8"public class Bare {}", {}, {}};
    CHECK_FALSE(context->LoadBehaviorModule(Span<const BehaviorModuleClass>{&bare, 1}, u8"behaviors#3").IsOk());
    CHECK(errors.kinds.Size() == 3);
    instance = nullptr;
    context->SetErrorHandler(nullptr);
}

TEST_CASE("dotnet backend: a C# stack's frames name the project's source paths, not the build's copies")
{
    const StringView trace =
        u8"NullReferenceException: Object reference not set to an instance of an object.\n"
        u8"   at Holdout.Weapon.Fire() in /tmp/csharp-build-0123456789abcdef/src/Code/Scripts/Weapon.cs:line 42\n"
        u8"   at Holdout.Player.Update(Double dt) in C:\\Temp\\csharp-build-0123456789abcdef\\src\\Code\\Player.cs:line 7\r\n"
        u8"   at Engine.Runtime.Call(UInt64 id)\n"
        u8"   at Tools.Helper() in /home/me/Helper.cs:line 3\n";
    const Array<ScriptStackFrame> frames = dotnet::ParseCSharpStack(trace);
    REQUIRE(frames.Size() == 4u);
    CHECK(frames[0].function == StringView(u8"Holdout.Weapon.Fire()"));
    CHECK(frames[0].file == StringView(u8"Code/Scripts/Weapon.cs"));
    CHECK(frames[0].line == 42);
    CHECK(frames[1].file == StringView(u8"Code/Player.cs"));
    CHECK(frames[1].line == 7);
    CHECK(frames[2].function == StringView(u8"Engine.Runtime.Call(UInt64 id)"));
    CHECK(frames[2].file.IsEmpty());
    CHECK(frames[3].file == StringView(u8"/home/me/Helper.cs")); // not a build's copy: as written
    CHECK(dotnet::ParseCSharpStack(u8"InvalidOperationException: no frames").IsEmpty());
}
