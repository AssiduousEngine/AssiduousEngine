// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A prefab as a resource: PrefabFactory loads the record's scene stream into a PrefabResource, a
// reference to a prefab binds, a load can run on a worker and reload, and PrefabPayloads opens
// loaded prefabs for a spawn, refusing a prefab that nests itself and a chain past its depth limit.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;

using namespace foundation::core;
using namespace foundation::scene;
namespace content = foundation::content;
namespace resource = foundation::resource;

namespace
{
    struct Rounds
    {
        i32 rounds = 0;
    };
    void Serialize(ISerializer& ar, Rounds& r) { foundation::core::Serialize(ar, "rounds", r.rounds); }
    class RoundsManager : public SerializableComponentManager<Rounds>
    {
    public:
        RoundsManager() : SerializableComponentManager<Rounds>(u8"demo.Rounds") {}
    };

    struct PrefabDatabase
    {
        String dir;
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<content::ContentDatabase> db;

        explicit PrefabDatabase(StringView name)
            : dir(name)
        {
            RegisterSceneResourceTypes();
            (void)RemoveDirectoryRecursive(dir.AsView());
            (void)CreateDirectories(dir.AsView());
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), dir.AsView(), DefaultAllocator());
            db = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount, BinarySerializerFactory(),
                                                      u8".rasset");
        }
        ~PrefabDatabase()
        {
            db.Reset();
            mount.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }

        // A turret (root with `rounds`, a Barrel child) captured as `encoding` under the prefab
        // record `name` (made, or the one there rewritten).
        Guid StoreTurret(StringView name, i32 rounds, foundation::scene::detail::SceneStreamEncoding encoding = foundation::scene::detail::SceneStreamEncoding::Text)
        {
            Scene author(DefaultAllocator(), u8"author");
            RoundsManager* manager = author.AddSystem<RoundsManager>();
            const EntityHandle root = author.CreateEntity(u8"Turret");
            const EntityHandle barrel = author.CreateEntity(u8"Barrel");
            author.SetParent(barrel, root);
            manager->Add(root).rounds = rounds;
            MemoryStream payload;
            REQUIRE(CapturePrefab(author, root, payload, encoding).IsOk());
            content::Instance* prefab = db->RootGroup()->CreateInstance(name, PrefabDocument::StaticType());
            REQUIRE(prefab != nullptr);
            REQUIRE(prefab->WriteData(u8"scene", payload.Bytes()).IsOk());
            return prefab->Id();
        }
    };

    // Spawns the loaded prefab into a fresh scene; its root's rounds, or -1.
    i32 SpawnedRounds(const PrefabResource& prefab)
    {
        Scene scene(DefaultAllocator(), u8"level");
        RoundsManager* rounds = scene.AddSystem<RoundsManager>();
        UniquePtr<IStream> payload = prefab.OpenPayload();
        const EntityHandle root = SpawnPrefab(scene, *payload, prefab.Id());
        if (!root.IsAssigned())
        {
            return -1;
        }
        CHECK(scene.GetEntityName(root) == StringView(u8"Turret"));
        CHECK(scene.GetFirstChild(root).IsAssigned()); // the child came with it
        const Rounds* held = rounds->Get(root);
        return held != nullptr ? held->rounds : -2;
    }
}

TEST_CASE("prefab resource: a text prefab and a binary one bind, and spawn")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_bind");
    const Guid text = fixture.StoreTurret(u8"turret", 30);
    const Guid binary = fixture.StoreTurret(u8"packed", 12, foundation::scene::detail::SceneStreamEncoding::Binary);
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    PrefabFactory factory(DefaultAllocator());
    manager.AddFactory(&factory);

    resource::Proxy<PrefabResource> fromText = manager.Bind<PrefabResource>(text);
    REQUIRE(fromText);
    CHECK(fromText->Id() == text);
    CHECK(fromText->Name() == StringView(u8"turret"));
    CHECK(fromText->Encoding() == foundation::scene::detail::SceneStreamEncoding::Text); // a source's text is kept as it is
    CHECK(fromText->PayloadSize() > 0u);
    CHECK(SpawnedRounds(*fromText) == 30);

    resource::Proxy<PrefabResource> fromBinary = manager.Bind<PrefabResource>(binary);
    REQUIRE(fromBinary);
    CHECK(fromBinary->Encoding() == foundation::scene::detail::SceneStreamEncoding::Binary);
    CHECK(SpawnedRounds(*fromBinary) == 12);

    // One handle per prefab: binding again shares the product.
    CHECK(manager.Bind<PrefabResource>(text).Get() == fromText.Get());

    // A reference to a prefab reaches it.
    resource::Ref<PrefabResource> reference;
    reference.SetId(text);
    CHECK_FALSE(reference);
    reference.Bind(manager);
    REQUIRE(reference);
    CHECK(reference->Id() == text);
}

