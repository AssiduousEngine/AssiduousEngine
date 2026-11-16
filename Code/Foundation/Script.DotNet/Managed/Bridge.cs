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
    Object = 3,
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
    public delegate* unmanaged<void*, void> AddRef;
    public delegate* unmanaged<void*, void> Release;
    public delegate* unmanaged<ulong*, byte*> LastError;
}

/// <summary>A call into the engine that failed; the engine's reason is the message.</summary>
public sealed class EngineException : Exception
{
    public EngineException(string message) : base(message) { }
}

public static unsafe class Bridge
{
    static EngineFunctions s_engine;

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

    /// <summary>Calls a method; throws EngineException with the engine's reason on failure.</summary>
    public static void Invoke(nint method, DotNetValue* self, DotNetValue* args, uint count, DotNetValue* result)
    {
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
