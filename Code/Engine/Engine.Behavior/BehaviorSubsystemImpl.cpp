// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Behavior - implementation unit: the reflection bodies and the
// component-destroy hook. They live outside the interface for GCC: REFLECT_*
// bodies in a module interface make GCC emit an unreadable gcm cluster for
// -fno-module-lazy consumers (and a cross-partition inline virtual is not reliably
// emitted by either compiler).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.behavior;
import engine.domain;

import foundation.core;
import foundation.scene;
import foundation.resource; // resource::Ref (SceneBehaviorSettings.script)
import foundation.script;
import foundation.script.resource;

using namespace foundation::core;
using namespace foundation::script; // BehaviorProperty, BehaviorPropertyValue, BehaviorClass (the replicated state)

namespace engine::behavior
{
    // Destroying an entity (or removing the component) delivers onDestroy through the
    // scene's script system before the instances are dropped.
    void BehaviorComponentManager::OnComponentDestroyed(BehaviorComponent& component,
                                                      foundation::scene::EntityHandle entity)
    {
        if (m_scriptSystem != nullptr)
        {
            m_scriptSystem->ReleaseComponentInstances(component, entity);
        }
        else
        {
            for (AttachedBehavior& behavior : component.behaviors)
            {
                behavior.instance = nullptr;
                behavior.boundClass = nullptr;
            }
        }
    }

    // The component itself carries no inspector-editable reflected properties (its
    // behavior array renders through the bespoke inspector section), but it must be
    // reflected so the Add Component menu lists it and versioned payloads carry a
    // data version.
    REFLECT_VALUE(BehaviorComponent, "rtti::engine::behavior")
    {
        builder.Attribute("displayName", String(u8"Behavior"))
            .Attribute("category", String(u8"Scripting"))
            .Attribute("description",
                       String(u8"Runs behaviors on the entity, in order, each with its own "
                              u8"property values."))
            .DataVersion(1);
    }

    // The scene's behavior block (one per scene). Reflected so the scene-settings inspector renders
    // its behavior picker and enable toggle, and so versioned payloads carry a data version.
    REFLECT_VALUE(SceneBehaviorSettings, "rtti::engine::behavior")
    {
        builder.Attribute("displayName", String(u8"Scene Behavior"))
            .Attribute("category", String(u8"Scripting"))
            .DataVersion(2) // v2: hash-keyed Level property overrides
            .Property<&SceneBehaviorSettings::behaviorClass>("class")
            .PropAttribute("displayName", String(u8"Behavior"))
            .PropAttribute("description",
                           String(u8"A SceneBehavior class (onStart, onUpdate, onFixedUpdate, onStop), "
                                  u8"one per scene"))
            .Property<&SceneBehaviorSettings::enabled>("enabled")
            .PropAttribute("displayName", String(u8"Enabled"));
    }

    namespace
    {
        // A script's live property value, as the property's own kind (numbers come back as f64).
        [[nodiscard]] bool VariantToPropertyValue(const Variant& value, BehaviorPropertyType type,
                                                  BehaviorPropertyValue& out)
        {
            out.kind = type;
            switch (type)
            {
            case BehaviorPropertyType::Float:
            case BehaviorPropertyType::Int:
                if (const f64* n = value.TryGet<f64>())
                {
                    out.number = *n;
                    return true;
                }
                if (const f32* f = value.TryGet<f32>())
                {
                    out.number = *f;
                    return true;
                }
                if (const i32* i = value.TryGet<i32>())
                {
                    out.number = *i;
                    return true;
                }
                return false;
            case BehaviorPropertyType::Bool:
                if (const bool* b = value.TryGet<bool>())
                {
                    out.boolean = *b;
                    return true;
                }
                return false;
            case BehaviorPropertyType::String:
                if (const String* t = value.TryGet<String>())
                {
                    out.text = *t;
                    return true;
                }
                return false;
            case BehaviorPropertyType::Color:
                if (const Color* c = value.TryGet<Color>())
                {
                    out.color = *c;
                    return true;
                }
                return false;
            case BehaviorPropertyType::Vec3:
                if (const Float3* v = value.TryGet<Float3>())
                {
                    out.vector = *v;
                    return true;
                }
                return false;
            default:
                return false; // entity and asset references are never replicated (refused at cook)
            }
        }

        // One replicated property of one behaviour, as the bytes carry it.
        struct ReplicatedEntry
        {
            u32 behavior = 0;
            u64 classHash = 0; // which class the behaviour was, so a mismatch is never applied
            u64 property = 0;
            BehaviorPropertyValue value;
        };

        void Serialize(ISerializer& ar, ReplicatedEntry& e)
        {
            foundation::core::Serialize(ar, "behavior", e.behavior);
            foundation::core::Serialize(ar, "class", e.classHash);
            foundation::core::Serialize(ar, "property", e.property);
            foundation::core::Serialize(ar, "value", e.value);
        }
    }

