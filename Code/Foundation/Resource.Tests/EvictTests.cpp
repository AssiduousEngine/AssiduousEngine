// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// An evicted resource reads unbound: its handle stays, Failed and empty, its product waits out the
// frames in flight, a reload of the identity revives the same handle, a load still decoding is
// finished before the eviction, and an unknown identity is left alone.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;

namespace
{
    class PlateRecord final : public ISerializable
    {
        RTTI_OBJECT(PlateRecord, ISerializable)
    public:
        i32 width = 0;
        i32 height = 0;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
        }
    };
    class Plate final : public Object
    {
        RTTI_OBJECT(Plate, Object)
    public:
        static inline i32 destroyed = 0;
        i32 area = 0;
        ~Plate() override { ++destroyed; }
    };

    // Builds a plate, synchronously or on a worker.
    class PlateFactory final : public IResourceFactory
    {
    public:
        bool async = false;
        [[nodiscard]] const TypeInfo* ProductType() const override { return &Plate::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &PlateRecord::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance& instance) override
        {
            return Build(instance);
        }
        [[nodiscard]] bool SupportsAsync() const override { return async; }
        [[nodiscard]] RefPtr<Object> DecodeStage(foundation::content::Instance& instance) override { return Build(instance); }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override { return decoded; }

    private:
        [[nodiscard]] static RefPtr<Object> Build(foundation::content::Instance& instance)
        {
            RefPtr<ISerializable> stored = instance.ReadObject();
            const PlateRecord* record = Cast<PlateRecord>(stored.Get());
            if (record == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<Plate> plate = MakeRef<Plate>(DefaultAllocator());
            plate->area = record->width * record->height;
            return plate;
        }
    };

    struct Bed
    {
        String dir;
        UniquePtr<NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        explicit Bed(StringView name)
            : dir(name)
        {
            GlobalTypeRegistry().Register(PlateRecord::StaticType());
            RegisterSerializable<PlateRecord>();
            RemoveDirectoryRecursive(dir.AsView());
            CreateDirectories(dir.AsView());
            mount = MakeUnique<NativeFileSystem>(DefaultAllocator(), dir.AsView(), DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                                   BinarySerializerFactory(), u8".rasset");
        }
        ~Bed()
        {
            db.Reset();
            mount.Reset();
            RemoveDirectoryRecursive(dir.AsView());
        }
        Guid Author(StringView name, i32 width, i32 height, const Guid* id = nullptr)
        {
            foundation::content::Instance* instance =
                id != nullptr ? db->RootGroup()->CreateInstanceWithId(*id, name, PlateRecord::StaticType())
                              : db->RootGroup()->CreateInstance(name, PlateRecord::StaticType());
            REQUIRE(instance != nullptr);
            PlateRecord record;
            record.width = width;
            record.height = height;
            REQUIRE(instance->WriteObject(record).IsOk());
            return instance->Id();
        }
    };
}

RTTI_DEFINE_OBJECT(PlateRecord, "rtti::resource::evict::test")
RTTI_DEFINE_OBJECT(Plate, "rtti::resource::evict::test")

TEST_CASE("resource: an evicted resource's held reference reads unbound, and its product waits out the frames")
{
    Bed bed(u8"scratch_resource_evict");
    ResourceManager manager(DefaultAllocator(), *bed.db);
    PlateFactory factory;
    manager.AddFactory(&factory);
    const Guid id = bed.Author(u8"plate", 3, 3);
    Ref<Plate> held;
    held.SetId(id);
    held.Bind(manager);
    REQUIRE(held);
    CHECK(held->area == 9);
    const ResourceHandle* handle = held.GetProxy().Handle();

    const i32 destroyedBefore = Plate::destroyed;
    manager.Evict(id);
    CHECK_FALSE(held); // the held reference reads nothing
    CHECK(handle->State() == ResourceState::Failed);
    CHECK(held.GetProxy().Handle() == handle); // the same handle, still the one held
    // Parked, not freed: a frame in flight may still read it.
    CHECK(manager.GraveyardCount() == 1u);
    CHECK(Plate::destroyed == destroyedBefore);
    for (i32 frame = 0; frame < 7; ++frame)
    {
        manager.CollectGarbage();
        CHECK(Plate::destroyed == destroyedBefore);
    }
    manager.CollectGarbage();
    CHECK(manager.GraveyardCount() == 0u);
    CHECK(Plate::destroyed == destroyedBefore + 1); // and then freed
}

TEST_CASE("resource: the record deleted, the resource evicted, the record back: a reload revives the same handle")
{
    Bed bed(u8"scratch_resource_evict_revive");
    ResourceManager manager(DefaultAllocator(), *bed.db);
    PlateFactory factory;
    manager.AddFactory(&factory);
    const Guid id = bed.Author(u8"plate", 2, 5);
    Proxy<Plate> proxy = manager.Bind<Plate>(id);
    REQUIRE(proxy);
    CHECK(proxy->area == 10);
    const ResourceHandle* handle = proxy.Handle();

    REQUIRE(bed.db->DeleteInstance(id).IsOk());
    manager.Evict(id);
    CHECK_FALSE(proxy);
    CHECK_FALSE(manager.Reload(id));
    CHECK(proxy.Handle()->State() == ResourceState::Failed); // nothing to rebuild from while the record is gone

    (void)bed.Author(u8"plate", 4, 5, &id);
    CHECK(manager.Reload(id));
    CHECK(proxy.Handle()->State() == ResourceState::Ready);
    REQUIRE(proxy);
    CHECK(proxy->area == 20); // the record that came back
    CHECK(proxy.Handle() == handle); // into the same handle
}

TEST_CASE("resource: an eviction during a load stays: the load is finished first")
{
    Bed bed(u8"scratch_resource_evict_pending");
    TaskPool tasks(DefaultAllocator(), TaskPool::DefaultWorkerCount());
    ResourceManager manager(DefaultAllocator(), *bed.db, &tasks);
    PlateFactory factory;
    factory.async = true;
    manager.AddFactory(&factory);
    const Guid id = bed.Author(u8"plate", 4, 4);
    Proxy<Plate> proxy = manager.BindAsync<Plate>(id);
    CHECK(proxy.Handle()->State() == ResourceState::Pending);
    manager.Evict(id);
    CHECK(proxy.Handle()->State() == ResourceState::Failed); // finished, then evicted
    manager.WaitAll();
    CHECK(proxy.Handle()->State() == ResourceState::Failed);
    CHECK_FALSE(proxy);
    CHECK(manager.PendingCount() == 0u);
    CHECK(manager.GraveyardCount() == 1u); // the product the load made, parked
}

TEST_CASE("resource: evicting an unknown identity does nothing")
{
    Bed bed(u8"scratch_resource_evict_unknown");
    ResourceManager manager(DefaultAllocator(), *bed.db);
    PlateFactory factory;
    manager.AddFactory(&factory);
    const Guid id = bed.Author(u8"plate", 1, 1);
    Proxy<Plate> proxy = manager.Bind<Plate>(id);
    Random rng(41);
    manager.Evict(Guid::Generate(rng));
    manager.Evict(Guid{});
    CHECK(manager.GraveyardCount() == 0u);
    CHECK(proxy.Handle()->State() == ResourceState::Ready);
    REQUIRE(proxy);
    CHECK(proxy->area == 1);
}
