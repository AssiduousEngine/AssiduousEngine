// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// luau.pipeline tests: the Luau cook service in isolation. The tier starters, the
// construct-and-walk property harvest (number/bool/string/Float3, non-scalars skipped, sorted),
// the compile-check, and the shared startCoroutine coroutine opt-in, driven straight through
// IScriptLanguageCook (no project or VFS needed, since Cook takes source bytes directly). The
// neutral pipeline suite covers the full round-trip from source through builder to factory.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.script;
import foundation.script.resource;
import luau.foundation; // luau::CreateLuauScriptManager and luau::LuauBytecodeVersion (the player path)
import script.pipeline;
import luau.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::script;

namespace
{
    [[nodiscard]] IScriptLanguageCook* LuauCook()
    {
        luau::RegisterLuauScriptCook(); // registers the Luau backend and cook (idempotent)
        return ScriptLanguageCookRegistry::Get().FindByLanguage(u8"luau");
    }

    // A class whose constructor sets one field of each harvestable kind plus two that must be
    // skipped (a table, and the nil owner, which Lua drops from the instance entirely).
    constexpr StringView kMultiProp = u8R"lua(
Multi = EntityBehavior.class()
function Multi.new(entity)
    local self = setmetatable({}, Multi)
    self.speed = 2.5
    self.enabled = true
    self.label = "hello"
    self.offset = Float3.new(1, 2, 3)
    self.data = {}
    self.owner = entity
    return self
end
function Multi:onStart() end
)lua";

    // A class whose constructor dereferences the entity. The harvest constructs with a nil owner,
    // so this faults; the cook must still succeed (valid at runtime) with no properties.
    constexpr StringView kFaultyCtor = u8R"lua(
Risky = EntityBehavior.class()
function Risky.new(entity)
    local self = setmetatable({}, Risky)
    self.name = entity:name()
    return self
end
function Risky:onStart() end
)lua";

    constexpr StringView kCoroutineUser = u8R"lua(
Waiter = EntityBehavior.class()
function Waiter.new(entity)
    return setmetatable({ entity = entity }, Waiter)
end
function Waiter:onStart()
    startCoroutine(function() waitSeconds(1.0) end, self)
end
)lua";

    constexpr StringView kBroken = u8"function (\n";
}

TEST_CASE("luau.cook: registered + resolvable by language; the Behavior starter cooks with its "
          "harvested property + handlers")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);
    CHECK_FALSE(cook->NewAssetTemplate(foundation::script::BehaviorKind::Entity, {}).IsEmpty());

    CookScriptErrorSink sink;
    BehaviorClassSource out;
    REQUIRE(
        cook->Cook(cook->NewAssetTemplate(foundation::script::BehaviorKind::Entity, {}), u8"NewBehavior.luau", sink, out));
    CHECK(out.language == u8"luau");
    CHECK(out.className == u8"NewBehavior");
    CHECK(out.kind == BehaviorKind::Entity);
    CHECK(out.sourceName == u8"NewBehavior.luau");
    // `speed` is the one scalar field; `entity` is nil at harvest so Lua drops it.
    REQUIRE(out.properties.Size() == 1u);
    CHECK(out.properties[0].name == u8"speed");
    CHECK(out.properties[0].type == BehaviorPropertyType::Float);
    CHECK(out.properties[0].defaultValue.number == doctest::Approx(1.0));
    CHECK(out.handlers.Size() == 3u); // onStart, onUpdate, onDestroy
    CHECK_FALSE(out.usesCoroutines);
}

TEST_CASE("luau.cook: the scene and run behavior starters cook to a class named after the asset, of their kind")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);

    const String behavior = cook->NewAssetTemplate(foundation::script::BehaviorKind::Entity, {});
    const String level = cook->NewAssetTemplate(foundation::script::BehaviorKind::Scene, u8"Arena");
    const String game = cook->NewAssetTemplate(foundation::script::BehaviorKind::Run, u8"Heist");
    CHECK_FALSE(level.IsEmpty());
    CHECK_FALSE(game.IsEmpty());
    CHECK(level != behavior);
    CHECK(game != behavior);

    CookScriptErrorSink levelSink;
    BehaviorClassSource levelOut;
    REQUIRE(cook->Cook(level, u8"Arena.luau", levelSink, levelOut));
    CHECK(levelOut.className == u8"Arena");
    CHECK(levelOut.kind == BehaviorKind::Scene);

    CookScriptErrorSink gameSink;
    BehaviorClassSource gameOut;
    REQUIRE(cook->Cook(game, u8"Heist.luau", gameSink, gameOut));
    CHECK(gameOut.className == u8"Heist");
    CHECK(gameOut.kind == BehaviorKind::Run);
}

