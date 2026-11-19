// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.script.resource tests: the tagged property-value wire (every kind, symmetric
// writer/reader), harvested-metadata round-trip through the cooked record, and the
// BehaviorClassFactory (cooked record to runtime product; metadata parse only, no VM).

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.script; // BehaviorKind
import foundation.script.resource;

using namespace foundation::core;
using namespace foundation::script;
namespace content = foundation::content;

namespace
{
    [[nodiscard]] BehaviorPropertyValue RoundTrip(const BehaviorPropertyValue& in)
    {
        MemoryStream stream;
        {
            BinarySerializer w(stream, SerializeMode::Write);
            BehaviorPropertyValue copy = in;
            Serialize(w, copy);
            REQUIRE(w.IsOk());
        }
        (void)stream.Seek(0, SeekOrigin::Begin);
        BehaviorPropertyValue out;
        {
            BinarySerializer r(stream, SerializeMode::Read);
            Serialize(r, out);
            REQUIRE(r.IsOk());
        }
        return out;
    }

    // The whole folder: a test that writes more records than cooked.rasset left them behind, and the
    // next run read the last run's records (one of a since-renamed type, which no longer reads).
    void RemoveDbTree(StringView dir) { (void)RemoveDirectoryRecursive(dir); }
}

TEST_CASE("script.resource: property values round-trip for every kind (symmetric wire)")
{
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Float;
        v.number = 4.25;
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Int;
        v.number = -12.0;
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Bool;
        v.boolean = true;
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::String;
        v.text = String(u8"héllo wörld");
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Color;
        v.color = Color{0.25f, 0.5f, 0.75f, 0.5f};
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Vec3;
        v.vector = Float3{1.0f, -2.0f, 3.5f};
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Entity;
        v.guid = Guid{0x12, 0x34};
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v;
        v.kind = BehaviorPropertyType::Asset;
        v.guid = Guid{0xAB, 0xCD};
        CHECK(ScriptPropertyValuesEqual(RoundTrip(v), v));
    }
    {
        BehaviorPropertyValue v; // None writes no payload and reads back as None
        CHECK(RoundTrip(v).kind == BehaviorPropertyType::None);
    }
}

TEST_CASE("script.resource: the type-string parser covers the v1 set (+ asset:<T>)")
{
    BehaviorPropertyType kind{};
    String assetType;
    CHECK(ParseScriptPropertyType(u8"float", kind, assetType));
    CHECK(kind == BehaviorPropertyType::Float);
    CHECK(ParseScriptPropertyType(u8"int", kind, assetType));
    CHECK(kind == BehaviorPropertyType::Int);
    CHECK(ParseScriptPropertyType(u8"bool", kind, assetType));
    CHECK(ParseScriptPropertyType(u8"string", kind, assetType));
    CHECK(ParseScriptPropertyType(u8"color", kind, assetType));
    CHECK(ParseScriptPropertyType(u8"vec3", kind, assetType));
    CHECK(ParseScriptPropertyType(u8"entity", kind, assetType));
    CHECK(kind == BehaviorPropertyType::Entity);
    REQUIRE(ParseScriptPropertyType(u8"asset:AudioClip", kind, assetType));
    CHECK(kind == BehaviorPropertyType::Asset);
    CHECK(assetType == u8"AudioClip");
    CHECK_FALSE(ParseScriptPropertyType(u8"quaternion", kind, assetType));
    CHECK_FALSE(ParseScriptPropertyType(u8"asset:", kind, assetType));
}

