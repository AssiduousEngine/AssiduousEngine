// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the bridge's functions (see DotNetBridge.cppm).

module;
#include "Core/Prelude.h"
#include <cstring>
#include <new> // std::launder: a typed call's String result

module foundation.script.dotnet;

import foundation.core;
import foundation.scene;          // a component handle's scene and entity (ReadComponentRef)
import foundation.script.facades; // Entity: what a scene component crosses to C# as

using namespace foundation::core;

namespace foundation::script::dotnet
{
    namespace
    {
        // Per calling thread: the last failure's reason, and text a result handed back (both valid
        // until the thread's next call into the bridge).
        thread_local String t_error;
        thread_local String t_text;
        // What handles are made from: the host's allocator, given with the table.
        IAllocator* s_allocator = nullptr;
        // How many calls took the typed path (TypedCallCount).
        Atomic<u64> s_typedCalls{0};

        i32 Fail(ErrorCode code, StringView why)
        {
            t_error = String(why);
            return -static_cast<i32>(code);
        }

        // A scene component type (the facades mark it, ComponentMethods): C# holds one of its
        // handles as the entity's bytes, and the engine finds the component again by it.
        bool IsSceneComponent(const TypeInfo* type)
        {
            if (type == nullptr || !type->hasIdentity)
                return false;
            // A type's attributes are fixed once registered, and a run of calls asks about the same
            // component type: the last answer is kept rather than searching the attributes by name
            // on every crossing.
            thread_local const TypeInfo* lastType = nullptr;
            thread_local bool lastAnswer = false;
            if (type != lastType)
            {
                const Variant* marked = FindAttribute(*type, "sceneComponent");
                lastAnswer = marked != nullptr && marked->TryGet<bool>() != nullptr && *marked->TryGet<bool>();
                lastType = type;
            }
            return lastAnswer;
        }

        // The component handle the bytes of a scene component held as a value stand for: empty when
        // its entity or scene is gone.
        Variant ComponentFromBytes(const TypeInfo& type, const void* bytes)
        {
            foundation::script::Entity entity;
            std::memcpy(static_cast<void*>(&entity), bytes, sizeof(entity));
            foundation::scene::Scene* scene = entity.scene.Get();
            if (scene == nullptr || !scene->IsValid(entity.Handle()))
                return Variant{};
            return scene->MakeComponentRef(entity.Handle(), type);
        }

        // The instance a call is made on: a value's bytes in place, a scene component found again by
        // its entity, or an object.
        Instance SelfOf(const DotNetValue* self)
        {
            if (self == nullptr || self->kind == DotNetValueKind::Empty)
                return Instance{};
            if (self->kind == DotNetValueKind::Handle)
                return self->data != nullptr ? ToInstance(*static_cast<Variant*>(self->data)) : Instance{};
            if (IsSceneComponent(self->type) && self->data != nullptr)
            {
                // A resolving handle: the instance is the component's own storage, not the handle's.
                // None (the entity is gone, or lacks it) is no instance: an empty Variant's would be
                // its own inline storage.
                Variant component = ComponentFromBytes(*self->type, self->data);
                return component.IsEmpty() ? Instance{} : ToInstance(component);
            }
            return Instance(self->data, self->type);
        }

        // A member reached through nothing: no object, or a component its entity no longer has
        // (Core's calls take the instance as given, so the caller refuses).
        i32 Gone(const char* member)
        {
            const StringView name(reinterpret_cast<const utf8char*>(member));
            return Fail(ErrorCode::NotFound, Format(u8"{}: the object is gone", name).AsView());
        }

        i32 ComponentAlive(const TypeInfo* type, const void* data)
        {
            if (type == nullptr || data == nullptr)
                return 0;
            Variant component = ComponentFromBytes(*type, data);
            return !component.IsEmpty() && ToInstance(component).Pointer() != nullptr ? 1 : 0;
        }

        // Registered types, and the scalars, which are not registered (the bindings name them by
        // id as they name any type).
        const TypeInfo* TypeById(u64 id)
        {
            if (const TypeInfo* found = GlobalTypeRegistry().FindById(id))
                return found;
            const TypeInfo* const scalars[] = {&TypeOf<f32>(), &TypeOf<f64>(), &TypeOf<i8>(),  &TypeOf<u8>(),
                                               &TypeOf<i16>(), &TypeOf<u16>(), &TypeOf<i32>(), &TypeOf<u32>(),
                                               &TypeOf<i64>(), &TypeOf<u64>(), &TypeOf<bool>()};
            for (const TypeInfo* scalar : scalars)
                if (scalar->id == id)
                    return scalar;
            return nullptr;
        }

