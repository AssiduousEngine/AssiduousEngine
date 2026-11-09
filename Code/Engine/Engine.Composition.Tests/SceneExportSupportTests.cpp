// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Engine.Composition.Tests - scene export support: the scene stream transcode and the scene
// reference scan every exporting host shares, both over the full manager set.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import engine.composition;
import engine.render;
import engine.script;
import foundation.script.resource;
using namespace foundation::core;
namespace scene = foundation::scene;
namespace content = foundation::content;

namespace
{
    constexpr StringView kRoot = u8"scratch_scene_export_support";

    void RemoveTree() { (void)RemoveDirectoryRecursive(kRoot); }
}

TEST_CASE("engine.composition: a scene's references and its stream come from the full manager set")
{
    engine::RegisterAllSceneComponentReflection();
    engine::RegisterAllResourceTypes();
    RemoveTree();
    foundation::vfs::NativeFileSystem mount(kRoot, DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");

    // A level whose one entity draws a mesh: the reference the scan must report.
    const Guid meshId(StringView(u8"5b1c0de4-2f3a-4c6e-9d10-7a8b9c0d1e2f"));
    content::Instance* level =
        db.RootGroup()->CreateInstance(u8"level", scene::SceneDocument::StaticType());
    REQUIRE(level != nullptr);
    {
        scene::Scene author(DefaultAllocator(), u8"level");
        engine::AddAllSceneManagers(author);
        const scene::EntityHandle entity = author.CreateEntity(u8"rock");
        author.GetSystem<engine::render::MeshComponentManager>()->Add(entity).mesh.SetId(meshId);
        REQUIRE(scene::SaveScene(author, *level).IsOk());
    }

    CHECK(engine::IsSceneLike(*level));

    Array<Guid> resources;
    Array<Guid> prefabs;
    REQUIRE(engine::ScanSceneReferences(DefaultAllocator(), *level, db, resources, prefabs));
    REQUIRE(resources.Size() == 1u);
    CHECK(resources[0] == meshId);
    CHECK(prefabs.IsEmpty());

    HashMap<Guid, Array<byte>> streams;
    engine::CollectSceneStreams(DefaultAllocator(), *db.RootGroup(), streams);
    CHECK(streams.Size() == 1u);
    const Array<byte>* binary = streams.Find(level->Id());
    REQUIRE(binary != nullptr);
    CHECK(!binary->IsEmpty());

    RemoveTree();
}

// A script property holding an asset is a reference a pruned export follows, though the scene never
// binds it: a behaviour's override and the Level's. An entity override's guid is the scene's, not
// an asset, and an asset nothing names stays out.
TEST_CASE("engine.composition: the scan reaches the assets a script's properties name")
{
    engine::RegisterAllSceneComponentReflection();
    engine::RegisterAllResourceTypes();
    RemoveTree();
    foundation::vfs::NativeFileSystem mount(kRoot, DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    const Guid dropper(StringView(u8"7a1c0de4-2f3a-4c6e-9d10-7a8b9c0d1e01"));
    const Guid pickup(StringView(u8"7a1c0de4-2f3a-4c6e-9d10-7a8b9c0d1e02"));
    const Guid theme(StringView(u8"7a1c0de4-2f3a-4c6e-9d10-7a8b9c0d1e03"));
    const Guid unused(StringView(u8"7a1c0de4-2f3a-4c6e-9d10-7a8b9c0d1e04"));
    content::Instance* level = db.RootGroup()->CreateInstance(u8"level", scene::SceneDocument::StaticType());
    REQUIRE(level != nullptr);
    {
        scene::Scene author(DefaultAllocator(), u8"level");
        engine::AddAllSceneManagers(author);
        const scene::EntityHandle spawner = author.CreateEntity(u8"Spawner");
        engine::script::ScriptComponent& scripts = author.GetSystem<engine::script::ScriptComponentManager>()->Add(spawner);
        engine::script::ScriptBehavior behavior;
        behavior.script.SetId(dropper);
        foundation::script::ScriptPropertyValue drop;
        drop.kind = foundation::script::ScriptPropertyType::Asset;
        drop.guid = pickup;
        behavior.SetOverride(foundation::script::ScriptPropertyNameHash(u8"drop"), drop);
        foundation::script::ScriptPropertyValue target;
        target.kind = foundation::script::ScriptPropertyType::Entity;
        target.guid = unused;
        behavior.SetOverride(foundation::script::ScriptPropertyNameHash(u8"target"), target);
        scripts.behaviors.PushBack(Move(behavior));
        foundation::script::ScriptPropertyValue music;
        music.kind = foundation::script::ScriptPropertyType::Asset;
        music.guid = theme;
        author.GetSystem<engine::script::SceneScriptSystem>()->Settings().SetOverride(
            foundation::script::ScriptPropertyNameHash(u8"music"), music);
        REQUIRE(scene::SaveScene(author, *level).IsOk());
    }

    Array<Guid> resources;
    Array<Guid> prefabs;
    REQUIRE(engine::ScanSceneReferences(DefaultAllocator(), *level, db, resources, prefabs));
    const auto has = [&resources](const Guid& id)
    {
        for (const Guid& r : resources)
        {
            if (r == id)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has(dropper)); // the script it binds
    CHECK(has(pickup));  // the prefab a behaviour's override names
    CHECK(has(theme));   // the asset the Level's override names
    CHECK_FALSE(has(unused)); // an entity override names no asset
    RemoveTree();
}


