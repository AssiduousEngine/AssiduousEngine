// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.AngelScript tests: the reflected-type emission suite in
// AngelScript syntax (the battery certifies the context contract; emission is
// certified per backend), the shared conformance battery, and the backend-registry
// integration (both languages resolving side by side).
//
// AngelScript has no top-level statements, so scripts follow the backend's load
// convention: module globals and a `void main()` that Load runs after Build.

#include <doctest/doctest.h>

#include <cstring> // std::strcmp (alias-name assertions)

#include <angelscript.h> // the engine's module count, through AngelScriptEngineHandle

#include "Core/Prelude.h" // <new> reachability for reflection containers (GCC)
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.script;
import angelscript.foundation;
#ifdef OPTION_HAS_LUAU
import luau.foundation;
#endif

using namespace foundation::core;
using namespace foundation::script;

// A reflected Object-derived type to exercise object classes in AngelScript.
namespace
{
    class Widget : public Object
    {
        RTTI_OBJECT(Widget, Object)
    public:
        int id = 0;
        int serial = 7; // published read-only: a script reads it, never assigns it
        int doubled() const { return id * 2; }
        int idOf(Widget* other) const { return other != nullptr ? other->id : -1; }
        // A Variant sink (`?&in`): whatever arrives, read as a Widget when it is one.
        int idOfAny(Variant value) const
        {
            Widget* other = value.AsObject<Widget>();
            return other != nullptr ? other->id : -1;
        }
    };
}

REFLECT_MEMBERS(Widget, "rtti::script::test")
{
    builder.Property<&Widget::id>("id");
    builder.Property<&Widget::serial>("serial", PropertyFlags::ReadOnly);
    builder.Method<&Widget::doubled>("doubled");
    builder.Method<&Widget::idOf>("idOf");
    builder.Method<&Widget::idOfAny>("idOfAny");
    builder.Constructor();
}

// Exercises natural-typed numeric facades: a 64-bit integer argument and return must round-trip
// exactly, not funnel through double (which would corrupt values above 2^53). 9007199254740993
// is 2^53 + 1, the smallest integer a double cannot represent, so it tells integer-exact
// marshalling apart from marshalling through double.
namespace
{
    class NumProbe : public Object
    {
        RTTI_OBJECT(NumProbe, Object)
    public:
        i64 lastI64 = 0;
        i32 lastI32 = 0;
        void takeI64(i64 v) { lastI64 = v; }
        void takeI32(i32 v) { lastI32 = v; }
        bool argWasExact() const { return lastI64 == 9007199254740993LL; } // 2^53 + 1
        i32 echoI32() const { return lastI32; }
        i64 bigConst() const { return 9007199254740993LL; } // 2^53 + 1
        // A 9-arg method, more than 8 marshalled args. The trailing args must not be truncated;
        // facades like DebugDraw.line take 9 (6 coordinates and 3 color components).
        f32 lastSum9 = 0.0f;
        void take9(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f, f32 g, f32 h, f32 i)
        {
            lastSum9 = a + b + c + d + e + f + g + h + i;
        }
        [[nodiscard]] f32 sum9() const { return lastSum9; }
        // A 16-arg method: the top of the marshalling cap (kMaxArgs = 16). The last arg is
        // the one a cap-boundary off-by-one would drop.
        f32 lastSum16 = 0.0f;
        void take16(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f, f32 g, f32 h, f32 i, f32 j, f32 k,
                    f32 l, f32 m, f32 n, f32 o, f32 p)
        {
            lastSum16 = a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p;
        }
        [[nodiscard]] f32 sum16() const { return lastSum16; }
    };
}

REFLECT_MEMBERS(NumProbe, "rtti::script::test")
{
    builder.Method<&NumProbe::takeI64>("takeI64");
    builder.Method<&NumProbe::takeI32>("takeI32");
    builder.Method<&NumProbe::argWasExact>("argWasExact");
    builder.Method<&NumProbe::echoI32>("echoI32");
    builder.Method<&NumProbe::bigConst>("bigConst");
    builder.Method<&NumProbe::take9>("take9",
                                     {"a", "b", "c", "d", "e", "f", "g", "h", "i"});
    builder.Method<&NumProbe::sum9>("sum9");
    builder.Method<&NumProbe::take16>("take16", {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
                                                 "k", "l", "m", "n", "o", "p"});
    builder.Method<&NumProbe::sum16>("sum16");
    builder.Constructor();
}

// The scriptName alias: a type declares a "scriptName" class attribute and binds to scripts under
// that name, not its C++ class name. AliasProbe binds as `aka`; AliasClash also claims `aka` (to
// exercise the collision guard). The native type name and registry lookup stay on the C++ name.
namespace
{
    class AliasProbe : public Object
    {
        RTTI_OBJECT(AliasProbe, Object)
    public:
        static i32 answer() { return 42; }
    };
    class AliasClash : public Object
    {
        RTTI_OBJECT(AliasClash, Object)
    public:
    };
}

REFLECT_MEMBERS(AliasProbe, "rtti::script::test")
{
    builder.Attribute("scriptName", "aka"); // bind to scripts as `aka`, not `AliasProbe`
    builder.Method<&AliasProbe::answer>("answer");
    builder.Constructor();
}
REFLECT_MEMBERS(AliasClash, "rtti::script::test")
{
    builder.Attribute("scriptName", "aka"); // the same script name as AliasProbe, to collide
    builder.Constructor();
}

TEST_CASE("script: ScriptTypeName resolves the alias; FindScriptTypeNameCollision guards duplicates")
{
    // ScriptTypeName: the "scriptName" alias when set, else the C++ name.
    CHECK(std::strcmp(ScriptTypeName(AliasProbe::StaticType()), "aka") == 0);
    CHECK(std::strcmp(ScriptTypeName(Widget::StaticType()), "Widget") == 0); // no alias: the C++ name

    // Native identity is untouched: the C++ type name (serialization, registry FindByName) is unaliased.
    CHECK(std::strcmp(AliasProbe::StaticType().name, "AliasProbe") == 0);

    // A clean set: distinct script names, no collision.
    const TypeInfo* clean[] = {&AliasProbe::StaticType(), &Widget::StaticType()};
    CHECK(FindScriptTypeNameCollision(Span<const TypeInfo* const>{clean, 2}) == nullptr);

    // A colliding set: two types bind to the same script name `aka`, reported (FinalizeTypes traps).
    const TypeInfo* clash[] = {&AliasProbe::StaticType(), &AliasClash::StaticType()};
    const char* hit = FindScriptTypeNameCollision(Span<const TypeInfo* const>{clash, 2});
    REQUIRE(hit != nullptr);
    CHECK(std::strcmp(hit, "aka") == 0);
}

TEST_CASE("angelscript: a scriptName alias binds the class under the alias, not the C++ name")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(AliasProbe::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    const Status ok = scriptContext->Load(u8"double Got = 0;\n"
                                u8"void main() { Got = double(aka::answer()); }\n", // the alias
                                u8"m");
    REQUIRE(ok.IsOk());
    CHECK(scriptContext->GetGlobal(u8"Got").Get<f64>() == 42.0);

    // The C++ name is not a script type (the alias replaces it on the surface): referencing it fails.
    RefPtr<IScriptContext> ctx2 = manager->CreateContext();
    const Status bad =
        ctx2->Load(u8"void main() { AliasProbe::answer(); }\n", u8"m2"); // the C++ name is undeclared
    CHECK_FALSE(bad.IsOk());
}

// A reflected Object with a property but no reflected constructor, plus a factory that returns one:
// AngelScript declares and binds a type's properties independent of any factory, so a
// constructor-less handle handed back from a facade method is fully usable.
namespace
{
    class Leaf : public Object
    {
        RTTI_OBJECT(Leaf, Object)
    public:
        int value = 0;
    };

    class LeafFactory : public Object
    {
        RTTI_OBJECT(LeafFactory, Object)
    public:
        RefPtr<Leaf> make() const { return MakeRef<Leaf>(DefaultAllocator()); }
    };
}

REFLECT_MEMBERS(Leaf, "rtti::script::test")
{
    builder.Property<&Leaf::value>("value"); // no Constructor(), on purpose
}
REFLECT_MEMBERS(LeafFactory, "rtti::script::test")
{
    builder.Method<&LeafFactory::make>("make");
    builder.Constructor();
}

// A polymorphic-container owner (Zoo holds Array<RefPtr<Animal>>); Dog and Cat are the concrete
// elements. Exercises container-member binding (count, at, add-by-name, removeAt) in AngelScript,
// where at/add return the element base handle (Animal@) and the concrete object is boxed inside.
namespace
{
    class Animal : public Object
    {
        RTTI_OBJECT(Animal, Object)
    public:
        int legs = 4;
    };
    class Dog : public Animal
    {
        RTTI_OBJECT(Dog, Animal)
    public:
        int barks = 1;
    };
    class Cat : public Animal
    {
        RTTI_OBJECT(Cat, Animal)
    public:
        int meows = 1;
    };
    RefPtr<Animal> CreateAnimal(const TypeInfo& t)
    {
        if (t.id == Dog::StaticType().id)
        {
            return MakeRef<Dog>(DefaultAllocator());
        }
        if (t.id == Cat::StaticType().id)
        {
            return MakeRef<Cat>(DefaultAllocator());
        }
        return RefPtr<Animal>{};
    }
    bool CanCreateAnimal(const TypeInfo& t)
    {
        return t.id == Dog::StaticType().id || t.id == Cat::StaticType().id;
    }
    class Zoo : public Object
    {
        RTTI_OBJECT(Zoo, Object)
    public:
        Array<RefPtr<Animal>> animals;
    };
}

REFLECT_MEMBERS(Animal, "rtti::script::test")
{
    builder.Property<&Animal::legs>("legs");
}
REFLECT_MEMBERS(Dog, "rtti::script::test")
{
    builder.Property<&Dog::barks>("barks");
}
REFLECT_MEMBERS(Cat, "rtti::script::test")
{
    builder.Property<&Cat::meows>("meows");
}
REFLECT_MEMBERS(Zoo, "rtti::script::test")
{
    builder.Nested<&Zoo::animals>("animals");
    builder.Constructor();
}

namespace
{
    void RegisterZoo(IScriptManager& manager)
    {
        static bool once = [] {
            RegisterPolymorphicArrayType<Animal>(&CreateAnimal, &CanCreateAnimal);
            GlobalTypeRegistry().Register(Animal::StaticType());
            GlobalTypeRegistry().Register(Dog::StaticType());
            GlobalTypeRegistry().Register(Cat::StaticType());
            GlobalTypeRegistry().Register(Zoo::StaticType());
            return true;
        }();
        (void)once;
        manager.RegisterType(Animal::StaticType());
        manager.RegisterType(Dog::StaticType());
        manager.RegisterType(Cat::StaticType());
        manager.RegisterType(Zoo::StaticType());
    }
}

// A non-Object value type reached only by address: House has a nested-value member, Shelf a
// homogeneous Array<UniquePtr<Room>>. Both need borrow-mode handles.
namespace
{
    struct Room
    {
        int size = 0;
    };
    class House : public Object
    {
        RTTI_OBJECT(House, Object)
    public:
        Room room;
        Room spare; // a plain (not nested) value property, as a component's Color is
    };
    class Shelf : public Object
    {
        RTTI_OBJECT(Shelf, Object)
    public:
        Array<UniquePtr<Room>> rooms;
    };
}
REFLECT_VALUE(Room, "rtti::script::test")
{
    builder.Property<&Room::size>("size");
    builder.Constructor();
}
REFLECT_MEMBERS(House, "rtti::script::test")
{
    builder.Nested<&House::room>("room");
    builder.Property<&House::spare>("spare");
    builder.Constructor();
}
REFLECT_MEMBERS(Shelf, "rtti::script::test")
{
    builder.Nested<&Shelf::rooms>("rooms");
    builder.Constructor();
}
namespace
{
    void RegisterHouse(IScriptManager& manager)
    {
        static bool once = [] {
            RttiRegisterValue_Room();
            RegisterUniquePtrArrayType<Room>();
            return true;
        }();
        (void)once;
        manager.RegisterType(TypeOf<Room>());
        manager.RegisterType(House::StaticType());
        manager.RegisterType(Shelf::StaticType());
    }
}