        template <typename T>
        bool ScalarFrom(const TypeInfo& type, const void* data, Variant& out)
        {
            if (type.id != TypeOf<T>().id)
                return false;
            T value{};
            std::memcpy(&value, data, sizeof(T));
            out = Variant::From<T>(value);
            return true;
        }

        // A scalar argument as a Variant: scalars record no value operations (only reflected types
        // do, through their TypeBuilder), so Variant::CopyOf cannot make them.
        Variant ScalarVariant(const TypeInfo& type, const void* data)
        {
            Variant out;
            (void)(ScalarFrom<f32>(type, data, out) || ScalarFrom<f64>(type, data, out) || ScalarFrom<i32>(type, data, out) ||
                   ScalarFrom<u32>(type, data, out) || ScalarFrom<i64>(type, data, out) || ScalarFrom<u64>(type, data, out) ||
                   ScalarFrom<i16>(type, data, out) || ScalarFrom<u16>(type, data, out) || ScalarFrom<i8>(type, data, out) ||
                   ScalarFrom<u8>(type, data, out) || ScalarFrom<bool>(type, data, out));
            return out;
        }

        // An enum value's underlying integer, read by its width.
        i64 EnumInt(const TypeInfo& type, const void* data)
        {
            switch (type.size)
            {
            case 1:
                return *static_cast<const i8*>(data);
            case 2:
            {
                i16 value = 0;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }
            case 4:
            {
                i32 value = 0;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }
            case 8:
            {
                i64 value = 0;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }
            default:
                return 0;
            }
        }

        const MethodInfo* MethodAt(const TypeInfo* type, u32 index)
        {
            return (type != nullptr && index < type->methodCount) ? &type->methods[index] : nullptr;
        }

        const PropertyInfo* PropertyAt(const TypeInfo* type, u32 index)
        {
            return (type != nullptr && index < type->propertyCount) ? &type->properties[index] : nullptr;
        }

        // The typed call (MethodInfo::call) made straight from what C# passed: when every argument
        // is in the form its parameter takes (a value's bytes of exactly its type, text for a string)
        // and the result is nothing, a plain value or a string. Anything else (an object, a Variant,
        // a value of another type to convert) is left to the Variant call: false, nothing done.
        bool TryInvokeTyped(const MethodInfo& method, const DotNetValue* self, const DotNetValue* args, u32 count,
                            DotNetValue* result)
        {
            constexpr u32 kMaxArgs = 8;
            constexpr usize kResultBytes = 64;
            if (method.call == nullptr || count > kMaxArgs)
                return false;
            const TypeInfo* returned = method.returnType != nullptr ? method.returnType() : nullptr;
            const bool returnsText = returned != nullptr && returned->id == TypeOf<String>().id;
            if (returned != nullptr && !returnsText &&
                (!returned->trivialCopy || returned->hasIdentity || returned->size > kResultBytes || result == nullptr ||
                 result->data == nullptr || result->size < returned->size))
                return false;
            void* object = nullptr;
            if (!method.isStatic)
            {
                const Instance instance = SelfOf(self);
                object = instance.Pointer();
                if (object == nullptr)
                    return false;
            }
            void* slots[kMaxArgs];
            InlineArray<String, kMaxArgs> strings; // made only for a String argument; never moved
            StringView views[kMaxArgs];
            for (u32 i = 0; i < count; ++i)
            {
                const ParamInfo& param = method.params[i];
                const DotNetValue& arg = args[i];
                if (param.form == ParamForm::View && arg.kind == DotNetValueKind::Text)
                {
                    views[i] = StringView(static_cast<const utf8char*>(arg.data), static_cast<usize>(arg.size));
                    slots[i] = &views[i];
                }
                else if (param.form == ParamForm::Value && arg.kind == DotNetValueKind::Text && param.type()->id == TypeOf<String>().id)
                {
                    strings.Add(String(StringView(static_cast<const utf8char*>(arg.data), static_cast<usize>(arg.size))));
                    slots[i] = &strings[strings.Size() - 1];
                }
                else if (param.form == ParamForm::Value && arg.kind == DotNetValueKind::Bytes && arg.type != nullptr &&
                         arg.type->id == param.type()->id && arg.data != nullptr) // by id: a type may have more than one record
                    slots[i] = arg.data;
                else
                    return false;
            }
            s_typedCalls.fetch_add(1, MemoryOrder::Relaxed);
            if (returned == nullptr)
            {
                method.call(object, slots, nullptr);
                if (result != nullptr)
                    *result = DotNetValue{};
                return true;
            }
            if (returnsText)
            {
                alignas(String) unsigned char storage[sizeof(String)];
                method.call(object, slots, storage);
                String* text = std::launder(reinterpret_cast<String*>(storage));
                t_text = Move(*text);
                text->~String();
                if (result != nullptr)
                {
                    result->kind = DotNetValueKind::Text;
                    result->type = nullptr;
                    result->data = const_cast<utf8char*>(t_text.CStr());
                    result->size = t_text.Size();
                }
                return true;
            }
            alignas(16) unsigned char storage[kResultBytes];
            method.call(object, slots, storage);
            std::memcpy(result->data, storage, returned->size);
            result->kind = DotNetValueKind::Bytes;
            result->type = returned;
            result->size = returned->size;
            return true;
        }

