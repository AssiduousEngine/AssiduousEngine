// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The engine's pure value functions written in C#: what a reflected method marked Pure computes
// (vector and colour arithmetic, the scalar Math), so the generated bindings run it here instead of
// crossing into the engine for a few instructions. Each is named <Type><method as C# names it> and
// matches the C++ it stands for (Core/Math): the generator calls one for every pure method, so a
// missing one fails this assembly's build.

using System;
using System.Runtime.CompilerServices;

namespace Engine;

public static class ValueMath
{
    const float kEpsilon = 1.0e-6f; // Core's kEpsilon: a shorter vector than this normalizes to zero
    const float kDegToRad = MathF.PI / 180.0f;
    const float kRadToDeg = 180.0f / MathF.PI;

    static Float2 F2(float x, float y) => new Float2 { x = x, y = y };
    static Float3 F3(float x, float y, float z) => new Float3 { x = x, y = y, z = z };
    static Float4 F4(float x, float y, float z, float w) => new Float4 { x = x, y = y, z = z, w = w };
    static Color C(float r, float g, float b, float a) => new Color { r = r, g = g, b = b, a = a };

    // Float2
    public static Float2 Float2Add(Float2 a, Float2 b) => F2(a.x + b.x, a.y + b.y);
    public static Float2 Float2Sub(Float2 a, Float2 b) => F2(a.x - b.x, a.y - b.y);
    public static Float2 Float2Mul(Float2 a, Float2 b) => F2(a.x * b.x, a.y * b.y);
    public static Float2 Float2MulScalar(Float2 v, float s) => F2(v.x * s, v.y * s);
    public static Float2 Float2DivScalar(Float2 v, float s) => F2(v.x / s, v.y / s);
    public static Float2 Float2Neg(Float2 v) => F2(-v.x, -v.y);
    public static bool Float2Equals(Float2 a, Float2 b) => a.x == b.x && a.y == b.y;

    // Float3
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Float3Dot(Float3 a, Float3 b) => a.x * b.x + a.y * b.y + a.z * b.z;
    public static Float3 Float3Cross(Float3 a, Float3 b) =>
        F3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    public static float Float3LengthSquared(Float3 v) => Float3Dot(v, v);
    public static float Float3Length(Float3 v) => MathF.Sqrt(Float3Dot(v, v));
    public static float Float3Distance(Float3 a, Float3 b) => Float3Length(Float3Sub(b, a));
    public static Float3 Float3Normalized(Float3 v)
    {
        float lengthSq = Float3Dot(v, v);
        if (lengthSq <= kEpsilon * kEpsilon)
            return default;
        float length = MathF.Sqrt(lengthSq);
        return F3(v.x / length, v.y / length, v.z / length);
    }
    public static Float3 Float3Lerp(Float3 a, Float3 b, float t) =>
        F3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    public static Float3 Float3Add(Float3 a, Float3 b) => F3(a.x + b.x, a.y + b.y, a.z + b.z);
    public static Float3 Float3Sub(Float3 a, Float3 b) => F3(a.x - b.x, a.y - b.y, a.z - b.z);
    public static Float3 Float3Mul(Float3 a, Float3 b) => F3(a.x * b.x, a.y * b.y, a.z * b.z);
    public static Float3 Float3MulScalar(Float3 v, float s) => F3(v.x * s, v.y * s, v.z * s);
    public static Float3 Float3Div(Float3 a, Float3 b) => F3(a.x / b.x, a.y / b.y, a.z / b.z);
    public static Float3 Float3DivScalar(Float3 v, float s) => F3(v.x / s, v.y / s, v.z / s);
    public static Float3 Float3Neg(Float3 v) => F3(-v.x, -v.y, -v.z);
    public static bool Float3Equals(Float3 a, Float3 b) => a.x == b.x && a.y == b.y && a.z == b.z;

    // Float4
    public static Float3 Float4XYZ(Float4 v) => F3(v.x, v.y, v.z);
    public static Float4 Float4Add(Float4 a, Float4 b) => F4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
    public static Float4 Float4Sub(Float4 a, Float4 b) => F4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
    public static Float4 Float4MulScalar(Float4 v, float s) => F4(v.x * s, v.y * s, v.z * s, v.w * s);
    public static Float4 Float4Neg(Float4 v) => F4(-v.x, -v.y, -v.z, -v.w);
    public static bool Float4Equals(Float4 a, Float4 b) => a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;

    // Color
    public static Color ColorAdd(Color a, Color b) => C(a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a);
    public static Color ColorMulScalar(Color c, float s) => C(c.r * s, c.g * s, c.b * s, c.a * s);
    public static bool ColorEquals(Color a, Color b) => a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;

    // Math
    public static float MathAbs(float x) => x < 0.0f ? -x : x;
    public static float MathSqrt(float x) => MathF.Sqrt(x);
    public static float MathSin(float x) => MathF.Sin(x);
    public static float MathCos(float x) => MathF.Cos(x);
    public static float MathTan(float x) => MathF.Tan(x);
    public static float MathAsin(float x) => MathF.Asin(x);
    public static float MathAcos(float x) => MathF.Acos(x);
    public static float MathAtan2(float y, float x) => MathF.Atan2(y, x);
    public static float MathFloor(float x) => MathF.Floor(x);
    public static float MathCeil(float x) => MathF.Ceiling(x);
    public static float MathRound(float x) => MathF.Round(x, MidpointRounding.AwayFromZero); // as std::round
    public static float MathPow(float b, float e) => MathF.Pow(b, e);
    public static float MathLog(float x) => MathF.Log(x);
    public static float MathExp(float x) => MathF.Exp(x);
    public static float MathDegreesToRadians(float degrees) => degrees * kDegToRad;
    public static float MathRadiansToDegrees(float radians) => radians * kRadToDeg;
    public static float MathLerp(float a, float b, float t) => a + (b - a) * t;
    public static float MathMin(float a, float b) => b < a ? b : a;
    public static float MathMax(float a, float b) => a < b ? b : a;
    public static float MathClamp(float x, float lo, float hi) => x < lo ? lo : (hi < x ? hi : x);
}