// A facade that returns an engine Array<T> renders as a native `array<T>`:
// a numeric array (scalar element path) and a reflected-value array (Room boxes as a handle element,
// the same flavor as Entity in the physics facades).
namespace
{
    class Bag : public Object
    {
        RTTI_OBJECT(Bag, Object)
    public:
        Array<i32> numbers() const
        {
            Array<i32> out;
            out.PushBack(10);
            out.PushBack(20);
            out.PushBack(30);
            return out;
        }
        Array<Room> rooms() const
        {
            Array<Room> out;
            Room a;
            a.size = 3;
            Room b;
            b.size = 4;
            out.PushBack(a);
            out.PushBack(b);
            return out;
        }
        Array<i32> empty() const { return {}; }
        Array<String> names() const
        {
            Array<String> out;
            out.PushBack(String(u8"ab"));
            out.PushBack(String(u8"cde"));
            return out;
        }
        // Lists handed in by a script (array parameters).
        f32 sum(Array<f32> values) const
        {
            f32 total = 0.0f;
            for (f32 v : values)
            {
                total += v;
            }
            return total;
        }
        i32 letters(Array<String> words) const
        {
            i32 total = 0;
            for (const String& w : words)
            {
                total += static_cast<i32>(w.Size());
            }
            return total;
        }
        Array<f32> doubled(Array<f32> values) const
        {
            for (f32& v : values)
            {
                v *= 2.0f;
            }
            return values;
        }
    };
}
REFLECT_MEMBERS(Bag, "rtti::script::test")
{
    builder.Method<&Bag::numbers>("numbers");
    builder.Method<&Bag::rooms>("rooms");
    builder.Method<&Bag::empty>("empty");
    builder.Method<&Bag::names>("names");
    builder.Method<&Bag::sum>("sum", {"values"});
    builder.Method<&Bag::letters>("letters", {"words"});
    builder.Method<&Bag::doubled>("doubled", {"values"});
    builder.Constructor();
}
namespace
{
    void RegisterBag(IScriptManager& manager)
    {
        static bool once = [] {
            RttiRegisterValue_Room();
            RegisterArrayType<i32>();
            RegisterArrayType<Room>();
            RegisterArrayType<String>();
            RegisterArrayType<f32>();
            return true;
        }();
        (void)once;
        manager.RegisterType(TypeOf<Room>());
        manager.RegisterType(Bag::StaticType());
    }
}

TEST_CASE("angelscript: a facade Array<T> return crosses as a native array<T> (numeric + value element)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterBag(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status = scriptContext->Load(u8"double N; double R; double E; double L;\n"
                                    u8"void main() {\n"
                                    u8"  Bag b;\n"
                                    u8"  array<int>@ ns = b.numbers();\n"
                                    u8"  N = ns.length() + ns[0] + ns[1] + ns[2];\n" // 3 + 60 = 63
                                    u8"  array<Room>@ rs = b.rooms();\n"
                                    u8"  R = rs.length() + rs[0].size + rs[1].size;\n" // 2 + 7 = 9
                                    u8"  E = b.empty().length();\n"                    // 0
                                    u8"  array<string>@ ss = b.names();\n"
                                    u8"  L = ss.length() + ss[0].length() + ss[1].length();\n" // 2+2+3=7
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"N").Get<f64>() == 63.0);
    CHECK(scriptContext->GetGlobal(u8"R").Get<f64>() == 9.0);
    CHECK(scriptContext->GetGlobal(u8"E").Get<f64>() == 0.0);
    CHECK(scriptContext->GetGlobal(u8"L").Get<f64>() == 7.0);
}

TEST_CASE("angelscript: a script array crosses into a facade Array<T> parameter")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterBag(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status = scriptContext->Load(u8"double S; double L; double E; double D;\n"
                                    u8"void main() {\n"
                                    u8"  Bag b;\n"
                                    u8"  array<float> xs = {1.5f, 2.25f, -0.75f};\n"
                                    u8"  S = b.sum(xs);\n"                                // 3.0
                                    u8"  array<string> ws = {\"ab\", \"cde\"};\n"
                                    u8"  L = b.letters(ws);\n"                            // 5
                                    u8"  array<float> none;\n"
                                    u8"  E = b.sum(none);\n"                              // 0
                                    u8"  array<float>@ twice = b.doubled(xs);\n"
                                    u8"  D = twice.length() + twice[0] + twice[2] + xs[0];\n" // 3 + 3 - 1.5 + 1.5 = 6
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"S").Get<f64>() == doctest::Approx(3.0));
    CHECK(scriptContext->GetGlobal(u8"L").Get<f64>() == 5.0);
    CHECK(scriptContext->GetGlobal(u8"E").Get<f64>() == 0.0);
    CHECK(scriptContext->GetGlobal(u8"D").Get<f64>() == doctest::Approx(6.0)); // the script's own array unchanged
}

TEST_CASE("angelscript: a nested plain member reads as a copy and is written whole")
{
    // A plain struct is a value in script, as a C# struct is: reading the member copies it.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterHouse(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status = scriptContext->Load(u8"double S; double T;\n"
                                    u8"void main() {\n"
                                    u8"  House h;\n"
                                    u8"  Room r = h.room;\n" // a copy of the House's Room
                                    u8"  r.size = 7;\n"
                                    u8"  S = h.room.size;\n" // the house's own is untouched
                                    u8"  h.room = r;\n"      // written whole
                                    u8"  T = h.room.size;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"S").Get<f64>() == 0.0);
    CHECK(scriptContext->GetGlobal(u8"T").Get<f64>() == 7.0);
}

TEST_CASE("angelscript: a value property is written whole; a field write through it is refused; a copy out of it stays apart")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterHouse(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    const Status status = scriptContext->Load(u8"double A; double B; double C; double D; double E; double F;\n"
                                    u8"void main() {\n"
                                    u8"  House h;\n"
                                    u8"  Room fresh; fresh.size = 4;\n"
                                    u8"  h.spare = fresh;\n"        // a plain value property, whole
                                    u8"  A = h.spare.size;\n"
                                    u8"  B = h.spare.size;\n"
                                    u8"  Room saved = h.spare;\n"   // a copy, not a view
                                    u8"  fresh.size = 1; h.spare = fresh;\n"
                                    u8"  C = saved.size;\n"
                                    u8"  h.room = fresh;\n"         // a nested member, whole
                                    u8"  D = h.room.size;\n"
                                    u8"  House g;\n"
                                    u8"  fresh.size = 6; g.spare = fresh;\n"
                                    u8"  h.spare = g.spare;\n"      // one member from another
                                    u8"  E = h.spare.size;\n"
                                    u8"  fresh.size = 2; g.spare = fresh;\n"
                                    u8"  F = h.spare.size;\n"       // still its own
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"A").Get<f64>() == 4.0);
    CHECK(scriptContext->GetGlobal(u8"B").Get<f64>() == 4.0);
    CHECK(scriptContext->GetGlobal(u8"C").Get<f64>() == 4.0);
    CHECK(scriptContext->GetGlobal(u8"D").Get<f64>() == 1.0);
    CHECK(scriptContext->GetGlobal(u8"E").Get<f64>() == 6.0);
    CHECK(scriptContext->GetGlobal(u8"F").Get<f64>() == 6.0);

    // A field written through a property read would change a temporary, so it is refused, as C#
    // refuses it: copy, change, write back.
    CHECK_FALSE(scriptContext->Load(u8"void main() { House h; h.spare.size = 9; }\n", u8"field").IsOk());
}

TEST_CASE("angelscript: a plain container element is reached in place by reference; a variable copies it")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterHouse(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status = scriptContext->Load(u8"double C; double Z;\n"
                                    u8"void main() {\n"
                                    u8"  Shelf s;\n"
                                    u8"  s.rooms_add().size = 42;\n" // default-constructs, written in place
                                    u8"  C = s.rooms_count();\n"
                                    u8"  Room copy = s.rooms_at(0);\n" // a variable copies the element
                                    u8"  copy.size = 1;\n"
                                    u8"  Z = s.rooms_at(0).size;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"C").Get<f64>() == 1.0);
    CHECK(scriptContext->GetGlobal(u8"Z").Get<f64>() == 42.0);
}

TEST_CASE("angelscript: a polymorphic container member binds as script ops (count/at/add/removeAt)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterZoo(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status = scriptContext->Load(u8"double C0; double C1; double L; double C2; double C3;\n"
                                    u8"void main() {\n"
                                    u8"  Zoo z;\n"
                                    u8"  C0 = z.animals_count();\n"        // 0
                                    u8"  Animal@ d = z.animals_add(\"Dog\");\n"
                                    u8"  d.legs = 3;\n"
                                    u8"  C1 = z.animals_count();\n"        // 1
                                    u8"  L = z.animals_at(0).legs;\n"      // 3 (write-through)
                                    u8"  z.animals_add(\"Cat\");\n"
                                    u8"  C2 = z.animals_count();\n"        // 2
                                    u8"  z.animals_removeAt(0);\n"
                                    u8"  C3 = z.animals_count();\n"        // 1
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"C0").Get<f64>() == 0.0);
    CHECK(scriptContext->GetGlobal(u8"C1").Get<f64>() == 1.0);
    CHECK(scriptContext->GetGlobal(u8"L").Get<f64>() == 3.0);
    CHECK(scriptContext->GetGlobal(u8"C2").Get<f64>() == 2.0);
    CHECK(scriptContext->GetGlobal(u8"C3").Get<f64>() == 1.0);
}

TEST_CASE("angelscript: a constructor-less reflected type is usable as a returned handle")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Leaf::StaticType());        // no ctor; declared and props bound anyway
    manager->RegisterType(LeafFactory::StaticType()); // hands a Leaf@ back
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status = scriptContext->Load(u8"double V;\n"
                                    u8"void main() {\n"
                                    u8"  LeafFactory f;\n"
                                    u8"  Leaf@ leaf = f.make();\n"
                                    u8"  leaf.value = 7;\n"
                                    u8"  V = leaf.value;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"V").Get<f64>() == 7.0);
}

TEST_CASE("angelscript: reflected value types support value assignment (Float3 p = expr)")
{
    // All reflected types register as asOBJ_REF boxes; without a registered opAssign, `Float3 p = q;`
    // fails with "no appropriate opAssign". The generic opAssign copies the boxed value.
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    const Status status =
        scriptContext->Load(u8"double X; double Y; double Z;\n"
                  u8"void main() {\n"
                  u8"  Float3 a = Float3(1, 2, 3);\n" // copy-init from a factory return (opAssign)
                  u8"  Float3 b = a;\n"               // value assignment from another local
                  u8"  X = b.x;\n"
                  u8"  b = Float3(7, 8, 9);\n" // re-assign; must not alias a
                  u8"  Y = b.y;\n"
                  u8"  Z = a.x;\n" // a stays 1 (value copy, not a shared handle)
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"X").Get<f64>() == 1.0);
    CHECK(scriptContext->GetGlobal(u8"Y").Get<f64>() == 8.0);
    CHECK(scriptContext->GetGlobal(u8"Z").Get<f64>() ==
          1.0); // value semantics: a not mutated by b's reassign
}

// The math values' operators, bound from what reflection marks: a + b, v * 2,
// -v, a == b, the compound p += v, and a quaternion product composing in the engine's order.
TEST_CASE("angelscript: reflected operators - a + b, v * 2.0f, -v, a == b, p += v, q * r")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    const Status status =
        scriptContext->Load(u8"double SumY; double ScaledZ; double NegX; bool Same; bool Differ;\n"
                  u8"double MovedX; double HalfY; double TwoD; double QW; double Hue;\n"
                  u8"void main() {\n"
                  u8"  Float3 a = Float3(1, 2, 3);\n"
                  u8"  Float3 b = Float3(4, 5, 6);\n"
                  u8"  Float3 sum = a + b;\n"
                  u8"  SumY = sum.y;\n"
                  u8"  ScaledZ = (a * 2.0f).z;\n"
                  u8"  NegX = (-a).x;\n"
                  u8"  Same = a == Float3(1, 2, 3);\n"
                  u8"  Differ = a != b;\n"
                  u8"  Float3 p = a;\n"
                  u8"  p += b;\n"
                  u8"  p -= Float3(1, 1, 1);\n"
                  u8"  MovedX = p.x;\n"
                  u8"  HalfY = (b / 2.0f).y;\n"
                  u8"  Float2 d = Float2(1, 1) - Float2(3, 4);\n"
                  u8"  TwoD = d.y;\n"
                  u8"  Quaternion q = Quaternion(0, 0, 0, 1) * Quaternion(0, 0, 0, 1);\n"
                  u8"  QW = q.w;\n"
                  u8"  Hue = (Color(0.25f, 0, 0, 1) + Color(0.5f, 0, 0, 0)).r;\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"SumY").Get<f64>() == doctest::Approx(7.0));
    CHECK(scriptContext->GetGlobal(u8"ScaledZ").Get<f64>() == doctest::Approx(6.0));
    CHECK(scriptContext->GetGlobal(u8"NegX").Get<f64>() == doctest::Approx(-1.0));
    CHECK(scriptContext->GetGlobal(u8"Same").Get<bool>());
    CHECK(scriptContext->GetGlobal(u8"Differ").Get<bool>());
    CHECK(scriptContext->GetGlobal(u8"MovedX").Get<f64>() == doctest::Approx(4.0)); // 1 + 4 - 1
    CHECK(scriptContext->GetGlobal(u8"HalfY").Get<f64>() == doctest::Approx(2.5));
    CHECK(scriptContext->GetGlobal(u8"TwoD").Get<f64>() == doctest::Approx(-3.0));
    CHECK(scriptContext->GetGlobal(u8"QW").Get<f64>() == doctest::Approx(1.0));
    CHECK(scriptContext->GetGlobal(u8"Hue").Get<f64>() == doctest::Approx(0.75));
}

