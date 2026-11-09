// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Where a scene's spawns read their prefabs, and a script's scene loads their scenes: the database
// the app's scenes load from.
//
// A game run from its project folder has two: its content database is the cooked one (the
// resources), and its scene database the sources (the scenes and the prefabs, never cooked).
// Spawns and a script's loadScene once read the content database, so in that mode a script's
// spawn, a replicated spawn and a script's scene load found nothing, while the boot scene and the
// prefabs placed in it, loaded from the sources, came through.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import engine.defaultapp;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace content = foundation::content;

namespace
{
    // A database of its own under the build tree.
    struct Database
    {
        String dir;
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<content::ContentDatabase> db;

        explicit Database(StringView name)
            : dir(name)
        {
            RemoveDirectoryRecursive(dir.AsView());
            CreateDirectories(dir.AsView());
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), dir.AsView(), DefaultAllocator());
            db = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                      BinarySerializerFactory(), u8".rasset");
        }
        ~Database()
        {
            db.Reset();
            mount.Reset();
            RemoveDirectoryRecursive(dir.AsView());
        }

        // A two-entity prefab, Crate with Lid under it; its guid.
        Guid StoreCrate()
        {
            scene::Scene author(DefaultAllocator(), u8"author");
            const scene::EntityHandle root = author.CreateEntity(u8"Crate");
            const scene::EntityHandle lid = author.CreateEntity(u8"Lid");
            author.SetParent(lid, root);
            MemoryStream payload;
            REQUIRE(scene::CapturePrefab(author, root, payload).IsOk());
            content::Instance* prefab = db->RootGroup()->CreateInstance(u8"Crate", scene::PrefabDocument::StaticType());
            REQUIRE(prefab != nullptr);
            REQUIRE(prefab->WriteData(u8"scene", payload.Bytes()).IsOk());
            return prefab->Id();
        }
    };
}

TEST_CASE("defaultapp: a scene spawns from the database scenes load from, not the cooked one")
{
    Database cooked(u8"scratch_defaultapp_spawn_cooked");
    Database sources(u8"scratch_defaultapp_spawn_sources");
    const Guid crate = sources.StoreCrate();

    engine::runtime::DefaultApplication app;
    app.SetContentDatabase(cooked.db.Get());
    app.SetSceneDatabase(sources.db.Get());
    CHECK(app.SceneDatabase() == sources.db.Get()); // where a script's loadScene resolves too

    scene::Scene level(DefaultAllocator(), u8"level");
    scene::PrefabSpawnSystem& spawner = *level.AddSystem<scene::PrefabSpawnSystem>();
    app.OnSystemsReady(level);
    const scene::EntityHandle root = spawner.Spawn(crate, Float3{1.0f, 2.0f, 3.0f});
    REQUIRE(root.IsAssigned());
    CHECK(level.GetFirstChild(root).IsAssigned()); // with its lid
    CHECK(level.GetLocalTransform(root).position.y == doctest::Approx(2.0f));

    // A replicated spawn resolves there as well.
    CHECK(engine::runtime::DefaultApplication::ResolveNetworkPrefab(app.SceneDatabase(), nullptr, level, crate).IsAssigned());
}

TEST_CASE("defaultapp: with one database a scene spawns from it")
{
    // The editor and an exported game name one database, the content one, which holds the
    // prefabs: the scene database falls back to it.
    Database content(u8"scratch_defaultapp_spawn_one");
    const Guid crate = content.StoreCrate();
    engine::runtime::DefaultApplication app;
    app.SetContentDatabase(content.db.Get());
    CHECK(app.SceneDatabase() == content.db.Get());

    scene::Scene level(DefaultAllocator(), u8"level");
    scene::PrefabSpawnSystem& spawner = *level.AddSystem<scene::PrefabSpawnSystem>();
    app.OnSystemsReady(level);
    CHECK(spawner.Spawn(crate, Float3{0.0f, 0.0f, 0.0f}).IsAssigned());
}
