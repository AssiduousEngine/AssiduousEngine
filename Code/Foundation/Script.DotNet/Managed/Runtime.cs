// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The engine's managed runtime: the entry points the engine calls to load a game's assembly and run
// its behaviours. A behaviour is a plain C# class the engine constructs with its entity and calls by
// handler name (onStart, onUpdate, ...), as it does an AngelScript or Luau class; its public fields
// are its inspector properties.

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace Engine;

/// <summary>Kept the same on every peer of a networked game (the host's value is sent).</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ReplicatedAttribute : Attribute
{
}

/// <summary>The inspector's tooltip for a property.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class TooltipAttribute : Attribute
{
    public TooltipAttribute(string text) { Text = text; }
    public string Text { get; }
}

public static unsafe class Runtime
{
    // A game assembly's context: it resolves the engine's runtime to this assembly, so the game's
    // Float3 is the runtime's Float3 (hostfxr loads the runtime into a context of its own).
    sealed class GameContext : AssemblyLoadContext
    {
        public GameContext() : base("Game", isCollectible: true) { }

        protected override Assembly? Load(AssemblyName name) =>
            name.Name == typeof(Runtime).Assembly.GetName().Name ? typeof(Runtime).Assembly : null;
    }

    static readonly List<Assembly> s_assemblies = new(); // newest last; a class is found newest first
    static readonly Dictionary<string, Type> s_classes = new();
    static readonly Dictionary<ulong, Type> s_valueTypes = new();
    static readonly Dictionary<Type, ulong> s_valueIds = new();
    static readonly Dictionary<(Type, string, int), MethodInfo?> s_methods = new();

    [ThreadStatic] static string? t_error;
    [ThreadStatic] static byte* t_errorBytes;
    [ThreadStatic] static byte* t_text;          // a text result, until the thread's next call
    [ThreadStatic] static int t_textCapacity;
    [ThreadStatic] static byte* t_bytes;         // a value result's bytes, until the thread's next call

    const int kValueCapacity = 256;

    // -1: it failed (a C# exception, a value with no form); -2: what was asked for is not there.
    static int Fail(string why)
    {
        t_error = why;
        return -1;
    }

    static int Missing(string why)
    {
        t_error = why;
        return -2;
    }

    static string Utf8(byte* text, int length) => length > 0 ? Encoding.UTF8.GetString(text, length) : "";

    /// <summary>Takes the engine's table and checks the bindings' layouts against it: 0, or -1 with
    /// LastError naming the first type that differs.</summary>
    [UnmanagedCallersOnly]
    public static int Initialize(EngineFunctions* engine)
    {
        try
        {
            if (engine == null || engine->Size != (uint)sizeof(EngineFunctions))
                return Fail("the engine's function table is not the one this runtime was built for");
            Bridge.Initialize(engine);
            if (Layouts.FirstMismatch() is string mismatch)
                return Fail($"the C# layout of {mismatch} differs from the engine's");
            s_valueTypes.Clear();
            s_valueIds.Clear();
            foreach ((ulong id, Type type) in Types.Values)
            {
                s_valueTypes[id] = type;
                s_valueIds[type] = id;
            }
            return 0;
        }
        catch (Exception e)
        {
            return Fail(e.Message);
        }
    }

    /// <summary>Loads a game assembly from its image, with its debug symbols when given (a fault then
    /// names its file and line); its classes are found before those of any loaded earlier (a rebuilt
    /// assembly's class replaces its predecessor's).</summary>
    [UnmanagedCallersOnly]
    public static int LoadAssembly(byte* image, int length, byte* symbols, int symbolsLength)
    {
        try
        {
            var context = new GameContext();
            using var stream = new System.IO.UnmanagedMemoryStream(image, length);
            using System.IO.Stream? symbolStream = symbols != null && symbolsLength > 0 ? new System.IO.UnmanagedMemoryStream(symbols, symbolsLength) : null;
            Assembly assembly = context.LoadFromStream(stream, symbolStream);
            s_assemblies.Add(assembly);
            foreach (Type type in assembly.GetTypes())
                if (type.IsClass && !type.IsAbstract && type.IsPublic)
                    s_classes[type.Name] = type;
            s_methods.Clear();
            return 0;
        }
        catch (Exception e)
        {
            return Fail(e is ReflectionTypeLoadException load && load.LoaderExceptions.Length > 0 && load.LoaderExceptions[0] != null
                            ? load.LoaderExceptions[0]!.Message
                            : e.Message);
        }
    }

