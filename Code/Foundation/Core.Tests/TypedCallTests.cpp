// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The typed entries beside the Variant ones: every reflected method kind (member, const, static,
// each noexcept), every parameter form (a value, a const reference, an enum, a view, a String, an
// object by pointer, reference and RefPtr, a Variant) and every return form (nothing, a value, a
// view returned as a String, an object) is called through `invoke` and through `call` and gives the
// same answer; a method taking a list has no typed call; a constructor and a property have their
// typed pair. Object references are counted, so a typed call leaves none behind.

#include <doctest/doctest.h>

#include <new>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

namespace
{
    class Cell : public Object
    {
        RTTI_OBJECT(Cell, Object)
    public:
        i32 value = 0;
        Cell() = default;
        explicit Cell(i32 v) : value(v) {}
    };

    enum class Shade : i32
    {
        Light = 1,
        Dark = 2,
    };

    struct Probe
    {
        f32 x = 1.5f;
        String label{u8"probe"};

        Probe() = default;
        Probe(f32 startX, String startLabel) : x(startX), label(Move(startLabel)) {}

        f32 Scale(f32 k) { return x *= k; }
        String Describe(StringView prefix) const { return Format(u8"{}{}", prefix, label); }
        void Shift(const Float3& by) noexcept { x += by.x + by.y + by.z; }
        f32 Read() const noexcept { return x; }
        StringView Label() const { return label.AsView(); }
        void SetLabel(StringView text) { label = String(text); }

        static i32 Sum(i32 a, i32 b) { return a + b; }
        static i32 ShadeValue(Shade shade) noexcept { return static_cast<i32>(shade) * 10; }
        static i32 Length(String text) { return static_cast<i32>(text.Size()); }
        static i32 ByPointer(Cell* cell) { return cell != nullptr ? cell->value : -1; }
        static i32 ByReference(Cell& cell) { return cell.value; }
        static i32 ByRef(RefPtr<Cell> cell) { return cell ? cell->value : -1; }
        static i32 Kind(Variant any) { return any.TryGet<i32>() != nullptr ? 1 : 2; }
        static i32 Many(Array<i32> values) { return static_cast<i32>(values.Size()); }
        static RefPtr<Cell> MakeCell(i32 v) { return MakeRef<Cell>(DefaultAllocator(), v); }
        static Float3 Twice(Float3 v) { return Float3{v.x * 2, v.y * 2, v.z * 2}; }
    };
}

RTTI_DEFINE_OBJECT(Cell, "rtti::tests::typed")

REFLECT_ENUM(Shade, "rtti::tests::typed")
{
    builder.Value("Light", Shade::Light);
    builder.Value("Dark", Shade::Dark);
}

REFLECT_VALUE(Probe, "rtti::tests::typed")
{
    builder.Property<&Probe::x>("x")
        .Property<&Probe::label>("label")
        .ComputedProperty<&Probe::Read>("read")
        .AccessorProperty<&Probe::Label, &Probe::SetLabel>("name")
        .Method<&Probe::Scale>("Scale")
        .Method<&Probe::Describe>("Describe", {"prefix"})
        .Method<&Probe::Shift>("Shift")
        .Method<&Probe::Read>("Read")
        .Method<&Probe::Label>("Label")
        .Method<&Probe::Sum>("Sum")
        .Pure()
        .Method<&Probe::ShadeValue>("ShadeValue")
        .Method<&Probe::Length>("Length")
        .Method<&Probe::ByPointer>("ByPointer")
        .Method<&Probe::ByReference>("ByReference")
        .Method<&Probe::ByRef>("ByRef")
        .Method<&Probe::Kind>("Kind")
        .Method<&Probe::Many>("Many")
        .Method<&Probe::MakeCell>("MakeCell")
        .Method<&Probe::Twice>("Twice")
        .Constructor<f32, String>({"x", "label"});
}

