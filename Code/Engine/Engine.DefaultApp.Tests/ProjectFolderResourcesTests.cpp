// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A game run from its project folder gives its application one database for the resources: its
// cooked products over its sources (LayeredContentDatabase). The manager the application makes
// over it binds a prefab, which is never cooked, from the sources, and gives the view its predicate
// only while it lives.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import foundation.content;
import foundation.shell;
import foundation.graphics;
import foundation.vfs;
import foundation.xml.serialization;
import foundation.scene;
import foundation.scene.resource;
import engine.defaultapp;

using namespace foundation::core;
namespace runtime = foundation::runtime;
namespace content = foundation::content;
namespace scene = foundation::scene;

namespace
{
    // Headless host stub: no shell, no graphics, no windows.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::SubsystemManager& Subsystems() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        TaskPool& Tasks() noexcept override
        {
            static TaskPool tasks(DefaultAllocator(), 0u); // runs what it is given when pumped
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
        runtime::SubsystemManager m_context{DefaultAllocator()};
    };
}

TEST_CASE("defaultapp: a project folder's manager binds a prefab from the sources beneath its cooked products")
{
    const StringView root = u8"scratch_project_folder_resources";
    (void)RemoveDirectoryRecursive(root);
    (void)CreateDirectories(PathJoin(root, u8"Content").AsView());
    (void)CreateDirectories(PathJoin(root, u8"Cooked").AsView());
    scene::RegisterSceneResourceTypes();
    {
        // The player's two databases, as it opens a project folder.
        foundation::vfs::NativeFileSystem contentMount(PathJoin(root, u8"Content").AsView(), DefaultAllocator());
        foundation::vfs::NativeFileSystem cookedMount(PathJoin(root, u8"Cooked").AsView(), DefaultAllocator());
        content::ContentDatabase sources(DefaultAllocator(), contentMount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        content::ContentDatabase cooked(DefaultAllocator(), cookedMount, BinarySerializerFactory(), u8".rasset");

        // A prefab, authored and never cooked.
        Guid prefab;
        {
            scene::Scene author(DefaultAllocator(), u8"crate");
            (void)author.CreateEntity(u8"Crate");
            content::Instance* instance = sources.RootGroup()->CreateInstance(u8"crate", scene::PrefabDocument::StaticType());
            REQUIRE(instance != nullptr);
            REQUIRE(scene::SavePrefab(author, *instance).IsOk());
            prefab = instance->Id();
        }

        content::LayeredContentDatabase view(cooked, &sources);
        StubHost host;
        engine::runtime::DefaultApplication app;
        app.Configure(host);
        host.Subsystems().Startup();
        app.SetContentDatabase(view);
        app.SetSceneDatabase(&sources);
        app.OnStartup(host);
        foundation::resource::ResourceManager* resources = app.Resources();
        REQUIRE(resources != nullptr);
        CHECK(&resources->Database() == &view); // the manager reads the view
        foundation::resource::Proxy<scene::PrefabResource> loaded = resources->Bind<scene::PrefabResource>(prefab);
        REQUIRE(loaded); // the prefab from the sources
        CHECK(loaded->Name() == StringView(u8"crate"));
        CHECK(app.SceneDatabase() == &sources); // scenes still load from the sources

        loaded = {};
        host.Subsystems().Shutdown();
        app.OnShutdown(host);
        host.Subsystems().Dispose();
        CHECK(view.GetInstance(prefab) == nullptr); // the manager gone, its predicate went with it
    }
    (void)RemoveDirectoryRecursive(root);
}