    /// <summary>Constructs the class `name` (by its public constructor of `count` parameters) with
    /// `args`; the instance is held for the engine until Release.</summary>
    [UnmanagedCallersOnly]
    public static int CreateInstance(byte* name, int nameLength, DotNetValue* args, int count, nint* instance)
    {
        try
        {
            string className = Utf8(name, nameLength);
            *instance = 0;
            if (!s_classes.TryGetValue(className, out Type? type))
                return Missing($"no C# class named {className} in the game's assembly");
            foreach (ConstructorInfo constructor in type.GetConstructors())
            {
                ParameterInfo[] parameters = constructor.GetParameters();
                if (parameters.Length != count)
                    continue;
                object? made = constructor.Invoke(Arguments(parameters, args, count));
                *instance = GCHandle.ToIntPtr(GCHandle.Alloc(made!));
                return 0;
            }
            return Missing($"{className} has no public constructor of {count} parameters");
        }
        catch (TargetInvocationException e) when (e.InnerException != null)
        {
            return Fail(Describe(e.InnerException));
        }
        catch (Exception e)
        {
            return Fail(Describe(e));
        }
    }

    /// <summary>Calls the instance's public method `name` of `count` parameters; "x=" sets its field
    /// or property x. The result (if any) is written to `result`, valid until the thread's next call.</summary>
    [UnmanagedCallersOnly]
    public static int Invoke(nint instance, byte* name, int nameLength, DotNetValue* args, int count, DotNetValue* result)
    {
        try
        {
            Bridge.ReleaseQueuedHandles();
            object target = GCHandle.FromIntPtr(instance).Target!;
            Type type = target.GetType();
            string member = Utf8(name, nameLength);
            *result = default;
            if (member.EndsWith('=') && count == 1)
            {
                string fieldName = member[..^1];
                if (type.GetField(fieldName, BindingFlags.Public | BindingFlags.Instance) is FieldInfo field)
                {
                    field.SetValue(target, FromEngine(args[0], field.FieldType));
                    return 0;
                }
                if (type.GetProperty(fieldName, BindingFlags.Public | BindingFlags.Instance) is PropertyInfo property && property.CanWrite)
                {
                    property.SetValue(target, FromEngine(args[0], property.PropertyType));
                    return 0;
                }
                return Missing($"{type.Name} has no public field {fieldName}");
            }
            MethodInfo? method = FindMethod(type, member, count);
            if (method == null)
                return Missing($"{type.Name} has no public method {member} of {count} parameters");
            object? returned = method.Invoke(target, Arguments(method.GetParameters(), args, count));
            if (method.ReturnType != typeof(void))
                ToEngine(returned, result);
            return 0;
        }
        catch (TargetInvocationException e) when (e.InnerException != null)
        {
            return Fail(Describe(e.InnerException));
        }
        catch (Exception e)
        {
            return Fail(Describe(e));
        }
    }

    /// <summary>Reads the instance's public field or property `name`.</summary>
    [UnmanagedCallersOnly]
    public static int GetField(nint instance, byte* name, int nameLength, DotNetValue* result)
    {
        try
        {
            object target = GCHandle.FromIntPtr(instance).Target!;
            Type type = target.GetType();
            string member = Utf8(name, nameLength);
            *result = default;
            if (type.GetField(member, BindingFlags.Public | BindingFlags.Instance) is FieldInfo field)
            {
                ToEngine(field.GetValue(target), result);
                return 0;
            }
            if (type.GetProperty(member, BindingFlags.Public | BindingFlags.Instance) is PropertyInfo property && property.CanRead)
            {
                ToEngine(property.GetValue(target), result);
                return 0;
            }
            return Missing($"{type.Name} has no public field {member}");
        }
        catch (Exception e)
        {
            return Fail(Describe(e));
        }
    }