TEST_CASE("luau.cook: construct-and-walk harvest - each scalar kind, non-scalars skipped, sorted")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    BehaviorClassSource out;
    REQUIRE(cook->Cook(kMultiProp, u8"Multi.luau", sink, out));
    CHECK(out.className == u8"Multi");

    // Four scalars harvested (speed/enabled/label/offset); `data` (table) and `owner` (nil) skipped.
    // pairs() order is unspecified, so the cook sorts by name: enabled, label, offset, speed.
    REQUIRE(out.properties.Size() == 4u);
    CHECK(out.properties[0].name == u8"enabled");
    CHECK(out.properties[0].type == BehaviorPropertyType::Bool);
    CHECK(out.properties[0].defaultValue.boolean == true);
    CHECK(out.properties[1].name == u8"label");
    CHECK(out.properties[1].type == BehaviorPropertyType::String);
    CHECK(out.properties[1].defaultValue.text == u8"hello");
    CHECK(out.properties[2].name == u8"offset");
    CHECK(out.properties[2].type == BehaviorPropertyType::Vec3);
    CHECK(out.properties[2].defaultValue.vector.x == doctest::Approx(1.0f));
    CHECK(out.properties[2].defaultValue.vector.z == doctest::Approx(3.0f));
    CHECK(out.properties[3].name == u8"speed");
    CHECK(out.properties[3].type == BehaviorPropertyType::Float);
    CHECK(out.properties[3].defaultValue.number == doctest::Approx(2.5));
}

TEST_CASE("luau.cook: the class's replicated list marks its properties; an unknown name fails")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);
    {
        CookScriptErrorSink sink;
        BehaviorClassSource out;
        REQUIRE(cook->Cook(u8"Guest = EntityBehavior.class()\n"
                           u8"Guest.replicated = { 'activity', 'pace' }\n"
                           u8"function Guest.new(owner) local self = setmetatable({}, Guest)\n"
                           u8"  self.activity = 0; self.pace = 1.5; self.chatty = true; return self end\n"
                           u8"function Guest:onUpdate(dt) end\n",
                           u8"Guest.luau", sink, out));
        REQUIRE(out.properties.Size() == 3u); // the list itself is not a property
        for (const BehaviorProperty& p : out.properties)
        {
            CHECK(p.replicated == (p.name != u8"chatty"));
        }
    }
    {
        CookScriptErrorSink sink;
        BehaviorClassSource out;
        CHECK_FALSE(cook->Cook(u8"Guest = EntityBehavior.class()\n"
                               u8"Guest.replicated = { 'activty' }\n"
                               u8"function Guest.new(owner) local self = setmetatable({}, Guest)\n"
                               u8"  self.activity = 0; return self end\n",
                               u8"Guest.luau", sink, out));
        REQUIRE_FALSE(sink.errors.IsEmpty());
        CHECK(sink.errors[sink.errors.Size() - 1].message.AsView().ContainsIgnoreCase(u8"'activty'"));
    }
}

TEST_CASE("luau.cook: a constructor that faults on the nil owner cooks with no properties")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    BehaviorClassSource out;
    // The class compiles and loads fine; only the harvest construction faults, and the cook succeeds.
    REQUIRE(cook->Cook(kFaultyCtor, u8"Risky.luau", sink, out));
    CHECK(out.className == u8"Risky");
    CHECK(out.properties.IsEmpty());
}

TEST_CASE("luau.cook: the shared startCoroutine opt-in is harvested")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    BehaviorClassSource out;
    REQUIRE(cook->Cook(kCoroutineUser, u8"Waiter.luau", sink, out));
    CHECK(out.usesCoroutines);
}

TEST_CASE("luau.cook: a broken source fails the cook with a reported error")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    BehaviorClassSource out;
    CHECK_FALSE(cook->Cook(kBroken, u8"Broken.luau", sink, out));
    CHECK_FALSE(sink.errors.IsEmpty());
}

TEST_CASE("luau.cook: the cook stores loadable bytecode in the pack (the player path)")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    BehaviorClassSource out;
    REQUIRE(
        cook->Cook(cook->NewAssetTemplate(foundation::script::BehaviorKind::Entity, {}), u8"NewBehavior.luau", sink, out));
    // Luau ships bytecode into the pack (source stays for dev-mode hot reload).
    REQUIRE_FALSE(out.bytecode.IsEmpty());
    CHECK_FALSE(out.source.IsEmpty());

    // Player path: a fresh Luau VM reconstructs the blob from the stored bytes with no compiler
    // (CreateBlob, then Serialize(read)), loads it, and instantiates the class, showing the pack
    // carries runnable bytecode, not just source.
    RefPtr<IScriptManager> manager = luau::CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptBlob> blob = manager->CreateBlob();
    REQUIRE(blob.Get() != nullptr);
    {
        MemoryStream stream;
        (void)stream.Write(out.bytecode.Data(), out.bytecode.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(stream, SerializeMode::Read);
        blob->Serialize(reader);
    }
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->LoadBlob(*blob).IsOk());
    RefPtr<BehaviorObject> instance = scriptContext->CreateInstance(u8"NewBehavior", Span<Variant>{});
    CHECK(instance.Get() != nullptr);
}

