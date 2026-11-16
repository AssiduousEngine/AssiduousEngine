// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The C# side of the host test, the same entry point as the web test (Code/Spikes/DotNetWeb): a
// table of the host's functions in, a call back through it, managed work, a value out.
using System;
using System.Runtime.InteropServices;

namespace Probe;

[StructLayout(LayoutKind.Sequential)]
public unsafe struct EngineTable
{
    public delegate* unmanaged<int, int, int> Add;
    public delegate* unmanaged<byte*, int, void> Log;
}

public static unsafe class Entry
{
    [UnmanagedCallersOnly]
    public static int Initialize(EngineTable* table)
    {
        int sum = table->Add(40, 2);
        var squares = new System.Collections.Generic.List<int>();
        for (int i = 0; i < 1000; ++i)
            squares.Add(i * i);
        GC.Collect();
        byte[] text = System.Text.Encoding.UTF8.GetBytes($"C# on CoreCLR: sum {sum}");
        fixed (byte* p = text)
            table->Log(p, text.Length);
        return sum + squares.Count;
    }
}