    /// <summary>Lets the engine's hold on an instance go.</summary>
    [UnmanagedCallersOnly]
    public static void Release(nint instance)
    {
        if (instance != 0)
            GCHandle.FromIntPtr(instance).Free();
    }

    /// <summary>What the class `name` offers the engine (see DescribeType), an instance made with
    /// `args` to read its defaults from. Valid until the thread's next call.</summary>
    [UnmanagedCallersOnly]
    public static int DescribeClass(byte* name, int nameLength, DotNetValue* args, int count, byte** text, int* length)
    {
        try
        {
            string className = Utf8(name, nameLength);
            if (!s_classes.TryGetValue(className, out Type? type))
                return Missing($"no C# class named {className} in the game's assembly");
            object? instance = null;
            foreach (ConstructorInfo constructor in type.GetConstructors())
                if (constructor.GetParameters().Length == count)
                    instance = constructor.Invoke(Arguments(constructor.GetParameters(), args, count));
            if (instance == null)
                return Missing($"{className} has no public constructor of {count} parameters");
            var lines = new StringBuilder();
            DescribeType(type, instance, lines);
            SetText(lines.ToString(), out *text, out *length);
            return 0;
        }
        catch (TargetInvocationException e) when (e.InnerException != null)
        {
            return Fail(Describe(e.InnerException));
        }
        catch (Exception e)
        {
            return Fail(Describe(e));
        }
    }

    /// <summary>Every public class of an assembly image, for the cook: "class NAME", then its lines
    /// (see DescribeType), its defaults read from an instance made by its public constructor of
    /// fewest parameters, each given its type's default (an invalid Entity, a null object). A
    /// constructor that throws leaves the class's fields without defaults and adds "warning TEXT".
    /// The image loads into a context of its own, unloaded after. Valid until the thread's next call.</summary>
    [UnmanagedCallersOnly]
    public static int DescribeAssembly(byte* image, int length, byte** text, int* textLength)
    {
        var context = new GameContext();
        try
        {
            using var stream = new System.IO.UnmanagedMemoryStream(image, length);
            Assembly assembly = context.LoadFromStream(stream);
            var lines = new StringBuilder();
            foreach (Type type in assembly.GetTypes())
            {
                if (!type.IsClass || type.IsAbstract || !type.IsPublic)
                    continue;
                lines.Append("class ").Append(type.Name).Append('\n');
                object? instance = null;
                ConstructorInfo? simplest = null;
                foreach (ConstructorInfo constructor in type.GetConstructors())
                    if (simplest == null || constructor.GetParameters().Length < simplest.GetParameters().Length)
                        simplest = constructor;
                if (simplest != null)
                {
                    try
                    {
                        ParameterInfo[] parameters = simplest.GetParameters();
                        var values = new object?[parameters.Length];
                        for (int i = 0; i < parameters.Length; ++i)
                            values[i] = parameters[i].ParameterType.IsValueType ? Activator.CreateInstance(parameters[i].ParameterType) : null;
                        instance = simplest.Invoke(values);
                    }
                    catch (Exception e)
                    {
                        Exception cause = e is TargetInvocationException { InnerException: not null } ? e.InnerException! : e;
                        lines.Append("warning ").Append(OneLine($"{type.Name}'s constructor threw at cook ({cause.Message}): its fields have no defaults")).Append('\n');
                    }
                }
                DescribeType(type, instance, lines);
            }
            SetText(lines.ToString(), out *text, out *textLength);
            return 0;
        }
        catch (Exception e)
        {
            return Fail(e is ReflectionTypeLoadException load && load.LoaderExceptions.Length > 0 && load.LoaderExceptions[0] != null
                            ? load.LoaderExceptions[0]!.Message
                            : Describe(e));
        }
        finally
        {
            context.Unload();
        }
    }

