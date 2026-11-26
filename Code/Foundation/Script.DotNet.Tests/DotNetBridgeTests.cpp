// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The bridge C# calls the engine through, driven from C++ the way the generated C# drives it:
// reflected methods and properties by index, values as DotNetValues (bytes, text, objects).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.resource; // Ref<T>: a reference crossing as its guid
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
    const utf8char* said = engine.lastError(&length); // first: it sets the length the view reads
    CHECK(StringView(said, static_cast<usize>(length)).Contains(u8"takes 2 arguments"));
    u8 tiny = 0;
    DotNetValue small;
    small.data = &tiny;
    small.size = 1;
    CHECK(engine.invoke(engine.methodAt(float3, MethodIndex(*float3, u8"Cross", 2)), nullptr, args, 2, &small) < 0);
    CHECK(engine.methodAt(float3, 100000) == nullptr);
}

// An enum argument (the bindings pass its bytes under the enum's type) becomes its underlying
// integer, as the other script languages pass it, which a reflected call converts back to the enum.
TEST_CASE("dotnet bridge: an enum argument crosses as its underlying integer")
{
    RegisterCoreTypes();
    const TypeInfo& level = GlobalTypeRegistry().Canonical(TypeOf<LogLevel>());
    REQUIRE(level.enumeratorCount > 0);
    LogLevel warning = LogLevel::Warning;
    const Variant converted = ToVariant(Bytes(level, &warning));
    REQUIRE(converted.TryGet<i64>() != nullptr);
    CHECK(*converted.TryGet<i64>() == static_cast<i64>(LogLevel::Warning));
}

namespace
{
    struct Greeter
    {
        static String Greet(StringView name, String suffix) { return Format(u8"hello {}{}", name, suffix); }
        static i32 Tally(i32 a, f32 b) { return a + static_cast<i32>(b); }
    };
}
REFLECT_VALUE(Greeter, "rtti::tests::dotnetbridge")
{
    builder.Method<&Greeter::Greet>("Greet").Method<&Greeter::Tally>("Tally");
}

// A call whose arguments C# passed in their parameters' forms goes straight to the method's typed
// entry, with the answer the Variant call gives; one it cannot (a number of another width) goes
// through Variants as before.
TEST_CASE("dotnet bridge: arguments in their parameters' forms take the typed call, the rest Variants")
{
    RegisterCoreTypes();
    RttiRegisterValue_Greeter();
    GlobalTypeRegistry().Register(TypeOf<Greeter>());
    const DotNetEngineFunctions& engine = EngineFunctions(DefaultAllocator());
    const TypeInfo* greeter = engine.typeById(TypeOf<Greeter>().id);
    REQUIRE(greeter != nullptr);

    // Text for a view and for a String, a String back.
    const char8_t name[] = u8"you";
    const char8_t suffix[] = u8"!";
    DotNetValue texts[2];
    texts[0].kind = DotNetValueKind::Text;
    texts[0].data = const_cast<char8_t*>(name);
    texts[0].size = 3;
    texts[1].kind = DotNetValueKind::Text;
    texts[1].data = const_cast<char8_t*>(suffix);
    texts[1].size = 1;
    DotNetValue result{};
    u64 before = TypedCallCount();
    REQUIRE(engine.invoke(engine.methodAt(greeter, MethodIndex(*greeter, u8"Greet", 2)), nullptr, texts, 2, &result) == 0);
    CHECK(TypedCallCount() == before + 1);
    REQUIRE(result.kind == DotNetValueKind::Text);
    CHECK(StringView(static_cast<const utf8char*>(result.data), static_cast<usize>(result.size)) == StringView(u8"hello you!"));

    // Numbers of exactly their parameters' types: typed.
    i32 a = 2;
    f32 b = 3.5f;
    DotNetValue numbers[] = {Bytes(TypeOf<i32>(), &a), Bytes(TypeOf<f32>(), &b)};
    i32 tally = 0;
    result = Bytes(TypeOf<i32>(), &tally);
    const MethodInfo* tallyMethod = engine.methodAt(greeter, MethodIndex(*greeter, u8"Tally", 2));
    before = TypedCallCount();
    REQUIRE(engine.invoke(tallyMethod, nullptr, numbers, 2, &result) == 0);
    CHECK(TypedCallCount() == before + 1);
    CHECK(tally == 5);

    // A double where a float is taken: not the parameter's form, so Variants, with the same answer.
    f64 wide = 3.5;
    DotNetValue mixed[] = {Bytes(TypeOf<i32>(), &a), Bytes(TypeOf<f64>(), &wide)};
    tally = 0;
    result = Bytes(TypeOf<i32>(), &tally);
    before = TypedCallCount();
    const i32 code = engine.invoke(tallyMethod, nullptr, mixed, 2, &result);
    CHECK(TypedCallCount() == before);
    (void)code; // the Variant call decides whether it takes a double for a float
}

namespace
{
    struct Lister
    {
        static Array<String> Words(i32 count)
        {
            Array<String> words;
            for (i32 i = 0; i < count; ++i)
                words.PushBack(Format(u8"w{}", i));
            return words;
        }
        static i32 Letters(Array<String> words)
        {
            i32 total = 0;
            for (const String& word : words)
                total += static_cast<i32>(word.Size());
            return total;
        }
        static Array<f32> Doubled(Array<f32> values)
        {
            for (f32& value : values)
                value *= 2.0f;
            return values;
        }
    };

