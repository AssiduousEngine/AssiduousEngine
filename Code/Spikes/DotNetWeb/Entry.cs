// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The C# half of the web test: one entry point the C++ host calls with a table of its functions.
// C# calls back through the table (a sum, a log line), allocates, collects, and returns a value
// the host checks, so the round trip, the managed heap and the collector all run in the page.
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Spike;

[StructLayout(LayoutKind.Sequential)]
public unsafe struct EngineTable
{
    public delegate* unmanaged<int, int, int> Add;
    public delegate* unmanaged<byte*, int, void> Log; // UTF-8 text and its length
}

public static unsafe class Entry
{
    static EngineTable s_engine;

    static void Log(string text)
    {
        byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(text);
        fixed (byte* p = utf8)
            s_engine.Log(p, utf8.Length);
    }

    [UnmanagedCallersOnly(EntryPoint = "Managed_Initialize")]
    public static int Initialize(EngineTable* table)
    {
        s_engine = *table;
        int sum = s_engine.Add(40, 2);
        // Some managed work: objects on the heap, a collection, a string built from them.
        var squares = new System.Collections.Generic.List<int>();
        for (int i = 0; i < 1000; ++i)
            squares.Add(i * i);
        GC.Collect();
        Log($"C# says: the engine's sum is {sum}, the 999th square {squares[999]}");
        return sum + squares.Count; // 42 + 1000
    }
}
