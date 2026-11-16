// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The C# end of the bridge (Code/Foundation/Script.DotNet/DotNetBridge.cppm): the engine's table of
// functions, the value that crosses, and the call the generated bindings make. The same under
// CoreCLR and NativeAOT: no DllImport, no reflection, no runtime code generation.
using System;
using System.Runtime.InteropServices;

namespace Engine;

/// <summary>What a DotNetValue holds.</summary>
public enum ValueKind : uint
{
    Empty = 0,
    Bytes = 1,
    Text = 2,
    Handle = 3, // a native Variant* the receiver of a result owns
}

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
        return true;
    }

    public static nint TypeById(ulong id) => s_engine.TypeById(id);
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

    public static string LastError()
    {
        ulong length = 0;
        byte* text = s_engine.LastError(&length);
        return text == null ? "" : System.Text.Encoding.UTF8.GetString(text, (int)length);
    }
}
