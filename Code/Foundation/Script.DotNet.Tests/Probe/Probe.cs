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
        // Lists through the engine both ways: packed as a list argument is, read back as a list
        // result is (the engine's makeList, listSize and listAt).
        using (var words = new Engine.ListArg(new[] { "ab", "cde" }))
        {
            string[] back = Engine.Bridge.ListOf<string>(words.Value);
            if (back.Length != 2 || back[0] != "ab" || back[1] != "cde")
                return -6;
        }
        using (var numbers = new Engine.ListArg(new[] { 1.5f, -2.0f }))
        {
            float[] back = Engine.Bridge.ListOf<float>(numbers.Value);
            if (back.Length != 2 || back[0] != 1.5f || back[1] != -2.0f)
                return -7;
        }
        using (var none = new Engine.ListArg(null))
        {
            if (Engine.Bridge.ListOf<int>(none.Value).Length != 0)
                return -8;
        }
        // 32 and (-3, 6, -3), encoded so the host can check all four.
        return (int)dot * 1000 + (int)(cross.x + 10) * 100 + (int)(cross.y) * 10 + (int)(cross.z + 10);
    }
}

// The engine's pure value functions as C# computes them (ValueMath, through the generated
// bindings), on fixed inputs: the host computes the same in C++ and compares. The order is the
// host test's (DotNetBindingsTests, "pure functions"): keep the two the same.
public static unsafe class PureValues
{
    [UnmanagedCallersOnly]
    public static int Run(float* output, int capacity)
    {
        int count = 0;
        void Put(float value)
        {
            if (count < capacity)
                output[count] = value;
            count += 1;
        }
        void V2(Engine.Float2 v) { Put(v.x); Put(v.y); }
        void V3(Engine.Float3 v) { Put(v.x); Put(v.y); Put(v.z); }
        void V4(Engine.Float4 v) { Put(v.x); Put(v.y); Put(v.z); Put(v.w); }
        void C(Engine.Color c) { Put(c.r); Put(c.g); Put(c.b); Put(c.a); }
        void B(bool b) => Put(b ? 1.0f : 0.0f);

        var a2 = new Engine.Float2 { x = 1.5f, y = -2.0f };
        var b2 = new Engine.Float2 { x = -4.0f, y = 0.5f };
        var a = new Engine.Float3 { x = 1.5f, y = -2.0f, z = 3.25f };
        var b = new Engine.Float3 { x = -4.0f, y = 0.5f, z = 2.0f };
        var a4 = new Engine.Float4 { x = 1.5f, y = -2.0f, z = 3.25f, w = 0.5f };
        var b4 = new Engine.Float4 { x = -4.0f, y = 0.5f, z = 2.0f, w = -1.0f };
        var ca = new Engine.Color { r = 0.25f, g = 0.5f, b = 0.75f, a = 1.0f };
        var cb = new Engine.Color { r = 0.5f, g = 0.25f, b = 0.125f, a = 0.5f };
        const float s = 0.75f, x = 0.6f, y = -1.25f;

        V2(a2 + b2); V2(a2 - b2); V2(a2 * b2); V2(a2 * s); V2(a2 / s); V2(-a2); B(Engine.Float2.Equals(a2, b2));
        Put(Engine.Float3.Dot(a, b)); V3(Engine.Float3.Cross(a, b)); Put(Engine.Float3.Length(a));
        Put(Engine.Float3.LengthSquared(a)); Put(Engine.Float3.Distance(a, b)); V3(Engine.Float3.Normalized(a));
        V3(Engine.Float3.Lerp(a, b, s)); V3(a + b); V3(a - b); V3(a * b); V3(a * s); V3(a / b); V3(a / s); V3(-a);
        B(Engine.Float3.Equals(a, a));
        V3(a4.XYZ()); V4(a4 + b4); V4(a4 - b4); V4(a4 * s); V4(-a4); B(Engine.Float4.Equals(a4, b4));
        C(ca + cb); C(ca * s); B(Engine.Color.Equals(ca, ca));
        Put(Engine.Math.Abs(y)); Put(Engine.Math.Sqrt(s)); Put(Engine.Math.Sin(x)); Put(Engine.Math.Cos(x));
        Put(Engine.Math.Tan(x)); Put(Engine.Math.Asin(x)); Put(Engine.Math.Acos(x)); Put(Engine.Math.Atan2(y, x));
        Put(Engine.Math.Floor(y)); Put(Engine.Math.Ceil(y)); Put(Engine.Math.Round(2.5f)); Put(Engine.Math.Pow(s, x));
        Put(Engine.Math.Log(s)); Put(Engine.Math.Exp(x)); Put(Engine.Math.DegreesToRadians(45.0f));
        Put(Engine.Math.RadiansToDegrees(x)); Put(Engine.Math.Lerp(x, y, s)); Put(Engine.Math.Min(x, y));
        Put(Engine.Math.Max(x, y)); Put(Engine.Math.Clamp(y, -1.0f, 1.0f));
        Put(Engine.Math.MinInt(7, -3)); Put(Engine.Math.MaxInt(7, -3)); Put(Engine.Math.ClampInt(12, 1, 10));
        return count;
    }
}
