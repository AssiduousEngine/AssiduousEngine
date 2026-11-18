// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The C# end of the bridge (Code/Foundation/Script.DotNet/DotNetBridge.cppm): the engine's table of
// functions, the value that crosses, and the call the generated bindings make. The same under
// CoreCLR and NativeAOT: no DllImport, no reflection, no runtime code generation.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Engine;

/// <summary>What a DotNetValue holds.</summary>
public enum ValueKind : uint
{
    Empty = 0,
    Bytes = 1,
    Text = 2,
    Handle = 3, // a native Variant* the receiver of a result owns
    Callback = 4, // a C# handler the engine calls back (a pinned handle it then owns)
}

/// <summary>A handler the engine calls back (a GUI event, a network message, a reply): the call's
/// arguments as C# objects, and what it returns (true when a GUI event is handled).</summary>
public delegate object? ScriptHandler(object?[] args);

/// <summary>A value crossing to or from the engine; the field order of the C++ DotNetValue.</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct DotNetValue
{
    public nint Type;
    public void* Data;
    public ulong Size;
    public ValueKind Kind;
    public uint Reserved;

    public static DotNetValue OfBytes(nint type, void* data, int size) =>
        new() { Type = type, Data = data, Size = (ulong)size, Kind = ValueKind.Bytes };
}

/// <summary>The engine's functions; the field order of the C++ DotNetEngineFunctions.</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct EngineFunctions
{
    public uint Size;
    public delegate* unmanaged<ulong, nint> TypeById;
    public delegate* unmanaged<nint, uint, nint> MethodAt;
    public delegate* unmanaged<nint, uint, nint> PropertyAt;
    public delegate* unmanaged<nint, DotNetValue*, DotNetValue*, uint, DotNetValue*, int> Invoke;
    public delegate* unmanaged<nint, DotNetValue*, DotNetValue*, int> GetProperty;
    public delegate* unmanaged<nint, DotNetValue*, DotNetValue*, int> SetProperty;
    public delegate* unmanaged<nint, DotNetValue*, uint, DotNetValue*, int> Construct;
    public delegate* unmanaged<nint, uint> TypeSize;
    public delegate* unmanaged<void*, void> ReleaseHandle;
    public delegate* unmanaged<nint, ulong> TypeIdOf;
    public delegate* unmanaged<ulong*, byte*> LastError;
    public delegate* unmanaged<nint, void*, int> ComponentAlive;
}

/// <summary>A call into the engine that failed; the engine's reason is the message.</summary>
public sealed class EngineException : Exception
{
    public EngineException(string message) : base(message) { }
}

/// <summary>An engine object, component or member C# holds: a handle to the engine's value. Dispose
/// gives it back at once; a collected one is given back on the engine's thread at its next call.</summary>
public abstract unsafe class EngineObject : IDisposable
{
    nint m_handle;

    protected EngineObject(DotNetValue handle) { m_handle = (nint)handle.Data; }

    internal DotNetValue EngineValue => new() { Data = (void*)m_handle, Kind = ValueKind.Handle };

    public void Dispose()
    {
        if (m_handle != 0)
            Bridge.ReleaseHandle(m_handle);
        m_handle = 0;
        GC.SuppressFinalize(this);
    }

    ~EngineObject()
    {
        if (m_handle != 0)
            Bridge.QueueRelease(m_handle); // the finalizer thread is not the engine's
    }
}

/// <summary>A string as an argument: its UTF-8 in native memory for the length of the call.</summary>
public unsafe struct TextArg : IDisposable
{
    byte* m_bytes;
    int m_length;

    public TextArg(string? text)
    {
        text ??= "";
        m_length = System.Text.Encoding.UTF8.GetByteCount(text);
        m_bytes = (byte*)System.Runtime.InteropServices.NativeMemory.Alloc((nuint)System.Math.Max(m_length, 1));
        fixed (char* chars = text)
            System.Text.Encoding.UTF8.GetBytes(chars, text.Length, m_bytes, m_length);
    }

    public DotNetValue Value => new() { Data = m_bytes, Size = (ulong)m_length, Kind = ValueKind.Text };

    public void Dispose()
    {
        if (m_bytes != null)
            System.Runtime.InteropServices.NativeMemory.Free(m_bytes);
        m_bytes = null;
    }
}

