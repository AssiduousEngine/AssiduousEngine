// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The generated C# bindings: what the generator writes for the engine's value types, and the probe
// (built with them) calling Float3's reflected methods through the bridge on CoreCLR.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <string>

import foundation.core;
import foundation.script.dotnet;
import foundation.script.facades; // Random, which the probe calls

using namespace foundation::core;
using namespace foundation::script::dotnet;

namespace
{
    String ProbePath(StringView file) { return PathJoin(StringView(reinterpret_cast<const utf8char*>(PROBE_DIR)), file); }
}

TEST_CASE("dotnet bindings: value types are found and written with their measured layout and methods")
{
    RegisterCoreTypes();
    const Array<const TypeInfo*> types = BindableValueTypes();
    bool hasFloat3 = false;
    for (const TypeInfo* type : types)
        hasFloat3 = hasFloat3 || type->id == TypeOf<Float3>().id;
    REQUIRE(hasFloat3);
    const String source = GenerateCSharpBindings(Span<const TypeInfo* const>{types.Data(), types.Size()});
    CHECK(source.AsView().Contains(u8"[StructLayout(LayoutKind.Explicit, Size = 12)]\npublic unsafe partial struct Float3"));
    CHECK(source.AsView().Contains(u8"[FieldOffset(4)] public float y;"));
    CHECK(source.AsView().Contains(u8"public static float Dot("));
    CHECK(source.AsView().Contains(u8"public static Float3 Cross(Float3 a, Float3 b)"));
    CHECK(source.AsView().Contains(u8"if (Float3.EngineType != 0 && sizeof(Float3) != Bridge.TypeSize(Float3.EngineType)) return \"Float3 size\";"));
}

TEST_CASE("dotnet bindings: C# calls Float3's reflected methods through the generated bindings")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    DotNetHost host;
    const Status started = host.Start(ProbePath(u8"Probe.runtimeconfig.json").AsView());
    INFO(std::string(reinterpret_cast<const char*>(host.Failure().Data()), host.Failure().Size()));
    REQUIRE(started.IsOk());
    auto* run = reinterpret_cast<int (*)(const DotNetEngineFunctions*)>(
        host.EntryPoint(ProbePath(u8"Probe.dll").AsView(), u8"Probe.UsesBindings, Probe", u8"Run"));
    INFO(std::string(reinterpret_cast<const char*>(host.Failure().Data()), host.Failure().Size()));
    REQUIRE(run != nullptr);
    // 32000 for Dot (32), then Cross (-3, 6, -3) as 700 + 60 + 7. -1: the table's layout refused;
    // -2: a struct's layout differs; -3: a call failed.
    const int got = run(&EngineFunctions(DefaultAllocator()));
    u64 length = 0;
    const utf8char* why = EngineFunctions(DefaultAllocator()).lastError(&length);
    INFO(std::string(reinterpret_cast<const char*>(why), static_cast<usize>(length)));
    CHECK(got == 32767);
}

// An engine value type held whole (no fields of its own in C#, CurveKey here) is written with
// fields that cover its bytes, so every compiler copies them: one with only a declared size lost an
// Entity's bytes in the web's ahead-of-time build.
TEST_CASE("dotnet bindings: an opaque value type has fields covering its bytes")
{
    RegisterCoreTypes();
    const Array<const TypeInfo*> types = GlobalTypeRegistry().All();
    const String source = GenerateCSharpBindings(Span<const TypeInfo* const>{types.Data(), types.Size()});
    const StringView key = u8"[StructLayout(LayoutKind.Sequential, Size = 20)]\npublic unsafe partial struct CurveKey\n{\n";
    const usize at = source.AsView().Find(key);
    REQUIRE(at != StringView::kNotFound);
    const StringView body = source.AsView().SubStr(at + key.Size(), 300);
    CHECK(body.Contains(u8"\n    uint m_held0;\n    uint m_held1;\n    uint m_held2;\n    uint m_held3;\n    uint m_held4;\n"));
}