// Two asset ids compare in a script (Snowline's music: is this the track already playing).
TEST_CASE("angelscript: Guids compare with == and !=")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));
    const Status status =
        scriptContext->Load(u8"bool Same; bool Differ; bool NilSame;\n"
                  u8"Guid kTrack = Guid(\"6b0a7709-5169-4a0e-8077-c78b87a132e6\");\n"
                  u8"void main() {\n"
                  u8"  Guid playing = Guid(\"6b0a7709-5169-4a0e-8077-c78b87a132e6\");\n"
                  u8"  Same = playing == kTrack;\n"
                  u8"  Differ = playing != Guid(\"0758e8d2-c6cd-47a7-bce6-9728a9501018\");\n"
                  u8"  Guid none;\n"
                  u8"  NilSame = none == Guid() && none.IsNil();\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"Same").Get<bool>());
    CHECK(scriptContext->GetGlobal(u8"Differ").Get<bool>());
    CHECK(scriptContext->GetGlobal(u8"NilSame").Get<bool>());
}

// A plain reflected-type field reads back as its value, and so does a plain global, so a playtest
// can probe a game script's Guid field.
TEST_CASE("angelscript: plain reflected-type members and globals read back as their values")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));
    REQUIRE(scriptContext->Load(u8"Guid kTrack = Guid(\"6b0a7709-5169-4a0e-8077-c78b87a132e6\");\n"
                      u8"class Holder {\n"
                      u8"  Guid track;\n"
                      u8"  Float3 where;\n"
                      u8"  Guid held;\n"
                      u8"  Holder() { track = kTrack; where = Float3(1, 2, 3); held = Guid(\"0758e8d2-c6cd-47a7-bce6-9728a9501018\"); }\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    Guid track;
    Guid held;
    REQUIRE(Guid::TryParse(u8"6b0a7709-5169-4a0e-8077-c78b87a132e6", track));
    REQUIRE(Guid::TryParse(u8"0758e8d2-c6cd-47a7-bce6-9728a9501018", held));

    RefPtr<BehaviorObject> holder = scriptContext->CreateInstance(u8"Holder", Span<Variant>{});
    REQUIRE(static_cast<bool>(holder));
    Result<Variant> got = holder->GetProperty(u8"track");
    REQUIRE(got.HasValue());
    REQUIRE(got.Value().Is<Guid>());
    CHECK(got.Value().Get<Guid>() == track);
    Result<Variant> where = holder->GetProperty(u8"where");
    REQUIRE(where.HasValue());
    REQUIRE(where.Value().Is<Float3>());
    CHECK(where.Value().Get<Float3>().y == doctest::Approx(2.0f));
    Result<Variant> handle = holder->GetProperty(u8"held");
    REQUIRE(handle.HasValue());
    REQUIRE(handle.Value().Is<Guid>());
    CHECK(handle.Value().Get<Guid>() == held);

    const Variant global = scriptContext->GetGlobal(u8"kTrack");
    REQUIRE(global.Is<Guid>());
    CHECK(global.Get<Guid>() == track);
}

// The typed path (MethodInfo::call with AngelScript's own addresses): numbers of every width, an
// enum of AngelScript's int width, a value argument and return, and a value `self` a method changes;
// a value's constructor and its properties read and written the same way. A string and a one-byte
// enum are not held natively, so their methods keep the Variant call.
namespace
{
    enum class Gear : i32
    {
        Low,
        High
    };
    enum class Notch : u8
    {
        Small,
        Big
    };
    struct Tally
    {
        Tally() = default;
        Tally(f64 startTotal, i64 startCount) : total(startTotal), count(startCount) {}
        f64 total = 0.0;
        i64 count = 0;
        void Add(f64 v)
        {
            total += v;
            ++count;
        }
        [[nodiscard]] f64 Mean() const { return count == 0 ? 0.0 : total / static_cast<f64>(count); }
    };
    class TypedProbe : public Object
    {
        RTTI_OBJECT(TypedProbe, Object)
    public:
        f64 level = 1.0;
        [[nodiscard]] f64 Mix(f64 a, i64 b, f32 c, i8 d, u16 e) const
        {
            return a + static_cast<f64>(b) + static_cast<f64>(c) + d + e;
        }
        [[nodiscard]] bool IsHigh(Gear g) const { return g == Gear::High; }
        [[nodiscard]] Gear Flip(Gear g) const { return g == Gear::Low ? Gear::High : Gear::Low; }
        [[nodiscard]] Tally Twice(Tally t) const
        {
            t.total *= 2.0;
            t.count *= 2;
            return t;
        }
        [[nodiscard]] i64 Big() const { return (i64{1} << 60) + 7; }
        [[nodiscard]] i32 Length(String text) const { return static_cast<i32>(text.Size()); }
        [[nodiscard]] Notch Grow(Notch) const { return Notch::Big; }
    };
}
REFLECT_ENUM(Gear, "rtti::script::test")
{
    builder.Value("Low", Gear::Low);
    builder.Value("High", Gear::High);
}
REFLECT_ENUM(Notch, "rtti::script::test")
{
    builder.Value("Small", Notch::Small);
    builder.Value("Big", Notch::Big);
}
REFLECT_VALUE(Tally, "rtti::script::test")
{
    builder.Property<&Tally::total>("total");
    builder.Property<&Tally::count>("count");
    builder.Method<&Tally::Add>("Add");
    builder.Method<&Tally::Mean>("Mean");
    builder.Constructor();
    builder.Constructor<f64, i64>();
}
REFLECT_MEMBERS(TypedProbe, "rtti::script::test")
{
    builder.Property<&TypedProbe::level>("level");
    builder.Method<&TypedProbe::Mix>("Mix");
    builder.Method<&TypedProbe::IsHigh>("IsHigh");
    builder.Method<&TypedProbe::Flip>("Flip");
    builder.Method<&TypedProbe::Twice>("Twice");
    builder.Method<&TypedProbe::Big>("Big");
    builder.Method<&TypedProbe::Length>("Length");
    builder.Method<&TypedProbe::Grow>("Grow");
    builder.Constructor();
}

TEST_CASE("angelscript: a method of natively held values takes the typed call, and agrees with the Variant one")
{
    static const bool registered = []
    {
        RttiRegisterEnum_Gear();
        RttiRegisterEnum_Notch();
        RttiRegisterValue_Tally();
        return true;
    }();
    (void)registered;
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(TypeOf<Gear>());
    manager->RegisterType(TypeOf<Notch>());
    manager->RegisterType(TypeOf<Tally>());
    manager->RegisterType(TypedProbe::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"double M; bool H; int G; bool BigOk; int L; int N;\n"
                                u8"double TT; int64 TC; double TM; double UT; double UM; double LV;\n"
                                u8"TypedProbe g;\n"
                                u8"void methods() {\n"
                                u8"  M = g.Mix(1.5, 1099511627776, 2.25f, -3, 40000);\n"
                                u8"  H = g.IsHigh(Gear::High);\n"
                                u8"  G = int(g.Flip(Gear::Low));\n"
                                u8"  BigOk = g.Big() == 1152921504606846983;\n" // 2^60 + 7, compared at 64 bits
                                u8"  L = g.Length(\"four\");\n"
                                u8"  N = int(g.Grow(Notch::Small));\n"
                                u8"}\n"
                                u8"void values() {\n"
                                u8"  Tally t;\n"
                                u8"  t.Add(2.5);\n"
                                u8"  t.Add(3.5);\n"
                                u8"  Tally d = g.Twice(t);\n"
                                u8"  TT = d.total; TC = d.count; TM = t.Mean();\n"
                                u8"  Tally u = Tally(10.0, 4);\n"
                                u8"  u.total = 2.0;\n"
                                u8"  UT = u.total; UM = u.Mean();\n"
                                u8"}\n"
                                u8"void readLevel() { LV = g.level; }\n"
                                u8"void writeLevel() { g.level = 2.5; }\n",
                                u8"main")
                .IsOk());
    const auto typedCalls = [&](const char8_t* function) -> u64
    {
        const u64 before = angelscript::TypedCallCount();
        REQUIRE(scriptContext->Call(function, Span<Variant>{}).HasValue());
        return angelscript::TypedCallCount() - before;
    };

    // Mix, IsHigh, Flip and Big; Length (a string) and Grow (a one-byte enum) go by Variant.
    CHECK(typedCalls(u8"methods") == 4u);
    CHECK(scriptContext->GetGlobal(u8"M").Get<f64>() == 1099511667776.75); // 8-byte arguments, misaligned or not
    CHECK(scriptContext->GetGlobal(u8"H").Get<bool>());
    CHECK(scriptContext->GetGlobal(u8"G").Get<f64>() == 1.0); // an int global reads as a number
    CHECK(scriptContext->GetGlobal(u8"BigOk").Get<bool>()); // a global reads as a double: compared in script
    CHECK(scriptContext->GetGlobal(u8"L").Get<f64>() == 4.0);
    CHECK(scriptContext->GetGlobal(u8"N").Get<f64>() == 1.0);

    // Add twice, Twice, Mean twice and the constructors (AngelScript makes a value more often than
    // the text shows); a value's fields it reaches itself, with no call.
    CHECK(typedCalls(u8"values") >= 6u);
    CHECK(scriptContext->GetGlobal(u8"TT").Get<f64>() == 12.0); // a value in, a value out
    CHECK(scriptContext->GetGlobal(u8"TC").Get<f64>() == 4.0);
    CHECK(scriptContext->GetGlobal(u8"TM").Get<f64>() == 3.0);  // the value `self` Add changed
    CHECK(scriptContext->GetGlobal(u8"UT").Get<f64>() == 2.0);  // constructed with its values, then written
    CHECK(scriptContext->GetGlobal(u8"UM").Get<f64>() == 0.5);  // 2.0 over the constructed count

    // An object's property, read and written: one typed access each.
    CHECK(typedCalls(u8"readLevel") == 1u);
    CHECK(scriptContext->GetGlobal(u8"LV").Get<f64>() == 1.0);
    CHECK(typedCalls(u8"writeLevel") == 1u);
    CHECK(typedCalls(u8"readLevel") == 1u);
    CHECK(scriptContext->GetGlobal(u8"LV").Get<f64>() == 2.5);
}

