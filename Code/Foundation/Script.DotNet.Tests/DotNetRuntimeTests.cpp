// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The engine's managed runtime: a game's assembly loads from its image, and a behaviour in it is
// constructed with its entity, has a field set as an inspector value arrives (a double for a float
// field), is called by handler name with numbers, text and value structs in and out, is read, says
// what it offers (its handlers, its properties with their defaults, tooltips and replication), and
// reports a C# exception with its message.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <string>

import foundation.core;
import foundation.script.dotnet;
import foundation.script.facades; // Entity

using namespace foundation::core;
using namespace foundation::script::dotnet;

namespace
{
    std::string Text(StringView text) { return std::string(reinterpret_cast<const char*>(text.Data()), text.Size()); }
}

TEST_CASE("dotnet runtime: a game's behaviour is made, set, called, read and described")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();

    DotNetRuntime runtime;
    const Status started = runtime.Start(DefaultAllocator(), StringView(reinterpret_cast<const utf8char*>(RUNTIME_DIR)));
    INFO(Text(runtime.Failure()));
    REQUIRE(started.IsOk());

    const Result<Array<byte>> image =
        ReadFile(PathJoin(StringView(reinterpret_cast<const utf8char*>(TESTGAME_DIR)), u8"TestGame.dll").AsView());
    REQUIRE(image.HasValue());
    const Status loaded = runtime.LoadAssembly(Span<const byte>{image.Value().Data(), image.Value().Size()});
    INFO(Text(runtime.Failure()));
    REQUIRE(loaded.IsOk());

    const Variant entity[1] = {Variant::From(foundation::script::Entity{})};
    const Result<u64> mover = runtime.CreateInstance(u8"Mover", Span<const Variant>{entity, 1});
    INFO(Text(runtime.Failure()));
    REQUIRE(mover.HasValue());

    // An inspector number arrives as a double; the float field takes it.
    const Variant speed[1] = {Variant::From<f64>(4.0)};
    REQUIRE(runtime.Invoke(mover.Value(), u8"speed=", Span<const Variant>{speed, 1}).HasValue());
    const Variant dt[1] = {Variant::From<f32>(0.5f)};
    REQUIRE(runtime.Invoke(mover.Value(), u8"onUpdate", Span<const Variant>{dt, 1}).HasValue());
    const Result<Variant> travelled = runtime.GetField(mover.Value(), u8"travelled");
    REQUIRE(travelled.HasValue());
    REQUIRE(travelled.Value().TryGet<f32>() != nullptr);
    CHECK(*travelled.Value().TryGet<f32>() == doctest::Approx(2.0f));
    const Result<Variant> updates = runtime.GetField(mover.Value(), u8"updates");
    REQUIRE(updates.HasValue());
    REQUIRE(updates.Value().TryGet<i32>() != nullptr);
    CHECK(*updates.Value().TryGet<i32>() == 1);

    const Variant name[1] = {Variant::From<String>(String(u8"Ada"))};
    const Result<Variant> greeting = runtime.Invoke(mover.Value(), u8"greet", Span<const Variant>{name, 1});
    REQUIRE(greeting.HasValue());
    REQUIRE(greeting.Value().TryGet<String>() != nullptr);
    CHECK(*greeting.Value().TryGet<String>() == StringView(u8"hello Ada from mover"));

    const Variant k[1] = {Variant::From<f32>(2.0f)};
    const Result<Variant> scaled = runtime.Invoke(mover.Value(), u8"scaled", Span<const Variant>{k, 1});
    REQUIRE(scaled.HasValue());
    REQUIRE(scaled.Value().TryGet<Float3>() != nullptr);
    CHECK(scaled.Value().TryGet<Float3>()->z == doctest::Approx(6.0f));

    // The entity it was made with: the default one, which names nothing.
    const Result<Variant> hasEntity = runtime.Invoke(mover.Value(), u8"hasEntity", Span<const Variant>{});
    INFO(Text(runtime.Failure()));
    REQUIRE(hasEntity.HasValue());
    REQUIRE(hasEntity.Value().TryGet<bool>() != nullptr);
    CHECK_FALSE(*hasEntity.Value().TryGet<bool>());

    // A C# exception comes back as a failure with its message.
    CHECK_FALSE(runtime.Invoke(mover.Value(), u8"fail", Span<const Variant>{}).HasValue());
    CHECK(runtime.Failure().Contains(u8"failed on purpose"));
    CHECK_FALSE(runtime.Invoke(mover.Value(), u8"missing", Span<const Variant>{}).HasValue());
    CHECK(runtime.Failure().Contains(u8"missing"));

    const Result<String> described = runtime.DescribeClass(u8"Mover", Span<const Variant>{entity, 1});
    INFO(Text(runtime.Failure()));
    REQUIRE(described.HasValue());
    const StringView lines = described.Value().AsView();
    INFO(Text(lines));
    CHECK(lines.Contains(u8"method onUpdate\n"));
    CHECK(lines.Contains(u8"field speed float 2.5\n"));
    CHECK(lines.Contains(u8"field updates int 0\n"));
    CHECK(lines.Contains(u8"field label string mover\n"));
    CHECK(lines.Contains(u8"field heading vec3 1 2 3\n"));
    CHECK(lines.Contains(u8"tooltip heading where it heads\n"));
    CHECK(lines.Contains(u8"replicated active\n"));
    CHECK_FALSE(lines.Contains(u8"m_entity"));

    runtime.Release(mover.Value());
    CHECK_FALSE(runtime.CreateInstance(u8"Nobody", Span<const Variant>{entity, 1}).HasValue());
    CHECK(runtime.Failure().Contains(u8"Nobody"));
}