// Every pure function C#'s bindings compute in C# (ValueMath) gives what the engine's gives, on the
// same inputs; and every method marked pure has an entry here, so a new one is not left unchecked.
// The order is the probe's (Probe.cs, PureValues).
TEST_CASE("dotnet bindings: the pure functions C# computes itself match the engine's")
{
    RegisterCoreTypes();
    usize pureMethods = 0;
    for (const TypeInfo* type : GlobalTypeRegistry().All())
        for (u32 m = 0; m < type->methodCount; ++m)
            pureMethods += type->methods[m].pure ? 1u : 0u;

    Array<f32> expected;
    usize entries = 0;
    const auto put = [&expected](f32 value) { expected.PushBack(value); };
    const auto v2 = [&put](Float2 v) { put(v.x); put(v.y); };
    const auto v3 = [&put](Float3 v) { put(v.x); put(v.y); put(v.z); };
    const auto v4 = [&put](Float4 v) { put(v.x); put(v.y); put(v.z); put(v.w); };
    const auto c4 = [&put](Color c) { put(c.r); put(c.g); put(c.b); put(c.a); };
    const auto b1 = [&put](bool b) { put(b ? 1.0f : 0.0f); };
    const Float2 a2{1.5f, -2.0f}, b2{-4.0f, 0.5f};
    const Float3 a{1.5f, -2.0f, 3.25f}, b{-4.0f, 0.5f, 2.0f};
    const Float4 a4{1.5f, -2.0f, 3.25f, 0.5f}, b4{-4.0f, 0.5f, 2.0f, -1.0f};
    const Color ca{0.25f, 0.5f, 0.75f, 1.0f}, cb{0.5f, 0.25f, 0.125f, 0.5f};
    const f32 s = 0.75f, x = 0.6f, y = -1.25f;

    v2(a2 + b2); v2(a2 - b2); v2(a2 * b2); v2(a2 * s); v2(a2 / s); v2(-a2); b1(a2 == b2); entries += 7;
    put(Dot(a, b)); v3(Cross(a, b)); put(Length(a)); put(LengthSquared(a)); put(Distance(a, b)); v3(Normalized(a));
    v3(Lerp(a, b, s)); v3(a + b); v3(a - b); v3(a * b); v3(a * s); v3(a / b); v3(a / s); v3(-a); b1(a == a); entries += 15;
    v3(a4.XYZ()); v4(a4 + b4); v4(a4 - b4); v4(a4 * s); v4(-a4); b1(a4 == b4); entries += 6;
    c4(ca + cb); c4(ca * s); b1(ca == ca); entries += 3;
    put(Abs(y)); put(Sqrt(s)); put(Sin(x)); put(Cos(x)); put(Tan(x)); put(Asin(x)); put(Acos(x)); put(Atan2(y, x));
    put(Floor(y)); put(Ceil(y)); put(Round(2.5f)); put(Pow(s, x)); put(Log(s)); put(Exp(x)); put(DegreesToRadians(45.0f));
    put(RadiansToDegrees(x)); put(Lerp(x, y, s)); put(Min(x, y)); put(Max(x, y)); put(Clamp(y, -1.0f, 1.0f)); entries += 20;
    put(static_cast<f32>(Min(7, -3))); put(static_cast<f32>(Max(7, -3))); put(static_cast<f32>(Clamp(12, 1, 10))); entries += 3;
    CHECK(entries == pureMethods); // a pure method with no entry here is not checked

    DotNetHost host;
    REQUIRE(host.Start(ProbePath(u8"Probe.runtimeconfig.json").AsView()).IsOk());
    auto* run = reinterpret_cast<int (*)(f32*, int)>(host.EntryPoint(ProbePath(u8"Probe.dll").AsView(), u8"Probe.PureValues, Probe", u8"Run"));
    REQUIRE(run != nullptr);
    Array<f32> got;
    got.Resize(expected.Size());
    REQUIRE(run(got.Data(), static_cast<int>(got.Size())) == static_cast<int>(expected.Size()));
    for (usize i = 0; i < expected.Size(); ++i)
    {
        INFO("value " << i);
        CHECK(got[i] == doctest::Approx(expected[i]).epsilon(1e-6));
    }
}

// A method marked pure runs in C# with no crossing; one not marked (a quaternion's product) still
// calls the engine; and the operators a value type implements are C#'s operators.
TEST_CASE("dotnet bindings: pure methods are computed in C#, operators are C#'s")
{
    RegisterCoreTypes();
    const Array<const TypeInfo*> all = GlobalTypeRegistry().All();
    const String source = GenerateCSharpBindings(Span<const TypeInfo* const>{all.Data(), all.Size()});
    CHECK(source.AsView().Contains(u8"public static Float3 Cross(Float3 a, Float3 b) => ValueMath.Float3Cross(a, b);"));
    CHECK(source.AsView().Contains(u8"public Float3 XYZ() => ValueMath.Float4XYZ(this);"));
    CHECK(source.AsView().Contains(u8"public static Float3 operator +(Float3 a, Float3 b) => Add(a, b);"));
    CHECK(source.AsView().Contains(u8"public static Float3 operator -(Float3 v) => Neg(v);"));
    CHECK_FALSE(source.AsView().Contains(u8"operator ==("));
    // Quaternion's product is not marked pure: it still goes through the bridge.
    const usize quaternion = source.AsView().Find(StringView(u8"partial struct Quaternion"));
    REQUIRE(quaternion != StringView::kNotFound);
    const StringView after = source.AsView().SubStr(quaternion, source.Size() - quaternion);
    CHECK(after.SubStr(0, after.Find(StringView(u8"\n}\n"))).Contains(u8"Bridge.Invoke("));
}