TEST_CASE("angelscript: 64-bit integer facade args/returns round-trip exactly (no double funnel)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    manager->RegisterType(NumProbe::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext(); // defensively finalizes
    REQUIRE(static_cast<bool>(scriptContext));

    // ArgOk: script passes 2^53+1 into an int64 facade param, and C++ must see it exactly.
    // ReturnOk: an int64 facade return equals the same literal, compared in-script (int64==int64).
    // I32Ok: a 32-bit natural-typed param also survives.
    const Status status = scriptContext->Load(u8"bool ArgOk; bool ReturnOk; bool I32Ok;\n"
                                    u8"void main() {\n"
                                    u8"  NumProbe p;\n"
                                    u8"  p.takeI64(9007199254740993);\n"
                                    u8"  ArgOk = p.argWasExact();\n"
                                    u8"  ReturnOk = (p.bigConst() == 9007199254740993);\n"
                                    u8"  p.takeI32(1234567);\n"
                                    u8"  I32Ok = (p.echoI32() == 1234567);\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(scriptContext->GetGlobal(u8"ArgOk").Get<bool>() == true);
    CHECK(scriptContext->GetGlobal(u8"ReturnOk").Get<bool>() == true);
    CHECK(scriptContext->GetGlobal(u8"I32Ok").Get<bool>() == true);
}

TEST_CASE("angelscript: a reflected method marshals 9 args without truncating the trailing ones")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    manager->RegisterType(NumProbe::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    // 9 args, more than 8 (DebugDraw.line takes 9). Powers of two so any dropped or garbled
    // argument changes the sum: 1+2+4+8+16+32+64+128+256 = 511. Truncation would lose the 256.
    const Status status =
        scriptContext->Load(u8"float S;\n"
                  u8"void main() {\n"
                  u8"  NumProbe p;\n"
                  u8"  p.take9(1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f);\n"
                  u8"  S = p.sum9();\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"S").Get<f64>() == doctest::Approx(511.0));
}

TEST_CASE("angelscript: a reflected method marshals 16 args - the top of the raised cap")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    manager->RegisterType(NumProbe::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    // 16 args, kMaxArgs exactly. Powers of two so any dropped or garbled argument
    // changes the sum: 1+2+...+32768 = 65535. A boundary off-by-one would lose the trailing 32768.
    const Status status =
        scriptContext->Load(u8"float S;\n"
                  u8"void main() {\n"
                  u8"  NumProbe p;\n"
                  u8"  p.take16(1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f,\n"
                  u8"           256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f, 8192.0f,\n"
                  u8"           16384.0f, 32768.0f);\n"
                  u8"  S = p.sum16();\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"S").Get<f64>() == doctest::Approx(65535.0));
}

TEST_CASE("angelscript: a context runs valid source")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));

    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    // A real class-based program: define a class, instantiate, call a method.
    const Status status = scriptContext->Load(u8"class Greeter {\n"
                                    u8"  string name;\n"
                                    u8"  Greeter(string n) { name = n; }\n"
                                    u8"  string greet() { return \"hi \" + name; }\n"
                                    u8"}\n"
                                    u8"string G;\n"
                                    u8"void main() { Greeter g(\"engine\"); G = g.greet(); }\n",
                                    u8"main");
    CHECK(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"G").Get<String>() == u8"hi engine");
}

TEST_CASE("angelscript: string concatenates numbers (opAdd from RegisterStdString, no utils needed)")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    // A HUD can build "Score: 1200" without RegisterStdStringUtils: the primitive
    // opAdd(int64/double) operators ship with RegisterStdString (int widens to int64).
    const Status status = scriptContext->Load(u8"string C;\n"
                                    u8"void main() {\n"
                                    u8"  int score = 1200;\n"
                                    u8"  double t = 3.5;\n"
                                    u8"  C = \"Score: \" + score + \" (\" + t + \")\";\n"
                                    u8"}\n",
                                    u8"main");
    CHECK(status.IsOk());
    CHECK(scriptContext->GetGlobal(u8"C").Get<String>() == u8"Score: 1200 (3.5)");
}

TEST_CASE("angelscript: a compile error is reported")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    const Status status = scriptContext->Load(u8"this is not valid angelscript @#$", u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::InvalidArgument);
}

namespace
{
    struct CapturingErrors final : IScriptErrorHandler
    {
        int count = 0;
        ScriptErrorKind lastKind = ScriptErrorKind::Compile;
        String lastMessage;
        i32 lastLine = -1;

        void OnError(const ScriptError& error) override
        {
            ++count;
            lastKind = error.kind;
            lastMessage = String(error.message);
            lastLine = error.line;
        }
    };
}

TEST_CASE("angelscript: errors are surfaced to a handler")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    CapturingErrors errors;
    scriptContext->SetErrorHandler(&errors);

    // Compile error: kind, a non-empty message and a source line.
    CHECK_FALSE(scriptContext->Load(u8"int = = =", u8"main").IsOk());
    CHECK(errors.count >= 1);
    CHECK(errors.lastKind == ScriptErrorKind::Compile);
    CHECK(!errors.lastMessage.IsEmpty());
    CHECK(errors.lastLine >= 1);

    // Runtime error (via the main() load convention): kind switches to Runtime.
    const int afterCompile = errors.count;
    CHECK_FALSE(
        scriptContext->Load(u8"void main() { int zero = 0; int boom = 10 / zero; }\n", u8"main").IsOk());
    CHECK(errors.count > afterCompile);
    CHECK(errors.lastKind == ScriptErrorKind::Runtime);
    CHECK(!errors.lastMessage.IsEmpty());

    // Clearing the handler restores default (console) reporting, so nothing more is captured.
    scriptContext->SetErrorHandler(nullptr);
    const int afterRuntime = errors.count;
    CHECK_FALSE(scriptContext->Load(u8"more @#$ garbage", u8"main").IsOk());
    CHECK(errors.count == afterRuntime);
}

TEST_CASE("angelscript: a runtime error is reported")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    const Status status =
        scriptContext->Load(u8"void main() { int zero = 0; int boom = 10 / zero; }\n", u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::Internal);
}

TEST_CASE("angelscript: a runtime fault in a GLOBAL INITIALIZER is a Runtime error")
{
    // Load runs the global initializers after the Build (InitGlobals); a failed one is a
    // Runtime error, not a Compile one.
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    CapturingErrors errors;
    scriptContext->SetErrorHandler(&errors);
    const Status status = scriptContext->Load(u8"int Zero() { return 0; }\n"
                                    u8"int boom = 10 / Zero();\n",
                                    u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::Internal);
    CHECK(errors.count >= 1);
    CHECK(errors.lastKind == ScriptErrorKind::Runtime);
}

namespace
{
    struct CheckErrors final : IScriptErrorHandler
    {
        int count = 0;
        ScriptErrorKind kind = ScriptErrorKind::Runtime;
        String module;
        i32 line = -1;
        i32 column = -1;

        void OnError(const ScriptError& error) override
        {
            if (count++ == 0)
            {
                kind = error.kind;
                module = String(error.module);
                line = error.line;
                column = error.column;
            }
        }
    };
}

TEST_CASE("angelscript: Check reports a compile error with its line and column")
{
    RefPtr<IScriptManager> manager =
        angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    CheckErrors errors;
    CHECK_FALSE(manager->Check(u8"int a = 1;\nint b = ;\n", u8"typed.as", errors));
    REQUIRE(errors.count >= 1);
    CHECK(errors.kind == ScriptErrorKind::Compile);
    CHECK(errors.module == u8"typed.as");
    CHECK(errors.line == 2);
    CHECK(errors.column == 9);

    CheckErrors none;
    CHECK(manager->Check(u8"int Twice(int v) { return v * 2; }\n", u8"typed.as", none));
    CHECK(none.count == 0);
}

TEST_CASE("angelscript: Check runs neither the global initializers nor main")
{
    RefPtr<IScriptManager> manager =
        angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    // Load fails on both of these at run time (see the global initializer test above); a
    // check that ran them would report a Runtime error.
    CheckErrors errors;
    CHECK(manager->Check(u8"int Zero() { return 0; }\n"
                         u8"int boom = 10 / Zero();\n"
                         u8"void main() { int zero = 0; int bang = 10 / zero; }\n",
                         u8"typed.as", errors));
    CHECK(errors.count == 0);

    // The engine still initializes globals for an ordinary load afterwards.
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"int Answer = 42;\n", u8"main").IsOk());
    CHECK(scriptContext->GetGlobal(u8"Answer").Get<f64>() == 42.0);
}

TEST_CASE("angelscript: each context is isolated")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RefPtr<IScriptContext> a = manager->CreateContext();
    RefPtr<IScriptContext> b = manager->CreateContext();
    CHECK(a.Get() != b.Get());
    CHECK(a->Load(u8"double f() { return 1; }\n", u8"main").IsOk());
    CHECK(b->Load(u8"double g() { return 2; }\n", u8"main").IsOk());
    CHECK(a->HasFunction(u8"f"));
    CHECK_FALSE(a->HasFunction(u8"g"));
    CHECK_FALSE(b->HasFunction(u8"f"));
}

TEST_CASE("angelscript: read module globals as Variant")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext->Load(u8"int Answer = 42;\n"
                      u8"string Name = \"engine\";\n"
                      u8"bool Flag = true;\n"
                      u8"double Pi = 3.5;\n",
                      u8"main")
                .IsOk());

    CHECK(scriptContext->GetGlobal(u8"Answer").Get<f64>() == 42.0); // numbers surface as f64
    CHECK(scriptContext->GetGlobal(u8"Name").Get<String>() == u8"engine");
    CHECK(scriptContext->GetGlobal(u8"Flag").Get<bool>() == true);
    CHECK(scriptContext->GetGlobal(u8"Pi").Get<f64>() == 3.5);

    CHECK(scriptContext->GetGlobal(u8"Missing").IsEmpty()); // absent: empty Variant
}

TEST_CASE("angelscript: SetGlobal writes typed module globals")
{
    // AngelScript globals are directly writable, so SetGlobal takes effect here.
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext->Load(u8"double Speed = 1;\n"
                      u8"string Tag = \"none\";\n"
                      u8"double ReadSpeed() { return Speed; }\n"
                      u8"string ReadTag() { return Tag; }\n",
                      u8"main")
                .IsOk());

    scriptContext->SetGlobal(u8"Speed", Variant::From(4.5));
    scriptContext->SetGlobal(u8"Tag", Variant::From(String(u8"fast")));
    CHECK(scriptContext->Call(u8"ReadSpeed", Span<Variant>{}).Value().Get<f64>() == 4.5);
    CHECK(scriptContext->Call(u8"ReadTag", Span<Variant>{}).Value().Get<String>() == u8"fast");
}