namespace
{
    // The BehaviorClassFactory is backend-neutral: it reconstructs a runtime BehaviorClass from the
    // cooked BehaviorClassSource metadata and never compiles the source (no backend is linked here),
    // so the same round-trip from record through factory to product holds for a record of either
    // language. Driven once per backend label to show the resource layer makes no backend
    // assumption.
    void DriveFactoryRoundTrip(StringView language, StringView sourceName, StringView source)
    {
        RegisterScriptResource();
        String outDir(u8"scratch_scriptres_out_");
        outDir += language;
        RemoveDbTree(outDir.AsView());
        foundation::vfs::NativeFileSystem outputMount(outDir.AsView(), foundation::core::DefaultAllocator());
        content::ContentDatabase outputDb(DefaultAllocator(), outputMount, BinarySerializerFactory(), u8".rasset");

        BehaviorClassSource cooked;
        cooked.language = String(language);
        cooked.className = String(u8"Mover");
        cooked.kind = BehaviorKind::Entity;
        cooked.sourceName = String(sourceName); // the source-file identity (breakpoint key)
        cooked.source = String(source);         // stored verbatim, never compiled here
        {
            BehaviorProperty speed;
            speed.name = String(u8"speed");
            speed.hash = BehaviorPropertyNameHash(u8"speed");
            speed.type = BehaviorPropertyType::Float;
            speed.defaultValue.kind = BehaviorPropertyType::Float;
            speed.defaultValue.number = 4.0;
            speed.description = String(u8"units per second");
            cooked.properties.PushBack(Move(speed));

            BehaviorProperty clip;
            clip.name = String(u8"clip");
            clip.hash = BehaviorPropertyNameHash(u8"clip");
            clip.type = BehaviorPropertyType::Asset;
            clip.assetType = String(u8"AudioClip");
            clip.defaultValue.kind = BehaviorPropertyType::Asset;
            cooked.properties.PushBack(Move(clip));
        }
        cooked.handlers.PushBack(String(u8"onStart"));
        cooked.handlers.PushBack(String(u8"onUpdate"));
        cooked.usesCoroutines = true;

        auto* instance =
            outputDb.RootGroup()->CreateInstance(u8"cooked", BehaviorClassSource::StaticType());
        REQUIRE(instance != nullptr);
        REQUIRE(instance->WriteObject(cooked).IsOk());

        BehaviorClassFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), outputDb);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<BehaviorClass> product =
            manager.Bind<BehaviorClass>(instance->Id());
        REQUIRE(product);
        CHECK(product->language == language);
        CHECK(product->className == u8"Mover");
        CHECK(product->kind == BehaviorKind::Entity);
        CHECK(product->sourceName == sourceName); // travels the cooked wire (symmetric)
        CHECK(product->source == source);
        REQUIRE(product->properties.Size() == 2u);
        const BehaviorProperty* speed = product->FindProperty(BehaviorPropertyNameHash(u8"speed"));
        REQUIRE(speed != nullptr);
        CHECK(speed->type == BehaviorPropertyType::Float);
        CHECK(speed->defaultValue.number == doctest::Approx(4.0));
        CHECK(speed->description == u8"units per second");
        const BehaviorProperty* clip = product->FindProperty(BehaviorPropertyNameHash(u8"clip"));
        REQUIRE(clip != nullptr);
        CHECK(clip->type == BehaviorPropertyType::Asset);
        CHECK(clip->assetType == u8"AudioClip");
        CHECK(product->HasHandler(u8"onStart"));
        CHECK(product->HasHandler(u8"onUpdate"));
        CHECK_FALSE(product->HasHandler(u8"onDestroy"));
        CHECK(product->usesCoroutines); // travels the cooked wire (symmetric)
        CHECK(product->ProfileName() != nullptr);

        RemoveDbTree(outDir.AsView());
    }
}

// The one table of authored spellings reads both ways:
// every kind's spelling names that kind, every spelling parses back to it, and None has none.
TEST_CASE("script.resource: the property type table reads both ways")
{
    for (const BehaviorPropertyTypeEntry& entry : ScriptPropertyTypeNames())
    {
        CHECK(ScriptPropertyTypeName(entry.kind) == entry.name);
        BehaviorPropertyType parsed = BehaviorPropertyType::None;
        String assetType;
        if (entry.kind == BehaviorPropertyType::Asset)
        {
            const String authored = Format(u8"{}AudioClip", kScriptAssetTypePrefix);
            CHECK(ParseScriptPropertyType(authored.AsView(), parsed, assetType));
            CHECK(assetType == u8"AudioClip");
        }
        else
        {
            CHECK(ParseScriptPropertyType(entry.name, parsed, assetType));
            CHECK(assetType.IsEmpty());
        }
        CHECK(parsed == entry.kind);
    }
    CHECK(ScriptPropertyTypeName(BehaviorPropertyType::None).IsEmpty());
}

TEST_CASE("script.resource: cooked record -> factory -> runtime product, angelscript label "
          "(metadata parse, hash lookups, handler set)")
{
    DriveFactoryRoundTrip(u8"angelscript", u8"Mover.as", u8"class Mover {\n Mover(Entity e) {}\n}\n");
}

TEST_CASE("script.resource: cooked record -> factory -> runtime product, luau label "
          "(the resource layer carries no backend assumption)")
{
    DriveFactoryRoundTrip(
        u8"luau", u8"Mover.luau",
        u8"Mover = {}\nMover.__index = Mover\n"
        u8"function Mover.new(entity) return setmetatable({}, Mover) end\n");
}