namespace
{
    const MethodInfo& Method(const char* name)
    {
        RttiRegisterValue_Probe();
        const MethodInfo* method = FindMethod(TypeOf<Probe>(), name);
        REQUIRE(method != nullptr);
        return *method;
    }

    // Storage for a typed call's result, destroyed as T when it was constructed.
    template <typename T>
    struct Slot
    {
        alignas(T) unsigned char bytes[sizeof(T)];
        void* Pointer() { return bytes; }
        T& Value() { return *std::launder(reinterpret_cast<T*>(bytes)); }
        ~Slot() = default;
    };
}

TEST_CASE("typed call: methods of every kind give through `call` what they give through `invoke`")
{
    // A member, changing its object.
    {
        Probe byVariant;
        Probe byCall;
        Variant k[] = {Variant::From<f32>(2.0f)};
        const f32 fromInvoke = InvokeMethod(Method("Scale"), Instance::From(&byVariant), Span<Variant>{k, 1}).Value().Get<f32>();
        f32 factor = 2.0f;
        void* args[] = {&factor};
        f32 fromCall = 0.0f;
        REQUIRE(Method("Scale").call != nullptr);
        Method("Scale").call(&byCall, args, &fromCall);
        CHECK(fromCall == fromInvoke);
        CHECK(byCall.x == byVariant.x);
    }
    // A const member taking a view and returning a String; a noexcept const member.
    {
        Probe probe;
        Variant prefix[] = {Variant::From<String>(String(u8"a "))};
        const String fromInvoke = InvokeMethod(Method("Describe"), Instance::From(&probe), Span<Variant>{prefix, 1}).Value().Get<String>();
        StringView view = u8"a ";
        void* args[] = {&view};
        CHECK(ParamAt(Method("Describe"), 0).form == ParamForm::View);
        Slot<String> slot;
        Method("Describe").call(&probe, args, slot.Pointer());
        CHECK(slot.Value() == fromInvoke);
        CHECK(slot.Value() == StringView(u8"a probe"));
        slot.Value().~String();

        f32 read = 0.0f;
        Method("Read").call(&probe, nullptr, &read);
        CHECK(read == probe.x);
    }
    // A view returned becomes a String in the caller's storage.
    {
        Probe probe;
        Slot<String> slot;
        Method("Label").call(&probe, nullptr, slot.Pointer());
        CHECK(slot.Value() == StringView(u8"probe"));
        slot.Value().~String();
    }
    // A noexcept member taking a const reference to a value struct, returning nothing.
    {
        Probe probe;
        Float3 by{1.0f, 2.0f, 3.0f};
        void* args[] = {&by};
        Method("Shift").call(&probe, args, nullptr);
        CHECK(probe.x == doctest::Approx(7.5f));
    }
    // Statics: numbers, an enum, a String, a value struct returned.
    {
        i32 a = 2;
        i32 b = 5;
        void* args[] = {&a, &b};
        i32 sum = 0;
        Method("Sum").call(nullptr, args, &sum);
        Variant both[] = {Variant::From<i32>(2), Variant::From<i32>(5)};
        CHECK(sum == InvokeStatic(Method("Sum"), Span<Variant>{both, 2}).Value().Get<i32>());

        Shade shade = Shade::Dark;
        void* shadeArgs[] = {&shade};
        i32 shadeValue = 0;
        Method("ShadeValue").call(nullptr, shadeArgs, &shadeValue);
        CHECK(shadeValue == 20);

        String text(u8"sixteen-chars-ab");
        void* textArgs[] = {&text};
        CHECK(ParamAt(Method("Length"), 0).form == ParamForm::Value);
        i32 length = 0;
        Method("Length").call(nullptr, textArgs, &length);
        CHECK(length == 16);

        Float3 v{1.0f, 2.0f, 3.0f};
        void* vArgs[] = {&v};
        Float3 twice{};
        Method("Twice").call(nullptr, vArgs, &twice);
        CHECK(twice.z == 6.0f);
    }
    // A Variant parameter takes the Variant itself.
    {
        Variant any = Variant::From<i32>(3);
        void* args[] = {&any};
        CHECK(ParamAt(Method("Kind"), 0).form == ParamForm::Variant);
        i32 kind = 0;
        Method("Kind").call(nullptr, args, &kind);
        CHECK(kind == 1);
    }
    // A list has no single native form: no typed call, and its parameter says so.
    CHECK(Method("Many").call == nullptr);
    CHECK(ParamAt(Method("Many"), 0).form == ParamForm::List);
    CHECK(Method("Many").invoke != nullptr);
}