// An argument the engine takes as any script value (its Variant), from a C# object: text as UTF-8
// it owns until disposed, an engine object by its handle, a number or a value struct as bytes in
// `storage` (Capacity bytes the caller keeps for the call). Null crosses as nothing.
public unsafe struct AnyArg : IDisposable
{
    public const int Capacity = 256;

    TextArg m_text;
    DotNetValue m_value;

    public AnyArg(object? value, byte* storage)
    {
        m_text = default;
        m_value = default;
        switch (value)
        {
        case null:
            return;
        case string text:
            m_text = new TextArg(text);
            m_value = m_text.Value;
            return;
        case EngineObject engineObject:
            m_value = engineObject.EngineValue;
            return;
        }
        m_value = Bridge.PackValue(value, storage, Capacity);
    }

    public DotNetValue Value => m_value;

    public void Dispose() => m_text.Dispose();
}

public static unsafe class Bridge
{
    static EngineFunctions s_engine;
    static readonly System.Collections.Concurrent.ConcurrentQueue<nint> s_released = new();

    /// <summary>Takes the engine's table; false (nothing taken) when it was built for another.</summary>
    public static bool Initialize(EngineFunctions* engine)
    {
        if (engine == null || engine->Size != (uint)sizeof(EngineFunctions))
            return false;
        s_engine = *engine;
        ValueTypes.Clear();
        ValueIds.Clear();
        foreach ((ulong id, Type type, Func<nint, object> read, Action<object, nint> write, int size) in Types.Values)
        {
            ValueTypes[id] = (type, read);
            ValueIds[type] = (id, write, size);
        }
        return true;
    }

    // The value types the engine and C# share, both ways: an engine id to its C# type and reader,
    // and a C# type to its engine id, writer and size (from the generated Types.Values).
    internal static readonly Dictionary<ulong, (Type Type, Func<nint, object> Read)> ValueTypes = new();
    internal static readonly Dictionary<Type, (ulong Id, Action<object, nint> Write, int Size)> ValueIds = new();

    /// <summary>A number or a value struct as the engine's bytes, written to `storage` (`capacity`
    /// bytes): what a result and an argument of any type (AnyArg) carry.</summary>
    public static DotNetValue PackValue(object value, byte* storage, int capacity)
    {
        switch (value)
        {
        case float f: *(float*)storage = f; return Bytes(Types.F32Id, storage, 4);
        case double d: *(double*)storage = d; return Bytes(Types.F64Id, storage, 8);
        case int i: *(int*)storage = i; return Bytes(Types.I32Id, storage, 4);
        case uint u: *(uint*)storage = u; return Bytes(Types.U32Id, storage, 4);
        case long l: *(long*)storage = l; return Bytes(Types.I64Id, storage, 8);
        case ulong ul: *(ulong*)storage = ul; return Bytes(Types.U64Id, storage, 8);
        case bool b: *storage = b ? (byte)1 : (byte)0; return Bytes(Types.BoolId, storage, 1);
        }
        if (ValueIds.TryGetValue(value.GetType(), out (ulong Id, Action<object, nint> Write, int Size) known))
        {
            if (known.Size > capacity)
                throw new ArgumentException($"{value.GetType().Name} is larger than a value holds");
            known.Write(value, (nint)storage);
            return Bytes(known.Id, storage, known.Size);
        }
        throw new ArgumentException($"a {value.GetType().Name} has no engine form");
    }

    static DotNetValue Bytes(ulong id, byte* data, int size) =>
        new DotNetValue { Type = TypeById(id), Data = data, Size = (ulong)size, Kind = ValueKind.Bytes };

    /// <summary>A value from the engine as the C# object it is: null for none, text, an engine object
    /// (a handle it then owns), a number or a value struct boxed.</summary>
    public static object? AnyOf(DotNetValue value)
    {
        switch (value.Kind)
        {
        case ValueKind.Empty:
            return null;
        case ValueKind.Text:
            return TextOf(value);
        case ValueKind.Handle:
        {
            EngineObject? wrapped = Types.Wrap(TypeIdOf(value.Type), value);
            if (wrapped == null)
                ReleaseHandle((nint)value.Data);
            return wrapped;
        }
        }
        ulong id = TypeIdOf(value.Type);
        if (ValueTypes.TryGetValue(id, out (Type Type, Func<nint, object> Read) known))
            return known.Read((nint)value.Data);
        return id switch
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
    }

    public static nint TypeById(ulong id) => s_engine.TypeById(id);