TEST_CASE("luau.cook: the fingerprint carries the vendored Luau bytecode version")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);
    // A vendor bump changes luau::LuauBytecodeVersion(), which flows through the cook's CookVersion()
    // into the registry's combined version and the ScriptClassAssetBuilder fingerprint, recooking
    // every Luau pack. Non-zero (a real bytecode version) and equal to the vendored constant.
    CHECK(luau::LuauBytecodeVersion() != 0u);
    CHECK(cook->CookVersion() == luau::LuauBytecodeVersion());
    CHECK(ScriptLanguageCookRegistry::Get().CombinedCookVersion() >= luau::LuauBytecodeVersion());
}

namespace luauprobe
{
    // A reflected type scripts reach as a global, standing in for a plugin's facade.
    struct ProbeFacade
    {
        foundation::core::i32 unused = 0;
    };
    REFLECT_VALUE(ProbeFacade, "rtti::luauprobe")
    {
        builder.Property<&ProbeFacade::unused>("unused");
    }
}

// A Luau compile resolves no globals, so the check walks the source for globals that name a
// registered type the surface leaves out (a type of a plugin the platform lacks): such a script
// fails the check there, naming the global and its line, and passes against the full surface.
TEST_CASE("luau.cook: a check without a type fails a script naming it as a global, and only that one")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);
    luauprobe::RttiRegisterValue_ProbeFacade();
    const TypeInfo& probe = TypeOf<luauprobe::ProbeFacade>();
    GlobalTypeRegistry().Register(probe);
    const TypeId hidden[] = {probe.id};
    constexpr const char8_t* kUses = u8"local Probe = {}\n"
                                     u8"function Probe.launch()\n"
                                     u8"    print(ProbeFacade)\n"
                                     u8"end\n"
                                     u8"return Probe\n";
    String className;
    CookScriptErrorSink full;
    CHECK(cook->Check(kUses, u8"Probe.luau", full, className, Span<const TypeId>{}));
    CookScriptErrorSink without;
    CHECK_FALSE(cook->Check(kUses, u8"Probe.luau", without, className, Span<const TypeId>(hidden, 1)));
    REQUIRE(without.errors.Size() == 1u);
    CHECK(without.errors[0].line == 3);
    CHECK(without.errors[0].message.AsView().Contains(u8"'ProbeFacade' is not available here"));
    CookScriptErrorSink unrelated;
    CHECK(cook->Check(u8"local function twice(v) return v * 2 end\nreturn twice\n", u8"Twice.luau", unrelated,
                      className, Span<const TypeId>(hidden, 1)));
    GlobalTypeRegistry().Unregister(probe.id);
}

TEST_CASE("luau.cook: a class made from a kind records it and constructs as a class; a plain table records None")
{
    IScriptLanguageCook* cook = LuauCook();
    REQUIRE(cook != nullptr);
    struct Case
    {
        const char8_t* source;
        const char8_t* asset;
        BehaviorKind kind;
        bool constructs = true; // the runtime finds the class and its constructor runs
    };
    // Each class sets a field in its constructor, which the harvest reads by constructing it: the
    // class made by `<Kind>.class()` is a working metatable for its instances.
    const Case cases[] = {
        {u8"Game = RunBehavior.class()\n"
         u8"function Game.new() return setmetatable({ lives = 3 }, Game) end\n",
         u8"Game.luau", BehaviorKind::Run},
        {u8"-- a comment first\nArena  =  SceneBehavior.class()\n"
         u8"function Arena.new(scene) return setmetatable({ lives = 3 }, Arena) end\n",
         u8"Arena.luau", BehaviorKind::Scene},
        {u8"Mover = EntityBehavior.class()\n"
         u8"function Mover.new(entity) return setmetatable({ lives = 3 }, Mover) end\n"
         u8"function Mover:onUpdate(dt) end\n",
         u8"Mover.luau", BehaviorKind::Entity},
        {u8"Helper = {}\nHelper.__index = Helper\n"
         u8"function Helper.new() return setmetatable({ lives = 3 }, Helper) end\n",
         u8"Helper.luau", BehaviorKind::None, false},
        // A local class is not one the runtime finds: it declares no kind.
        {u8"local Hidden = EntityBehavior.class()\n"
         u8"function Hidden.new(entity) return setmetatable({ lives = 3 }, Hidden) end\n",
         u8"Hidden.luau", BehaviorKind::None, false},
    };
    for (const Case& c : cases)
    {
        CookScriptErrorSink sink;
        BehaviorClassSource out;
        REQUIRE(cook->Cook(StringView(c.source), StringView(c.asset), sink, out));
        CHECK(out.kind == c.kind);
        REQUIRE(out.properties.Size() == (c.constructs ? 1u : 0u));
        if (c.constructs)
        {
            CHECK(out.properties[0].name == u8"lives");
        }
    }
}