TEST_CASE("prefab resource: an unknown id, a prefab with no stream, and a scene record fail")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_fail");
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    PrefabFactory factory(DefaultAllocator());
    manager.AddFactory(&factory);

    Random rng(31);
    CHECK_FALSE(manager.Bind<PrefabResource>(Guid::Generate(rng)));
    content::Instance* empty = fixture.db->RootGroup()->CreateInstance(u8"empty", PrefabDocument::StaticType());
    REQUIRE(empty != nullptr);
    CHECK_FALSE(manager.Bind<PrefabResource>(empty->Id()));
    // A scene, stream and all: never loaded through a prefab reference.
    Scene level(DefaultAllocator(), u8"level");
    (void)level.CreateEntity(u8"Ground");
    content::Instance* sceneRecord = fixture.db->RootGroup()->CreateInstance(u8"level", SceneDocument::StaticType());
    REQUIRE(sceneRecord != nullptr);
    REQUIRE(SaveScene(level, *sceneRecord).IsOk());
    CHECK_FALSE(manager.Bind<PrefabResource>(sceneRecord->Id()));
}

TEST_CASE("prefab resource: an async bind completes on pump, and a reload reads the rewritten stream")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_async");
    const Guid turret = fixture.StoreTurret(u8"turret", 9);
    TaskPool tasks(DefaultAllocator(), TaskPool::DefaultWorkerCount());
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db, &tasks);
    PrefabFactory factory(DefaultAllocator());
    manager.AddFactory(&factory);

    resource::Proxy<PrefabResource> proxy = manager.BindAsync<PrefabResource>(turret);
    CHECK(proxy.Handle()->State() == resource::ResourceState::Pending); // the load is on a worker
    manager.WaitAll();
    REQUIRE(proxy);
    CHECK(SpawnedRounds(*proxy) == 9);
    CHECK(manager.PendingCount() == 0u);

    // Rewritten on disk: the loaded prefab is what it was until a reload.
    REQUIRE(fixture.StoreTurret(u8"turret", 45) == turret); // the same record rewritten
    CHECK(SpawnedRounds(*proxy) == 9);
    REQUIRE(manager.Reload(turret));
    REQUIRE(proxy);
    CHECK(SpawnedRounds(*proxy) == 45); // the held proxy sees the new bytes
}

TEST_CASE("prefab resource: a prefab nesting itself is refused, not spawned again")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_self");
    const Guid loop = fixture.StoreTurret(u8"loop", 3);
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    PrefabFactory factory(DefaultAllocator());
    manager.AddFactory(&factory);
    // Its template: a shell holding an instance of the prefab itself, written past the save's own
    // refusal, as a bad edit outside the editor would.
    {
        Scene author(DefaultAllocator(), u8"loop");
        author.AddSystem<RoundsManager>();
        const EntityHandle shell = author.CreateEntity(u8"Shell");
        resource::Proxy<PrefabResource> plain = manager.Bind<PrefabResource>(loop);
        REQUIRE(plain);
        UniquePtr<IStream> payload = plain->OpenPayload();
        REQUIRE(SpawnPrefab(author, *payload, loop, shell).IsAssigned());
        MemoryStream captured;
        REQUIRE(CapturePrefab(author, shell, captured).IsOk());
        REQUIRE(fixture.db->GetInstance(loop)->WriteData(u8"scene", captured.Bytes()).IsOk());
    }
    REQUIRE(manager.Reload(loop));

    PrefabPayloads payloads(&manager);
    Scene scene(DefaultAllocator(), u8"level");
    scene.AddSystem<RoundsManager>();
    {
        UniquePtr<IStream> payload = payloads.Open(loop);
        REQUIRE(payload);
        CHECK(payloads.Depth() == 1u);
        CHECK_FALSE(payloads.Open(loop)); // open already: it would nest itself
        const EntityHandle root = SpawnPrefab(scene, *payload, loop, EntityHandle::Invalid(), nullptr, &payloads.Resolver());
        REQUIRE(root.IsAssigned());
        CHECK(scene.GetEntityName(root) == StringView(u8"Shell"));
        CHECK(scene.PrefabInstanceCount() == 1u); // the nested instance of itself was refused
        CHECK_FALSE(scene.GetFirstChild(root).IsAssigned());
    }
    CHECK(payloads.Depth() == 0u); // closed with its stream
    CHECK(static_cast<bool>(payloads.Open(loop))); // and open again once closed
}

TEST_CASE("prefab resource: distinct prefabs open together past the depth limit, the one beyond is refused")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_depth");
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    PrefabFactory factory(DefaultAllocator());
    manager.AddFactory(&factory);
    PrefabPayloads payloads(&manager);
    Array<UniquePtr<IStream>> open;
    for (usize i = 0; i < PrefabPayloads::kMaxDepth; ++i)
    {
        UniquePtr<IStream> stream = payloads.Open(fixture.StoreTurret(Format(u8"link{}", i).AsView(), static_cast<i32>(i)));
        REQUIRE(stream);
        open.PushBack(Move(stream));
    }
    const Guid beyond = fixture.StoreTurret(u8"beyond", 1);
    CHECK_FALSE(payloads.Open(beyond));
    Random rng(32);
    CHECK_FALSE(payloads.Open(Guid::Generate(rng))); // an unknown prefab opens nothing
    open.PopBack();
    UniquePtr<IStream> inside = payloads.Open(beyond);
    CHECK(static_cast<bool>(inside)); // within the limit once one closed
}

