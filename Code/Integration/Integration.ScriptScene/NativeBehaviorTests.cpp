// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.ScriptScene: a behavior written in C++ from its cook to a scene. The Native Behavior
// cook harvests a registered class's declaration (its kind, its marked fields with their defaults,
// its on<Name> handlers) into the record a script's cook writes; that class, made on a scene by the
// run host with no VM, takes an entity's override and hears an entity event with its entity
// argument an Entity, as a script's behavior does.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import engine.behavior;
import script.pipeline;

using namespace foundation::core;
using namespace foundation::script;
using namespace engine::behavior;
namespace scene = foundation::scene;

namespace nativecook
{
    // A target that counts what hits it: a property of its own (toughness), one marked as an asset
    // reference (sound), a field it keeps to itself (hits), its start and an entity event handler.
    class Target final : public EntityBehavior
    {
        RTTI_OBJECT(Target, EntityBehavior)
    public:
        f32 toughness = 3.0f;
        Guid sound;
        String label = String(u8"crate");
        i32 hits = 0;
        f64 lastStrength = 0.0;
        String lastBy;

        void OnStart() {}
        void OnHit(Entity by, f64 strength)
        {
            ++hits;
            lastStrength = strength;
            lastBy = by.name();
        }
        void Shake() {} // a helper: no handler
    };

    REFLECT_MEMBERS(Target, "rtti::nativecook")
    {
        builder.Property<&Target::toughness>("toughness")
            .PropAttribute("behaviorProperty", true)
            .PropAttribute("description", String(u8"Hits it takes"));
        builder.Property<&Target::sound>("sound")
            .PropAttribute("behaviorProperty", true)
            .PropAttribute("assetType", String(u8"AudioClip"));
        builder.Property<&Target::label>("label").PropAttribute("behaviorProperty", true);
        builder.Property<&Target::hits>("hits");
        builder.Property<&Target::lastStrength>("lastStrength");
        builder.Property<&Target::lastBy>("lastBy");
        builder.Method<&Target::OnStart>("onStart");
        builder.Method<&Target::OnHit>("onHit");
        builder.Method<&Target::Shake>("shake");
        builder.Creator();
    }

    // A class that marks a field no property can hold.
    class Odd final : public EntityBehavior
    {
        RTTI_OBJECT(Odd, EntityBehavior)
    public:
        Quaternion turn;
    };

    REFLECT_MEMBERS(Odd, "rtti::nativecook")
    {
        builder.Property<&Odd::turn>("turn").PropAttribute("behaviorProperty", true);
        builder.Creator();
    }

    void RegisterOnce()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            RegisterBehaviorComponentReflection();
            RegisterScriptFacadeReflection();
            GlobalTypeRegistry().Register(Target::StaticType());
            GlobalTypeRegistry().Register(Odd::StaticType());
            return true;
        }();
        (void)once;
    }

    // The runtime class a cooked record becomes, as the resource factory makes it.
    RefPtr<BehaviorClass> ClassFrom(const BehaviorClassSource& cooked)
    {
        RefPtr<BehaviorClass> cls = MakeRef<BehaviorClass>(DefaultAllocator());
        cls->language = cooked.language;
        cls->className = cooked.className;
        cls->kind = cooked.kind;
        cls->sourceName = cooked.sourceName;
        cls->properties = cooked.properties;
        cls->handlers = cooked.handlers;
        cls->BuildProfileName();
        return cls;
    }
}