// A record cooked before each property carried its replicated flag (data version 0), before it
// carried its included files (version 1), before it named its assembly (version 2), or before it
// recorded its behavior kind (version 3), is in another layout. Read as this one it would misread
// its counts; the data version refuses it, and the script recooks.
TEST_CASE("script.resource: a record cooked before the replicated flag, the included files, the assembly or the kind is refused")
{
    RegisterScriptResource();
    TypeInfo& type = const_cast<TypeInfo&>(BehaviorClassSource::StaticType());
    const u32 current = type.dataVersion;
    CHECK(current == 4u);

    for (u32 stale = 0; stale < current; ++stale)
    {
        const StringView dir = u8"scratch_scriptres_stale";
        RemoveDbTree(dir);
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        BehaviorClassSource cooked;
        cooked.language = String(u8"angelscript");
        cooked.className = String(u8"Mover");
        BehaviorProperty speed;
        speed.name = String(u8"speed");
        cooked.properties.PushBack(Move(speed));
        auto* instance = db.RootGroup()->CreateInstance(u8"cooked", BehaviorClassSource::StaticType());
        REQUIRE(instance != nullptr);
        type.dataVersion = stale; // as the cook of that version stamped it
        REQUIRE(instance->WriteObject(cooked).IsOk());
        type.dataVersion = current;

        BehaviorClassFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<BehaviorClass> product = manager.Bind<BehaviorClass>(instance->Id());
        CHECK_FALSE(product);
        RemoveDbTree(dir);
    }
}

// A class built into its project's assembly (C#) names the assembly in its cooked record; the
// factory binds the assembly's product, whose image and symbols arrive whole, and the record keeps
// no bytecode of its own. The assembly's class descriptions travel its record for the cook.
TEST_CASE("script.resource: a class built into a project assembly binds the assembly")
{
    RegisterScriptResource();
    const StringView outDir = u8"scratch_scriptres_assembly";
    RemoveDbTree(outDir);
    {
        foundation::vfs::NativeFileSystem outputMount(outDir, DefaultAllocator());
        content::ContentDatabase outputDb(DefaultAllocator(), outputMount, BinarySerializerFactory(), u8".rasset");

        ScriptAssemblySource assembly;
        assembly.language = String(u8"csharp");
        for (u8 b : {u8{0x4d}, u8{0x5a}, u8{0x90}})
            assembly.image.PushBack(static_cast<byte>(b));
        assembly.symbols.PushBack(static_cast<byte>(0x42));
        ScriptAssemblyClass mover;
        mover.className = String(u8"Mover");
        mover.handlers.PushBack(String(u8"onUpdate"));
        assembly.classes.PushBack(Move(mover));
        auto* assemblyInstance = outputDb.RootGroup()->CreateInstance(u8"assembly", ScriptAssemblySource::StaticType());
        REQUIRE(assemblyInstance != nullptr);
        REQUIRE(assemblyInstance->WriteObject(assembly).IsOk());

        BehaviorClassSource cooked;
        cooked.language = String(u8"csharp");
        cooked.className = String(u8"Mover");
        cooked.assembly = assemblyInstance->Id();
        auto* classInstance = outputDb.RootGroup()->CreateInstance(u8"mover", BehaviorClassSource::StaticType());
        REQUIRE(classInstance != nullptr);
        REQUIRE(classInstance->WriteObject(cooked).IsOk());

        // The record reads back with its classes, for the cook of each class's asset.
        RefPtr<ISerializable> read = assemblyInstance->ReadObject();
        const ScriptAssemblySource* readBack = Cast<ScriptAssemblySource>(read.Get());
        REQUIRE(readBack != nullptr);
        REQUIRE(readBack->FindClass(u8"Mover") != nullptr);
        CHECK(readBack->FindClass(u8"Mover")->handlers.Size() == 1u);
        CHECK(readBack->FindClass(u8"Nobody") == nullptr);

        BehaviorClassFactory classes(DefaultAllocator());
        ScriptAssemblyFactory assemblies(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), outputDb);
        manager.AddFactory(&classes);
        manager.AddFactory(&assemblies);
        foundation::resource::Proxy<BehaviorClass> product = manager.Bind<BehaviorClass>(classInstance->Id());
        REQUIRE(product);
        CHECK(product->bytecode.IsEmpty());
        REQUIRE(product->assembly.Get() != nullptr);
        CHECK(product->assembly->language == u8"csharp");
        REQUIRE(product->assembly->image.Size() == 3u);
        CHECK(product->assembly->image[0] == static_cast<byte>(0x4d));
        REQUIRE(product->assembly->symbols.Size() == 1u);
    }
    RemoveDbTree(outDir);
}