namespace
{
    // A composer that gives a scratch scene the turret's manager, as the application's gives every
    // manager it has.
    SceneComposer RoundsComposer()
    {
        return SceneComposer([](Scene& scene) { scene.AddSystem<RoundsManager>(); });
    }

    foundation::scene::detail::SceneStreamEncoding StoredEncoding(content::ContentDatabase& db, const Guid& id)
    {
        UniquePtr<IStream> stored = db.GetInstance(id)->ReadData(u8"scene");
        REQUIRE(stored);
        return foundation::scene::detail::DetectSceneStreamEncoding(*stored);
    }
}

TEST_CASE("prefab resource: a text prefab loads in the binary format given a composer, its record untouched")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_transcode");
    const Guid turret = fixture.StoreTurret(u8"turret", 30);
    SceneComposer composer = RoundsComposer();
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    PrefabFactory factory(DefaultAllocator(), &composer);
    manager.AddFactory(&factory);
    resource::Proxy<PrefabResource> loaded = manager.Bind<PrefabResource>(turret);
    REQUIRE(loaded);
    CHECK(loaded->Encoding() == foundation::scene::detail::SceneStreamEncoding::Binary); // transcoded as it loaded
    CHECK(loaded->Name() == StringView(u8"turret"));
    CHECK(SpawnedRounds(*loaded) == 30); // nothing lost on the way
    CHECK(StoredEncoding(*fixture.db, turret) == foundation::scene::detail::SceneStreamEncoding::Text); // the source stays text
}

TEST_CASE("prefab resource: an async load transcodes in its main-thread stage")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_transcode_async");
    const Guid turret = fixture.StoreTurret(u8"turret", 7);
    SceneComposer composer = RoundsComposer();
    TaskPool tasks(DefaultAllocator(), TaskPool::DefaultWorkerCount());
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db, &tasks);
    PrefabFactory factory(DefaultAllocator(), &composer);
    manager.AddFactory(&factory);
    resource::Proxy<PrefabResource> proxy = manager.BindAsync<PrefabResource>(turret);
    manager.WaitAll();
    REQUIRE(proxy);
    CHECK(proxy->Encoding() == foundation::scene::detail::SceneStreamEncoding::Binary);
    CHECK(SpawnedRounds(*proxy) == 7);
}

TEST_CASE("prefab resource: a prefab with a component no manager reads stays text")
{
    // The composer knows no rounds: the binary would be written without them, so the text is kept,
    // and a scene that has the manager spawns the prefab whole.
    PrefabDatabase fixture(u8"scratch_prefab_resource_transcode_absent");
    const Guid turret = fixture.StoreTurret(u8"turret", 30);
    SceneComposer composer([](Scene&) {});
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    PrefabFactory factory(DefaultAllocator(), &composer);
    manager.AddFactory(&factory);
    resource::Proxy<PrefabResource> loaded = manager.Bind<PrefabResource>(turret);
    REQUIRE(loaded);
    CHECK(loaded->Encoding() == foundation::scene::detail::SceneStreamEncoding::Text); // kept as stored
    CHECK(SpawnedRounds(*loaded) == 30); // its rounds kept
}

TEST_CASE("prefab resource: the scene module's factory takes the host's composer, and works without one")
{
    PrefabDatabase fixture(u8"scratch_prefab_resource_transcode_module");
    const Guid turret = fixture.StoreTurret(u8"turret", 4);
    SceneComposer composer = RoundsComposer();
    const resource::ResourceModule* modules[] = {&kSceneResourceModule};

    resource::ResourceServiceTable services;
    services.Add<SceneComposer>(&composer);
    resource::ResourceFactorySet factories;
    factories.Create(Span<const resource::ResourceModule* const>(modules, 1), DefaultAllocator(), services);
    CHECK(factories.Count() == 1u);
    resource::ResourceManager manager(DefaultAllocator(), *fixture.db);
    factories.Register(manager);
    resource::Proxy<PrefabResource> composed = manager.Bind<PrefabResource>(turret);
    REQUIRE(composed);
    CHECK(composed->Encoding() == foundation::scene::detail::SceneStreamEncoding::Binary);

    // A host offering none still loads prefabs, as stored.
    resource::NoResourceServices none;
    resource::ResourceFactorySet headless;
    headless.Create(Span<const resource::ResourceModule* const>(modules, 1), DefaultAllocator(), none);
    CHECK(headless.Count() == 1u);
    resource::ResourceManager plain(DefaultAllocator(), *fixture.db);
    headless.Register(plain);
    resource::Proxy<PrefabResource> stored = plain.Bind<PrefabResource>(turret);
    REQUIRE(stored);
    CHECK(stored->Encoding() == foundation::scene::detail::SceneStreamEncoding::Text);
}
