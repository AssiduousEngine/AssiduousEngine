// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Behavior - :native partition. Behaviors written in C++: a class derives from one of the
// three kinds' bases (RunBehavior, SceneBehavior, EntityBehavior), reflects itself, and registers
// its type when its plugin loads. A BehaviorClass of language `native` names it by its qualified
// reflected name ("rtti::game::Orb"); the run host makes it through NativeBehaviorClasses, and the
// layer calls it through the same BehaviorObject a script's goes through, by the names scripts
// use: its handlers are its reflected methods named `on<Name>`, its properties the reflected fields
// it marks `behaviorProperty`. No VM, no context: each call runs under its run's services.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.behavior:native;

import foundation.core;
import foundation.script;
import foundation.script.resource;
import foundation.script.facades;

using namespace foundation::core;
using namespace foundation::script;

export namespace engine::behavior
{
    /// What the three kinds' bases share.
    class NativeBehavior : public Object
    {
        RTTI_OBJECT(NativeBehavior, Object)
    public:
        /// Disables this behavior, as a script error does: its handlers stop, `reason` is logged and
        /// is the fault's reason (a run behavior's, RunBehaviorFault). For a class that finds its own
        /// state broken; it takes effect as the running handler returns.
        void Fault(StringView reason)
        {
            if (m_faultReason.IsEmpty())
            {
                m_faultReason = reason.IsEmpty() ? String(u8"faulted") : String(reason);
            }
        }
        /// Why it faulted; empty while it has not.
        [[nodiscard]] StringView FaultReason() const noexcept { return m_faultReason.AsView(); }

    private:
        String m_faultReason;
    };

    /// A behavior that lives for the run, across scene loads: the game's own object. Made with
    /// nothing, as a script's is.
    class RunBehavior : public NativeBehavior
    {
        RTTI_OBJECT(RunBehavior, NativeBehavior)
    };

    /// A behavior of one scene, made when the scene starts.
    class SceneBehavior : public NativeBehavior
    {
        RTTI_OBJECT(SceneBehavior, NativeBehavior)
    public:
        /// The scene it belongs to, as a script's constructor receives it: a handle, by value.
        [[nodiscard]] foundation::script::Scene GetScene() const noexcept { return m_scene; }
        /// The layer gives it its scene as it makes it.
        void BindScene(const foundation::script::Scene& scene) noexcept { m_scene = scene; }

    private:
        foundation::script::Scene m_scene;
    };

    /// A behavior attached to an entity, one per slot.
    class EntityBehavior : public NativeBehavior
    {
        RTTI_OBJECT(EntityBehavior, NativeBehavior)
    public:
        /// The entity it is attached to, as a script's constructor receives it: a handle, by value,
        /// so `Self().setPosition(...)` reads as a script's `self.setPosition(...)`.
        [[nodiscard]] foundation::script::Entity Self() const noexcept { return m_self; }
        /// The layer gives it its entity as it makes it.
        void BindEntity(const foundation::script::Entity& self) noexcept { m_self = self; }

    private:
        foundation::script::Entity m_self;
    };

    /// The base type of `kind`'s native classes; null for None.
    [[nodiscard]] const TypeInfo* NativeBaseOf(BehaviorKind kind) noexcept;

    class NativeBehaviorClasses;

    /// A native class's object behind the layer's BehaviorObject. Every call runs under its run's
    /// services (ServiceScope), so the facades it calls find its run as a script's do, and under a
    /// crash note naming it, its handler and its entity, so a crash in it says where it was;
    /// numbers are fitted to the declared types as a script's are (ScriptNumbers). A call after
    /// which the object has faulted (NativeBehavior::Fault) is an error, which disables it. Its
    /// classes (its run host's) outlive it.
    class NativeBehaviorObject final : public BehaviorObject
    {
    public:
        NativeBehaviorObject(RefPtr<NativeBehavior> object, NativeBehaviorClasses& classes, String where) noexcept
            : m_object(Move(object)), m_classes(&classes), m_where(Move(where))
        {
        }

        [[nodiscard]] Result<Variant> Invoke(StringView method, Span<Variant> args) override;
        [[nodiscard]] BehaviorMethod Bind(StringView method, usize argCount) override;
        [[nodiscard]] Result<Variant> Invoke(BehaviorMethod method, Span<Variant> args) override;
        [[nodiscard]] Result<Variant> GetProperty(StringView name) override;
        [[nodiscard]] Status SetProperty(StringView name, const Variant& value) override;

        /// The C++ object (for a test, or a host that must reach it).
        [[nodiscard]] NativeBehavior* Get() const noexcept { return m_object.Get(); }

    private:
        [[nodiscard]] Result<Variant> Call(const MethodInfo& method, Span<Variant> args);

        RefPtr<NativeBehavior> m_object;
        NativeBehaviorClasses* m_classes;
        String m_where; // " on 'orb'" for an entity's: what a crash note and a fault name after the call
    };

    /// Makes the objects of a run's native classes. A class is found in the type registry by the
    /// qualified name its BehaviorClass records, checked to derive from its kind's base, and made
    /// through its reflected creator (TypeBuilder::Creator()); an entity behavior is then given its
    /// entity and a scene behavior its scene. The run host holds one.
    class NativeBehaviorClasses
    {
    public:
        /// What the objects' calls find their services through (the run host's). Set once.
        void SetServices(ServiceResolver services) { m_services = Move(services); }

        /// An object of `nativeClass`, given `args` as a script's constructor would be (an entity
        /// behavior its Entity, a scene behavior its Scene). Null, logged, when the class is not
        /// registered, is not of `kind`, or cannot be made.
        [[nodiscard]] RefPtr<BehaviorObject> Create(const BehaviorClass& nativeClass, Span<Variant> args, BehaviorKind kind,
                                                    IAllocator& allocator);

        /// The services its objects' calls find (their run's).
        [[nodiscard]] const ServiceResolver& Services() const noexcept { return m_services; }
        /// The reason the last native object to fault gave, then forgotten.
        [[nodiscard]] String TakeLastError() { return Move(m_lastError); }
        void SetLastError(StringView reason) { m_lastError = String(reason); }

    private:
        ServiceResolver m_services;
        String m_lastError;
    };
}