    // A class's lines: "method NAME" for each of its public methods, "field NAME KIND DEFAULT" for
    // each public field of an inspector kind (its default from `instance`, empty without one),
    // then "tooltip NAME TEXT" and "replicated NAME".
    static void DescribeType(Type type, object? instance, StringBuilder lines)
    {
        foreach (MethodInfo method in type.GetMethods(BindingFlags.Public | BindingFlags.Instance))
            if (method.DeclaringType != typeof(object) && !method.IsSpecialName)
                lines.Append("method ").Append(method.Name).Append('\n');
        foreach (FieldInfo field in type.GetFields(BindingFlags.Public | BindingFlags.Instance))
        {
            string? kind = KindOf(field.FieldType);
            if (kind == null)
                continue;
            lines.Append("field ").Append(field.Name).Append(' ').Append(kind).Append(' ')
                 .Append(instance != null ? DefaultText(field.GetValue(instance)) : "").Append('\n');
            if (field.GetCustomAttribute<TooltipAttribute>() is TooltipAttribute tooltip)
                lines.Append("tooltip ").Append(field.Name).Append(' ').Append(OneLine(tooltip.Text)).Append('\n');
            if (field.GetCustomAttribute<ReplicatedAttribute>() != null)
                lines.Append("replicated ").Append(field.Name).Append('\n');
        }
    }

    /// <summary>The reason the thread's last failed call failed, UTF-8.</summary>
    [UnmanagedCallersOnly]
    public static byte* LastError(int* length)
    {
        string text = t_error ?? "";
        if (t_errorBytes != null)
            NativeMemory.Free(t_errorBytes);
        int count = Encoding.UTF8.GetByteCount(text);
        t_errorBytes = (byte*)NativeMemory.Alloc((nuint)System.Math.Max(count, 1));
        fixed (char* chars = text)
            Encoding.UTF8.GetBytes(chars, text.Length, t_errorBytes, count);
        *length = count;
        return t_errorBytes;
    }

    // ---- values ----

    static string Describe(Exception e) => $"{e.GetType().Name}: {e.Message}\n{e.StackTrace}";

    static string OneLine(string text) => text.Replace("\r", "").Replace("\n", "\\n");

    // The inspector kind of a field's type, as the engine's script property kinds name them.
    static string? KindOf(Type type)
    {
        if (type == typeof(float) || type == typeof(double))
            return "float";
        if (type == typeof(int) || type == typeof(long) || type == typeof(uint))
            return "int";
        if (type == typeof(bool))
            return "bool";
        if (type == typeof(string))
            return "string";
        if (type == typeof(Float3))
            return "vec3";
        if (type == typeof(Color))
            return "color";
        if (type == typeof(Entity))
            return "entity";
        return null;
    }

    static string DefaultText(object? value) => value switch
    {
        null => "",
        float f => f.ToString("R", System.Globalization.CultureInfo.InvariantCulture),
        double d => d.ToString("R", System.Globalization.CultureInfo.InvariantCulture),
        bool b => b ? "true" : "false",
        string s => OneLine(s),
        Float3 v => string.Create(System.Globalization.CultureInfo.InvariantCulture, $"{v.x} {v.y} {v.z}"),
        Color c => string.Create(System.Globalization.CultureInfo.InvariantCulture, $"{c.r} {c.g} {c.b} {c.a}"),
        Entity => "",
        IFormattable n => n.ToString(null, System.Globalization.CultureInfo.InvariantCulture),
        _ => "",
    };

    static object?[] Arguments(ParameterInfo[] parameters, DotNetValue* args, int count)
    {
        var values = new object?[count];
        for (int i = 0; i < count; ++i)
            values[i] = FromEngine(args[i], parameters[i].ParameterType);
        return values;
    }

    static MethodInfo? FindMethod(Type type, string name, int count)
    {
        if (s_methods.TryGetValue((type, name, count), out MethodInfo? found))
            return found;
        foreach (MethodInfo method in type.GetMethods(BindingFlags.Public | BindingFlags.Instance))
            if (method.Name == name && method.GetParameters().Length == count)
            {
                found = method;
                break;
            }
        s_methods[(type, name, count)] = found;
        return found;
    }

