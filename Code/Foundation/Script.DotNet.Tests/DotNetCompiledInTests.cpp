// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The managed runtime compiled ahead of time with a game, as an exported player has it: the runtime
// starts without .NET (it is compiled in), the game's classes are there without loading an image,
// and a behaviour is made, set, called, read and described through the same calls; the backend runs
// one through the script interfaces, its class's assembly image left unloaded.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <string>

import foundation.core;
import foundation.script;
import foundation.script.dotnet;
import foundation.script.facades; // Entity

using namespace foundation::core;

// A script context as the runtime knows it: what it loads and finds is its own.
constexpr u64 kOwner = 1;
using namespace foundation::script;

namespace
{
    std::string Text(StringView text) { return std::string(reinterpret_cast<const char*>(text.Data()), text.Size()); }
}

TEST_CASE("dotnet compiled in: the runtime and the game run without .NET")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();

    dotnet::DotNetRuntime runtime;
    const Status started = runtime.Start(DefaultAllocator(), u8"/nowhere"); // no .NET is looked for
    INFO(Text(runtime.Failure()));
    REQUIRE(started.IsOk());
    CHECK(runtime.IsCompiledIn());

    const byte notAnImage[] = {byte{1}, byte{2}};
    CHECK(runtime.LoadAssembly(kOwner, Span<const byte>{notAnImage, 2}).IsOk()); // nothing to load

    const Variant entity[1] = {Variant::From(Entity{})};
    const Result<u64> mover = runtime.CreateInstance(kOwner, u8"Mover", Span<const Variant>{entity, 1});
    INFO(Text(runtime.Failure()));
    REQUIRE(mover.HasValue());
    const Variant speed[1] = {Variant::From<f64>(4.0)};
    REQUIRE(runtime.Invoke(mover.Value(), u8"speed=", Span<const Variant>{speed, 1}).HasValue());
    const Variant dt[1] = {Variant::From<f32>(0.5f)};
    REQUIRE(runtime.Invoke(mover.Value(), u8"onUpdate", Span<const Variant>{dt, 1}).HasValue());
    const Result<Variant> travelled = runtime.GetField(mover.Value(), u8"travelled");
    REQUIRE(travelled.HasValue());
    REQUIRE(travelled.Value().TryGet<f32>() != nullptr);
    CHECK(*travelled.Value().TryGet<f32>() == doctest::Approx(2.0f));
    const Variant name[1] = {Variant::From<String>(String(u8"Ada"))};
    const Result<Variant> greeting = runtime.Invoke(mover.Value(), u8"greet", Span<const Variant>{name, 1});
    REQUIRE(greeting.HasValue());
    CHECK(*greeting.Value().TryGet<String>() == StringView(u8"hello Ada from mover"));
    for (const char8_t* method : {u8"sameEntity", u8"registryFinds"})
    {
        INFO(Text(StringView(method)));
        const Result<Variant> same = runtime.Invoke(mover.Value(), method, Span<const Variant>{});
        REQUIRE(same.HasValue());
        REQUIRE(same.Value().TryGet<bool>() != nullptr);
        CHECK(*same.Value().TryGet<bool>());
    }
    CHECK_FALSE(runtime.Invoke(mover.Value(), u8"fail", Span<const Variant>{}).HasValue());
    CHECK(runtime.Failure().Contains(u8"failed on purpose"));
    const Result<String> described = runtime.DescribeClass(kOwner, u8"Mover", Span<const Variant>{entity, 1});
    REQUIRE(described.HasValue());
    CHECK(described.Value().AsView().Contains(u8"field speed float 2.5\n"));
    runtime.Release(mover.Value());
    // The engine's own classes are not the game's.
    CHECK_FALSE(runtime.CreateInstance(kOwner, u8"Runtime", Span<const Variant>{}).HasValue());
}

TEST_CASE("dotnet compiled in: the backend runs a behaviour with its assembly image left unloaded")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    dotnet::RegisterDotNetScriptBackend();
    RefPtr<IScriptManager> manager = CreateScriptManagerForLanguage(dotnet::kCSharpLanguage, DefaultAllocator());
    REQUIRE(manager.Get() != nullptr);
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context.Get() != nullptr);
    const byte image[] = {byte{7}};
    const BehaviorModuleClass mover{u8"Mover.cs", u8"", {}, Span<const byte>{image, 1}};
    REQUIRE(context->LoadBehaviorModule(Span<const BehaviorModuleClass>{&mover, 1}, u8"behaviors#1").IsOk());
    Variant entity[1] = {Variant::From(Entity{})};
    RefPtr<BehaviorObject> instance = context->CreateInstance(u8"Mover", Span<Variant>{entity, 1});
    REQUIRE(instance.Get() != nullptr);
    Variant dt[1] = {Variant::From<f32>(2.0f)};
    CHECK(instance->Invoke(u8"onUpdate", Span<Variant>{dt, 1}).HasValue());
    const Result<Variant> travelled = instance->GetProperty(u8"travelled");
    REQUIRE(travelled.HasValue());
    CHECK(*travelled.Value().TryGet<f32>() == doctest::Approx(5.0f)); // 2.5 a second
}
