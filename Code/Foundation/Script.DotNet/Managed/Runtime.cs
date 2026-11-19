// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The engine's managed runtime: the entry points the engine calls to load an application's assembly and run
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
    // An application assembly's context: it resolves the engine's runtime to this assembly, so the application's
    // Float3 is the runtime's Float3 (hostfxr loads the runtime into a context of its own).
    sealed class ApplicationContext : AssemblyLoadContext
    {
        public ApplicationContext() : base("Application", isCollectible: true) { }

        protected override Assembly? Load(AssemblyName name) =>
            name.Name == typeof(Runtime).Assembly.GetName().Name ? typeof(Runtime).Assembly : null;
    }

    // A loaded application assembly: its context, the image's hash, and how many instances the engine holds
    // of its classes. Once a newer assembly has taken all its class names (superseded) and the last
    // instance goes, its context unloads.
    sealed class ApplicationAssembly
    {
        public required ApplicationContext? Context; // null: the assembly compiled in with the runtime
        public required Assembly Assembly;
        public required byte[] Hash;
        public int Instances;
        public bool Superseded;
    }

    // What one owner (a script context: one run's C#) has loaded, and the newest class of each name
    // among it. Each owner loads its own copy of an image, so two runs never share a class's statics;
    // an owner reloading a rebuilt image supersedes its own older one.
    sealed class Owner
    {
        public readonly List<ApplicationAssembly> Assemblies = new(); // loaded and current or in use, newest last
        public readonly Dictionary<string, (Type Type, ApplicationAssembly From)> Classes = new();
    }

    static readonly Dictionary<nint, Owner> s_owners = new();
    static readonly List<ApplicationAssembly> s_assemblies = new(); // every one loaded and not unloaded
    // Compiled in (an exported player): the application's classes, one copy every owner shares.
    static readonly Dictionary<string, (Type Type, ApplicationAssembly From)> s_compiledClasses = new();
    static readonly Dictionary<nint, ApplicationAssembly> s_instances = new(); // the assembly of each instance held
    static readonly List<WeakReference> s_unloaded = new(); // contexts told to unload, until collected
    // The value types the bindings know, by engine id and by C# type, with their typed copies.
    static readonly Dictionary<(Type, string, int), MethodInfo?> s_methods = new();

    [ThreadStatic] static string? t_error;
    [ThreadStatic] static byte* t_errorBytes;
    [ThreadStatic] static byte* t_text;          // a text result, until the thread's next call
    [ThreadStatic] static int t_textCapacity;
    [ThreadStatic] static byte* t_bytes;         // a value result's bytes, until the thread's next call

    const int kValueCapacity = 256;

    // An entry point returns 0 when it worked, -1 when it failed (a C# exception, a value with no
    // form) and -2 when the class, constructor, method or field it was given does not exist.
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
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_Initialize")]
    public static int Initialize(EngineFunctions* engine)
    {
        try
        {
            if (engine == null || engine->Size != (uint)sizeof(EngineFunctions))
                return Fail("the engine's function table is not the one this runtime was built for");
            Bridge.Initialize(engine);
            if (Layouts.FirstMismatch() is string mismatch)
                return Fail($"the C# layout of {mismatch} differs from the engine's");
            if (CompiledIn && s_compiledClasses.Count == 0)
                RegisterCompiledIn();
            return 0;
        }
        catch (Exception e)
        {
            return Fail(e.Message);
        }
    }

    /// <summary>Loads an application assembly for `ownerId` (a script context) from its image, with its
    /// debug symbols when given (a fault then names its file and line); its classes are found, for
    /// that owner, before those of any it loaded earlier (a rebuilt assembly's class replaces its
    /// predecessor's).</summary>
    /// An image already current for the owner is not loaded again, so every class of a project
    /// asking for its assembly loads it once per owner; another owner loads its own copy, with its
    /// own statics; an image loaded before and since replaced (an edit undone) loads anew.
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_LoadAssembly")]
    public static int LoadAssembly(nint ownerId, byte* image, int length, byte* symbols, int symbolsLength)
    {
        try
        {
            if (CompiledIn)
                return 0; // the application is compiled in: there is nothing to load
            if (!s_owners.TryGetValue(ownerId, out Owner? owner))
                s_owners[ownerId] = owner = new Owner();
            byte[] hash = System.Security.Cryptography.SHA256.HashData(new ReadOnlySpan<byte>(image, length));
            foreach (ApplicationAssembly loaded in owner.Assemblies)
                if (!loaded.Superseded && loaded.Hash.AsSpan().SequenceEqual(hash))
                    return 0;
            var context = new ApplicationContext();
            using var stream = new System.IO.UnmanagedMemoryStream(image, length);
            using System.IO.Stream? symbolStream = symbols != null && symbolsLength > 0 ? new System.IO.UnmanagedMemoryStream(symbols, symbolsLength) : null;
            Assembly assembly = context.LoadFromStream(stream, symbolStream);
            var added = new ApplicationAssembly { Context = context, Assembly = assembly, Hash = hash };
            s_assemblies.Add(added);
            owner.Assemblies.Add(added);
            foreach (Type type in assembly.GetTypes())
                if (type.IsClass && !type.IsAbstract && type.IsPublic)
                    owner.Classes[type.Name] = (type, added);
            s_methods.Clear();
            // An older assembly of this owner none of whose classes is the newest any more is
            // superseded; with no instance left it unloads now, else when its last one goes.
            foreach (ApplicationAssembly older in owner.Assemblies.ToArray())
            {
                if (older == added || older.Superseded)
                    continue;
                bool current = false;
                foreach ((Type _, ApplicationAssembly from) in owner.Classes.Values)
                    current = current || from == older;
                if (!current)
                    Supersede(owner, older);
            }
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
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_CreateInstance")]
    public static int CreateInstance(nint ownerId, byte* name, int nameLength, DotNetValue* args, int count, nint* instance)
    {
        try
        {
            string className = Utf8(name, nameLength);
            *instance = 0;
            if (!FindClass(ownerId, className, out (Type Type, ApplicationAssembly From) found))
                return Missing($"no C# class named {className} in the application's assembly");
            foreach (ConstructorInfo constructor in found.Type.GetConstructors())
            {
                ParameterInfo[] parameters = constructor.GetParameters();
                if (parameters.Length != count)
                    continue;
                object? made = constructor.Invoke(Arguments(parameters, args, count));
                nint handle = GCHandle.ToIntPtr(GCHandle.Alloc(made!));
                s_instances[handle] = found.From;
                found.From.Instances += 1;
                *instance = handle;
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
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_Invoke")]
    public static int Invoke(nint instance, byte* name, int nameLength, DotNetValue* args, int count, DotNetValue* result)
    {
        try
        {
            Bridge.ReleaseQueuedHandles();
            object target = GCHandle.FromIntPtr(instance).Target!;
            Type type = target.GetType();
            string member = Utf8(name, nameLength);
            *result = default;
            if (target is ScriptHandler handler)
            {
                // A handler the engine calls back: every argument as the object it is.
                var values = new object?[count];
                for (int i = 0; i < count; ++i)
                    values[i] = Bridge.AnyOf(args[i]);
                ToEngine(handler(values), result);
                return 0;
            }
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

    // A method bound once for one instance, which the engine then calls by its id: no name to look
    // up, no parameters to fetch and no array to box into on each call. A handler with no argument
    // or one number is a delegate over its instance; any other goes through its method, with the
    // parameter types read once.
    sealed class BoundMethod
    {
        public required nint Instance;
        public required MethodInfo Method;
        public required ParameterInfo[] Parameters;
        public Action? Plain;
        public Action<float>? Single;
        public Action<double>? Double;
    }
    static readonly List<BoundMethod?> s_bound = new();
    static readonly Stack<int> s_freeBound = new();
    static readonly Dictionary<nint, List<int>> s_boundOf = new(); // each instance's, let go with it
    static nint s_f32Type;
    static nint s_f64Type;

    /// <summary>Binds the instance's public method `name` of `count` parameters for InvokeBound,
    /// writing its id. Not found (a member it does not have, a field's "x=", a handler object) when
    /// the engine must call it by name.</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_Bind")]
    public static int Bind(nint instance, byte* name, int nameLength, int count, int* id)
    {
        try
        {
            *id = -1;
            object target = GCHandle.FromIntPtr(instance).Target!;
            string member = Utf8(name, nameLength);
            if (target is ScriptHandler || member.EndsWith('='))
                return Missing($"{member} is called by name");
            MethodInfo? method = FindMethod(target.GetType(), member, count);
            if (method == null)
                return Missing($"{target.GetType().Name} has no public method {member} of {count} parameters");
            var bound = new BoundMethod { Instance = instance, Method = method, Parameters = method.GetParameters() };
            if (method.ReturnType == typeof(void))
            {
                if (count == 0)
                    bound.Plain = method.CreateDelegate<Action>(target);
                else if (count == 1 && bound.Parameters[0].ParameterType == typeof(float))
                    bound.Single = method.CreateDelegate<Action<float>>(target);
                else if (count == 1 && bound.Parameters[0].ParameterType == typeof(double))
                    bound.Double = method.CreateDelegate<Action<double>>(target);
            }
            int slot;
            if (s_freeBound.Count > 0)
            {
                slot = s_freeBound.Pop();
                s_bound[slot] = bound;
            }
            else
            {
                slot = s_bound.Count;
                s_bound.Add(bound);
            }
            if (!s_boundOf.TryGetValue(instance, out List<int>? ids))
                s_boundOf[instance] = ids = new List<int>();
            ids.Add(slot);
            *id = slot;
            return 0;
        }
        catch (Exception e)
        {
            return Fail(Describe(e));
        }
    }

    /// <summary>Calls a method Bind bound, as Invoke calls it by name.</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_InvokeBound")]
    public static int InvokeBound(int id, DotNetValue* args, int count, DotNetValue* result)
    {
        try
        {
            Bridge.ReleaseQueuedHandles();
            *result = default;
            BoundMethod bound = s_bound[id]!;
            if (bound.Plain != null && count == 0)
            {
                bound.Plain();
                return 0;
            }
            if ((bound.Single != null || bound.Double != null) && count == 1 && args[0].Kind == ValueKind.Bytes)
            {
                double number = NumberOf(args[0]);
                if (bound.Single != null)
                    bound.Single((float)number);
                else
                    bound.Double!(number);
                return 0;
            }
            object target = GCHandle.FromIntPtr(bound.Instance).Target!;
            object? returned = bound.Method.Invoke(target, Arguments(bound.Parameters, args, count));
            if (bound.Method.ReturnType != typeof(void))
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

    // A number the engine passes (a frame's time) without boxing it: f32 and f64 by their type,
    // anything else through the general reading.
    static double NumberOf(in DotNetValue value)
    {
        if (s_f32Type == 0)
        {
            s_f32Type = Bridge.TypeById(Types.F32Id);
            s_f64Type = Bridge.TypeById(Types.F64Id);
        }
        if (value.Type == s_f64Type)
            return *(double*)value.Data;
        if (value.Type == s_f32Type)
            return *(float*)value.Data;
        return Convert.ToDouble(Bridge.AnyOf(value), System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>Reads the instance's public field or property `name`.</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_GetField")]
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
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_Release")]
    public static void Release(nint instance)
    {
        if (instance == 0)
            return;
        GCHandle.FromIntPtr(instance).Free();
        if (s_boundOf.Remove(instance, out List<int>? ids))
            foreach (int id in ids)
            {
                s_bound[id] = null;
                s_freeBound.Push(id);
            }
        if (s_instances.Remove(instance, out ApplicationAssembly? from))
        {
            from.Instances -= 1;
            if (from.Superseded && from.Instances == 0)
                Unload(from);
        }
    }

    // The class `name` as `ownerId` sees it: its own newest, else the one compiled in.
    static bool FindClass(nint ownerId, string name, out (Type Type, ApplicationAssembly From) found)
    {
        if (s_owners.TryGetValue(ownerId, out Owner? owner) && owner.Classes.TryGetValue(name, out found))
            return true;
        return s_compiledClasses.TryGetValue(name, out found);
    }

    // An owner's assembly that no class of it names any more: it unloads now with no instance left,
    // else when its last one goes (Release).
    static void Supersede(Owner owner, ApplicationAssembly assembly)
    {
        assembly.Superseded = true;
        owner.Assemblies.Remove(assembly);
        if (assembly.Instances == 0)
            Unload(assembly);
    }

    /// <summary>Lets `ownerId` (a script context that is going) go: its classes are no longer found,
    /// and each of its assemblies unloads once the engine releases its last instance of it.</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_ReleaseOwner")]
    public static void ReleaseOwner(nint ownerId)
    {
        if (!s_owners.Remove(ownerId, out Owner? owner))
            return;
        foreach (ApplicationAssembly assembly in owner.Assemblies.ToArray())
            Supersede(owner, assembly);
        s_methods.Clear();
    }

    // Lets a superseded assembly go: nothing here refers to its types any more, and its context
    // unloads once the collector finds nothing else does.
    static void Unload(ApplicationAssembly assembly)
    {
        s_assemblies.Remove(assembly);
        s_methods.Clear();
        if (assembly.Context == null)
            return; // compiled in: never unloads
        s_unloaded.Add(new WeakReference(assembly.Context));
        assembly.Context.Unload();
    }

    /// <summary>How many application assemblies are loaded: the ones in use, and the ones told to unload
    /// that the collector has not yet freed (after a full collection, which this runs).</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_LoadedAssemblyCount")]
    public static int LoadedAssemblyCount()
    {
        for (int i = 0; i < 3; ++i)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        s_unloaded.RemoveAll(weak => !weak.IsAlive);
        return s_assemblies.Count + s_unloaded.Count;
    }

    /// <summary>What the class `name` offers the engine (see DescribeType), an instance made with
    /// `args` to read its defaults from. Valid until the thread's next call.</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_DescribeClass")]
    public static int DescribeClass(nint ownerId, byte* name, int nameLength, DotNetValue* args, int count, byte** text, int* length)
    {
        try
        {
            string className = Utf8(name, nameLength);
            if (!FindClass(ownerId, className, out (Type Type, ApplicationAssembly From) found))
                return Missing($"no C# class named {className} in the application's assembly");
            Type type = found.Type;
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
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_DescribeAssembly")]
    public static int DescribeAssembly(byte* image, int length, byte** text, int* textLength)
    {
        var context = new ApplicationContext();
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
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_LastError")]
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

    /// <summary>Whether this runtime was compiled ahead of time with the application (an exported player):
    /// then nothing loads at run time, and the application's classes are the ones compiled in.</summary>
    static bool CompiledIn => !RuntimeFeature.IsDynamicCodeSupported;

    // The application's classes compiled in beside the runtime: every public class outside the engine's
    // namespace. The images a class's record carries are not loaded.
    static void RegisterCompiledIn()
    {
        Assembly assembly = typeof(Runtime).Assembly;
        var compiled = new ApplicationAssembly { Context = null, Assembly = assembly, Hash = Array.Empty<byte>() };
        s_assemblies.Add(compiled);
        foreach (Type type in assembly.GetTypes())
            if (type.IsClass && !type.IsAbstract && type.IsPublic && type.Namespace != "Engine")
                s_compiledClasses[type.Name] = (type, compiled);
    }

    /// <summary>1: the runtime is compiled into this program (the engine's stand-in answers 0).</summary>
    [UnmanagedCallersOnly(EntryPoint = "DotNetRuntime_Present")]
    public static int Present() => 1;

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
        object? any = Bridge.AnyOf(value);
        if (any == null)
            return target.IsValueType ? Activator.CreateInstance(target) : null;
        if (target.IsInstanceOfType(any))
            return any;
        Type wanted = Nullable.GetUnderlyingType(target) ?? target;
        if (any is IConvertible && wanted.IsPrimitive)
            return Convert.ChangeType(any, wanted, System.Globalization.CultureInfo.InvariantCulture);
        throw new ArgumentException($"a {any.GetType().Name} is not a {target.Name}");
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
        case ScriptHandler handler:
            *result = Bridge.Lend(handler);
            return;
        }
        *result = Bridge.PackValue(value, t_bytes, kValueCapacity);
    }
}
