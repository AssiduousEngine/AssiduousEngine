// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the C# bindings generator (see DotNetBindings.cppm).

module;
#include "Core/Prelude.h"

module foundation.script.dotnet;

import foundation.core;

using namespace foundation::core;

namespace foundation::script::dotnet
{
    namespace
    {
        StringView NameOf(const char* text) { return StringView(reinterpret_cast<const utf8char*>(text)); }

        // A type id as the hex digits of a C# ulong literal.
        String HexOf(u64 value)
        {
            constexpr utf8char kDigits[] = u8"0123456789abcdef";
            String text;
            for (i32 shift = 60; shift >= 0; shift -= 4)
                text.PushBack(kDigits[(value >> shift) & 0xFu]);
            return text;
        }

        // The C# spelling of a scalar type; empty when `type` is not one.
        StringView ScalarName(const TypeInfo& type)
        {
            struct Scalar
            {
                const TypeInfo* type;
                StringView name;
            };
            const Scalar scalars[] = {
                {&TypeOf<f32>(), u8"float"}, {&TypeOf<f64>(), u8"double"}, {&TypeOf<i8>(), u8"sbyte"},
                {&TypeOf<u8>(), u8"byte"},   {&TypeOf<i16>(), u8"short"},  {&TypeOf<u16>(), u8"ushort"},
                {&TypeOf<i32>(), u8"int"},   {&TypeOf<u32>(), u8"uint"},   {&TypeOf<i64>(), u8"long"},
                {&TypeOf<u64>(), u8"ulong"}, {&TypeOf<bool>(), u8"bool"},
            };
            for (const Scalar& scalar : scalars)
                if (scalar.type->id == type.id)
                    return scalar.name;
            return StringView{};
        }

        bool IsValueCandidate(const TypeInfo& type)
        {
            return type.trivialCopy && type.valueOps != nullptr && !type.hasIdentity && type.reference == nullptr &&
                   type.container == nullptr && type.enumeratorCount == 0 && type.base == nullptr &&
                   type.propertyCount > 0 && ScalarName(type).IsEmpty();
        }

        // Where each field of `type` sits: measured on zeroed storage through the reflected field
        // addresses (a trivially copied value needs no construction to be addressed).
        bool MeasureFields(const TypeInfo& type, Array<u32>& offsets)
        {
            offsets.Clear();
            Array<u8> storage;
            storage.Resize(type.size + type.align);
            u8* base = storage.Data();
            const usize misalign = reinterpret_cast<usize>(base) % (type.align > 0 ? type.align : 1);
            if (misalign != 0)
                base += type.align - misalign;
            const Instance instance(base, &type);
            for (u32 i = 0; i < type.propertyCount; ++i)
            {
                const PropertyInfo& property = type.properties[i];
                if (property.address == nullptr || property.type == nullptr)
                    return false;
                const u8* at = static_cast<const u8*>(property.address(instance));
                if (at < base || at + property.type->size > base + type.size)
                    return false;
                offsets.PushBack(static_cast<u32>(at - base));
            }
            return true;
        }

        bool Contains(Span<const TypeInfo* const> types, const TypeInfo& type)
        {
            for (const TypeInfo* t : types)
                if (t->id == type.id)
                    return true;
            return false;
        }

        // The C# spelling of `type` among `types`, empty when C# has no form for it.
        String CSharpName(const TypeInfo* type, Span<const TypeInfo* const> types)
        {
            if (type == nullptr)
                return String(u8"void");
            if (const StringView scalar = ScalarName(*type); !scalar.IsEmpty())
                return String(scalar);
            if (Contains(types, *type))
                return String(NameOf(type->name));
            return String{};
        }

        // The C# expression for the engine's TypeInfo of `type` (scalars and bound types alike).
        String TypeExpression(const TypeInfo& type) { return Format(u8"Types.Get(0x{}ul)", HexOf(type.id)); }
    }

