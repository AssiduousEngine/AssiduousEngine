// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A scene reference is an id that never loads: SceneRef is an id reference to scene documents
// (tooling reads and sets its guid as it does a Ref<T>'s, and nothing binds it), it is stored as
// its guid, and a reflected component's manager reports the scenes its SceneRef fields name, so a
// pruned export keeps them.
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

namespace
{
    // A level's exit: the scene it leads to.
    struct LevelExit
    {
        SceneRef next;
        f32 radius = 1.0f;
    };
    void Serialize(ISerializer& ar, LevelExit& e)
    {
        foundation::core::Serialize(ar, "next", e.next);
        foundation::core::Serialize(ar, "radius", e.radius);
    }
    class LevelExitManager : public SerializableComponentManager<LevelExit>
    {
    public:
        LevelExitManager() : SerializableComponentManager<LevelExit>(u8"demo.LevelExit") {}
    };
}

REFLECT_VALUE(LevelExit, "demo")
{
    builder.Property<&LevelExit::next>("next");
    builder.Property<&LevelExit::radius>("radius");
}

TEST_CASE("scene ref: an id reference to scene documents, read and set by guid, binding nothing")
{
    const TypeInfo& type = TypeOf<SceneRef>();
    REQUIRE(type.reference != nullptr);
    CHECK(type.reference->idOnly);
    CHECK(type.reference->Target() == &SceneDocument::StaticType());
    CHECK_FALSE(TypeOf<foundation::resource::Ref<PrefabResource>>().reference->idOnly); // a Ref binds

    SceneRef exit;
    CHECK_FALSE(exit.IsSet());
    Random rng(61);
    const Guid level = Guid::Generate(rng);
    type.reference->SetId(&exit, level);
    CHECK(exit.id == level);
    CHECK(*type.reference->Id(&exit) == level);
    type.reference->ClearBinding(&exit); // nothing to drop
    CHECK(exit.id == level);
}

TEST_CASE("scene ref: a component's scene field is stored as its guid and reported to a pruned export")
{
    RegisterSceneResourceTypes();
    RttiRegisterValue_LevelExit(); // its properties, which the manager reads
    const StringView dir = u8"scratch_scene_ref";
    (void)RemoveDirectoryRecursive(dir);
    (void)CreateDirectories(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        Random rng(62);
        const Guid next = Guid::Generate(rng);
        content::Instance* level = db.RootGroup()->CreateInstance(u8"level", SceneDocument::StaticType());
        REQUIRE(level != nullptr);
        {
            Scene author(DefaultAllocator(), u8"level");
            LevelExitManager* exits = author.AddSystem<LevelExitManager>();
            exits->Add(author.CreateEntity(u8"Door")).next.id = next;
            exits->Add(author.CreateEntity(u8"Window")); // names none
            exits->Add(author.CreateEntity(u8"Back door")).next.id = next; // the same scene again
            REQUIRE(SaveScene(author, *level).IsOk());
        }
        Scene loaded(DefaultAllocator(), u8"loaded");
        LevelExitManager* exits = loaded.AddSystem<LevelExitManager>();
        REQUIRE(LoadScene(*level, loaded).IsOk());
        const EntityHandle door = loaded.FindEntityByName(u8"Door");
        REQUIRE(door.IsAssigned());
        CHECK(exits->Get(door)->next.id == next); // its guid, round-tripped

        Array<Guid> ids;
        exits->CollectUnboundReferences(ids);
        REQUIRE(ids.Size() == 1u); // once, and nothing for the one naming none
        CHECK(ids[0] == next);
    }
    (void)RemoveDirectoryRecursive(dir);
}