TEST_CASE("angelscript: a read-only property reads, and an assignment fails to compile - no setter exists")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Widget::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    REQUIRE(scriptContext->Load(u8"double S = 0;\n"
                      u8"void main() { Widget@ w = Widget(); S = w.serial; }\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"S").Get<f64>() == 7.0);

    // The assignment is refused by the compiler: the backend registers get_serial only.
    RefPtr<IScriptContext> other = manager->CreateContext();
    CHECK_FALSE(other->Load(u8"void main() { Widget@ w = Widget(); w.serial = 3; }\n", u8"main").IsOk());
}

TEST_CASE("angelscript: reflected value types are usable from script (construct + properties)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager); // collect, then FinalizeTypes (two-phase)
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    // Construct a reflected Float3, read and write its properties.
    const Status status = scriptContext->Load(u8"Float3 v = Float3(1, 2, 3);\n"
                                    u8"double X = 0;\n"
                                    u8"double Z = 0;\n"
                                    u8"double X2 = 0;\n"
                                    u8"void main() {\n"
                                    u8"  X = v.x;\n"
                                    u8"  Z = v.z;\n"
                                    u8"  v.x = 9;\n"
                                    u8"  X2 = v.x;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(scriptContext->GetGlobal(u8"X").Get<f64>() == 1.0); // construct and getter
    CHECK(scriptContext->GetGlobal(u8"Z").Get<f64>() == 3.0);
    CHECK(scriptContext->GetGlobal(u8"X2").Get<f64>() == 9.0); // setter took effect
}

TEST_CASE("angelscript: call reflected methods (static, instance, struct return, object args)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    // Static method (namespace emission): Float3::Dot(a, b) with object args.
    REQUIRE(scriptContext->Load(u8"Float3 a = Float3(1, 2, 3);\n"
                      u8"Float3 b = Float3(4, 5, 6);\n"
                      u8"double D = 0;\n"
                      u8"void main() { D = Float3::Dot(a, b); }\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"D").Get<f64>() == 32.0);

    // Instance method returning a struct (Float4.XYZ() returns a Float3), then read it.
    REQUIRE(scriptContext->Load(u8"Float4 v4 = Float4(7, 8, 9, 10);\n"
                      u8"double XX = 0;\n"
                      u8"double ZZ = 0;\n"
                      u8"void main() { Float3 xyz = v4.XYZ(); XX = xyz.x; ZZ = xyz.z; }\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"XX").Get<f64>() == 7.0);
    CHECK(scriptContext->GetGlobal(u8"ZZ").Get<f64>() == 9.0);

    // Instance method taking an object arg, returning bool; constructed from
    // object args too (AABB(Float3, Float3)).
    REQUIRE(scriptContext->Load(u8"AABB box = AABB(Float3(0, 0, 0), Float3(10, 10, 10));\n"
                      u8"bool inside = false;\n"
                      u8"bool outside = true;\n"
                      u8"void main() {\n"
                      u8"  inside = box.Contains(Float3(5, 5, 5));\n"
                      u8"  outside = box.Contains(Float3(20, 0, 0));\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"inside").Get<bool>() == true);
    CHECK(scriptContext->GetGlobal(u8"outside").Get<bool>() == false);
}

TEST_CASE("angelscript: same-arity type overloads carry distinct script names (overloadedName)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    // Same-arity type overloads carry distinct script names (overloadedName): Mul
    // (Float3, Float3) componentwise and MulScalar (Float3, float) scale, both bound.
    REQUIRE(scriptContext->Load(u8"Float3 p = Float3(2, 3, 4);\n"
                      u8"double CX = 0;\n"
                      u8"double CZ = 0;\n"
                      u8"double SX = 0;\n"
                      u8"void main() {\n"
                      u8"  Float3 comp = Float3::Mul(p, Float3(1, 2, 3));\n" // -> (2, 6, 12)
                      u8"  Float3 scaled = Float3::MulScalar(p, 2.0f);\n"     // -> (4, 6, 8)
                      u8"  CX = comp.x;\n"
                      u8"  CZ = comp.z;\n"
                      u8"  SX = scaled.x;\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"CX").Get<f64>() == 2.0); // chose (Float3, Float3)
    CHECK(scriptContext->GetGlobal(u8"CZ").Get<f64>() == 12.0);
    CHECK(scriptContext->GetGlobal(u8"SX").Get<f64>() == 4.0); // chose (Float3, float)
}

TEST_CASE("angelscript: reflected math ops (Math statics, Float3/Quaternion vector ops)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    REQUIRE(scriptContext->Load(u8"double R = 0;\n"
                      u8"double K = 0;\n"
                      u8"double CZ = 0;\n"
                      u8"double RX = 0;\n"
                      u8"void main() {\n"
                      u8"  R = Math::Sqrt(16.0f);\n"                                // -> 4
                      u8"  K = Math::Clamp(5.0f, 0.0f, 1.0f) + Math::Min(2.0f, -3.0f) * 10.0f +\n"
                      u8"      Math::Max(2.0f, -3.0f) * 100.0f;\n"                      // -> 1 - 30 + 200
                      u8"  Float3 c = Float3::Cross(Float3(1, 0, 0), Float3(0, 1, 0));\n" // -> +Z
                      u8"  CZ = c.z;\n"
                      // 90 degrees about +Y rotates +Z to +X.
                      u8"  Quaternion q = Quaternion::FromAxisAngle(Float3(0, 1, 0), 1.5707963f);\n"
                      u8"  Float3 rv = Quaternion::RotateVector(q, Float3(0, 0, 1));\n"
                      u8"  RX = rv.x;\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"R").Get<f64>() == 4.0);
    CHECK(scriptContext->GetGlobal(u8"K").Get<f64>() == 171.0);
    CHECK(scriptContext->GetGlobal(u8"CZ").Get<f64>() == 1.0);
    CHECK(scriptContext->GetGlobal(u8"RX").Get<f64>() == doctest::Approx(1.0));
}

// Math had only float Min, Max and Clamp: an int pair went in as floats and came back a float the
// script truncated into its int, with a compiler warning on every such line.
TEST_CASE("angelscript: Math's whole-number Min, Max and Clamp return int")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"int K = 0;\n"
                                u8"void main() {\n"
                                u8"  int low = Math::MinInt(7, -3);\n"
                                u8"  int high = Math::MaxInt(7, -3);\n"
                                u8"  int held = Math::ClampInt(12, 1, 10);\n"
                                u8"  K = low * 100 + high * 10 + held;\n" // -300 + 70 + 10
                                u8"}\n",
                                u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"K").Get<f64>() == -220.0); // a global reads back as a number
}

TEST_CASE("angelscript: bound-api signatures carry reflected parameter names")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Widget::StaticType()); // a test-local type beside the registry's
    RegisterReflectedTypes(*manager);

    const Array<ScriptApiType> api = manager->DescribeBoundApi();

    const auto findType = [&](StringView name) -> const ScriptApiType* {
        for (const ScriptApiType& t : api)
        {
            if (t.scriptName.AsView() == name)
            {
                return &t;
            }
        }
        return nullptr;
    };
    const auto findMember = [](const ScriptApiType& t, StringView name) -> const ScriptApiMember* {
        for (const ScriptApiMember& m : t.members)
        {
            if (m.name.AsView() == name)
            {
                return &m;
            }
        }
        return nullptr;
    };

    // The signature spells parameter names, not just types, for the API browser: it ends with the
    // last parameter's name before the closing paren.
    const ScriptApiType* float3 = findType(u8"Float3");
    REQUIRE(float3 != nullptr);
    const ScriptApiMember* cross = findMember(*float3, u8"Cross");
    REQUIRE(cross != nullptr);
    CHECK(cross->signature.AsView().EndsWith(u8" b)"));
    // The operators it binds are listed as such, as a script writes them.
    const ScriptApiMember* plus = findMember(*float3, u8"+");
    REQUIRE(plus != nullptr);
    CHECK(plus->kind == ScriptApiMemberKind::Operator);
    CHECK(plus->signature.AsView().StartsWith(u8"Float3 + "));
    CHECK(plus->signature.AsView().EndsWith(u8"(and +=)"));

    const ScriptApiType* math = findType(u8"Math");
    REQUIRE(math != nullptr);
    const ScriptApiMember* atan2 = findMember(*math, u8"Atan2");
    REQUIRE(atan2 != nullptr);
    CHECK(atan2->signature.AsView().EndsWith(u8" x)")); // Atan2(y, x)

    // A read-only property says so: the flag for tooling, the suffix for a reader of the
    // signature (the API browser), so an agent knows before it writes.
    const ScriptApiType* widget = findType(u8"Widget");
    REQUIRE(widget != nullptr);
    const ScriptApiMember* serial = findMember(*widget, u8"serial");
    REQUIRE(serial != nullptr);
    CHECK(serial->readOnly);
    CHECK(serial->kind == ScriptApiMemberKind::Property);
    CHECK(serial->signature.AsView() == u8"Widget.serial (read only)");
    const ScriptApiMember* id = findMember(*widget, u8"id");
    REQUIRE(id != nullptr);
    CHECK_FALSE(id->readOnly);
    CHECK(id->signature.AsView() == u8"Widget.id");
}

TEST_CASE("angelscript: Object-derived type as a script-visible class")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Widget::StaticType());           // register just the Object type
    RefPtr<IScriptContext> scriptContext = manager->CreateContext(); // defensively finalizes

    REQUIRE(scriptContext->Load(u8"double I = 0;\n"
                      u8"double D = 0;\n"
                      u8"double O = 0;\n"
                      u8"double H = 0;\n"
                      u8"void main() {\n"
                      u8"  Widget@ w = Widget();\n"
                      u8"  w.id = 21;\n"
                      u8"  Widget@ w2 = Widget();\n"
                      u8"  w2.id = 5;\n"
                      u8"  I = w.id;\n"
                      u8"  D = w.doubled();\n" // instance method: 42
                      u8"  O = w.idOf(w2);\n"  // object argument: 5
                      u8"  H = w.idOfAny(@w2);\n" // an explicit handle into a Variant sink: 5
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"I").Get<f64>() == 21.0);
    CHECK(scriptContext->GetGlobal(u8"D").Get<f64>() == 42.0);
    CHECK(scriptContext->GetGlobal(u8"O").Get<f64>() == 5.0);
    CHECK(scriptContext->GetGlobal(u8"H").Get<f64>() == 5.0); // read one level deeper than a bare object
}

TEST_CASE("angelscript: a default-constructed reflected type")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    // Guid has a default ctor and (u64, u64); its props surface as doubles.
    REQUIRE(scriptContext->Load(u8"Guid g = Guid(7, 42);\n"
                      u8"double Hi = 0;\n"
                      u8"double Lo = 0;\n"
                      u8"void main() { Hi = double(g.high); Lo = double(g.low); }\n",
                      u8"main")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"Hi").Get<f64>() == 7.0);
    CHECK(scriptContext->GetGlobal(u8"Lo").Get<f64>() == 42.0);
}

TEST_CASE("angelscript: call a script function with marshalled args")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext->Load(u8"double add(double a, double b) { return a + b; }\n"
                      u8"string greeting() { return \"hi\"; }\n"
                      u8"string greet(string name) { return \"hi \" + name; }\n",
                      u8"main")
                .IsOk());

    CHECK(scriptContext->HasFunction(u8"add"));
    CHECK_FALSE(scriptContext->HasFunction(u8"nope"));

    // int args marshal to script doubles; result comes back as f64.
    Variant addArgs[] = {Variant::From(2), Variant::From(3)};
    CHECK(scriptContext->Call(u8"add", Span<Variant>{addArgs, 2}).Value().Get<f64>() == 5.0);

    // no-arg call returning a string.
    CHECK(scriptContext->Call(u8"greeting", Span<Variant>{}).Value().Get<String>() == u8"hi");

    // String argument in, string out.
    Variant greetArgs[] = {Variant::From(String(u8"engine"))};
    CHECK(scriptContext->Call(u8"greet", Span<Variant>{greetArgs, 1}).Value().Get<String>() ==
          u8"hi engine");

    // A missing callable is NotFound.
    CHECK(scriptContext->Call(u8"nope", Span<Variant>{}).Error() == ErrorCode::NotFound);
}

TEST_CASE("angelscript: instantiate a script class and invoke its methods")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext->Load(u8"class Counter {\n"
                      u8"  double n;\n"
                      u8"  Counter(double start) { n = start; }\n"
                      u8"  void add(double x) { n = n + x; }\n"
                      u8"  double value() { return n; }\n"
                      u8"  void reset() { n = 0; }\n"
                      u8"}\n",
                      u8"main")
                .IsOk());

    Variant ctorArgs[] = {Variant::From(10)};
    RefPtr<BehaviorObject> counter = scriptContext->CreateInstance(u8"Counter", Span<Variant>{ctorArgs, 1});
    REQUIRE(static_cast<bool>(counter));

    Variant addArgs[] = {Variant::From(5)};
    CHECK(counter->Invoke(u8"add", Span<Variant>{addArgs, 1}).HasValue());

    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 15.0);
    // A member field reads as a property; a method or a missing name does not.
    CHECK(counter->GetProperty(u8"n").Value().Get<f64>() == 15.0);
    CHECK_FALSE(counter->GetProperty(u8"value").HasValue());
    CHECK_FALSE(counter->GetProperty(u8"missing").HasValue());

    CHECK(counter->Invoke(u8"reset", Span<Variant>{}).HasValue());
    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 0.0);
}

TEST_CASE("angelscript: a bound method is called by its handle, matched by its parameter count")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext->Load(u8"class Counter {\n"
                                u8"  double n = 0;\n"
                                u8"  void add(double x) { n = n + x; }\n"
                                u8"  double value() { return n; }\n"
                                u8"}\n",
                                u8"main")
                .IsOk());
    RefPtr<BehaviorObject> counter = scriptContext->CreateInstance(u8"Counter", Span<Variant>{});
    REQUIRE(static_cast<bool>(counter));

    const BehaviorMethod add = counter->Bind(u8"add", 1);
    const BehaviorMethod value = counter->Bind(u8"value", 0);
    REQUIRE(add.IsValid());
    REQUIRE(value.IsValid());
    Variant five[] = {Variant::From(5)};
    CHECK(counter->Invoke(add, Span<Variant>{five, 1}).HasValue());
    CHECK(counter->Invoke(add, Span<Variant>{five, 1}).HasValue());
    CHECK(counter->Invoke(value, Span<Variant>{}).Value().Get<f64>() == 10.0);
    // No method by that name with that many parameters: nothing to bind, and a call with the
    // wrong count through a bound one is refused.
    CHECK_FALSE(counter->Bind(u8"add", 0).IsValid());
    CHECK_FALSE(counter->Bind(u8"missing", 0).IsValid());
    CHECK(counter->Invoke(add, Span<Variant>{}).Error() == ErrorCode::NotFound);
    CHECK(counter->Invoke(BehaviorMethod{}, Span<Variant>{}).Error() == ErrorCode::NotFound);
}

TEST_CASE("angelscript: CreateInstance returns null for an unknown class")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext->Load(u8"class Known { Known() {} }\n", u8"main").IsOk());
    CHECK_FALSE(static_cast<bool>(scriptContext->CreateInstance(u8"Missing", Span<Variant>{})));
}

