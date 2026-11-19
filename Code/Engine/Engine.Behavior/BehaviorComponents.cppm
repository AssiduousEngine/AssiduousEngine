// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Behavior - :components partition.
//
// The attachment model: one BehaviorComponent per entity holding an ordered array of
// behaviors, each a cooked BehaviorClass reference, an enabled flag, and property overrides
// keyed by name hash (only the values that differ from the class's harvested defaults).
// Runtime fields (the live ScriptObject instance, dispatch state) are transient and
// never serialized. Because overrides live in the component payload, the prefab delta
// machinery (baseline blob compare) covers them with no extra code.

module;
#include "Core/Prelude.h"

export module engine.behavior:components;

import foundation.core;
import foundation.scene;
import foundation.resource;
import foundation.script;
import foundation.script.resource;

using namespace foundation::core;
using namespace foundation::script;

export namespace engine::behavior
{
    struct BehaviorPropertyOverride
    {
        u64 nameHash = 0; // BehaviorPropertyNameHash of the property's name
        BehaviorPropertyValue value;
    };

    inline void Serialize(ISerializer& ar, BehaviorPropertyOverride& o)
    {
        foundation::core::Serialize(ar, "nameHash", o.nameHash);
        foundation::core::Serialize(ar, "value", o.value);
    }

    /// The assets `overrides` name, appended to `outIds` once each: a nil one names nothing, and
    /// an entity reference is a guid of the scene's, not an asset. (A script class's asset
    /// property has no default naming an asset: only an override names one.)
    inline void CollectOverrideAssetIds(Span<const BehaviorPropertyOverride> overrides, Array<Guid>& outIds)
    {
        for (const BehaviorPropertyOverride& entry : overrides)
        {
            if (entry.value.kind != BehaviorPropertyType::Asset || entry.value.guid.IsNil())
            {
                continue;
            }
            bool listed = false;
            for (const Guid& id : outIds)
            {
                listed = listed || id == entry.value.guid;
            }
            if (!listed)
            {
                outIds.PushBack(entry.value.guid);
            }
        }
    }

    /// A behaviour's lifecycle handlers as its instance bound them, once, when it was made: an
    /// invalid one is a handler its class does not declare.
    struct BehaviorLifecycle
    {
        ScriptMethod enable;
        ScriptMethod start;
        ScriptMethod update;
        ScriptMethod disable;
        ScriptMethod destroy;
    };

    struct AttachedBehavior
    {
        // Authored:
        foundation::resource::Ref<BehaviorClass> behaviorClass;
        bool enabled = true;
        f32 updateInterval = 0.0f; // seconds between onUpdate calls; <=0 means every tick.
                                   // The delivered dt is the accumulated time.
        Array<BehaviorPropertyOverride> overrides;

        // Runtime (transient):
        RefPtr<ScriptObject> instance;
        BehaviorLifecycle lifecycle;        // bound with the instance, gone with it
        const BehaviorClass* boundClass = nullptr; // product the instance was built from
                                                 // (a reload swaps the product: re-instantiate)
        bool started = false;                    // onStart delivered
        bool active = false;          // last delivered enable state (onEnable/onDisable edges)
        bool entitySuspended = false; // entity-active latch: frozen by an inactive entity
                                      // (runtime only)
        bool faulted = false;         // a fault disables the one behavior (cleared by reload)
        f32 updateAccumulator = 0.0f; // time banked toward the next throttled onUpdate

        [[nodiscard]] const BehaviorPropertyOverride* FindOverride(u64 nameHash) const
        {
            for (const BehaviorPropertyOverride& entry : overrides)
            {
                if (entry.nameHash == nameHash)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
        void SetOverride(u64 nameHash, const BehaviorPropertyValue& value)
        {
            for (BehaviorPropertyOverride& entry : overrides)
            {
                if (entry.nameHash == nameHash)
                {
                    entry.value = value;
                    return;
                }
            }
            overrides.PushBack(BehaviorPropertyOverride{nameHash, value});
        }
        void RemoveOverride(u64 nameHash)
        {
            for (usize i = 0; i < overrides.Size(); ++i)
            {
                if (overrides[i].nameHash == nameHash)
                {
                    overrides.RemoveAt(i);
                    return;
                }
            }
        }
    };

    inline void Serialize(ISerializer& ar, AttachedBehavior& b)
    {
        foundation::core::Serialize(ar, "class", b.behaviorClass);
        foundation::core::Serialize(ar, "enabled", b.enabled);
        foundation::core::Serialize(ar, "updateInterval", b.updateInterval);
        foundation::core::Serialize(ar, "overrides", b.overrides); // count-prefixed array scope
    }

    struct BehaviorComponent
    {
        Array<AttachedBehavior> behaviors; // execution order = array order
    };

    inline void Serialize(ISerializer& ar, BehaviorComponent& c)
    {
        foundation::core::Serialize(ar, "behaviors", c.behaviors);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager, BehaviorComponent& c)
    {
        for (AttachedBehavior& behavior : c.behaviors)
        {
            behavior.behaviorClass.Bind(manager);
        }
    }

    class EntityBehaviorSystem; // forward (:subsystem); the destroy hook dispatches onDestroy

    class BehaviorComponentManager final
        : public foundation::scene::SerializableComponentManager<BehaviorComponent>,
          public foundation::scene::IReplicatedStateSource
    {
    public:
        BehaviorComponentManager() : SerializableComponentManager<BehaviorComponent>(u8"behavior") {}

        /// A networked entity's behaviours keep their replicated properties (BehaviorProperty::
        /// replicated) the same on every peer: the host writes their live values, a client writes
        /// them back in (and keeps them as overrides, for an instance made later). Bodies in
        /// BehaviorSubsystemImpl.cpp.
        [[nodiscard]] foundation::scene::IReplicatedStateSource* AsReplicatedStateSource() noexcept override
        {
            return this;
        }

        /// Every behaviour's asset-valued overrides: a script property holds an id the script
        /// binds itself, so the post-load pass leaves it unbound.
        void CollectUnboundReferences(Array<Guid>& outIds) override
        {
            ForEach([&outIds](BehaviorComponent& component, foundation::scene::EntityHandle)
                    {
                        for (const AttachedBehavior& behavior : component.behaviors)
                        {
                            CollectOverrideAssetIds(behavior.overrides.AsSpan(), outIds);
                        }
                    });
        }
        [[nodiscard]] bool WriteReplicatedState(foundation::scene::EntityHandle entity, Array<byte>& out) override;
        void ReadReplicatedState(foundation::scene::EntityHandle entity, Span<const byte> state) override;

        /// The per-scene script system, wired by the subsystem right after AddSystem;
        /// entity and component destruction route onDestroy through it.
        void SetScriptSystem(EntityBehaviorSystem* system) noexcept { m_scriptSystem = system; }
        [[nodiscard]] EntityBehaviorSystem* ScriptSystem() const noexcept { return m_scriptSystem; }

    protected:
        // Defined in the :subsystem partition's implementation (needs EntityBehaviorSystem).
        void OnComponentDestroyed(BehaviorComponent& component,
                                  foundation::scene::EntityHandle entity) override;

    private:
        EntityBehaviorSystem* m_scriptSystem = nullptr;
    };

    // Defined in BehaviorSubsystemImpl.cpp (REFLECT_* bodies never sit in a module
    // interface unit: GCC emits a gcm cluster for them).
    void RegisterBehaviorComponentReflection();
}