        i32 Invoke(const MethodInfo* method, const DotNetValue* self, const DotNetValue* args, u32 count,
                   DotNetValue* result)
        {
            t_error.Clear();
            if (method == nullptr)
                return Fail(ErrorCode::InvalidArgument, u8"no method");
            if (count == method->paramCount && TryInvokeTyped(*method, self, args, count, result))
                return 0;
            if (count != method->paramCount)
                return Fail(ErrorCode::InvalidArgument,
                            Format(u8"{} takes {} arguments, not {}", StringView(reinterpret_cast<const utf8char*>(method->name)),
                                   method->paramCount, count).AsView());
            Array<Variant> values;
            values.Reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                values.PushBack(ToVariant(args[i]));
                if (values.Back().IsEmpty() && args[i].kind != DotNetValueKind::Empty)
                    return Fail(ErrorCode::InvalidArgument, Format(u8"argument {} has no value form", i).AsView());
            }
            const Instance instance = SelfOf(self);
            if (!method->isStatic && instance.Pointer() == nullptr)
                return Gone(method->name);
            Result<Variant> returned = method->invoke(instance, Span<Variant>{values.Data(), values.Size()});
            if (!returned.HasValue())
                return Fail(returned.Error(), Format(u8"{} failed", StringView(reinterpret_cast<const utf8char*>(method->name))).AsView());
            if (result == nullptr)
                return 0;
            const Status written = FromVariant(returned.Value(), *result);
            return written.IsOk() ? 0 : -static_cast<i32>(written.Code());
        }

        i32 GetPropertyOf(const PropertyInfo* property, const DotNetValue* self, DotNetValue* result)
        {
            t_error.Clear();
            if (property == nullptr || result == nullptr)
                return Fail(ErrorCode::InvalidArgument, u8"no property");
            const Instance instance = SelfOf(self);
            if (instance.Pointer() == nullptr)
                return Gone(property->name);
            const Status written = FromVariant(GetProperty(*property, instance), *result);
            return written.IsOk() ? 0 : -static_cast<i32>(written.Code());
        }

        i32 SetPropertyOf(const PropertyInfo* property, const DotNetValue* self, const DotNetValue* value)
        {
            t_error.Clear();
            if (property == nullptr || value == nullptr)
                return Fail(ErrorCode::InvalidArgument, u8"no property");
            const Instance instance = SelfOf(self);
            if (instance.Pointer() == nullptr)
                return Gone(property->name);
            const Status set = SetProperty(*property, instance, ToVariant(*value));
            if (!set.IsOk())
                return Fail(set.Code(), Format(u8"{} was not set", StringView(reinterpret_cast<const utf8char*>(property->name))).AsView());
            return 0;
        }

        i32 ConstructOf(const TypeInfo* type, const DotNetValue* args, u32 count, DotNetValue* result)
        {
            t_error.Clear();
            if (type == nullptr || result == nullptr)
                return Fail(ErrorCode::InvalidArgument, u8"no type");
            Array<Variant> values;
            for (u32 i = 0; i < count; ++i)
                values.PushBack(ToVariant(args[i]));
            Result<Variant> made = Construct(*type, Span<Variant>{values.Data(), values.Size()});
            if (!made.HasValue())
                return Fail(made.Error(), Format(u8"{} has no constructor of {} arguments",
                                                 StringView(reinterpret_cast<const utf8char*>(type->name)), count).AsView());
            const Status written = FromVariant(made.Value(), *result);
            return written.IsOk() ? 0 : -static_cast<i32>(written.Code());
        }

