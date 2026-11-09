// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A loaded prefab follows its record: the editor's resources read the cooked products over the
// sources (EditorProject::ResourceDb), so a prefab binds; a change the context is told of
// (NotifyAssetExternallyModified) reloads it, a deletion (NotifyAssetDeleted) evicts it, and only
// an authored record's resource is touched.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;
import editor.core;

using namespace foundation::core;
using namespace editor;
namespace content = foundation::content;
namespace scene = foundation::scene;

namespace
{
    // Saves a prefab whose root is called `rootName` into `record`.
    void SaveCrate(content::Instance& record, StringView rootName)
    {
        scene::Scene author(DefaultAllocator(), u8"crate");
        (void)author.CreateEntity(rootName);
        REQUIRE(scene::SavePrefab(author, record).IsOk());
    }

    // The loaded prefab's root name, spawned into a fresh scene.
    String RootName(const scene::PrefabResource& prefab)
    {
        scene::Scene level(DefaultAllocator(), u8"level");
        UniquePtr<IStream> payload = prefab.OpenPayload();
        const scene::EntityHandle root = scene::SpawnPrefab(level, *payload, prefab.Id());
        return root.IsAssigned() ? String(level.GetEntityName(root)) : String();
    }
}

TEST_CASE("editor-context: a loaded prefab reloads when told its record changed, and is evicted when told it went")
{
    const StringView dir = u8"scratch_authored_resource_notify";
    (void)RemoveDirectoryRecursive(dir);
    scene::RegisterSceneResourceTypes();
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Notified").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        content::Instance* record = project->SourceDb().RootGroup()->CreateInstance(u8"crate", scene::PrefabDocument::StaticType());
        REQUIRE(record != nullptr);
        SaveCrate(*record, u8"Crate");
        const Guid id = record->Id();

        // The editor's resources, as the session makes them: over the view, its predicate theirs.
        foundation::content::LayeredContentDatabase& view = project->ResourceDb();
        foundation::resource::ResourceManager resources(DefaultAllocator(), view);
        scene::PrefabFactory factory(DefaultAllocator());
        resources.AddFactory(&factory);
        view.SetFallthrough([&resources](StringView ns, StringView name) { return resources.ReadsAuthoredRecord(ns, name); });
        EditorContext context{DefaultAllocator()};
        context.SetProject(project.Get());
        context.SetResources(&resources);

        foundation::resource::Proxy<scene::PrefabResource> loaded = resources.Bind<scene::PrefabResource>(id);
        REQUIRE(loaded); // a prefab binds, from the sources beneath the cooked products
        CHECK(RootName(*loaded) == String(u8"Crate"));

        // Changed outside the page: told, it reads the record again.
        SaveCrate(*record, u8"Barrel");
        CHECK(RootName(*loaded) == String(u8"Crate")); // what was loaded, until told
        (void)context.NotifyAssetExternallyModified(id);
        REQUIRE(loaded);
        CHECK(RootName(*loaded) == String(u8"Barrel"));

        // Deleted outside: told, it is evicted, and a held reference reads nothing.
        (void)context.NotifyAssetDeleted(id, u8"crate", record->TypeNamespace(), record->TypeName());
        CHECK_FALSE(loaded);
        // Told of a record that is no authored one (none loaded): nothing happens.
        Random rng(51);
        (void)context.NotifyAssetExternallyModified(Guid::Generate(rng));

        context.SetResources(nullptr);
        context.SetProject(nullptr);
        view.SetFallthrough({});
    }
    (void)RemoveDirectoryRecursive(dir);
}
