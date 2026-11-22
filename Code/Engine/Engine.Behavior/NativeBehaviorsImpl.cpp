// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Behavior - the :native partition's implementation: the kinds' bases' reflection (hidden
// from the script languages), a native object's calls, and the making of a native class.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module engine.behavior;

import foundation.core;
import foundation.script;
import foundation.script.resource;
import foundation.script.facades;

using namespace foundation::core;
using namespace foundation::script;

namespace engine::behavior
{
    // The bases are the engine's, never a script's type: a script derives from its language's own
    // RunBehavior, and a native class is reached by its messages and events, not by name.
    REFLECT_MEMBERS(NativeBehavior, "rtti::behavior")
    {
        builder.Attribute(kHiddenFromScriptsAttribute, true); // and so every type deriving from it
    }
    RTTI_DEFINE_OBJECT(RunBehavior, "rtti::behavior")
    RTTI_DEFINE_OBJECT(SceneBehavior, "rtti::behavior")
    RTTI_DEFINE_OBJECT(EntityBehavior, "rtti::behavior")

    const TypeInfo* NativeBaseOf(BehaviorKind kind) noexcept
    {
        switch (kind)
        {
        case BehaviorKind::Run:
            return &RunBehavior::StaticType();
        case BehaviorKind::Scene:
            return &SceneBehavior::StaticType();
        case BehaviorKind::Entity:
            return &EntityBehavior::StaticType();
        case BehaviorKind::None:
        default:
            return nullptr;
        }
    }

    namespace
    {
        [[nodiscard]] const char* AsChars(const String& text) noexcept { return reinterpret_cast<const char*>(text.CStr()); }

        // `value` as the type `expected` declares where it is a number of another width (a script's
        // and the layer's numbers are doubles; a C++ field or parameter is often an f32 or an i32), as
        // a script backend fits a number; anything else as it is.
        [[nodiscard]] Variant Fit(const Variant& value, const TypeInfo* expected)
        {
            if (expected == nullptr || value.Type() == nullptr || value.Type()->id == expected->id)
            {
                return value;
            }
            bool ok = false;
            const double number = ScriptNumbers::Of(value, ok);
            if (!ok)
            {
                return value;
            }
            Variant fitted = ScriptNumbers::As(number, expected);
            return fitted.IsEmpty() ? value : fitted;
        }

        // The method `name` taking `argCount` arguments, on the type or a base; null when none.
        [[nodiscard]] const MethodInfo* FindMethodOf(const TypeInfo& type, StringView name, usize argCount)
        {
            const String wanted(name);
            for (const TypeInfo* t = &type; t != nullptr; t = t->base)
            {
                for (u32 i = 0; i < t->methodCount; ++i)
                {
                    const MethodInfo& method = t->methods[i];
                    if (!method.isStatic && method.paramCount == argCount && StringView(reinterpret_cast<const utf8char*>(method.name)) == wanted.AsView())
                    {
                        return &method;
                    }
                }
            }
            return nullptr;
        }

        // The property is one the class marks as its own (kBehaviorPropertyAttribute).
        [[nodiscard]] bool IsBehaviorProperty(const PropertyInfo& property) noexcept
        {
            const Attribute* marked = FindAttribute(property, StringView(reinterpret_cast<const utf8char*>(kBehaviorPropertyAttribute)));
            return marked != nullptr && marked->value.TryGet<bool>() != nullptr && *marked->value.TryGet<bool>();
        }
    }

    Result<Variant> NativeBehaviorObject::Call(const MethodInfo& method, Span<Variant> args)
    {
        Array<Variant> fitted;
        fitted.Reserve(args.Size());
        for (usize i = 0; i < args.Size(); ++i)
        {
            fitted.PushBack(Fit(args[i], method.params[i].type()));
        }
        const StringView className(reinterpret_cast<const utf8char*>(m_object->GetType()->name));
        const StringView handler(reinterpret_cast<const utf8char*>(method.name));
        CrashNoteScope note(className, u8".", handler, m_where.AsView()); // "Orb.onUpdate on 'orb'
        ServiceScope scope(&m_classes->Services());
        const Instance instance(m_object.Get(), m_object->GetType());
        Result<Variant> result = InvokeMethod(method, instance, Span<Variant>{fitted.Data(), fitted.Size()});
        if (!m_object->FaultReason().IsEmpty())
        {
            LOG_ERROR(u8"Script", u8"{}.{}{} faulted: {}", className, handler, m_where, m_object->FaultReason());
            m_classes->SetLastError(m_object->FaultReason());
            return Err(ErrorCode::Internal);
        }
        return result;
    }

