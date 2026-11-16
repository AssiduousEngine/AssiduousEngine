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

// Through the generated bindings: Float3's fields and methods as C# writes them, each call going to
// the engine's reflected method through the bridge.
public static unsafe class UsesBindings
{
    // An exception must not leave an [UnmanagedCallersOnly] method (the runtime ends the process):
    // anything the calls throw comes back as -5.
    [UnmanagedCallersOnly]
    public static int Run(Engine.EngineFunctions* engine)
    {
        try
        {
            return RunChecked(engine);
        }
        catch (System.Exception e)
        {
            System.Console.Error.WriteLine($"C# threw: {e.Message}");
            return -5;
        }
    }

    static int RunChecked(Engine.EngineFunctions* engine)
    {
        if (!Engine.Bridge.Initialize(engine))
            return -1; // a table built for another layout
        if (Engine.Layouts.FirstMismatch() is string mismatch)
        {
            System.Console.Error.WriteLine($"layout differs: {mismatch}");
            return -2; // a struct's layout differs from the engine's
        }
        var a = new Engine.Float3 { x = 1, y = 2, z = 3 };
        var b = new Engine.Float3 { x = 4, y = 5, z = 6 };
        float dot = Engine.Float3.Dot(a, b);
        Engine.Float3 cross = Engine.Float3.Cross(a, b);
        try
        {
            Engine.Float3.Lerp(a, b, float.NaN); // succeeds: NaN is a float
        }
        catch (Engine.EngineException e)
        {
            System.Console.Error.WriteLine($"Lerp failed: {e.Message}");
            return -3;
        }
        // A facade called from C#: Random's static range, through the generated bindings.
        float r = Engine.Random.range(1.0f, 2.0f);
        if (r < 1.0f || r > 2.0f)
            return -4;
        // 32 and (-3, 6, -3), encoded so the host can check all four.
        return (int)dot * 1000 + (int)(cross.x + 10) * 100 + (int)(cross.y) * 10 + (int)(cross.z + 10);
    }
}