TEST_CASE("native behaviors: the cook harvests a class's kind, its marked fields with their defaults, and its handlers")
{
    nativecook::RegisterOnce();
    const TypeInfo* type = FindNativeBehaviorClass(u8"rtti::nativecook::Target");
    REQUIRE(type != nullptr);
    BehaviorClassSource cooked;
    String why;
    REQUIRE(pipeline::HarvestNativeBehaviorClass(*type, DefaultAllocator(), cooked, why));
    CHECK(cooked.language == String(kNativeLanguage));
    CHECK(cooked.className == String(u8"rtti::nativecook::Target"));
    CHECK(cooked.kind == BehaviorKind::Entity);
    REQUIRE(cooked.properties.Size() == 3u); // toughness, sound, label: not hits
    CHECK(cooked.properties[0].name == String(u8"toughness"));
    CHECK(cooked.properties[0].type == BehaviorPropertyType::Float);
    CHECK(cooked.properties[0].defaultValue.number == doctest::Approx(3.0));
    CHECK(cooked.properties[0].description == String(u8"Hits it takes"));
    CHECK(cooked.properties[0].hash == BehaviorPropertyNameHash(u8"toughness"));
    CHECK(cooked.properties[1].type == BehaviorPropertyType::Asset);
    CHECK(cooked.properties[1].assetType == String(u8"AudioClip"));
    CHECK(cooked.properties[2].type == BehaviorPropertyType::String);
    CHECK(cooked.properties[2].defaultValue.text == String(u8"crate"));
    REQUIRE(cooked.handlers.Size() == 2u); // onStart, onHit: not shake
    CHECK(cooked.handlers[0] == String(u8"onStart"));
    CHECK(cooked.handlers[1] == String(u8"onHit"));
    CHECK(pipeline::NativeBehaviorsHash(GlobalTypeRegistry()) != 0);

    BehaviorClassSource odd;
    CHECK_FALSE(pipeline::HarvestNativeBehaviorClass(*FindNativeBehaviorClass(u8"rtti::nativecook::Odd"), DefaultAllocator(), odd, why));
    CHECK(why.AsView().Contains(u8"turn"));
    CHECK(pipeline::NativeBehaviorKindOf(TypeOf<Float3>()) == BehaviorKind::None);
}

TEST_CASE("native behaviors: a cooked native class takes an entity's override and hears an entity event, its argument an Entity")
{
    nativecook::RegisterOnce();
    BehaviorClassSource cooked;
    String why;
    REQUIRE(pipeline::HarvestNativeBehaviorClass(*FindNativeBehaviorClass(u8"rtti::nativecook::Target"), DefaultAllocator(), cooked, why));

    foundation::run::RunBase run{DefaultAllocator(), u8"native"};
    scene::Scene world{DefaultAllocator(), u8"native"};
    BehaviorRunHost host{DefaultAllocator()};
    world.SetRun(&run);
    auto* components = world.AddSystem<BehaviorComponentManager>();
    auto* behaviors = world.AddSystem<EntityBehaviorSystem>();
    components->SetScriptSystem(behaviors);
    behaviors->SetRunHost(&host);

    const scene::EntityHandle target = world.CreateEntity(u8"target");
    const scene::EntityHandle rock = world.CreateEntity(u8"rock");
    AttachedBehavior behavior;
    behavior.behaviorClass = nativecook::ClassFrom(cooked);
    BehaviorPropertyValue five;
    five.kind = BehaviorPropertyType::Float;
    five.number = 5.0;
    behavior.SetOverride(BehaviorPropertyNameHash(u8"toughness"), five);
    components->Add(target).behaviors.PushBack(Move(behavior));

    world.Start();
    world.SetSimulationEnabled(true);
    world.Update(1.0f / 60.0f);
    const Variant args[] = {Variant::From<scene::EntityHandle>(rock), Variant::From<f64>(2.5)};
    world.PostEntityEvent(scene::EntityEvent{target, u8"hit", Span<const Variant>(args, 2)});
    world.Update(1.0f / 60.0f);

    BehaviorObject* object = components->Get(target)->behaviors[0].instance.Get();
    REQUIRE(object != nullptr);
    CHECK(host.Scripts().ScriptContext() == nullptr);
    CHECK(object->GetProperty(u8"toughness").Value().Get<f32>() == doctest::Approx(5.0f));
    CHECK(object->GetProperty(u8"hits").Value().Get<i32>() == 1);
    CHECK(object->GetProperty(u8"lastStrength").Value().Get<f64>() == doctest::Approx(2.5));
    CHECK(object->GetProperty(u8"lastBy").Value().Get<String>() == String(u8"rock"));
    CHECK_FALSE(components->Get(target)->behaviors[0].faulted);
}