TEST_CASE("angelscript: a script object outlives the local context reference")
{
    RefPtr<BehaviorObject> obj;
    {
        RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
        REQUIRE(scriptContext->Load(u8"class Echo {\n"
                          u8"  Echo() {}\n"
                          u8"  double ping() { return 42; }\n"
                          u8"}\n",
                          u8"main")
                    .IsOk());
        obj = scriptContext->CreateInstance(u8"Echo", Span<Variant>{});
        REQUIRE(static_cast<bool>(obj));
        // ctx goes out of scope here; obj retains it (keeps the engine alive).
    }
    CHECK(obj->Invoke(u8"ping", Span<Variant>{}).Value().Get<f64>() == 42.0);
}

#ifdef OPTION_HAS_LUAU // AngelScript and a second backend resolve side by side
TEST_CASE("angelscript: registry - both backends resolve side by side")
{
    angelscript::RegisterAngelScriptBackend();
    luau::RegisterLuauScriptBackend();
    ScriptBackendRegistry& registry = ScriptBackendRegistry::Get();

    const ScriptBackendDesc* as = registry.FindByLanguage(u8"angelscript");
    REQUIRE(as != nullptr);
    CHECK(as->displayName == u8"AngelScript");
    REQUIRE(as->fileExtensions.Size() == 1u);
    CHECK(as->fileExtensions[0] == u8"as");
    CHECK(registry.FindByExtension(u8"as") == as);

    // Extension dispatch creates the right manager: prove it by feeding each one
    // its own language's source.
    RefPtr<IScriptManager> asManager = CreateScriptManagerForFile(u8"Scripts/game.as", DefaultAllocator());
    REQUIRE(asManager.Get() != nullptr);
    RefPtr<IScriptContext> asCtx = asManager->CreateContext();
    CHECK(asCtx->Load(u8"double f() { return 1; }\n", u8"probe").IsOk());
    CHECK_FALSE(asCtx->Load(u8"local W = 1\n", u8"probe").IsOk()); // Luau source rejected

    RefPtr<IScriptManager> luauManager = CreateScriptManagerForFile(u8"Scripts/game.luau", DefaultAllocator());
    REQUIRE(luauManager.Get() != nullptr);
    RefPtr<IScriptContext> luauCtx = luauManager->CreateContext();
    CHECK(luauCtx->Load(u8"local A = 1\n", u8"main").IsOk());
    CHECK_FALSE(luauCtx->Load(u8"double f() { return 1; }", u8"main").IsOk()); // AngelScript source rejected
}
#endif // OPTION_HAS_LUAU

// The behaviors module is loaded with each class in its own script section named by its
// sourceName (not one flat "behaviors#N"), so editor gutter breakpoints, keyed on the source
// file, line up with what GetLineNumber reports. A breakpoint set on ("Mover.as", line) stops,
// and CaptureStackFrames()[0].file is "Mover.as".
TEST_CASE("angelscript: LoadBehaviorModule reports each class's sourceName as its section")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);

    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(static_cast<bool>(scriptContext));

    // One behavior class, source file "Mover.as"; line 5 is the breakpoint line.
    const StringView moverSource =
        u8"class Mover {\n"        // 1
        u8"  double x;\n"          // 2
        u8"  Mover() { x = 0; }\n" // 3
        u8"  void tick() {\n"      // 4
        u8"    x = x + 1;\n"       // 5  <- breakpoint (keyed on the source file)
        u8"  }\n"                  // 6
        u8"}\n";                   // 7
    const BehaviorModuleClass classes[] = {{u8"Mover.as", moverSource, {}, {}}};
    REQUIRE(scriptContext->LoadBehaviorModule(Span<const BehaviorModuleClass>{classes, 1}, u8"behaviors#1")
                .IsOk());

    struct Sink final : IScriptDebuggerListener
    {
        ScriptDebuggerState last = ScriptDebuggerState::Running;
        void OnDebuggerStateChanged(ScriptDebuggerState state) override { last = state; }
    } sink;
    debugger->SetListener(&sink);

    // The breakpoint is keyed on the source file, as an editor gutter would set it, not on the
    // "behaviors#1" module name.
    debugger->SetBreakpoint(u8"Mover.as", 5);

    RefPtr<BehaviorObject> mover = scriptContext->CreateInstance(u8"Mover", Span<Variant>{});
    REQUIRE(static_cast<bool>(mover));
    (void)mover->Invoke(u8"tick", Span<Variant>{}); // suspends at the breakpoint

    CHECK(sink.last == ScriptDebuggerState::Breakpoint);
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 5);
    CHECK(StringView(frames[0].file) == u8"Mover.as"); // the section is the source file

    debugger->Continue();
    CHECK(sink.last == ScriptDebuggerState::Terminated);
    debugger->SetListener(nullptr);
}

#include "Script.Tests/BackendConformance.h"

TEST_CASE("angelscript: CERTIFIED - the backend conformance battery")
{
    foundation::script::conformance::Dialect dialect;
    dialect.languageId = u8"angelscript";
    dialect.functionsModule = u8"int answer = 42;\n"
                              u8"double add(double a, double b) { return a + b; }\n"
                              u8"string greeting() { return \"hi\"; }\n";
    dialect.counterClass = u8"class Counter {\n"
                           u8"  double count;\n"
                           u8"  Counter(double start) { count = start; }\n"
                           u8"  void increment() { count = count + 1; }\n"
                           u8"  double value() { return count; }\n"
                           u8"}\n";
    dialect.compileBroken = u8"int = = = @#$";
    dialect.runtimeFault = u8"void main() { int zero = 0; int boom = 10 / zero; }\n";
    dialect.nestedFault = u8"int inner() { int zero = 0; return 10 / zero; }\n"
                          u8"int outer() { return inner() + 1; }\n";
    // Delegate: subscribe an AngelScript funcdef handle (value * 2) to DelegateSignal.
    dialect.delegateModule = u8"double dbl(double x) { return x * 2; }\n"
                             u8"DelegateSignal@ signal = DelegateSignal();\n"
                             u8"void main() { signal.Connect(ScriptDelegate(dbl)); }\n";
    // Overload contract: an arity family (ping and ping(x); AngelScript overloads natively) and a
    // distinct-name overload (combineText). Set the globals in main() (run at load, like the
    // delegate module).
    dialect.overloadModule = u8"Overloads@ over = Overloads();\n"
                             u8"double OP0 = 0; double OP1 = 0; double OC = 0; double OCT = 0;\n"
                             u8"void main() {\n"
                             u8"  OP0 = over.ping();\n"
                             u8"  OP1 = over.ping(5);\n"
                             u8"  OC = over.combine(2, 3);\n"
                             u8"  OCT = over.combineText(\"x\", 7);\n"
                             u8"}\n";
    dialect.widthsModule = u8"Widths@ widths = Widths();\n"
                           u8"double WG = 0; double WO = 0;\n"
                           u8"void main() {\n"
                           u8"  widths.group = 200;\n"
                           u8"  WG = widths.group;\n"
                           u8"  WO = widths.offset(-300);\n"
                           u8"}\n"
                           u8"double compareNothing() { return widths.sameGroup(null); }\n";
    // Inheritance contract: the base's property and method through the derived handle.
    dialect.inheritModule = u8"Square@ square = Square();\n"
                            u8"double IA = 0; double IB = 0; double IC = 0;\n"
                            u8"void main() { IA = square.sides; IB = square.twice(); IC = square.edge; }\n";
    // Gone-handle contract: valid() answers, any other use of the lamp errors once it is gone.
    dialect.goneModule = u8"Lamp@ lamp = Lamp::take(0);\n"
                         u8"double valid() { return lamp.isValid() ? 1 : 0; }\n"
                         u8"double readLevel() { return lamp.level; }\n"
                         u8"void writeLevel() { lamp.level = 7; }\n"
                         u8"void bump() { lamp.bump(); }\n"
                         u8"double countItems() { return lamp.items_count(); }\n";
    // Container contract: two adds through returned handles, zero-based at, move, removeAt.
    dialect.containerModule =
        u8"Crate@ crate = Crate();\n"
        u8"double KN = 0; double KZ = 0; double KM = 0; double KR = 0; double KL = 0;\n"
        u8"void main() {\n"
        u8"  crate.items_add().size = 5;\n" // an element is reached in place, through a reference
        u8"  crate.items_add().size = 9;\n"
        u8"  KN = crate.items_count();\n"
        u8"  KZ = crate.items_at(0).size;\n"
        u8"  crate.items_move(1, 0);\n"
        u8"  KM = crate.items_at(0).size;\n"
        u8"  crate.items_removeAt(0);\n"
        u8"  KR = crate.items_count();\n"
        u8"  KL = crate.items_at(0).size;\n"
        u8"}\n";
    // AngelScript's natural coroutine surface: a delegate to a method (`this.RunWait`)
    // wrapped in the ScriptCoroutine funcdef, started with startCoroutine; `wait` is a
    // host function, `waitUntil` a script helper (injected per module): the shared
    // coroutine concept, spelled in this backend's own syntax.
    dialect.coroutineClass =
        u8"class Coro {\n"
        u8"  double p;\n"
        u8"  bool gate;\n"
        u8"  Coro() { p = 0; gate = false; }\n"
        u8"  double progress() { return p; }\n"
        u8"  void flip() { gate = true; }\n"
        u8"  bool GateOpen() { return gate; }\n"
        u8"  void RunWait() { wait(1.0f); p = 1; }\n"
        u8"  void RunUntil() { Coroutine::waitUntil(CoroutinePredicate(this.GateOpen)); p = 1; }\n"
        u8"  void begin() { startCoroutine(ScriptCoroutine(this.RunWait)); }\n"
        u8"  void beginUntil() { startCoroutine(ScriptCoroutine(this.RunUntil)); }\n"
        u8"  void RunFault() { wait(0.2f); int zero = 0; p = 1 / zero; }\n"
        u8"  void beginFault() { startCoroutine(ScriptCoroutine(this.RunFault)); }\n"
        u8"  bool InScope() { return gate && CallScope::inCall() == 1; }\n"
        u8"  void RunScoped() { Coroutine::waitUntil(CoroutinePredicate(this.InScope)); p = 1; }\n"
        u8"  void beginScoped() { startCoroutine(ScriptCoroutine(this.RunScoped)); }\n"
        u8"  bool Faulting() { int zero = 0; return 1 / zero == 1; }\n"
        u8"  void RunBad() { Coroutine::waitUntil(CoroutinePredicate(this.Faulting)); p = 1; }\n"
        u8"  void beginBadPredicate() { startCoroutine(ScriptCoroutine(this.RunBad)); }\n"
        u8"}\n";
    // Debugger: a zero-arg entry. Line 1 `gLast`, line 2 the signature, line 3 sets `tag`,
    // line 4 (the breakpoint) has `tag == "hit"` in scope; a step lands on line 5, and
    // continue runs to completion.
    dialect.debugModule = u8"int gLast = 0;\n"          // 1
                          u8"void debugRun() {\n"       // 2
                          u8"  string tag = \"hit\";\n" // 3
                          u8"  int a = 7;\n"            // 4  <- breakpoint (tag in scope, == "hit")
                          u8"  gLast = a;\n"            // 5
                          u8"}\n";                      // 6
    dialect.debugSection = u8"debug.script";
    dialect.debugCoroutineModule = u8"int gDone = 0;\n"                                      // 1
                                   u8"class Runner {\n"                                      // 2
                                   u8"  void Body() {\n"                                     // 3
                                   u8"    wait(0.1f);\n"                                     // 4
                                   u8"    int here = 1;\n"                                   // 5 <- breakpoint
                                   u8"    gDone = here;\n"                                   // 6
                                   u8"  }\n"                                                 // 7
                                   u8"}\n"                                                   // 8
                                   u8"Runner@ gRunner = Runner();\n"                         // 9
                                   u8"void debugCoroutine() { startCoroutine(ScriptCoroutine(gRunner.Body)); }\n";
    dialect.debugCoroutineBreakLine = 5;
    dialect.debugFunction = u8"debugRun";
    dialect.debugBreakLine = 4;
    dialect.debugLocalName = u8"tag";
    dialect.debugLocalValue = u8"\"hit\"";

    foundation::script::conformance::RunScriptBackendConformance(
        []() { return angelscript::CreateScriptManager(foundation::core::DefaultAllocator()); }, dialect);
}

TEST_CASE("angelscript: declares Coroutines + Delegates + Debugger + Bytecode; profiler absent (B4)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Delegates));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Debugger));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Bytecode));
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Profiler));
    CHECK(manager->CreateDebugger().Get() != nullptr); // Debugger declared, so a real factory
    CHECK(manager->CreateProfiler().Get() == nullptr);
}