        // The container a list handle holds, and the instance to read it through; false when the
        // value is no handle to a container.
        bool ListOf(const DotNetValue* list, const ContainerInfo*& container, Instance& instance)
        {
            if (list == nullptr || list->kind != DotNetValueKind::Handle || list->data == nullptr)
                return false;
            Variant& held = *static_cast<Variant*>(list->data);
            const TypeInfo* type = held.Type();
            if (type == nullptr || !IsContainer(*type))
                return false;
            container = type->container;
            instance = ToInstance(held);
            return instance.Pointer() != nullptr;
        }

        i32 ListSize(const DotNetValue* list, u64* count)
        {
            t_error.Clear();
            const ContainerInfo* container = nullptr;
            Instance instance;
            if (count == nullptr || !ListOf(list, container, instance))
                return Fail(ErrorCode::InvalidArgument, u8"not a list");
            *count = container->size(instance);
            return 0;
        }

        i32 ListAt(const DotNetValue* list, u64 index, DotNetValue* out)
        {
            t_error.Clear();
            const ContainerInfo* container = nullptr;
            Instance instance;
            if (out == nullptr || !ListOf(list, container, instance))
                return Fail(ErrorCode::InvalidArgument, u8"not a list");
            const usize count = container->size(instance);
            if (index >= count)
                return Fail(ErrorCode::OutOfRange, Format(u8"element {} of a list of {}", index, count).AsView());
            const Status written = FromVariant(container->getAt(instance, static_cast<usize>(index)), *out);
            return written.IsOk() ? 0 : -static_cast<i32>(written.Code());
        }

