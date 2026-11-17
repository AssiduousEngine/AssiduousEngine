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