    Result<Variant> NativeBehaviorObject::Invoke(StringView method, Span<Variant> args)
    {
        const MethodInfo* found = FindMethodOf(*m_object->GetType(), method, args.Size());
        if (found == nullptr)
        {
            return Err(ErrorCode::NotFound);
        }
        return Call(*found, args);
    }

    BehaviorMethod NativeBehaviorObject::Bind(StringView method, usize argCount)
    {
        // The handle is the method's MethodInfo: a registered type's methods are fixed while it is.
        return BehaviorMethod{reinterpret_cast<u64>(FindMethodOf(*m_object->GetType(), method, argCount))};
    }

    Result<Variant> NativeBehaviorObject::Invoke(BehaviorMethod method, Span<Variant> args)
    {
        const auto* found = reinterpret_cast<const MethodInfo*>(method.id);
        if (found == nullptr || found->paramCount != args.Size())
        {
            return Err(ErrorCode::NotFound);
        }
        return Call(*found, args);
    }

    Result<Variant> NativeBehaviorObject::GetProperty(StringView name)
    {
        // Any reflected field reads (a playtest's probe); only a marked one is written.
        const String wanted(name);
        const PropertyInfo* property = FindProperty(*m_object->GetType(), AsChars(wanted));
        if (property == nullptr || IsNested(*property))
        {
            return Err(ErrorCode::NotFound);
        }
        return foundation::core::GetProperty(*property, Instance(m_object.Get(), m_object->GetType()));
    }

    Status NativeBehaviorObject::SetProperty(StringView name, const Variant& value)
    {
        const String wanted(name);
        const PropertyInfo* property = FindProperty(*m_object->GetType(), AsChars(wanted));
        if (property == nullptr || !IsBehaviorProperty(*property))
        {
            return Status{ErrorCode::NotFound};
        }
        return foundation::core::SetProperty(*property, Instance(m_object.Get(), m_object->GetType()),
                                             Fit(value, property->type));
    }

    const TypeInfo* NativeBehaviorClasses::FindClass(StringView qualifiedName)
    {
        // "rtti::game::Orb": the namespace before the last "::", the name after it.
        for (usize i = qualifiedName.Size(); i-- > 1;)
        {
            if (qualifiedName[i] == u8':' && qualifiedName[i - 1] == u8':')
            {
                const String namespaceName(qualifiedName.SubStr(0, i - 1));
                const String name(qualifiedName.SubStr(i + 1, qualifiedName.Size() - (i + 1)));
                return GlobalTypeRegistry().FindByName(AsChars(namespaceName), AsChars(name));
            }
        }
        return nullptr;
    }

    RefPtr<BehaviorObject> NativeBehaviorClasses::Create(const BehaviorClass& nativeClass, Span<Variant> args,
                                                         BehaviorKind kind, IAllocator& allocator)
    {
        const TypeInfo* type = FindClass(nativeClass.className.AsView());
        if (type == nullptr)
        {
            LOG_ERROR(u8"Script", u8"native class '{}' is not registered (is its plugin enabled?)", nativeClass.className);
            return nullptr;
        }
        const TypeInfo* base = NativeBaseOf(kind);
        if (base == nullptr || !IsDerivedFrom(type, base))
        {
            LOG_ERROR(u8"Script", u8"native class '{}' does not derive from {}", nativeClass.className,
                      BehaviorKinds::TypeName(kind));
            return nullptr;
        }
        RefPtr<Object> made = CreateObject(*type, allocator);
        if (made.Get() == nullptr)
        {
            LOG_ERROR(u8"Script", u8"native class '{}' has no creator (builder.Creator())", nativeClass.className);
            return nullptr;
        }
        RefPtr<NativeBehavior> object(static_cast<NativeBehavior*>(made.Get()));
        String where;
        if (!args.IsEmpty())
        {
            if (const auto* entity = args[0].TryGet<foundation::script::Entity>(); entity != nullptr && kind == BehaviorKind::Entity)
            {
                static_cast<EntityBehavior*>(object.Get())->BindEntity(*entity);
                where = Format(u8" on '{}'", entity->name());
            }
            if (const auto* scene = args[0].TryGet<foundation::script::Scene>(); scene != nullptr && kind == BehaviorKind::Scene)
            {
                static_cast<SceneBehavior*>(object.Get())->BindScene(*scene);
            }
        }
        return RefPtr<BehaviorObject>(MakeRef<NativeBehaviorObject>(allocator, Move(object), *this, Move(where)));
    }
}