        i32 MakeList(const DotNetValue* items, u32 count, DotNetValue* out)
        {
            t_error.Clear();
            if (out == nullptr || (items == nullptr && count > 0))
                return Fail(ErrorCode::InvalidArgument, u8"no list");
            Array<Variant> list;
            list.Reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                list.PushBack(ToVariant(items[i]));
                if (list.Back().IsEmpty() && items[i].kind != DotNetValueKind::Empty)
                    return Fail(ErrorCode::InvalidArgument, Format(u8"element {} has no value form", i).AsView());
            }
            const Variant made = Variant::From<Array<Variant>>(Move(list));
            out->kind = DotNetValueKind::Handle;
            out->type = made.Type();
            out->data = s_allocator->New<Variant>(made);
            out->size = 0;
            return 0;
        }

        u32 TypeSizeOf(const TypeInfo* type) { return type != nullptr ? type->size : 0u; }

        u64 TypeIdOf(const TypeInfo* type) { return type != nullptr ? type->id : 0u; }

        void ReleaseHandle(void* handle)
        {
            if (handle != nullptr)
                s_allocator->Delete(static_cast<Variant*>(handle));
        }

        const utf8char* LastError(u64* length)
        {
            if (length != nullptr)
                *length = t_error.Size();
            return t_error.CStr();
        }
    }

    namespace
    {
        CallbackMaker s_callbackMaker = nullptr; // SetCallbackMaker
    }

    Variant ToVariant(const DotNetValue& value)
    {
        switch (value.kind)
        {
        case DotNetValueKind::Bytes:
            if (value.type == nullptr || value.data == nullptr)
                return Variant{};
            if (IsSceneComponent(value.type))
                return ComponentFromBytes(*value.type, value.data);
            if (Variant scalar = ScalarVariant(*value.type, value.data); !scalar.IsEmpty())
                return scalar;
            // An enum crosses as its underlying integer, as from the other script languages: the
            // call converts it back, and an enum records no value operations to copy it by.
            if (value.type->enumeratorCount > 0)
                return Variant::From<i64>(EnumInt(*value.type, value.data));
            return Variant::CopyOf(*value.type, value.data);
        case DotNetValueKind::Text:
            return Variant::From<String>(
                String(StringView(static_cast<const utf8char*>(value.data), static_cast<usize>(value.size))));
        case DotNetValueKind::Handle:
            return value.data != nullptr ? *static_cast<const Variant*>(value.data) : Variant{};
        case DotNetValueKind::Callback:
            return value.data != nullptr && s_callbackMaker != nullptr ? s_callbackMaker(value.data) : Variant{};
        case DotNetValueKind::Empty:
            break;
        }
        return Variant{};
    }

    void SetCallbackMaker(CallbackMaker maker) noexcept { s_callbackMaker = maker; }

    Status FromVariant(const Variant& variant, DotNetValue& out)
    {
        if (variant.IsEmpty())
        {
            out.kind = DotNetValueKind::Empty;
            out.type = nullptr;
            return Status{};
        }
        // A scene component's handle crosses as its entity under the component's type: C# holds it
        // as a value, with no handle to allocate and none to give back.
        foundation::scene::Scene* scene = nullptr;
        foundation::scene::EntityHandle handle;
        if (foundation::scene::ReadComponentRef(variant, scene, handle) && IsSceneComponent(variant.Type()))
        {
            if (out.data == nullptr || out.size < sizeof(foundation::script::Entity))
                return Status{ErrorCode::OutOfRange};
            foundation::script::Entity entity;
            entity.scene = scene;
            entity.entityIndex = handle.index;
            entity.entityGeneration = handle.generation;
            std::memcpy(out.data, static_cast<const void*>(&entity), sizeof(entity));
            out.kind = DotNetValueKind::Bytes;
            out.type = variant.Type();
            out.size = sizeof(entity);
            return Status{};
        }
        if (const String* text = variant.TryGet<String>())
        {
            t_text = *text;
            out.kind = DotNetValueKind::Text;
            out.type = nullptr;
            out.data = const_cast<utf8char*>(t_text.CStr());
            out.size = t_text.Size();
            return Status{};
        }
        const TypeInfo* type = variant.Type();
        // A plain value (copied by its bytes, no identity, owned outright) crosses as bytes;
        // everything else (an object, a component handle, a borrow) as a handle.
        const bool plain = type != nullptr && type->trivialCopy && !type->hasIdentity && !variant.IsObject() && !variant.IsMember();
        if (!plain)
        {
            out.kind = DotNetValueKind::Handle;
            out.type = type;
            out.data = s_allocator->New<Variant>(variant);
            out.size = 0;
            return Status{};
        }
        if (out.data == nullptr || out.size < type->size)
        {
            t_error = Format(u8"{} needs {} bytes, {} given", StringView(reinterpret_cast<const utf8char*>(type->name)),
                             type->size, out.size);
            return Status{ErrorCode::OutOfRange};
        }
        std::memcpy(out.data, variant.ValuePointer(), type->size);
        out.kind = DotNetValueKind::Bytes;
        out.type = type;
        out.size = type->size;
        return Status{};
    }

    DotNetValue LendValue(const Variant& variant)
    {
        DotNetValue out{};
        if (variant.IsEmpty())
            return out;
        if (const String* text = variant.TryGet<String>())
        {
            out.kind = DotNetValueKind::Text;
            out.data = const_cast<utf8char*>(text->Data());
            out.size = text->Size();
            return out;
        }
        const TypeInfo* type = variant.Type();
        // A scene component lends its entity's bytes, from storage the thread keeps for the call's
        // arguments (a handler takes a few; the ring is as long as the most one call lends).
        foundation::scene::Scene* scene = nullptr;
        foundation::scene::EntityHandle handle;
        if (IsSceneComponent(type) && foundation::scene::ReadComponentRef(variant, scene, handle))
        {
            thread_local foundation::script::Entity lent[16];
            thread_local u32 next = 0;
            out.size = sizeof(foundation::script::Entity);
            out.data = &lent[next];
            next = (next + 1) % 16;
            (void)FromVariant(variant, out); // a component: its entity's bytes, nothing allocated
            return out;
        }
        const bool plain = type != nullptr && type->trivialCopy && !type->hasIdentity && !variant.IsObject() && !variant.IsMember();
        out.type = type;
        if (plain)
        {
            out.kind = DotNetValueKind::Bytes;
            out.data = const_cast<void*>(variant.ValuePointer());
            out.size = type->size;
        }
        else
        {
            out.kind = DotNetValueKind::Handle;
            out.data = s_allocator->New<Variant>(variant);
        }
        return out;
    }

    u64 TypedCallCount() noexcept { return s_typedCalls.load(MemoryOrder::Relaxed); }

    const DotNetEngineFunctions& EngineFunctions(IAllocator& allocator) noexcept
    {
        if (s_allocator == nullptr)
            s_allocator = &allocator;
        static const DotNetEngineFunctions functions = []
        {
            RegisterArrayType<Variant>(); // what a list argument is made as (makeList), read as a list
            DotNetEngineFunctions f;
            f.typeById = &TypeById;
            f.methodAt = &MethodAt;
            f.propertyAt = &PropertyAt;
            f.invoke = &Invoke;
            f.getProperty = &GetPropertyOf;
            f.setProperty = &SetPropertyOf;
            f.construct = &ConstructOf;
            f.typeSize = &TypeSizeOf;
            f.releaseHandle = &ReleaseHandle;
            f.typeIdOf = &TypeIdOf;
            f.lastError = &LastError;
            f.componentAlive = &ComponentAlive;
            f.listSize = &ListSize;
            f.listAt = &ListAt;
            f.makeList = &MakeList;
            return f;
        }();
        return functions;
    }
}
