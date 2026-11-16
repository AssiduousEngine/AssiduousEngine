// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The bridge C# calls the engine through, driven from C++ the way the generated C# drives it:
// reflected methods and properties by index, values as DotNetValues (bytes, text, objects).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script.dotnet;

using namespace foundation::core;
using namespace foundation::script::dotnet;

namespace
{
    u32 MethodIndex(const TypeInfo& type, StringView name, u32 params)
    {
        for (u32 i = 0; i < type.methodCount; ++i)
            if (StringView(reinterpret_cast<const utf8char*>(type.methods[i].name)) == name && type.methods[i].paramCount == params)
                return i;
        return ~0u;
    }
    DotNetValue Bytes(const TypeInfo& type, void* data)
    {
        DotNetValue v;
        v.type = &type;
        v.data = data;
        v.size = type.size;
        v.kind = DotNetValueKind::Bytes;
        return v;
    }
}

TEST_CASE("dotnet bridge: reflected methods and properties called through the table, values as bytes")
{
    RegisterCoreTypes();
    const DotNetEngineFunctions& engine = EngineFunctions(DefaultAllocator());
    CHECK(engine.size == sizeof(DotNetEngineFunctions));
    const TypeInfo* float3 = engine.typeById(TypeOf<Float3>().id);
    REQUIRE(float3 == &GlobalTypeRegistry().Canonical(TypeOf<Float3>()));

    Float3 a{1.0f, 2.0f, 3.0f};
    Float3 b{4.0f, 5.0f, 6.0f};
    DotNetValue args[] = {Bytes(*float3, &a), Bytes(*float3, &b)};

    // Dot: a float back, into the caller's bytes.
    f32 dot = 0.0f;
    DotNetValue result = Bytes(TypeOf<f32>(), &dot);
    const MethodInfo* dotMethod = engine.methodAt(float3, MethodIndex(*float3, u8"Dot", 2));
    REQUIRE(dotMethod != nullptr);
    REQUIRE(engine.invoke(dotMethod, nullptr, args, 2, &result) == 0);
    CHECK(result.kind == DotNetValueKind::Bytes);
    CHECK(dot == doctest::Approx(32.0f));

    // Cross: a Float3 back.
    Float3 cross{};
    result = Bytes(*float3, &cross);
    REQUIRE(engine.invoke(engine.methodAt(float3, MethodIndex(*float3, u8"Cross", 2)), nullptr, args, 2, &result) == 0);
    CHECK(cross.x == doctest::Approx(-3.0f));
    CHECK(cross.y == doctest::Approx(6.0f));
    CHECK(cross.z == doctest::Approx(-3.0f));

    // A property of a value, read in place.
    f32 y = 0.0f;
    result = Bytes(TypeOf<f32>(), &y);
    DotNetValue self = Bytes(*float3, &a);
    u32 yIndex = ~0u;
    for (u32 i = 0; i < float3->propertyCount; ++i)
        if (StringView(reinterpret_cast<const utf8char*>(float3->properties[i].name)) == StringView(u8"y"))
            yIndex = i;
    REQUIRE(engine.getProperty(engine.propertyAt(float3, yIndex), &self, &result) == 0);
    CHECK(y == doctest::Approx(2.0f));

    // A wrong argument count is refused, saying so; a result too small for the value likewise.
    CHECK(engine.invoke(dotMethod, nullptr, args, 1, &result) < 0);
    u64 length = 0;
    CHECK(StringView(engine.lastError(&length), static_cast<usize>(length)).Contains(u8"takes 2 arguments"));
    u8 tiny = 0;
    DotNetValue small;
    small.data = &tiny;
    small.size = 1;
    CHECK(engine.invoke(engine.methodAt(float3, MethodIndex(*float3, u8"Cross", 2)), nullptr, args, 2, &small) < 0);
    CHECK(engine.methodAt(float3, 100000) == nullptr);
}