    /// <summary>`handler` given to the engine, which keeps it until it lets the handler go (the
    /// runtime's Release frees it); null crosses as nothing.</summary>
    public static DotNetValue Lend(ScriptHandler? handler) => handler == null
        ? default
        : new DotNetValue { Data = (void*)GCHandle.ToIntPtr(GCHandle.Alloc(handler)), Kind = ValueKind.Callback };
    public static nint MethodAt(nint type, uint index) => s_engine.MethodAt(type, index);
    public static nint PropertyAt(nint type, uint index) => s_engine.PropertyAt(type, index);
    public static uint TypeSize(nint type) => s_engine.TypeSize(type);
    public static ulong TypeIdOf(nint type) => s_engine.TypeIdOf(type);

    public static void ReleaseHandle(nint handle) => s_engine.ReleaseHandle((void*)handle);
    internal static void QueueRelease(nint handle) => s_released.Enqueue(handle);

    // What the collector let go of, given back on this (the engine's) thread.
    public static void ReleaseQueuedHandles()
    {
        while (s_released.TryDequeue(out nint handle))
            s_engine.ReleaseHandle((void*)handle);
    }

    public static void Construct(nint type, DotNetValue* args, uint count, DotNetValue* result)
    {
        ReleaseQueuedHandles();
        if (s_engine.Construct(type, args, count, result) < 0)
            throw new EngineException(LastError());
    }

    public static void GetProperty(nint property, DotNetValue* self, DotNetValue* result)
    {
        ReleaseQueuedHandles();
        if (s_engine.GetProperty(property, self, result) < 0)
            throw new EngineException(LastError());
    }

    public static void SetProperty(nint property, DotNetValue* self, DotNetValue* value)
    {
        ReleaseQueuedHandles();
        if (s_engine.SetProperty(property, self, value) < 0)
            throw new EngineException(LastError());
    }

    /// <summary>A text result as a string.</summary>
    public static string TextOf(in DotNetValue value) =>
        value.Kind == ValueKind.Text && value.Data != null ? System.Text.Encoding.UTF8.GetString((byte*)value.Data, (int)value.Size) : "";

    /// <summary>Calls a method; throws EngineException with the engine's reason on failure.</summary>
    public static void Invoke(nint method, DotNetValue* self, DotNetValue* args, uint count, DotNetValue* result)
    {
        ReleaseQueuedHandles();
        if (s_engine.Invoke(method, self, args, count, result) < 0)
            throw new EngineException(LastError());
    }

    /// <summary>Whether a scene component held as a value (its entity, under the component's type)
    /// is there: its entity has it.</summary>
    public static bool ComponentAlive(nint type, void* value) => s_engine.ComponentAlive(type, value) != 0;

    public static string LastError()
    {
        ulong length = 0;
        byte* text = s_engine.LastError(&length);
        return text == null ? "" : System.Text.Encoding.UTF8.GetString(text, (int)length);
    }
}

/// <summary>The engine's Guid, made from its text as the engine writes it
/// ("3f2a9c1e-0b7d-4e5a-9c21-7d0e5b6a4f38", braces allowed): the first 16 hex digits are `high`,
/// the last 16 `low`. A script names an asset (a clip, a prefab) by it.</summary>
public partial struct Guid
{
    public static Guid Parse(string text)
    {
        if (!TryParse(text, out Guid parsed))
            throw new FormatException($"'{text}' is not a guid");
        return parsed;
    }

    public static bool TryParse(string? text, out Guid result)
    {
        result = default;
        if (text == null)
            return false;
        ReadOnlySpan<char> span = text.AsSpan().Trim();
        if (span.Length == 38 && span[0] == '{' && span[37] == '}')
            span = span.Slice(1, 36);
        if (span.Length != 36)
            return false;
        ulong high = 0, low = 0;
        int digits = 0;
        for (int i = 0; i < 36; ++i)
        {
            char c = span[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != '-')
                    return false;
                continue;
            }
            int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (value < 0)
                return false;
            if (digits < 16)
                high = (high << 4) | (ulong)value;
            else
                low = (low << 4) | (ulong)value;
            ++digits;
        }
        result = new Guid { high = high, low = low };
        return true;
    }

    public override string ToString() => $"{high >> 32:x8}-{(high >> 16) & 0xFFFF:x4}-{high & 0xFFFF:x4}-{low >> 48:x4}-{low & 0xFFFFFFFFFFFF:x12}";
}
