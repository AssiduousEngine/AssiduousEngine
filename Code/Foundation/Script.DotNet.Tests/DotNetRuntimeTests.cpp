// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The engine's managed runtime: an application's assembly loads from its image, and a behaviour in it is
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

// A script context as the runtime knows it: what it loads and finds is its own.
constexpr u64 kOwner = 1;
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
    const Status loaded = runtime.LoadAssembly(kOwner, Span<const byte>{image.Value().Data(), image.Value().Size()});
    INFO(Text(runtime.Failure()));
    REQUIRE(loaded.IsOk());

    const Variant entity[1] = {Variant::From(foundation::script::Entity{})};
    const Result<u64> mover = runtime.CreateInstance(kOwner, u8"Mover", Span<const Variant>{entity, 1});
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

    // A guid's text parses in C# to the engine's Guid, and writes back the same, braces or not.
    const StringView idText = u8"3f2a9c1e-0b7d-4e5a-9c21-7d0e5b6a4f38";
    const Variant idArg[1] = {Variant::From<String>(String(u8"{3f2a9c1e-0b7d-4e5a-9c21-7d0e5b6a4f38}"))};
    const Result<Variant> id = runtime.Invoke(mover.Value(), u8"asset", Span<const Variant>{idArg, 1});
    INFO(Text(runtime.Failure()));
    REQUIRE(id.HasValue());
    REQUIRE(id.Value().TryGet<Guid>() != nullptr);
    CHECK(*id.Value().TryGet<Guid>() == Guid(idText));
    const Variant plainArg[1] = {Variant::From<String>(String(idText))};
    const Result<Variant> written = runtime.Invoke(mover.Value(), u8"assetText", Span<const Variant>{plainArg, 1});
    REQUIRE(written.HasValue());
    REQUIRE(written.Value().TryGet<String>() != nullptr);
    CHECK(*written.Value().TryGet<String>() == idText);

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

    const Result<String> described = runtime.DescribeClass(kOwner, u8"Mover", Span<const Variant>{entity, 1});
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
    CHECK_FALSE(runtime.CreateInstance(kOwner, u8"Nobody", Span<const Variant>{entity, 1}).HasValue());
    CHECK(runtime.Failure().Contains(u8"Nobody"));
}

namespace
{
    Array<byte> Image(const char* folder)
    {
        const Result<Array<byte>> image =
            ReadFile(PathJoin(StringView(reinterpret_cast<const utf8char*>(folder)), u8"TestGame.dll").AsView());
        REQUIRE(image.HasValue());
        return image.Value();
    }

    f32 TravelledAfterOneSecond(DotNetRuntime& runtime, u64 mover)
    {
        const Variant dt[1] = {Variant::From<f32>(1.0f)};
        REQUIRE(runtime.Invoke(mover, u8"onUpdate", Span<const Variant>{dt, 1}).HasValue());
        const Result<Variant> travelled = runtime.GetField(mover, u8"travelled");
        REQUIRE(travelled.HasValue());
        REQUIRE(travelled.Value().TryGet<f32>() != nullptr);
        return *travelled.Value().TryGet<f32>();
    }
}