    bool BehaviorComponentManager::WriteReplicatedState(foundation::scene::EntityHandle entity, Array<byte>& out)
    {
        const BehaviorComponent* component = Get(entity);
        if (component == nullptr)
        {
            return false;
        }
        Array<ReplicatedEntry> entries;
        for (usize b = 0; b < component->behaviors.Size(); ++b)
        {
            const AttachedBehavior& behavior = component->behaviors[b];
            const BehaviorClass* scriptClass = behavior.behaviorClass.Get();
            if (scriptClass == nullptr)
            {
                continue;
            }
            for (const BehaviorProperty& property : scriptClass->properties)
            {
                if (!property.replicated)
                {
                    continue;
                }
                ReplicatedEntry entry;
                entry.behavior = static_cast<u32>(b);
                entry.classHash = BehaviorPropertyNameHash(scriptClass->className.AsView());
                entry.property = property.hash;
                // The live value when the script runs; else what it would start with.
                bool live = false;
                if (behavior.instance.Get() != nullptr)
                {
                    const Result<Variant> current = behavior.instance->GetProperty(property.name.AsView());
                    live = current.HasValue() && VariantToPropertyValue(current.Value(), property.type, entry.value);
                }
                if (!live)
                {
                    const BehaviorPropertyOverride* over = behavior.FindOverride(property.hash);
                    entry.value = over != nullptr ? over->value : property.defaultValue;
                    entry.value.kind = property.type;
                }
                entries.PushBack(Move(entry));
            }
        }
        if (entries.IsEmpty())
        {
            return false;
        }
        MemoryStream stream;
        {
            BinarySerializer writer(stream, SerializeMode::Write);
            foundation::core::Serialize(writer, "entries", entries);
        }
        out.Clear();
        for (const byte b : stream.Bytes())
        {
            out.PushBack(b);
        }
        return true;
    }

    void BehaviorComponentManager::ReadReplicatedState(foundation::scene::EntityHandle entity, Span<const byte> state)
    {
        BehaviorComponent* component = Edit(entity);
        if (component == nullptr)
        {
            return;
        }
        MemoryStream stream;
        (void)stream.Write(state.Data(), state.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        Array<ReplicatedEntry> entries;
        {
            BinarySerializer reader(stream, SerializeMode::Read);
            foundation::core::Serialize(reader, "entries", entries);
        }
        for (const ReplicatedEntry& entry : entries)
        {
            if (entry.behavior >= component->behaviors.Size())
            {
                continue;
            }
            AttachedBehavior& behavior = component->behaviors[entry.behavior];
            const BehaviorClass* scriptClass = behavior.behaviorClass.Get();
            if (scriptClass == nullptr ||
                BehaviorPropertyNameHash(scriptClass->className.AsView()) != entry.classHash)
            {
                continue; // not the behaviour the host meant
            }
            for (const BehaviorProperty& property : scriptClass->properties)
            {
                if (property.hash != entry.property || !property.replicated)
                {
                    continue;
                }
                behavior.SetOverride(property.hash, entry.value); // for an instance made later
                if (behavior.instance.Get() != nullptr)
                {
                    (void)behavior.instance->SetProperty(property.name.AsView(),
                                                         ScriptPropertyValueToVariant(entry.value, property.type, nullptr));
                }
            }
        }
    }

    void RegisterBehaviorComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_BehaviorComponent();
            GlobalTypeRegistry().Register(TypeOf<BehaviorComponent>());
            RttiRegisterValue_SceneBehaviorSettings();
            GlobalTypeRegistry().Register(TypeOf<SceneBehaviorSettings>());
            return true;
        }();
        (void)once;
    }
}

namespace engine::behavior
{
    REFLECT_VALUE(SceneBehaviorsFacade, "rtti::engine::behavior")
    {
        builder.Attribute("scriptName", "SceneBehaviors");
        builder.Method<&SceneBehaviorsFacade::deltaTime>("deltaTime");
        builder.Method<&SceneBehaviorsFacade::elapsed>("elapsed");
        // send and emit: arity families with a Variant payload, as entity.send and scene.events.emit.
        builder.Method<static_cast<void (SceneBehaviorsFacade::*)(foundation::script::Entity, String) const>(&SceneBehaviorsFacade::send)>(
            "send", {"target", "message"});
        builder.Method<static_cast<void (SceneBehaviorsFacade::*)(foundation::script::Entity, String, Variant) const>(
            &SceneBehaviorsFacade::send)>("send", {"target", "message", "payload"});
        builder.Method<static_cast<void (SceneBehaviorsFacade::*)(String) const>(&SceneBehaviorsFacade::emit)>(
            "emit", {"name"});
        builder.Method<static_cast<void (SceneBehaviorsFacade::*)(String, Variant) const>(&SceneBehaviorsFacade::emit)>(
            "emit", {"name", "payload"});
        builder.Method<&SceneBehaviorsFacade::addBehavior>("addBehavior", {"entity", "behaviorClass"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    void RegisterBehaviorSceneFacade()
    {
        static const bool once = []()
        {
            RttiRegisterValue_SceneBehaviorsFacade();
            GlobalTypeRegistry().Register(TypeOf<SceneBehaviorsFacade>());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SceneBehaviorsFacade>());
            foundation::script::RegisterExtraFacadeName(u8"SceneBehaviors");
            foundation::script::ExtendScene<&SceneBehaviorsFacade::ForScene>("behaviors"); // scene.behaviors
            return true;
        }();
        (void)once;
    }

    const engine::DomainModule& BehaviorDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::script::kScriptResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"behavior",
            .installScene = &AddBehaviorSceneManagers,
            .registerReflection = &RegisterBehaviorComponentReflection,
            .registerScriptFacade = &RegisterBehaviorSceneFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