TEST_CASE("angelscript: bytecode capability - compile to blob, serialize, load in the player")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());

    // Cook side: compile source (module-level functions) to an opaque bytecode blob.
    constexpr StringView kSource = u8"double add(double a, double b) { return a + b; }\n"
                                   u8"double answer() { return 42.0; }\n";
    Result<RefPtr<IScriptBlob>> compiled = manager->CompileToBlob(kSource, u8"as.blob");
    REQUIRE(compiled.HasValue());
    REQUIRE(compiled.Value().Get() != nullptr);

    // Store and reconstruct: the blob serializes to bytes; a fresh blob deserializes them.
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        compiled.Value()->Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    RefPtr<IScriptBlob> reloaded = manager->CreateBlob();
    REQUIRE(reloaded.Get() != nullptr);
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        reloaded->Serialize(reader);
    }

    // Player side: LoadByteCode into a context (no Build) and call the restored functions.
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->LoadBlob(*reloaded).IsOk());
    Variant args[] = {Variant::From<f64>(2.0), Variant::From<f64>(3.0)};
    CHECK(scriptContext->Call(u8"add", Span<Variant>{args, 2}).Value().Get<f64>() == doctest::Approx(5.0));
    CHECK(scriptContext->Call(u8"answer", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(42.0));
}

TEST_CASE("angelscript: CompileToBlob rejects a broken source at cook")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    CHECK_FALSE(manager->CompileToBlob(u8"int broken( {", u8"as.broken").HasValue());
}

TEST_CASE("angelscript: a script function is a native callback via IScriptDelegate (the real use)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    // A behavior subscribes a function handle to a native event; native code fires it.
    REQUIRE(scriptContext->Load(u8"double add5(double x) { return x + 5; }\n"
                      u8"DelegateSignal@ signal = DelegateSignal();\n"
                      u8"void main() { signal.Connect(ScriptDelegate(add5)); }\n",
                      u8"main")
                .IsOk());

    Variant signalVar = scriptContext->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(signal->Emit(10.0) == doctest::Approx(15.0));   // native fires, the function runs
    CHECK(signal->Emit(100.0) == doctest::Approx(105.0)); // reusable across firings
}

// A reflected static that resolves a per-context service through CurrentScriptContext(), the
// shape a facade uses to reach an engine singleton without a process global. If a delegate
// callback does not scope its owning context, CurrentScriptContext() is null during the call and
// this silently does nothing; with the context scoped it resolves and writes the flag.
namespace
{
    class ContextProbe : public Object
    {
        RTTI_OBJECT(ContextProbe, Object)
    public:
        void Poke() const
        {
            if (int* flag = CurrentService<int>())
            {
                *flag = 42;
            }
        }
    };
}

REFLECT_MEMBERS(ContextProbe, "rtti::script::test")
{
    builder.Method<&ContextProbe::Poke>("Poke");
    builder.Constructor();
}

TEST_CASE("angelscript: a delegate callback scopes its owning context (facades resolve services)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    manager->RegisterType(ContextProbe::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    conformance::CapturedErrors errors;
    scriptContext->SetErrorHandler(&errors);

    // The host service the handler's facade will resolve (engine services reach scripts through
    // CurrentService, never a process global).
    int flag = 0;
    ScriptServices services;
    services.Add(flag);
    scriptContext->SetServiceResolver(services.Resolver());

    // A script function subscribed to a native signal; its body calls the reflected static that
    // reads CurrentService and writes through it.
    REQUIRE(scriptContext->Load(u8"double onFire(double x) { ContextProbe p; p.Poke(); return x; }\n"
                      u8"DelegateSignal@ signal = DelegateSignal();\n"
                      u8"void main() { signal.Connect(ScriptDelegate(onFire)); }\n",
                      u8"main")
                .IsOk());
    CHECK(errors.count == 0);

    Variant signalVar = scriptContext->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(flag == 0);   // not fired yet
    signal->Emit(1.0);  // native fires the delegate; Invoke scopes the owning context
    CHECK(flag == 42);  // the handler's facade resolved this context's service
}

TEST_CASE("angelscript: a facade's delegate shape is a type scripts use without declaring it")
{
    // A facade registers the function type its callbacks take (Net.on's handlers, say); a manager
    // made after declares it, so a script names it like the built-in Action.
    RegisterDelegateShape(u8"double ProbeShape(double a, double b)");
    RegisterDelegateShape(u8"double ProbeShape(double a, double b)"); // again: kept once
    usize found = 0;
    for (const String& shape : DelegateShapes())
    {
        found += shape.AsView() == StringView(u8"double ProbeShape(double a, double b)") ? 1u : 0u;
    }
    CHECK(found == 1u);

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    conformance::CapturedErrors errors;
    scriptContext->SetErrorHandler(&errors);
    REQUIRE(scriptContext->Load(u8"double add(double a, double b) { return a + b; }\n"
                      u8"double Sum = 0;\n"
                      u8"void main() { ProbeShape @f = add; Sum = f(2, 3); }\n",
                      u8"main")
                .IsOk());
    CHECK(errors.count == 0);
    CHECK(scriptContext->GetGlobal(u8"Sum").Get<f64>() == doctest::Approx(5.0));
}

TEST_CASE("angelscript: a METHOD delegate is a native callback via IScriptDelegate")
{
    // A delegate to a method (`ScriptDelegate(h.bump)`, an asFUNC_DELEGATE that binds an object
    // and its method) through IScriptDelegate. A global-function delegate is covered above; this
    // covers the bound-method form: it must reach MakeAngelScriptDelegateVariant through
    // ValueFromArg and survive AddRef, then invoke on the captured object so the object's field
    // is mutated.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    conformance::CapturedErrors errors;
    scriptContext->SetErrorHandler(&errors);

    // A script class whose method matches the ScriptDelegate funcdef (double(double)); a bound
    // method delegate to a live instance is subscribed to the native signal.
    REQUIRE(scriptContext->Load(u8"class Handler { double v = 0;\n"
                      u8"  double bump(double x) { v += x; return v; } }\n"
                      u8"Handler@ h = Handler();\n"
                      u8"DelegateSignal@ signal = DelegateSignal();\n"
                      u8"void main() { signal.Connect(ScriptDelegate(h.bump)); }\n",
                      u8"main")
                .IsOk());
    CHECK(errors.count == 0);

    Variant signalVar = scriptContext->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(signal->Emit(10.0) == doctest::Approx(10.0));  // native fires, h.bump ran (v: 0 -> 10)
    CHECK(signal->Emit(5.0) == doctest::Approx(15.0));   // same bound object accumulates (v -> 15)
}

TEST_CASE("angelscript: many engine lifecycles in one process (the cook pattern)")
{
    // The cook builds one engine per script; interleaved create and destroy could unbalance
    // the process-global thread manager and assert at teardown. The constructor's
    // process-lifetime pin makes any sequence safe; this exercises the multi-engine path.
    for (int i = 0; i < 8; ++i)
    {
        RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
        REQUIRE(manager.Get() != nullptr);
        manager->FinalizeTypes();
        RefPtr<IScriptContext> scriptContext = manager->CreateContext();
        REQUIRE(scriptContext.Get() != nullptr);
        CHECK(scriptContext->Load(u8"void main() { }", u8"cook_cycle").IsOk());
    }
}

namespace
{
    struct AsBudgetErrors final : IScriptErrorHandler
    {
        Array<String> messages;
        void OnError(const ScriptError& error) override { messages.PushBack(String(error.message)); }
        [[nodiscard]] bool Any(StringView needle) const
        {
            for (const String& m : messages)
                if (m.AsView().Find(needle) != StringView::kNotFound)
                    return true;
            return false;
        }
    };
}

TEST_CASE("angelscript: a call past its time budget is stopped, and the context goes on")
{
    // A script loop that never ended hung the game.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    ScriptBudget budget;
    budget.callSeconds = 0.5;
    manager->SetBudget(budget);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    AsBudgetErrors errors;
    scriptContext->SetErrorHandler(&errors);
    REQUIRE(scriptContext->Load(u8"class Probe {\n"
                                u8"  void forever() { int i = 0; while (true) { i++; } }\n"
                                u8"  int deep(int n) { return deep(n + 1) + 1; }\n"
                                u8"  double fine() { return 7; }\n"
                                u8"}\n",
                                u8"budget")
                .IsOk());
    RefPtr<BehaviorObject> probe = scriptContext->CreateInstance(u8"Probe", Span<Variant>{});
    REQUIRE(static_cast<bool>(probe));

    const Stopwatch clock = Stopwatch::StartNew();
    CHECK_FALSE(probe->Invoke(u8"forever", Span<Variant>{}).HasValue());
    const f64 took = clock.Elapsed().AsSeconds();
    MESSAGE("stopped after ", took, " s");
    CHECK(took >= 0.5);
    CHECK(took < 2.0);
    CHECK(errors.Any(u8"budget"));
    CHECK(probe->Invoke(u8"fine", Span<Variant>{}).Value().Get<f64>() == 7.0); // the context goes on

    // Again, on a pooled context that keeps the budget's line callback from the calls before (it
    // is set once per context, not per call): still stopped.
    const Stopwatch again = Stopwatch::StartNew();
    CHECK_FALSE(probe->Invoke(u8"forever", Span<Variant>{}).HasValue());
    CHECK(again.Elapsed().AsSeconds() >= 0.5);
    CHECK(again.Elapsed().AsSeconds() < 2.0);
    CHECK(probe->Invoke(u8"fine", Span<Variant>{}).Value().Get<f64>() == 7.0);

    // Recursion that never ends fails its call at the stack limit instead of taking memory. The time
    // budget is raised first: a sanitizer build is slow enough to reach it before the limit.
    budget.callSeconds = 60.0;
    manager->SetBudget(budget);
    Variant zero[] = {Variant::From(0)};
    const Stopwatch deepClock = Stopwatch::StartNew();
    CHECK_FALSE(probe->Invoke(u8"deep", Span<Variant>{zero, 1}).HasValue());
    MESSAGE("stack limit reached after ", deepClock.Elapsed().AsSeconds(), " s");
    CHECK(errors.Any(u8"Stack overflow"));
    CHECK(probe->Invoke(u8"fine", Span<Variant>{}).Value().Get<f64>() == 7.0);
}

TEST_CASE("angelscript: a global initializer runs at load under the budget, never in a compile")
{
    // Build ran the global initializers on the engine's own context: one that never returned hung
    // the load, and the cook's compile ran them too.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    ScriptBudget budget;
    budget.callSeconds = 0.5;
    manager->SetBudget(budget);
    const StringView looping = u8"int forever() { while (true) {} return 1; }\nint x = forever();\n";

    // The cook's compile and a check build it without running a line of it.
    const Stopwatch compileClock = Stopwatch::StartNew();
    CHECK(manager->CompileToBlob(looping, u8"globals").HasValue());
    AsBudgetErrors none;
    CHECK(manager->Check(looping, u8"globals", none));
    CHECK(compileClock.Elapsed().AsSeconds() < 0.4);

    // A load runs the initializer, and stops it at the budget.
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    AsBudgetErrors errors;
    scriptContext->SetErrorHandler(&errors);
    CHECK_FALSE(scriptContext->Load(looping, u8"globals").IsOk());
    CHECK(errors.Any(u8"budget"));

    // An initializer that returns still sets its global.
    RefPtr<IScriptContext> fine = manager->CreateContext();
    REQUIRE(fine->Load(u8"int seven() { return 7; }\nint x = seven();\ndouble read() { return x; }\n", u8"globals.ok").IsOk());
    CHECK(fine->Call(u8"read", Span<Variant>{}).Value().Get<f64>() == 7.0);
}

TEST_CASE("angelscript: a compiler warning reaches the error handler as a warning, and the load succeeds")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    struct Kinds final : IScriptErrorHandler
    {
        int warnings = 0, others = 0;
        void OnError(const ScriptError& error) override { ++(error.kind == ScriptErrorKind::Warning ? warnings : others); }
    } kinds;
    scriptContext->SetErrorHandler(&kinds);
    CHECK(scriptContext->Load(u8"int f() { int a = 1; { int a = 2; return a; } }\n", u8"shade").IsOk());
    CHECK(kinds.warnings == 1);
    CHECK(kinds.others == 0);
    scriptContext->SetErrorHandler(nullptr);
}

