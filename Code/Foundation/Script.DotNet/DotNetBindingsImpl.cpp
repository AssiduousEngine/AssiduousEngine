// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the C# bindings generator (see DotNetBindings.cppm).

module;
#include "Core/Prelude.h"

module foundation.script.dotnet;

import foundation.core;
import foundation.script;

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

        bool IsText(const TypeInfo& type) { return type.id == TypeOf<String>().id || type.id == TypeOf<StringView>().id; }

        // C#'s reserved words a script name may be spelled as (`@` makes them identifiers).
        bool IsKeyword(StringView name)
        {
            const StringView keywords[] = {
                u8"abstract", u8"as",       u8"base",     u8"bool",      u8"break",     u8"byte",     u8"case",     u8"catch",
                u8"char",     u8"checked",  u8"class",    u8"const",     u8"continue",  u8"decimal",  u8"default",  u8"delegate",
                u8"do",       u8"double",   u8"else",     u8"enum",      u8"event",     u8"explicit", u8"extern",   u8"false",
                u8"finally",  u8"fixed",    u8"float",    u8"for",       u8"foreach",   u8"goto",     u8"if",       u8"implicit",
                u8"in",       u8"int",      u8"interface", u8"internal", u8"is",        u8"lock",     u8"long",     u8"namespace",
                u8"new",      u8"null",     u8"object",   u8"operator",  u8"out",       u8"override", u8"params",   u8"private",
                u8"protected", u8"public",  u8"readonly", u8"ref",       u8"return",    u8"sbyte",    u8"sealed",   u8"short",
                u8"sizeof",   u8"stackalloc", u8"static", u8"string",    u8"struct",    u8"switch",   u8"this",     u8"throw",
                u8"true",     u8"try",      u8"typeof",   u8"uint",      u8"ulong",     u8"unchecked", u8"unsafe",  u8"ushort",
                u8"using",    u8"virtual",  u8"void",     u8"volatile",  u8"while",     u8"value",    u8"args",     u8"self",
                u8"result",   u8"copy",     u8"returned"};
            for (const StringView k : keywords)
                if (k == name)
                    return true;
            return false;
        }

        String Identifier(StringView name) { return IsKeyword(name) ? Format(u8"@{}", name) : String(name); }

        bool IsIdentifier(StringView name)
        {
            if (name.IsEmpty())
                return false;
            for (usize i = 0; i < name.Size(); ++i)
            {
                const utf8char c = name[i];
                const bool letter = (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || c == u8'_';
                const bool digit = c >= u8'0' && c <= u8'9';
                if (!letter && !(digit && i > 0))
                    return false;
            }
            return true;
        }

        enum class Shape : u8
        {
            None,   // C# has no form for it here
            Void,
            Scalar,
            Struct, // a value struct, fields measured
            Opaque, // a value C# holds as bytes it does not look into
            Enum,
            Text,
            Class, // a handle to the engine's value
            Any,   // a script value of any type (Variant): C# object, its form chosen at the call
            Handler, // a script delegate the engine calls back (IScriptDelegate): C# ScriptHandler
        };

        struct Bound
        {
            const TypeInfo* type = nullptr;
            Shape shape = Shape::None;
            String name;        // the C# spelling
            Array<u32> offsets; // Struct: each field's offset
        };

        // Where each field of `type` sits: measured on zeroed storage through the reflected field
        // addresses (a trivially copied value needs no construction to be addressed).
        bool MeasureFields(const TypeInfo& type, Array<u32>& offsets)
        {
            offsets.Clear();
            Array<u8> storage;
            storage.Resize(type.size + type.align + 1);
            u8* base = storage.Data();
            const usize alignment = type.align > 0 ? type.align : 1;
            const usize misalign = reinterpret_cast<usize>(base) % alignment;
            if (misalign != 0)
                base += alignment - misalign;
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

        struct Surface
        {
            Array<Bound> types;

            const Bound* Find(const TypeInfo* type) const
            {
                if (type == nullptr)
                    return nullptr;
                for (const Bound& b : types)
                    if (b.type->id == type->id)
                        return &b;
                return nullptr;
            }

            // How a parameter or return of `type` crosses.
            Shape ShapeOf(const TypeInfo* type, bool isReturn) const
            {
                if (type == nullptr)
                    return isReturn ? Shape::Void : Shape::None;
                if (!ScalarName(*type).IsEmpty())
                    return Shape::Scalar;
                if (IsText(*type))
                    return Shape::Text;
                if (type->id == TypeOf<Variant>().id)
                    return Shape::Any;
                if (type->id == IScriptDelegate::StaticType().id)
                    return isReturn ? Shape::None : Shape::Handler; // a handler is only ever given
                const Bound* b = Find(type);
                return b != nullptr ? b->shape : Shape::None;
            }

            String CSharpName(const TypeInfo* type) const
            {
                if (type == nullptr)
                    return String(u8"void");
                if (const StringView scalar = ScalarName(*type); !scalar.IsEmpty())
                    return String(scalar);
                if (IsText(*type))
                    return String(u8"string");
                if (type->id == TypeOf<Variant>().id)
                    return String(u8"object");
                if (type->id == IScriptDelegate::StaticType().id)
                    return String(u8"ScriptHandler");
                const Bound* b = Find(type);
                return b != nullptr ? b->name : String{};
            }

            // As a parameter or return is declared: a class may be null.
            String DeclaredName(const TypeInfo* type) const
            {
                const Shape shape = ShapeOf(type, true);
                const Shape given = ShapeOf(type, false);
                return shape == Shape::Class || shape == Shape::Any || given == Shape::Handler ? Format(u8"{}?", CSharpName(type))
                                                                                                 : CSharpName(type);
            }
        };

        // The bound types of `types`, each given its shape and C# name; a value type whose fields
        // are all scalars or structs becomes a struct of fields, any other one is held opaquely.
        Surface Classify(Span<const TypeInfo* const> types)
        {
            Surface surface;
            Array<String> taken;
            for (const TypeInfo* type : types)
            {
                if (type == nullptr || !ScalarName(*type).IsEmpty() || IsText(*type) || type->container != nullptr ||
                    type->reference != nullptr)
                    continue;
                const StringView scriptName = NameOf(ScriptTypeName(*type));
                if (!IsIdentifier(scriptName) || IsKeyword(scriptName))
                    continue;
                bool clash = false;
                for (const String& t : taken)
                    clash = clash || t.AsView() == scriptName;
                if (clash)
                    continue;
                Bound b;
                b.type = type;
                b.name = String(scriptName);
                if (type->enumeratorCount > 0)
                    b.shape = Shape::Enum;
                else if (IsScriptValue(type))
                    b.shape = Shape::Opaque; // a struct of fields when they measure (below)
                else
                    b.shape = Shape::Class;
                taken.PushBack(String(scriptName));
                surface.types.PushBack(Move(b));
            }
            // Repeated until none joins: a struct of structs joins after its parts.
            bool grew = true;
            while (grew)
            {
                grew = false;
                for (Bound& b : surface.types)
                {
                    if (b.shape != Shape::Opaque || b.type->propertyCount == 0 || b.type->base != nullptr)
                        continue;
                    bool fits = true;
                    for (u32 i = 0; fits && i < b.type->propertyCount; ++i)
                    {
                        const PropertyInfo& field = b.type->properties[i];
                        const Shape s = surface.ShapeOf(field.type, false);
                        fits = (s == Shape::Scalar || s == Shape::Struct) && IsIdentifier(NameOf(field.name)) &&
                               NameOf(field.name) != b.name.AsView();
                    }
                    if (fits && MeasureFields(*b.type, b.offsets))
                    {
                        b.shape = Shape::Struct;
                        grew = true;
                    }
                }
            }
            return surface;
        }

        String TypeExpression(const TypeInfo& type) { return Format(u8"Types.Get(0x{}ul)", HexOf(type.id)); }

        StringView EnumUnderlying(const TypeInfo& type)
        {
            switch (type.size)
            {
            case 1:
                return u8"byte";
            case 2:
                return u8"short";
            case 8:
                return u8"long";
            default:
                return u8"int";
            }
        }

        // Packs argument `index` (C# name `name`) into args[index].
        void EmitArgument(String& out, const Surface& surface, const TypeInfo& type, u32 index, StringView name)
        {
            switch (surface.ShapeOf(&type, false))
            {
            case Shape::Scalar:
            case Shape::Struct:
            case Shape::Opaque:
                AppendFormat(out, u8"        args[{}] = DotNetValue.OfBytes({}, &{}, sizeof({}));\n", index, TypeExpression(type), name,
                             surface.CSharpName(&type));
                break;
            case Shape::Enum:
                AppendFormat(out, u8"        {} e{} = ({}){};\n        args[{}] = DotNetValue.OfBytes({}, &e{}, sizeof({}));\n",
                             EnumUnderlying(type), index, EnumUnderlying(type), name, index, TypeExpression(type), index,
                             EnumUnderlying(type));
                break;
            case Shape::Text:
                AppendFormat(out, u8"        using TextArg t{} = new({});\n        args[{}] = t{}.Value;\n", index, name, index, index);
                break;
            case Shape::Class:
                AppendFormat(out, u8"        args[{}] = {} != null ? {}.EngineValue : default;\n", index, name, name);
                break;
            case Shape::Handler:
                AppendFormat(out, u8"        args[{}] = Bridge.Lend({});\n", index, name);
                break;
            case Shape::Any:
                // Packed by the value's own type, into storage of its own: several in one call never
                // share a buffer, and text is freed when the call returns.
                AppendFormat(out,
                             u8"        byte* v{} = stackalloc byte[AnyArg.Capacity];\n        using AnyArg a{} = new({}, v{});\n"
                             u8"        args[{}] = a{}.Value;\n",
                             index, index, name, index, index, index);
                break;
            default:
                break;
            }
        }

        // The result's local, the call (`call`, which takes the result pointer last), and the return.
        void EmitCallAndReturn(String& out, const Surface& surface, const TypeInfo* returned, StringView call)
        {
            const String name = surface.CSharpName(returned);
            switch (surface.ShapeOf(returned, true))
            {
            case Shape::Void:
                AppendFormat(out, u8"        {}null);\n", call);
                break;
            case Shape::Scalar:
            case Shape::Struct:
            case Shape::Opaque:
                AppendFormat(out,
                             u8"        {} returned = default;\n        DotNetValue result = DotNetValue.OfBytes(0, &returned, sizeof({}));\n"
                             u8"        {}&result);\n        return returned;\n",
                             name, name, call);
                break;
            case Shape::Enum:
                AppendFormat(out,
                             u8"        {} returned = default;\n        DotNetValue result = DotNetValue.OfBytes(0, &returned, sizeof({}));\n"
                             u8"        {}&result);\n        return ({})returned;\n",
                             EnumUnderlying(*returned), EnumUnderlying(*returned), call, name);
                break;
            case Shape::Text:
                AppendFormat(out, u8"        DotNetValue result = default;\n        {}&result);\n        return Bridge.TextOf(result);\n", call);
                break;
            case Shape::Class:
                AppendFormat(out,
                             u8"        DotNetValue result = default;\n        {}&result);\n"
                             u8"        return result.Kind == ValueKind.Handle ? new {}(result) : null;\n",
                             call, name);
                break;
            case Shape::Any:
                AppendFormat(out,
                             u8"        byte* returnedBytes = stackalloc byte[AnyArg.Capacity];\n"
                             u8"        DotNetValue result = DotNetValue.OfBytes(0, returnedBytes, AnyArg.Capacity);\n"
                             u8"        {}&result);\n        return Bridge.AnyOf(result);\n",
                             call);
                break;
            default:
                break;
            }
        }

        String ParamName(const MethodInfo& method, u32 p)
        {
            const StringView given = method.params[p].name != nullptr ? NameOf(method.params[p].name) : StringView{};
            return (given.IsEmpty() || !IsIdentifier(given)) ? Format(u8"arg{}", p) : Identifier(given);
        }

        // The value a member of `b` is called on.
        void EmitSelf(String& out, const Bound& b)
        {
            if (b.shape == Shape::Class)
                out.Append(StringView(u8"        DotNetValue self = EngineValue;\n"));
            else
                AppendFormat(out, u8"        {} copy = this;\n        DotNetValue self = DotNetValue.OfBytes(EngineType, &copy, sizeof({}));\n",
                             b.name, b.name);
        }

        void EmitMembers(String& out, const Surface& surface, const Bound& b)
        {
            const TypeInfo& type = *b.type;
            Array<String> signatures; // a C# method's name, parameter types and staticness: one each
            Array<String> memberNames;
            // Properties: on a class or an opaque value, through the bridge (a struct's are fields).
            if (b.shape == Shape::Class || b.shape == Shape::Opaque)
            {
                for (u32 i = 0; i < type.propertyCount; ++i)
                {
                    const PropertyInfo& property = type.properties[i];
                    const StringView rawName = NameOf(property.name);
                    const Shape shape = surface.ShapeOf(property.type, true);
                    if (!IsIdentifier(rawName) || shape == Shape::None || shape == Shape::Void || rawName == b.name.AsView())
                        continue;
                    memberNames.PushBack(String(rawName));
                    AppendFormat(out, u8"\n    static nint s_p{};\n    public {} {}\n    {{\n        get\n        {{\n", i,
                                 surface.DeclaredName(property.type), Identifier(rawName));
                    AppendFormat(out, u8"        if (s_p{} == 0) s_p{} = Bridge.PropertyAt(EngineType, {});\n", i, i, i);
                    EmitSelf(out, b);
                    EmitCallAndReturn(out, surface, property.type, Format(u8"Bridge.GetProperty(s_p{}, &self, ", i).AsView());
                    out.Append(StringView(u8"        }\n"));
                    const bool readOnly = (static_cast<u32>(property.flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0;
                    if (!readOnly && b.shape == Shape::Class)
                    {
                        AppendFormat(out, u8"        set\n        {{\n        if (s_p{} == 0) s_p{} = Bridge.PropertyAt(EngineType, {});\n", i,
                                     i, i);
                        out.Append(StringView(u8"        DotNetValue* args = stackalloc DotNetValue[1];\n"));
                        EmitArgument(out, surface, *property.type, 0, u8"value");
                        EmitSelf(out, b);
                        AppendFormat(out, u8"        Bridge.SetProperty(s_p{}, &self, args);\n        }}\n", i);
                    }
                    out.Append(StringView(u8"    }\n"));
                }
            }
            // Methods.
            for (u32 m = 0; m < type.methodCount; ++m)
            {
                const MethodInfo& method = type.methods[m];
                const StringView rawName = NameOf(ScriptMethodName(method));
                if (!IsIdentifier(rawName) || rawName == b.name.AsView() || (!method.isStatic && b.shape == Shape::Enum))
                    continue;
                bool nameTaken = false;
                for (const String& n : memberNames)
                    nameTaken = nameTaken || n.AsView() == rawName;
                if (nameTaken)
                    continue;
                const TypeInfo* returned = method.returnType != nullptr ? method.returnType() : nullptr;
                bool bindable = surface.ShapeOf(returned, true) != Shape::None;
                String signature(rawName);
                for (u32 p = 0; bindable && p < method.paramCount; ++p)
                {
                    const TypeInfo* paramType = method.params[p].type != nullptr ? method.params[p].type() : nullptr;
                    bindable = surface.ShapeOf(paramType, false) != Shape::None;
                    if (bindable)
                        AppendFormat(signature, u8",{}", surface.CSharpName(paramType));
                }
                for (const String& s : signatures)
                    bindable = bindable && s.AsView() != signature.AsView(); // C# overloads by types, not staticness
                if (!bindable)
                    continue;
                signatures.PushBack(Move(signature));
                AppendFormat(out, u8"\n    static nint s_m{};\n    public {}{} {}(", m, method.isStatic ? StringView(u8"static ") : StringView(u8""),
                             surface.DeclaredName(returned), Identifier(rawName));
                for (u32 p = 0; p < method.paramCount; ++p)
                    AppendFormat(out, u8"{}{} {}", p > 0 ? StringView(u8", ") : StringView(u8""), surface.DeclaredName(method.params[p].type()),
                                 ParamName(method, p));
                out.Append(StringView(u8")\n    {\n"));
                AppendFormat(out, u8"        if (s_m{} == 0) s_m{} = Bridge.MethodAt(EngineType, {});\n", m, m, m);
                AppendFormat(out, u8"        DotNetValue* args = stackalloc DotNetValue[{}];\n", method.paramCount > 0 ? method.paramCount : 1u);
                for (u32 p = 0; p < method.paramCount; ++p)
                    EmitArgument(out, surface, *method.params[p].type(), p, ParamName(method, p).AsView());
                if (!method.isStatic)
                    EmitSelf(out, b);
                EmitCallAndReturn(out, surface, returned,
                                  Format(u8"Bridge.Invoke(s_m{}, {}, args, {}, ", m, method.isStatic ? StringView(u8"null") : StringView(u8"&self"),
                                         method.paramCount)
                                      .AsView());
                out.Append(StringView(u8"    }\n"));
            }
            // A class with a constructor of no arguments is made from C#.
            if (b.shape == Shape::Class)
            {
                bool hasDefault = false;
                for (u32 c = 0; c < type.constructorCount; ++c)
                    hasDefault = hasDefault || type.constructors[c].paramCount == 0;
                if (hasDefault)
                    AppendFormat(out,
                                 u8"\n    public static {} Create()\n    {{\n        DotNetValue result = default;\n"
                                 u8"        Bridge.Construct(EngineType, null, 0, &result);\n        return new {}(result);\n    }}\n",
                                 b.name, b.name);
            }
        }
    }

    Array<const TypeInfo*> BindableValueTypes()
    {
        const Array<const TypeInfo*> all = GlobalTypeRegistry().All();
        const Surface surface = Classify(Span<const TypeInfo* const>{all.Data(), all.Size()});
        Array<const TypeInfo*> values;
        for (const Bound& b : surface.types)
            if (b.shape == Shape::Struct)
                values.PushBack(b.type);
        return values;
    }

    String GenerateCSharpBindings(Span<const TypeInfo* const> types)
    {
        Surface surface = Classify(types);
        // In name order, so the generated file reads the same from one build to the next.
        surface.types.Sort(
            [](const Bound& a, const Bound& b)
            {
                const StringView x = a.name.AsView();
                const StringView y = b.name.AsView();
                for (usize i = 0; i < x.Size() && i < y.Size(); ++i)
                    if (x[i] != y[i])
                        return x[i] < y[i];
                return x.Size() < y.Size();
            });
        String out;
        out.Append(StringView(u8"// <auto-generated/> by Foundation::Script.DotNet from the engine's reflection. Do not edit.\n"
                              u8"#nullable enable\n"
                              u8"using System;\n"
                              u8"using System.Runtime.CompilerServices;\n"
                              u8"using System.Runtime.InteropServices;\n\n"
                              u8"namespace Engine;\n\n"));
        out.Append(StringView(u8"public static unsafe partial class Types\n{\n"
                              u8"    static readonly System.Collections.Generic.Dictionary<ulong, nint> s_found = new();\n"
                              u8"    public static nint Get(ulong id)\n    {\n"
                              u8"        if (!s_found.TryGetValue(id, out nint type))\n"
                              u8"            s_found[id] = type = Bridge.TypeById(id);\n"
                              u8"        return type;\n    }\n}\n\n"));
        String checks;
        for (const Bound& b : surface.types)
        {
            const TypeInfo& type = *b.type;
            switch (b.shape)
            {
            case Shape::Enum:
            {
                AppendFormat(out, u8"public enum {} : {}\n{{\n", b.name, EnumUnderlying(type));
                Array<String> seen;
                for (u32 i = 0; i < type.enumeratorCount; ++i)
                {
                    const StringView name = NameOf(type.enumerators[i].name);
                    bool dup = false;
                    for (const String& s : seen)
                        dup = dup || s.AsView() == name;
                    if (!IsIdentifier(name) || dup)
                        continue;
                    seen.PushBack(String(name));
                    AppendFormat(out, u8"    {} = unchecked(({}){}),\n", Identifier(name), EnumUnderlying(type), type.enumerators[i].value);
                }
                out.Append(StringView(u8"}\n\n"));
                break;
            }
            case Shape::Struct:
            case Shape::Opaque:
            {
                AppendFormat(out, u8"[StructLayout(LayoutKind.{}, Size = {})]\npublic unsafe partial struct {}\n{{\n",
                             b.shape == Shape::Struct ? StringView(u8"Explicit") : StringView(u8"Sequential"), type.size, b.name);
                AppendFormat(out, u8"    internal static nint EngineType => Types.Get(0x{}ul);\n", HexOf(type.id));
                if (b.shape == Shape::Struct)
                {
                    AppendFormat(checks, u8"        if ({}.EngineType != 0 && sizeof({}) != Bridge.TypeSize({}.EngineType)) return \"{} size\";\n", b.name, b.name, b.name,
                                 b.name);
                    for (u32 i = 0; i < type.propertyCount; ++i)
                    {
                        const StringView fieldName = NameOf(type.properties[i].name);
                        AppendFormat(out, u8"    [FieldOffset({})] public {} {};\n", b.offsets[i], surface.CSharpName(type.properties[i].type),
                                     Identifier(fieldName));
                        AppendFormat(checks, u8"        if (Marshal.OffsetOf<{}>(\"{}\") != (nint){}) return \"{}.{}\";\n", b.name, fieldName, b.offsets[i],
                                     b.name, fieldName);
                    }
                }
                else
                {
                    // Held whole as bytes, sized for 64-bit (a 32-bit engine's is no larger).
                    AppendFormat(checks, u8"        if ({}.EngineType != 0 && sizeof({}) < Bridge.TypeSize({}.EngineType)) return \"{} size\";\n", b.name, b.name, b.name,
                                 b.name);
                }
                EmitMembers(out, surface, b);
                out.Append(StringView(u8"}\n\n"));
                break;
            }
            case Shape::Class:
            {
                AppendFormat(out, u8"public sealed unsafe partial class {} : EngineObject\n{{\n", b.name);
                AppendFormat(out, u8"    internal {}(DotNetValue handle) : base(handle) {{ }}\n", b.name);
                AppendFormat(out, u8"    internal static nint EngineType => Types.Get(0x{}ul);\n", HexOf(type.id));
                EmitMembers(out, surface, b);
                out.Append(StringView(u8"}\n\n"));
                break;
            }
            default:
                break;
            }
        }
        // The C# type of each value an engine id names, the wrapper of each class's handle, and the
        // scalars' ids: what the runtime converts a value crossing into C# code it did not
        // generate (a behaviour's fields and handlers) with.
        // Each value type's typed read and write of its bytes: a copy that needs no marshalling data,
        // so it holds compiled ahead of time.
        out.Append(StringView(u8"public static unsafe partial class Types\n{\n"
                              u8"    public static readonly (ulong Id, System.Type Type, System.Func<nint, object> Read, "
                              u8"System.Action<object, nint> Write, int Size)[] Values =\n    {\n"));
        for (const Bound& b : surface.types)
            if (b.shape == Shape::Struct || b.shape == Shape::Opaque)
                AppendFormat(out, u8"        (0x{}ul, typeof({}), static p => *({}*)p, static (o, p) => *({}*)p = ({})o, sizeof({})),\n",
                             HexOf(b.type->id), b.name, b.name, b.name, b.name, b.name);
        out.Append(StringView(u8"    };\n\n    public static EngineObject? Wrap(ulong id, DotNetValue handle) => id switch\n    {\n"));
        for (const Bound& b : surface.types)
            if (b.shape == Shape::Class)
                AppendFormat(out, u8"        0x{}ul => new {}(handle),\n", HexOf(b.type->id), b.name);
        out.Append(StringView(u8"        _ => null,\n    };\n\n"));
        struct ScalarId
        {
            StringView name;
            u64 id;
        };
        const ScalarId scalarIds[] = {{u8"F32", TypeOf<f32>().id}, {u8"F64", TypeOf<f64>().id}, {u8"I8", TypeOf<i8>().id},
                                      {u8"U8", TypeOf<u8>().id},   {u8"I16", TypeOf<i16>().id}, {u8"U16", TypeOf<u16>().id},
                                      {u8"I32", TypeOf<i32>().id}, {u8"U32", TypeOf<u32>().id}, {u8"I64", TypeOf<i64>().id},
                                      {u8"U64", TypeOf<u64>().id}, {u8"Bool", TypeOf<bool>().id}, {u8"Text", TypeOf<String>().id}};
        for (const ScalarId& scalar : scalarIds)
            AppendFormat(out, u8"    public const ulong {}Id = 0x{}ul;\n", scalar.name, HexOf(scalar.id));
        out.Append(StringView(u8"}\n\n"));
        // The layouts C# was built with, against the engine's: the first that differs, or null. A
        // type the running engine has not registered (a host without that domain) is not checked:
        // nothing of it can cross.
        out.Append(StringView(u8"public static unsafe class Layouts\n{\n    public static string? FirstMismatch()\n    {\n"));
        out.Append(checks.AsView());
        out.Append(StringView(u8"        return null;\n    }\n}\n"));
        return out;
    }
}
