// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A manager reading through a layered database: a cooked database first, the sources beneath it,
// and the sources answering only for the stored types a registered factory declares authored
// records (IResourceFactory::ReadsAuthoredRecord).

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
    // What the cook never builds: read as it was written (a prefab document stands for these).
    class AuthoredRecord final : public ISerializable
    {
        RTTI_OBJECT(AuthoredRecord, ISerializable)
    public:
        i32 value = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "value", value); }
    };
    class AuthoredProduct final : public Object
    {
        RTTI_OBJECT(AuthoredProduct, Object)
    public:
        i32 value = 0;
    };
    // What the cook builds: its factory reads only the cooked form.
    class SourceRecord final : public ISerializable
    {
        RTTI_OBJECT(SourceRecord, ISerializable)
    public:
        i32 width = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "width", width); }
    };
    class SourceProduct final : public Object
    {
        RTTI_OBJECT(SourceProduct, Object)
    public:
        i32 width = 0;
    };

    // Builds from an authored record, and declares it one, so a layered database lets the sources
    // answer for it.
    class AuthoredRecordFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override { return &AuthoredProduct::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &AuthoredRecord::StaticType(); }
        [[nodiscard]] bool ReadsAuthoredRecord() const override { return true; }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> stored = instance.ReadObject();
            const AuthoredRecord* record = Cast<AuthoredRecord>(stored.Get());
            if (record == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<AuthoredProduct> product = MakeRef<AuthoredProduct>(DefaultAllocator());
            product->value = record->value;
            return product;
        }
    };
    // The default: a cooked product's factory, declaring nothing.
    class SourceFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override { return &SourceProduct::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &SourceRecord::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> stored = instance.ReadObject();
            const SourceRecord* record = Cast<SourceRecord>(stored.Get());
            if (record == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<SourceProduct> product = MakeRef<SourceProduct>(DefaultAllocator());
            product->width = record->width;
            return product;
        }
    };

    struct Layer
    {
        String dir;
        UniquePtr<NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        explicit Layer(StringView name)
            : dir(name)
        {
            RemoveDirectoryRecursive(dir.AsView());
            CreateDirectories(dir.AsView());
            mount = MakeUnique<NativeFileSystem>(DefaultAllocator(), dir.AsView(), DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount, BinarySerializerFactory(), u8".rasset");
        }
        ~Layer()
        {
            db.Reset();
            mount.Reset();
            RemoveDirectoryRecursive(dir.AsView());
        }
    };

    void Register()
    {
        GlobalTypeRegistry().Register(AuthoredRecord::StaticType());
        GlobalTypeRegistry().Register(SourceRecord::StaticType());
        RegisterSerializable<AuthoredRecord>();
        RegisterSerializable<SourceRecord>();
    }

    Guid StoreAuthored(foundation::content::ContentDatabase& db, const Guid& id, i32 value)
    {
        foundation::content::Instance* instance =
            db.RootGroup()->CreateInstanceWithId(id, Format(u8"record{}", value).AsView(), AuthoredRecord::StaticType());
        REQUIRE(instance != nullptr);
        AuthoredRecord record;
        record.value = value;
        REQUIRE(instance->WriteObject(record).IsOk());
        return instance->Id();
    }

    Guid StoreSource(foundation::content::ContentDatabase& db, StringView name, i32 width)
    {
        foundation::content::Instance* instance = db.RootGroup()->CreateInstance(name, SourceRecord::StaticType());
        REQUIRE(instance != nullptr);
        SourceRecord record;
        record.width = width;
        REQUIRE(instance->WriteObject(record).IsOk());
        return instance->Id();
    }

    StringView NameOf(const TypeInfo& type) { return StringView(reinterpret_cast<const utf8char*>(type.name)); }
    StringView NamespaceOf(const TypeInfo& type) { return StringView(reinterpret_cast<const utf8char*>(type.namespaceName)); }
}

RTTI_DEFINE_OBJECT(AuthoredRecord, "rtti::resource::layered::test")
RTTI_DEFINE_OBJECT(AuthoredProduct, "rtti::resource::layered::test")
RTTI_DEFINE_OBJECT(SourceRecord, "rtti::resource::layered::test")
RTTI_DEFINE_OBJECT(SourceProduct, "rtti::resource::layered::test")

TEST_CASE("resource: only a factory declaring its record authored reads the sources beneath a layered database")
{
    Register();
    Layer cooked(u8"scratch_layered_resource_cooked");
    Layer sources(u8"scratch_layered_resource_sources");
    foundation::content::LayeredContentDatabase view(*cooked.db, sources.db.Get());
    ResourceManager manager(DefaultAllocator(), view);
    view.SetFallthrough([&manager](StringView ns, StringView name) { return manager.ReadsAuthoredRecord(ns, name); });

    AuthoredRecordFactory authored;
    SourceFactory products;
    manager.AddFactory(&authored);
    manager.AddFactory(&products);
    CHECK(manager.ReadsAuthoredRecord(NamespaceOf(AuthoredRecord::StaticType()), NameOf(AuthoredRecord::StaticType())));
    CHECK_FALSE(manager.ReadsAuthoredRecord(NamespaceOf(SourceRecord::StaticType()), NameOf(SourceRecord::StaticType()))); // the default declares nothing
    CHECK_FALSE(manager.ReadsAuthoredRecord(u8"rtti::nowhere", u8"NothingReadsThis"));

    // An authored record only the sources hold binds.
    Random rng(21);
    const Guid record = StoreAuthored(*sources.db, Guid::Generate(rng), 5);
    Proxy<AuthoredProduct> bound = manager.Bind<AuthoredProduct>(record);
    REQUIRE(bound);
    CHECK(bound->value == 5);

    // A source envelope of a type the cook builds does not: its factory expects the cooked form.
    const Guid source = StoreSource(*sources.db, u8"steel", 2);
    CHECK_FALSE(manager.Bind<SourceProduct>(source));

    // Cooked, it binds from the cooked database.
    const Guid cookedSource = StoreSource(*cooked.db, u8"iron", 4);
    Proxy<SourceProduct> iron = manager.Bind<SourceProduct>(cookedSource);
    REQUIRE(iron);
    CHECK(iron->width == 4);
}

TEST_CASE("resource: the cooked record wins, and the fall-through follows the factories")
{
    Register();
    Layer cooked(u8"scratch_layered_resource_wins_cooked");
    Layer sources(u8"scratch_layered_resource_wins_sources");
    foundation::content::LayeredContentDatabase view(*cooked.db, sources.db.Get());
    ResourceManager manager(DefaultAllocator(), view);
    view.SetFallthrough([&manager](StringView ns, StringView name) { return manager.ReadsAuthoredRecord(ns, name); });

    // The predicate asks the manager at each lookup: before the factory comes, nothing falls through.
    Random rng(22);
    const Guid below = StoreAuthored(*sources.db, Guid::Generate(rng), 3);
    CHECK(view.GetInstance(below) == nullptr);
    AuthoredRecordFactory authored;
    manager.AddFactory(&authored);
    CHECK(view.GetInstance(below) != nullptr);
    CHECK(manager.ProductTypeOf(below) == &AuthoredProduct::StaticType());

    const Guid id = Guid::Generate(rng);
    (void)StoreAuthored(*cooked.db, id, 1);
    (void)StoreAuthored(*sources.db, id, 2);
    Proxy<AuthoredProduct> bound = manager.Bind<AuthoredProduct>(id);
    REQUIRE(bound);
    CHECK(bound->value == 1); // the cooked database answers first
}