namespace
{
    // A native function past the parameters a call carries, beside one within them.
    class Wide : public Object
    {
        RTTI_OBJECT(Wide, Object)
    public:
        [[nodiscard]] static f64 Sum17(f64 a, f64 b, f64 c, f64 d, f64 e, f64 f, f64 g, f64 h, f64 i, f64 j, f64 k,
                                       f64 l, f64 m, f64 n, f64 o, f64 p, f64 q)
        {
            return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q;
        }
        [[nodiscard]] static f64 Sum2(f64 a, f64 b) { return a + b; }
    };

    struct LastError final : IScriptErrorHandler
    {
        String message;
        void OnError(const ScriptError& error) override { message = String(error.message); }
    };
}

REFLECT_MEMBERS(Wide, "rtti::script::test")
{
    builder.Constructor();
    builder.Method<&Wide::Sum17>("sum17");
    builder.Method<&Wide::Sum2>("sum2");
}

TEST_CASE("angelscript: past 16 parameters a call is refused, never made with arguments dropped")
{
    // A native method past the limit was bound and called with its last arguments dropped; a script
    // function past it was called the same way from native code.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Wide::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    LastError said;
    scriptContext->SetErrorHandler(&said);
    // The native method past the limit is not bound, so its use does not compile; the other is.
    CHECK_FALSE(scriptContext->Load(u8"double f() { return Wide::sum17(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17); }\n",
                                    u8"wide").IsOk());
    REQUIRE(scriptContext->Load(u8"double two() { return Wide::sum2(1, 2); }\n"
                                u8"double many(double a, double b, double c, double d, double e, double f, double g,\n"
                                u8"            double h, double i, double j, double k, double l, double m, double n,\n"
                                u8"            double o, double p, double q) { return q; }\n",
                                u8"calls").IsOk());
    CHECK(scriptContext->Call(u8"two", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(3.0));
    // A script function past the limit, called from native code: refused, and said.
    Variant args[17];
    for (Variant& arg : args)
    {
        arg = Variant::From<f64>(1.0);
    }
    CHECK_FALSE(scriptContext->Call(u8"many", Span<Variant>{args, 17}).HasValue());
    CHECK(said.message.AsView().Find(StringView(u8"a call carries at most 16")) != StringView::kNotFound);
    scriptContext->SetErrorHandler(nullptr);
}

// A class compiled from source gets its includes as the cook recorded them, each a section named
// by its path; the source's own #include lines are left out, keeping its other lines where they
// were. The runtime refused any source with an #include line.
TEST_CASE("angelscript: Load compiles the recorded includes beside the source, each its own section")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(DefaultAllocator())->CreateContext();
    struct Errors final : IScriptErrorHandler
    {
        String module;
        i32 line = -1;
        void OnError(const ScriptError& error) override
        {
            module = String(error.module);
            line = error.line;
        }
    } errors;
    scriptContext->SetErrorHandler(&errors);

    const ScriptSection numbers[] = {{u8"Shared/Numbers.as", u8"double twice(double v) { return v * 2; }\n"}};
    REQUIRE(scriptContext
                ->Load(u8"#include \"../Shared/Numbers.as\"\ndouble answer() { return twice(21); }\n", u8"Game/Boot.as",
                       Span<const ScriptSection>{numbers, 1})
                .IsOk());
    CHECK(scriptContext->Call(u8"answer", Span<Variant>{}).Value().Get<f64>() == 42.0);

    // A runtime fault inside the included file is said in that file, at its line.
    const ScriptSection faulty[] = {{u8"Shared/Divide.as", u8"int one() { return 1; }\nint divide(int v) { int z = 0; return v / z; }\n"}};
    REQUIRE(scriptContext
                ->Load(u8"#include \"../Shared/Divide.as\"\nint run() { return divide(4); }\n", u8"Game/Run.as",
                       Span<const ScriptSection>{faulty, 1})
                .IsOk());
    CHECK_FALSE(scriptContext->Call(u8"run", Span<Variant>{}).HasValue());
    CHECK(errors.module == u8"Shared/Divide.as");
    CHECK(errors.line == 2);
    scriptContext->SetErrorHandler(nullptr);
}

TEST_CASE("angelscript: a behaviour class's includes load with it, and a breakpoint in one names that file")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const ScriptSection steps[] = {{u8"Shared/Steps.as",
                                    u8"double step(double x)\n" // 1
                                    u8"{\n"                     // 2
                                    u8"    return x + 1;\n"     // 3 <- breakpoint
                                    u8"}\n"}};
    const BehaviorModuleClass classes[] = {
        {u8"Mover.as",
         u8"#include \"Shared/Steps.as\"\nclass Mover { double x = 0; void tick() { x = step(x); } }\n",
         Span<const ScriptSection>{steps, 1}, {}},
    };
    REQUIRE(scriptContext->LoadBehaviorModule(Span<const BehaviorModuleClass>{classes, 1}, u8"behaviors#1").IsOk());

    struct Sink final : IScriptDebuggerListener
    {
        ScriptDebuggerState last = ScriptDebuggerState::Running;
        void OnDebuggerStateChanged(ScriptDebuggerState state) override { last = state; }
    } sink;
    debugger->SetListener(&sink);
    debugger->SetBreakpoint(u8"Shared/Steps.as", 3);
    RefPtr<BehaviorObject> mover = scriptContext->CreateInstance(u8"Mover", Span<Variant>{});
    REQUIRE(static_cast<bool>(mover));
    (void)mover->Invoke(u8"tick", Span<Variant>{});
    CHECK(sink.last == ScriptDebuggerState::Breakpoint);
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(StringView(frames[0].file) == u8"Shared/Steps.as");
    CHECK(frames[0].line == 3);
    debugger->Continue();
    debugger->SetListener(nullptr);
}

// A reload loads a class beside its predecessor. The predecessor's module was kept until the
// context closed; discarding it resets its globals, so it goes once nothing runs its code.
namespace
{
    [[nodiscard]] asUINT ModuleCount(IScriptManager& manager)
    {
        return static_cast<asIScriptEngine*>(angelscript::AngelScriptEngineHandle(manager))->GetModuleCount();
    }

    [[nodiscard]] Status LoadCounter(IScriptContext& scriptContext, StringView source, StringView moduleName)
    {
        const BehaviorModuleClass entry{u8"Counter.as", source, {}, {}};
        return scriptContext.LoadBehaviorModule(Span<const BehaviorModuleClass>{&entry, 1}, moduleName);
    }

    [[nodiscard]] f64 Next(BehaviorObject& counter, f64 value)
    {
        Variant args[] = {Variant::From<f64>(value)};
        return counter.Invoke(u8"next", Span<Variant>{args, 1}).Value().Get<f64>();
    }

    constexpr StringView kCounterBy3 = u8"double g_step = 3;\nclass Counter { double next(double v) { return v + g_step; } }\n";
    constexpr StringView kCounterBy10 = u8"double g_step = 10;\nclass Counter { double next(double v) { return v + g_step; } }\n";
}

TEST_CASE("angelscript: a replaced class's module stays while an old instance lives, and goes with the last")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(LoadCounter(*scriptContext, kCounterBy3, u8"behaviors#1").IsOk());
    const asUINT loaded = ModuleCount(*manager);
    RefPtr<BehaviorObject> old = scriptContext->CreateInstance(u8"Counter", Span<Variant>{});
    REQUIRE(static_cast<bool>(old));

    REQUIRE(LoadCounter(*scriptContext, kCounterBy10, u8"behaviors#2").IsOk());
    CHECK(ModuleCount(*manager) == loaded + 1); // the old instance still runs the old module
    CHECK(Next(*old, 1.0) == 4.0);              // and its globals are intact
    RefPtr<BehaviorObject> fresh = scriptContext->CreateInstance(u8"Counter", Span<Variant>{});
    REQUIRE(static_cast<bool>(fresh));
    CHECK(Next(*fresh, 1.0) == 11.0);

    old = nullptr;
    CHECK(ModuleCount(*manager) == loaded); // the last old instance gone, so is its module
    CHECK(Next(*fresh, 1.0) == 11.0);
}

TEST_CASE("angelscript: a replaced class's module with no instance goes at once; one with another class stays")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(LoadCounter(*scriptContext, kCounterBy3, u8"behaviors#1").IsOk());
    const asUINT loaded = ModuleCount(*manager);
    REQUIRE(LoadCounter(*scriptContext, kCounterBy10, u8"behaviors#2").IsOk());
    CHECK(ModuleCount(*manager) == loaded);

    // A module that also declares a class the newer one does not is not replaced.
    REQUIRE(LoadCounter(*scriptContext,
                        u8"class Counter { double next(double v) { return v; } }\nclass Other {}\n",
                        u8"behaviors#3")
                .IsOk());
    REQUIRE(LoadCounter(*scriptContext, kCounterBy3, u8"behaviors#4").IsOk());
    CHECK(ModuleCount(*manager) == loaded + 1);
    RefPtr<BehaviorObject> other = scriptContext->CreateInstance(u8"Other", Span<Variant>{});
    CHECK(static_cast<bool>(other));
}

TEST_CASE("angelscript: an object finds each method once and keeps calling the right overload")
{
    RefPtr<IScriptContext> scriptContext = angelscript::CreateScriptManager(DefaultAllocator())->CreateContext();
    REQUIRE(scriptContext
                ->Load(u8"class Pick { double f() { return 1; } double f(double v) { return v + 2; } }\n", u8"pick")
                .IsOk());
    RefPtr<BehaviorObject> pick = scriptContext->CreateInstance(u8"Pick", Span<Variant>{});
    REQUIRE(static_cast<bool>(pick));
    Variant five[] = {Variant::From<f64>(5.0)};
    for (int round = 0; round < 2; ++round) // the second round runs from what the first found
    {
        CHECK(pick->Invoke(u8"f", Span<Variant>{}).Value().Get<f64>() == 1.0);
        CHECK(pick->Invoke(u8"f", Span<Variant>{five, 1}).Value().Get<f64>() == 7.0);
        CHECK(pick->Invoke(u8"missing", Span<Variant>{}).Error() == ErrorCode::NotFound);
    }
}

TEST_CASE("angelscript: a frame's collector step stops at its time, and steps finish the cycle")
{
    // Every frame ran a full collection, however much garbage there was.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext
                ->Load(u8"class Node { Node@ next; }\n"
                       u8"void churn(double n) { for (double i = 0; i < n; i++) { Node a; Node b; @a.next = b; @b.next = a; } }\n",
                       u8"churn")
                .IsOk());
    manager->CollectGarbage();
    Variant count[] = {Variant::From<f64>(2000.0)};
    REQUIRE(scriptContext->Call(u8"churn", Span<Variant>{count, 1}).HasValue());
    asIScriptEngine* engine = static_cast<asIScriptEngine*>(angelscript::AngelScriptEngineHandle(*manager));
    asUINT held = 0;
    engine->GetGCStatistics(&held);
    REQUIRE(held >= 2000u);

    manager->StepGarbage(0.0); // one step: a little of the work, never all of it
    asUINT afterOne = 0;
    engine->GetGCStatistics(&afterOne);
    CHECK(afterOne > 0u);

    for (int frame = 0; frame < 1000 && afterOne > 0u; ++frame)
    {
        manager->StepGarbage(0.01);
        engine->GetGCStatistics(&afterOne);
    }
    CHECK(afterOne == 0u);
}

TEST_CASE("angelscript: a script that asks for more memory than the budget fails its call, and the context goes on")
{
    // AngelScript's memory was malloc's, without a cap: a script that grew without end took the
    // machine's memory. The cap is the manager's heap budget (process-wide for AngelScript).
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    ScriptBudget budget;
    budget.heapBytes = usize{64} << 20;
    manager->SetBudget(budget);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    AsBudgetErrors errors;
    scriptContext->SetErrorHandler(&errors);
    REQUIRE(scriptContext
                ->Load(u8"double grow() { array<double> a; a.resize(100000000); return a.length(); }\n"
                       u8"double small() { array<double> a; a.resize(1000); return a.length(); }\n",
                       u8"memory")
                .IsOk());
    CHECK_FALSE(scriptContext->Call(u8"grow", Span<Variant>{}).HasValue()); // 800 MB asked for
    CHECK(errors.Any(u8"Out of memory"));
    CHECK(scriptContext->Call(u8"small", Span<Variant>{}).Value().Get<f64>() == 1000.0); // the context goes on
    scriptContext->SetErrorHandler(nullptr);
    manager->SetBudget(ScriptBudget{}); // the cap is process-wide: leave it as the other tests expect
}
