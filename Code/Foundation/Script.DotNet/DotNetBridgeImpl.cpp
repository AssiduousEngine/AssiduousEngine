// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the bridge's functions (see DotNetBridge.cppm).

module;
#include "Core/Prelude.h"
#include <cstring>

module foundation.script.dotnet;

import foundation.core;

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

        i32 Fail(ErrorCode code, StringView why)
        {
            t_error = String(why);
            return -static_cast<i32>(code);
        }

        // The instance a call is made on: a value's bytes in place, or an object.
        Instance SelfOf(const DotNetValue* self)
        {
            if (self == nullptr || self->kind == DotNetValueKind::Empty)
                return Instance{};
            if (self->kind == DotNetValueKind::Handle)
                return self->data != nullptr ? ToInstance(*static_cast<Variant*>(self->data)) : Instance{};
            return Instance(self->data, self->type);
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

        const MethodInfo* MethodAt(const TypeInfo* type, u32 index)
        {
            return (type != nullptr && index < type->methodCount) ? &type->methods[index] : nullptr;
        }

        const PropertyInfo* PropertyAt(const TypeInfo* type, u32 index)
        {
            return (type != nullptr && index < type->propertyCount) ? &type->properties[index] : nullptr;
        }

        i32 Invoke(const MethodInfo* method, const DotNetValue* self, const DotNetValue* args, u32 count,
                   DotNetValue* result)
        {
            t_error.Clear();
            if (method == nullptr)
                return Fail(ErrorCode::InvalidArgument, u8"no method");
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
            Result<Variant> returned = method->invoke(SelfOf(self), Span<Variant>{values.Data(), values.Size()});
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
            const Status written = FromVariant(GetProperty(*property, SelfOf(self)), *result);
            return written.IsOk() ? 0 : -static_cast<i32>(written.Code());
        }

        i32 SetPropertyOf(const PropertyInfo* property, const DotNetValue* self, const DotNetValue* value)
        {
            t_error.Clear();
            if (property == nullptr || value == nullptr)
                return Fail(ErrorCode::InvalidArgument, u8"no property");
            const Status set = SetProperty(*property, SelfOf(self), ToVariant(*value));
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

        u32 TypeSizeOf(const TypeInfo* type) { return type != nullptr ? type->size : 0u; }

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

    Variant ToVariant(const DotNetValue& value)
    {
        switch (value.kind)
        {
        case DotNetValueKind::Bytes:
            if (value.type == nullptr || value.data == nullptr)
                return Variant{};
            if (Variant scalar = ScalarVariant(*value.type, value.data); !scalar.IsEmpty())
                return scalar;
            return Variant::CopyOf(*value.type, value.data);
        case DotNetValueKind::Text:
            return Variant::From<String>(
                String(StringView(static_cast<const utf8char*>(value.data), static_cast<usize>(value.size))));
        case DotNetValueKind::Handle:
            return value.data != nullptr ? *static_cast<const Variant*>(value.data) : Variant{};
        case DotNetValueKind::Empty:
            break;
        }
        return Variant{};
    }

    Status FromVariant(const Variant& variant, DotNetValue& out)
    {
        if (variant.IsEmpty())
        {
            out.kind = DotNetValueKind::Empty;
            out.type = nullptr;
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

    const DotNetEngineFunctions& EngineFunctions(IAllocator& allocator) noexcept
    {
        if (s_allocator == nullptr)
            s_allocator = &allocator;
        static const DotNetEngineFunctions functions = []
        {
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
            f.lastError = &LastError;
            return f;
        }();
        return functions;
    }
}