    // A value from the engine as `target`: numbers convert between widths (an inspector number
    // arrives as a double), a value struct is copied from its bytes, a handle is wrapped.
    static object? FromEngine(DotNetValue value, Type target)
    {
        switch (value.Kind)
        {
        case ValueKind.Empty:
            return target.IsValueType ? Activator.CreateInstance(target) : null;
        case ValueKind.Text:
            return Bridge.TextOf(value);
        case ValueKind.Handle:
        {
            EngineObject? wrapped = Types.Wrap(Bridge.TypeIdOf(value.Type), value);
            if (wrapped == null)
                Bridge.ReleaseHandle((nint)value.Data);
            return wrapped;
        }
        }
        ulong id = Bridge.TypeIdOf(value.Type);
        if (s_valueTypes.TryGetValue(id, out Type? valueType))
        {
            if (!target.IsAssignableFrom(valueType))
                throw new ArgumentException($"a {valueType.Name} is not a {target.Name}");
            return Marshal.PtrToStructure((nint)value.Data, valueType);
        }
        object number = id switch
        {
            Types.F64Id => *(double*)value.Data,
            Types.F32Id => *(float*)value.Data,
            Types.I32Id => *(int*)value.Data,
            Types.U32Id => *(uint*)value.Data,
            Types.I64Id => *(long*)value.Data,
            Types.U64Id => *(ulong*)value.Data,
            Types.I16Id => *(short*)value.Data,
            Types.U16Id => *(ushort*)value.Data,
            Types.I8Id => *(sbyte*)value.Data,
            Types.U8Id => *(byte*)value.Data,
            Types.BoolId => *(byte*)value.Data != 0,
            _ => throw new ArgumentException($"a value of engine type {id:x} has no C# form"),
        };
        Type wanted = Nullable.GetUnderlyingType(target) ?? target;
        return wanted == typeof(object) ? number : Convert.ChangeType(number, wanted, System.Globalization.CultureInfo.InvariantCulture);
    }

    static void SetText(string text, out byte* bytes, out int length)
    {
        int count = Encoding.UTF8.GetByteCount(text);
        if (t_textCapacity < count + 1)
        {
            if (t_text != null)
                NativeMemory.Free(t_text);
            t_textCapacity = System.Math.Max(count + 1, 256);
            t_text = (byte*)NativeMemory.Alloc((nuint)t_textCapacity);
        }
        fixed (char* chars = text)
            Encoding.UTF8.GetBytes(chars, text.Length, t_text, count);
        t_text[count] = 0;
        bytes = t_text;
        length = count;
    }

    // A C# value to the engine: numbers and value structs as bytes (in a buffer the thread keeps
    // until its next call), text as UTF-8, an engine object by its handle.
    static void ToEngine(object? value, DotNetValue* result)
    {
        *result = default;
        if (value == null)
            return;
        if (t_bytes == null)
            t_bytes = (byte*)NativeMemory.AlignedAlloc(kValueCapacity, 16);
        switch (value)
        {
        case string s:
            SetText(s, out byte* text, out int length);
            *result = new DotNetValue { Data = text, Size = (ulong)length, Kind = ValueKind.Text };
            return;
        case EngineObject engineObject:
            *result = engineObject.EngineValue;
            return;
        case float f: *(float*)t_bytes = f; Scalar(result, Types.F32Id, 4); return;
        case double d: *(double*)t_bytes = d; Scalar(result, Types.F64Id, 8); return;
        case int i: *(int*)t_bytes = i; Scalar(result, Types.I32Id, 4); return;
        case uint u: *(uint*)t_bytes = u; Scalar(result, Types.U32Id, 4); return;
        case long l: *(long*)t_bytes = l; Scalar(result, Types.I64Id, 8); return;
        case ulong ul: *(ulong*)t_bytes = ul; Scalar(result, Types.U64Id, 8); return;
        case bool b: *t_bytes = b ? (byte)1 : (byte)0; Scalar(result, Types.BoolId, 1); return;
        }
        if (s_valueIds.TryGetValue(value.GetType(), out ulong id))
        {
            int size = Marshal.SizeOf(value.GetType());
            if (size > kValueCapacity)
                throw new ArgumentException($"{value.GetType().Name} is larger than a result holds");
            Marshal.StructureToPtr(value, (nint)t_bytes, false);
            Scalar(result, id, size);
            return;
        }
        throw new ArgumentException($"a {value.GetType().Name} has no engine form");
    }

    static void Scalar(DotNetValue* result, ulong id, int size) =>
        *result = new DotNetValue { Type = Bridge.TypeById(id), Data = t_bytes, Size = (ulong)size, Kind = ValueKind.Bytes };
}
