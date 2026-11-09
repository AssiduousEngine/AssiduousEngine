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
import foundation.resource;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import engine.defaultapp;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace content = foundation::content;

namespace
{
    // Headless host stub: no shell, no graphics, no windows.
    class StubHost final : public foundation::runtime::IApplicationHost
    {
    public:
        foundation::runtime::SubsystemManager& Subsystems() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        TaskPool& Tasks() noexcept override
        {
            static TaskPool tasks(DefaultAllocator(), 0u);
            return tasks;
        }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&,
                                                       const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        foundation::runtime::SubsystemManager m_context{DefaultAllocator()};
    };

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

TEST_CASE("defaultapp: a scene spawns through the app's resources: a project folder's prefab from its sources beneath the cooked")
{
    Database cooked(u8"scratch_defaultapp_spawn_cooked");
    Database sources(u8"scratch_defaultapp_spawn_sources");
    const Guid crate = sources.StoreCrate();

    // A game run from its project folder: its resources read the cooked products over the
    // sources, and its scenes load from the sources.
    foundation::content::LayeredContentDatabase view(*cooked.db, sources.db.Get());
    foundation::resource::ResourceManager resources(DefaultAllocator(), view);
    view.SetFallthrough([&resources](StringView ns, StringView name) { return resources.ReadsAuthoredRecord(ns, name); });
    StubHost host;
    engine::runtime::DefaultApplication app;
    app.SetSceneDatabase(sources.db.Get());
    app.AttachResourceManager(&resources, host); // the standard factories, the prefabs' among them
    CHECK(app.SceneDatabase() == sources.db.Get()); // where a script's loadScene resolves

    scene::Scene level(DefaultAllocator(), u8"level");
    scene::PrefabSpawnSystem& spawner = *level.AddSystem<scene::PrefabSpawnSystem>();
    app.OnSystemsReady(level);
    const scene::EntityHandle root = spawner.Spawn(crate, Float3{1.0f, 2.0f, 3.0f});
    REQUIRE(root.IsAssigned());
    CHECK(level.GetFirstChild(root).IsAssigned()); // with its lid
    CHECK(level.GetLocalTransform(root).position.y == doctest::Approx(2.0f));

    // A replicated spawn resolves there as well.
    CHECK(engine::runtime::DefaultApplication::ResolveNetworkPrefab(app.Resources(), level, crate).IsAssigned());

    app.AttachResourceManager(nullptr, host);
    view.SetFallthrough({});
}

TEST_CASE("defaultapp: with one database a scene spawns from it")
{
    // The editor and an exported game name one database, the content one, which holds the
    // prefabs: the scene database falls back to it, and the resources read it.
    Database content(u8"scratch_defaultapp_spawn_one");
    const Guid crate = content.StoreCrate();
    foundation::resource::ResourceManager resources(DefaultAllocator(), *content.db);
    StubHost host;
    engine::runtime::DefaultApplication app;
    app.SetContentDatabase(content.db.Get());
    app.AttachResourceManager(&resources, host);
    CHECK(app.SceneDatabase() == content.db.Get());

    scene::Scene level(DefaultAllocator(), u8"level");
    scene::PrefabSpawnSystem& spawner = *level.AddSystem<scene::PrefabSpawnSystem>();
    app.OnSystemsReady(level);
    CHECK(spawner.Spawn(crate, Float3{0.0f, 0.0f, 0.0f}).IsAssigned());
    app.AttachResourceManager(nullptr, host);
}