    Array<const TypeInfo*> BindableValueTypes()
    {
        Array<const TypeInfo*> chosen;
        Array<const TypeInfo*> candidates;
        for (const TypeInfo* type : GlobalTypeRegistry().All())
            if (IsValueCandidate(*type))
                candidates.PushBack(type);
        // A type of fields whose types are scalars or chosen types; repeated until no more join,
        // so a struct of structs (Float3x3 of Float3) joins after its parts.
        bool grew = true;
        while (grew)
        {
            grew = false;
            for (const TypeInfo* type : candidates)
            {
                if (Contains(Span<const TypeInfo* const>{chosen.Data(), chosen.Size()}, *type))
                    continue;
                bool fits = true;
                for (u32 i = 0; fits && i < type->propertyCount; ++i)
                {
                    const TypeInfo* field = type->properties[i].type;
                    fits = field != nullptr && type->properties[i].address != nullptr &&
                           (!ScalarName(*field).IsEmpty() || Contains(Span<const TypeInfo* const>{chosen.Data(), chosen.Size()}, *field));
                }
                Array<u32> offsets;
                if (fits && MeasureFields(*type, offsets))
                {
                    chosen.PushBack(type);
                    grew = true;
                }
            }
        }
        // In name order, so the generated file reads the same from one build to the next.
        chosen.Sort([](const TypeInfo* a, const TypeInfo* b)
                    {
                        const StringView x = NameOf(a->name);
                        const StringView y = NameOf(b->name);
                        for (usize i = 0; i < x.Size() && i < y.Size(); ++i)
                            if (x[i] != y[i])
                                return x[i] < y[i];
                        return x.Size() < y.Size();
                    });
        return chosen;
    }