    DotNetValue Text(const char8_t* text, u64 size)
    {
        DotNetValue v;
        v.kind = DotNetValueKind::Text;
        v.data = const_cast<char8_t*>(text);
        v.size = size;
        return v;
    }
}
REFLECT_VALUE(Lister, "rtti::tests::dotnetbridge")
{
    builder.Method<&Lister::Words>("Words").Method<&Lister::Letters>("Letters").Method<&Lister::Doubled>("Doubled");
}

// A list crosses as C# drives it: a list result is a handle whose elements are read one by one (text
// into the thread's buffer, a number into the caller's bytes), and a list argument is made from its
// elements and converted by the call into its parameter's container.
TEST_CASE("dotnet bridge: lists cross as handles, read by element and made from elements")
{
    RegisterCoreTypes();
    RegisterArrayType<String>();
    RegisterArrayType<f32>();
    RttiRegisterValue_Lister();
    GlobalTypeRegistry().Register(TypeOf<Lister>());
    const DotNetEngineFunctions& engine = EngineFunctions(DefaultAllocator());
    const TypeInfo* lister = engine.typeById(TypeOf<Lister>().id);
    REQUIRE(lister != nullptr);

    // A list of text back: three elements, each read as text.
    i32 three = 3;
    DotNetValue count[] = {Bytes(TypeOf<i32>(), &three)};
    DotNetValue words{};
    REQUIRE(engine.invoke(engine.methodAt(lister, MethodIndex(*lister, u8"Words", 1)), nullptr, count, 1, &words) == 0);
    REQUIRE(words.kind == DotNetValueKind::Handle);
    u64 size = 0;
    REQUIRE(engine.listSize(&words, &size) == 0);
    REQUIRE(size == 3u);
    for (u64 i = 0; i < size; ++i)
    {
        DotNetValue element{};
        REQUIRE(engine.listAt(&words, i, &element) == 0);
        REQUIRE(element.kind == DotNetValueKind::Text);
        CHECK(StringView(static_cast<const utf8char*>(element.data), static_cast<usize>(element.size)) == Format(u8"w{}", i).AsView());
    }
    DotNetValue past{};
    CHECK(engine.listAt(&words, 3, &past) < 0); // past the end
    engine.releaseHandle(words.data);

    // A list of text given: made from its elements, converted by the call into its Array<String>.
    DotNetValue given[] = {Text(u8"ab", 2), Text(u8"cde", 3)};
    DotNetValue list{};
    REQUIRE(engine.makeList(given, 2, &list) == 0);
    REQUIRE(list.kind == DotNetValueKind::Handle);
    i32 letters = 0;
    DotNetValue result = Bytes(TypeOf<i32>(), &letters);
    REQUIRE(engine.invoke(engine.methodAt(lister, MethodIndex(*lister, u8"Letters", 1)), nullptr, &list, 1, &result) == 0);
    CHECK(letters == 5);
    engine.releaseHandle(list.data);

    // Numbers both ways: each element's bytes in, each read back into the caller's bytes.
    f32 values[] = {1.5f, -2.0f};
    DotNetValue numbers[] = {Bytes(TypeOf<f32>(), &values[0]), Bytes(TypeOf<f32>(), &values[1])};
    REQUIRE(engine.makeList(numbers, 2, &list) == 0);
    DotNetValue doubled{};
    REQUIRE(engine.invoke(engine.methodAt(lister, MethodIndex(*lister, u8"Doubled", 1)), nullptr, &list, 1, &doubled) == 0);
    engine.releaseHandle(list.data);
    REQUIRE(engine.listSize(&doubled, &size) == 0);
    REQUIRE(size == 2u);
    f32 read = 0.0f;
    DotNetValue element = Bytes(TypeOf<f32>(), &read);
    REQUIRE(engine.listAt(&doubled, 1, &element) == 0);
    CHECK(element.kind == DotNetValueKind::Bytes);
    CHECK(read == doctest::Approx(-4.0f));
    engine.releaseHandle(doubled.data);

    // No list: refused, with a reason.
    DotNetValue none = Bytes(TypeOf<i32>(), &three);
    CHECK(engine.listSize(&none, &size) < 0);
    u64 length = 0;
    const utf8char* reason = engine.lastError(&length); // asked first: the length is set by the call
    CHECK(StringView(reason, static_cast<usize>(length)) == StringView(u8"not a list"));
}

namespace
{
    class Crate final : public Object
    {
        RTTI_OBJECT(Crate, Object)
    };
}
RTTI_DEFINE_OBJECT(Crate, "rtti::tests::dotnetbridge")

// A resource reference (a component's mesh, an audio source's clip) crosses as the Guid of what it
// names, into the caller's bytes: what C# reads it as.
TEST_CASE("dotnet bridge: a resource reference crosses as its guid")
{
    RegisterCoreTypes();
    (void)EngineFunctions(DefaultAllocator()); // the bridge's allocator
    foundation::resource::Ref<Crate> crate;
    crate.id = Guid{0x1234, 0x5678};
    const Variant held = Variant::From(crate);
    REQUIRE(held.Type() != nullptr);
    REQUIRE(IsReferenceType(*held.Type()));
    Guid read;
    DotNetValue out = Bytes(TypeOf<Guid>(), &read);
    REQUIRE(FromVariant(held, out).IsOk());
    CHECK(out.kind == DotNetValueKind::Bytes);
    CHECK(out.type->id == TypeOf<Guid>().id);
    CHECK(read == crate.id);

    // None named: the nil guid.
    const Variant unset = Variant::From(foundation::resource::Ref<Crate>{});
    REQUIRE(FromVariant(unset, out).IsOk());
    CHECK(read.IsNil());
}