TEST_CASE("typed call: objects pass as pointers and come back with a reference the caller releases")
{
    RefPtr<Cell> cell = MakeRef<Cell>(DefaultAllocator(), 7);
    Cell* raw = cell.Get();
    void* args[] = {&raw};
    const char* const names[] = {"ByPointer", "ByReference", "ByRef"};
    for (const char* name : names)
    {
        CHECK(ParamAt(Method(name), 0).form == ParamForm::Object);
        i32 value = 0;
        Method(name).call(nullptr, args, &value);
        CHECK(value == 7);
    }
    CHECK(cell->RefCount() == 1u); // the RefPtr parameter's reference went with the call

    i32 v = 9;
    void* makeArgs[] = {&v};
    Cell* made = nullptr;
    Method("MakeCell").call(nullptr, makeArgs, &made);
    REQUIRE(made != nullptr);
    CHECK(made->value == 9);
    CHECK(made->RefCount() == 1u); // the caller's reference, and only it
    made->Release();
}

TEST_CASE("typed call: a constructor builds in the caller's storage, and properties read and write typed")
{
    RttiRegisterValue_Probe();
    const ConstructorInfo& constructor = ConstructorAt(TypeOf<Probe>(), 0);
    REQUIRE(constructor.construct != nullptr);
    f32 x = 4.0f;
    String label(u8"made");
    void* args[] = {&x, &label};
    Slot<Probe> slot;
    constructor.construct(args, slot.Pointer());
    CHECK(slot.Value().x == 4.0f);
    CHECK(slot.Value().label == StringView(u8"made"));

    // A field reads and writes its value; a computed property reads through its getter; an
    // accessor writes through its setter, from a view.
    Probe& probe = slot.Value();
    const PropertyInfo* field = FindProperty(TypeOf<Probe>(), "x");
    const PropertyInfo* computed = FindProperty(TypeOf<Probe>(), "read");
    const PropertyInfo* accessor = FindProperty(TypeOf<Probe>(), "name");
    REQUIRE(field != nullptr);
    REQUIRE(computed != nullptr);
    REQUIRE(accessor != nullptr);
    f32 next = 8.0f;
    field->write(&probe, &next);
    f32 read = 0.0f;
    field->read(&probe, &read);
    CHECK(read == 8.0f);
    f32 viaGetter = 0.0f;
    computed->read(&probe, &viaGetter);
    CHECK(viaGetter == 8.0f);
    CHECK(computed->write == nullptr); // read-only
    StringView renamed = u8"renamed";
    accessor->write(&probe, &renamed);
    Slot<String> name;
    accessor->read(&probe, name.Pointer());
    CHECK(name.Value() == StringView(u8"renamed"));
    name.Value().~String();
    probe.~Probe();
}

TEST_CASE("typed call: a method marked pure says so; the core math is marked, the rest not")
{
    CHECK(Method("Sum").pure);
    CHECK_FALSE(Method("Scale").pure);
    RegisterCoreTypes();
    const MethodInfo* cross = FindMethod(TypeOf<Float3>(), "Cross");
    REQUIRE(cross != nullptr);
    CHECK(cross->pure);
    const MethodInfo* rotate = FindMethod(TypeOf<Quaternion>(), "RotateVector");
    REQUIRE(rotate != nullptr);
    CHECK_FALSE(rotate->pure);
}