// Hot reload in the runtime: a rebuilt game loads beside the one it replaces, a new instance is of
// the rebuilt class while one made before keeps running the old; an image that is current does not
// load again, one replaced and back (an edit undone) does; and a replaced assembly unloads (its
// context collected) once the last instance of its classes is released.
TEST_CASE("dotnet runtime: a rebuilt game loads beside the old, which unloads with its last instance")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    DotNetRuntime runtime;
    REQUIRE(runtime.Start(DefaultAllocator(), StringView(reinterpret_cast<const utf8char*>(RUNTIME_DIR))).IsOk());
    const Array<byte> first = Image(TESTGAME_DIR);
    const Array<byte> rebuilt = Image(TESTGAME_REBUILT_DIR);
    REQUIRE(HashBytes(first.Data(), first.Size()) != HashBytes(rebuilt.Data(), rebuilt.Size()));
    const Variant entity[1] = {Variant::From(foundation::script::Entity{})};
    auto make = [&]()
    {
        const Result<u64> made = runtime.CreateInstance(kOwner, u8"Mover", Span<const Variant>{entity, 1});
        REQUIRE(made.HasValue());
        return made.Value();
    };

    REQUIRE(runtime.LoadAssembly(kOwner, Span<const byte>{first.Data(), first.Size()}).IsOk());
    const u32 base = runtime.LoadedAssemblyCount();
    REQUIRE(runtime.LoadAssembly(kOwner, Span<const byte>{first.Data(), first.Size()}).IsOk()); // current: not again
    CHECK(runtime.LoadedAssemblyCount() == base);
    const u64 old = make();
    CHECK(TravelledAfterOneSecond(runtime, old) == doctest::Approx(2.5f));

    REQUIRE(runtime.LoadAssembly(kOwner, Span<const byte>{rebuilt.Data(), rebuilt.Size()}).IsOk());
    CHECK(runtime.LoadedAssemblyCount() == base + 1); // the old one has an instance yet
    const u64 fresh = make();
    CHECK(TravelledAfterOneSecond(runtime, fresh) == doctest::Approx(5.0f)); // the rebuilt class
    CHECK(TravelledAfterOneSecond(runtime, old) == doctest::Approx(5.0f));   // the old one's, a second second

    // The first image again (the edit undone): it was replaced, so it loads anew.
    REQUIRE(runtime.LoadAssembly(kOwner, Span<const byte>{first.Data(), first.Size()}).IsOk());
    CHECK(runtime.LoadedAssemblyCount() == base + 2);
    const u64 undone = make();
    CHECK(TravelledAfterOneSecond(runtime, undone) == doctest::Approx(2.5f));

    // Each replaced assembly unloads with its last instance.
    runtime.Release(old);
    CHECK(runtime.LoadedAssemblyCount() == base + 1);
    runtime.Release(fresh);
    CHECK(runtime.LoadedAssemblyCount() == base);
    runtime.Release(undone);
    CHECK(runtime.LoadedAssemblyCount() == base); // current: stays loaded
}

// Each owner (a script context: one run's C#) loads its own copy of an image, so two runs never
// share a class's statics; releasing an owner unloads its copy once its last instance goes.
TEST_CASE("dotnet runtime: each owner has its own copy of the game, statics and all")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    DotNetRuntime runtime;
    REQUIRE(runtime.Start(DefaultAllocator(), StringView(reinterpret_cast<const utf8char*>(RUNTIME_DIR))).IsOk());
    const Array<byte> image = Image(TESTGAME_DIR);
    const Variant entity[1] = {Variant::From(foundation::script::Entity{})};
    constexpr u64 kRunA = 11;
    constexpr u64 kRunB = 12;
    auto make = [&](u64 owner)
    {
        const Result<u64> made = runtime.CreateInstance(owner, u8"Mover", Span<const Variant>{entity, 1});
        REQUIRE(made.HasValue());
        return made.Value();
    };
    auto madeSoFar = [&](u64 mover)
    {
        const Result<Variant> count = runtime.Invoke(mover, u8"madeSoFar", Span<const Variant>{});
        REQUIRE(count.HasValue());
        REQUIRE(count.Value().TryGet<i32>() != nullptr);
        return *count.Value().TryGet<i32>();
    };

    const u32 base = runtime.LoadedAssemblyCount();
    REQUIRE(runtime.LoadAssembly(kRunA, Span<const byte>{image.Data(), image.Size()}).IsOk());
    REQUIRE(runtime.LoadAssembly(kRunB, Span<const byte>{image.Data(), image.Size()}).IsOk());
    CHECK(runtime.LoadedAssemblyCount() == base + 2); // the same image, a copy each

    // Two made in one run, one in the other: each counts its own (a static per copy).
    const u64 a1 = make(kRunA);
    const u64 a2 = make(kRunA);
    const u64 b1 = make(kRunB);
    CHECK(madeSoFar(a2) == 2);
    CHECK(madeSoFar(b1) == 1);

    // An owner not loaded finds nothing of the others'.
    CHECK_FALSE(runtime.CreateInstance(13, u8"Mover", Span<const Variant>{entity, 1}).HasValue());

    // Released, a run's copy unloads with its last instance; the other's stays.
    runtime.ReleaseOwner(kRunA);
    CHECK_FALSE(runtime.CreateInstance(kRunA, u8"Mover", Span<const Variant>{entity, 1}).HasValue());
    runtime.Release(a1);
    CHECK(runtime.LoadedAssemblyCount() == base + 2);
    runtime.Release(a2);
    CHECK(runtime.LoadedAssemblyCount() == base + 1);
    CHECK(madeSoFar(b1) == 1);
    runtime.Release(b1);
    runtime.ReleaseOwner(kRunB);
    CHECK(runtime.LoadedAssemblyCount() == base);
}