    String GenerateCSharpBindings(Span<const TypeInfo* const> types)
    {
        String out;
        out.Append(StringView(u8"// <auto-generated/> by Foundation::Script.DotNet from the engine's reflection. Do not edit.\n"
                              u8"#nullable enable\n"
                              u8"using System;\n"
                              u8"using System.Runtime.CompilerServices;\n"
                              u8"using System.Runtime.InteropServices;\n\n"
                              u8"namespace Engine;\n\n"));
        // The engine's TypeInfo of each type a binding passes, found once by id.
        out.Append(StringView(u8"internal static class Types\n{\n"
                              u8"    static readonly System.Collections.Generic.Dictionary<ulong, nint> s_found = new();\n"
                              u8"    public static nint Get(ulong id)\n    {\n"
                              u8"        if (!s_found.TryGetValue(id, out nint type))\n"
                              u8"            s_found[id] = type = Bridge.TypeById(id);\n"
                              u8"        return type;\n    }\n}\n\n"));

        String checks;
        for (const TypeInfo* type : types)
        {
            Array<u32> offsets;
            if (!MeasureFields(*type, offsets))
                continue;
            const StringView name = NameOf(type->name);
            AppendFormat(out, u8"[StructLayout(LayoutKind.Explicit, Size = {})]\npublic unsafe partial struct {}\n{{\n", type->size, name);
            AppendFormat(out, u8"    internal static nint EngineType => Types.Get(0x{}ul);\n", HexOf(type->id));
            AppendFormat(checks, u8"        ok &= sizeof({}) == {};\n", name, type->size);
            for (u32 i = 0; i < type->propertyCount; ++i)
            {
                const PropertyInfo& property = type->properties[i];
                const String field = CSharpName(property.type, types);
                AppendFormat(out, u8"    [FieldOffset({})] public {} {};\n", offsets[i], field, NameOf(property.name));
                AppendFormat(checks, u8"        ok &= Marshal.OffsetOf<{}>(\"{}\") == (nint){};\n", name, NameOf(property.name), offsets[i]);
            }
            // Methods: each found once by its index in the type's reflection.
            Array<String> emitted;
            for (u32 m = 0; m < type->methodCount; ++m)
            {
                const MethodInfo& method = type->methods[m];
                const StringView methodName = (method.overloadedName != nullptr && method.overloadedName[0] != '\0')
                                                  ? NameOf(method.overloadedName)
                                                  : NameOf(method.name);
                const TypeInfo* returned = method.returnType != nullptr ? method.returnType() : nullptr;
                const String returnName = CSharpName(returned, types);
                bool bindable = !returnName.IsEmpty();
                Array<String> paramTypes;
                for (u32 p = 0; bindable && p < method.paramCount; ++p)
                {
                    const TypeInfo* paramType = method.params[p].type != nullptr ? method.params[p].type() : nullptr;
                    paramTypes.PushBack(CSharpName(paramType, types));
                    bindable = paramType != nullptr && !paramTypes.Back().IsEmpty();
                }
                bool taken = false;
                for (const String& e : emitted)
                    taken = taken || e.AsView() == methodName;
                for (u32 i = 0; i < type->propertyCount; ++i)
                    taken = taken || NameOf(type->properties[i].name) == methodName;
                if (!bindable || taken)
                    continue;
                emitted.PushBack(String(methodName));
                AppendFormat(out, u8"\n    static nint s_m{};\n", m);
                AppendFormat(out, u8"    public {}{} {}(", method.isStatic ? StringView(u8"static ") : StringView(u8""), returnName, methodName);
                for (u32 p = 0; p < method.paramCount; ++p)
                {
                    const StringView given = method.params[p].name != nullptr ? NameOf(method.params[p].name) : StringView{};
                    const String paramName = given.IsEmpty() ? Format(u8"arg{}", p) : String(given);
                    AppendFormat(out, u8"{}{} {}", p > 0 ? StringView(u8", ") : StringView(u8""), paramTypes[p], paramName);
                }
                out.Append(StringView(u8")\n    {\n"));
                AppendFormat(out, u8"        if (s_m{} == 0) s_m{} = Bridge.MethodAt(EngineType, {});\n", m, m, m);
                AppendFormat(out, u8"        DotNetValue* args = stackalloc DotNetValue[{}];\n", method.paramCount > 0 ? method.paramCount : 1u);
                for (u32 p = 0; p < method.paramCount; ++p)
                {
                    const StringView given = method.params[p].name != nullptr ? NameOf(method.params[p].name) : StringView{};
                    const String paramName = given.IsEmpty() ? Format(u8"arg{}", p) : String(given);
                    const TypeInfo& paramType = *method.params[p].type();
                    AppendFormat(out, u8"        args[{}] = DotNetValue.OfBytes({}, &{}, sizeof({}));\n", p,
                                 TypeExpression(paramType), paramName, paramTypes[p]);
                }
                if (!method.isStatic)
                    AppendFormat(out, u8"        {} copy = this;\n        DotNetValue self = DotNetValue.OfBytes(EngineType, &copy, sizeof({}));\n",
                                 name, name);
                const StringView selfArg = method.isStatic ? StringView(u8"null") : StringView(u8"&self");
                if (returned == nullptr)
                {
                    AppendFormat(out, u8"        Bridge.Invoke(s_m{}, {}, args, {}, null);\n", m, selfArg, method.paramCount);
                }
                else
                {
                    AppendFormat(out, u8"        {} value = default;\n        DotNetValue result = DotNetValue.OfBytes(0, &value, sizeof({}));\n",
                                 returnName, returnName);
                    AppendFormat(out, u8"        Bridge.Invoke(s_m{}, {}, args, {}, &result);\n        return value;\n", m, selfArg,
                                 method.paramCount);
                }
                out.Append(StringView(u8"    }\n"));
            }
            out.Append(StringView(u8"}\n\n"));
        }
        // The layouts C# was built with, against the engine's: run once at start.
        out.Append(StringView(u8"public static unsafe class Layouts\n{\n    public static bool Check()\n    {\n        bool ok = true;\n"));
        out.Append(checks.AsView());
        out.Append(StringView(u8"        return ok;\n    }\n}\n"));
        return out;
    }
}
