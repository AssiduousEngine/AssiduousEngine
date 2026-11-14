// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// script.pipeline's neutral cook helpers: the class-name/handler scan and the shared
// startCoroutine detection. These are pure text scanners in the backend-neutral Pipeline::Script
// library (no VM, no language), so they always build. The per-language neutral-builder cook
// round-trips (source through builder to factory) live in AngelScriptPipelineTests.cpp and
// LuauPipelineTests.cpp, each compiled only when its backend is on.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import pipeline.core;
import foundation.vfs; // SourcePath
import script.pipeline;

using namespace foundation::core;
using namespace pipeline;

TEST_CASE("script.pipeline: the class scan lists every top-level class in order, comments ignored")
{
    const Array<String> two = ListScriptClassNames(u8"class Helper {\n}\nclass Mover {\n}\n");
    REQUIRE(two.Size() == 2u);
    CHECK(two[0] == u8"Helper");
    CHECK(two[1] == u8"Mover");
    const Array<String> real = ListScriptClassNames(u8"// class Fake {\n/* class Ghost { */\nclass Real {\n}\n");
    REQUIRE(real.Size() == 1u);
    CHECK(real[0] == u8"Real");
    CHECK(ListScriptClassNames(u8"var x = 1\n").IsEmpty());
}

// The class a script plays: the preferred one (the asset's, else its file's stem), else its only
// one; several and none preferred is never a guess, but a problem naming them; none is a utility
// module.
TEST_CASE("script.pipeline: the class a script plays is the preferred one or its only one, never a guess")
{
    const Array<String> several = ListScriptClassNames(u8"class EffectDef {}\nclass Fighter {}\nclass Battle {}\n");
    const Span<const String> declared(several.Data(), several.Size());
    String problem;
    CHECK(ChooseScriptClass(declared, u8"Battle", problem) == u8"Battle");
    CHECK(problem.IsEmpty());
    CHECK(ChooseScriptClass(declared, u8"Battle.2", problem).IsEmpty()); // a copy's stem names none
    CHECK(problem.AsView().Contains(u8"EffectDef, Fighter, Battle"));
    CHECK(problem.AsView().Contains(u8"'Battle.2'"));

    const Array<String> one = ListScriptClassNames(u8"class Game {}\n");
    CHECK(ChooseScriptClass(Span<const String>(one.Data(), one.Size()), u8"Lamplight", problem) == u8"Game");
    CHECK(problem.IsEmpty());
    CHECK(ChooseScriptClass(Span<const String>{}, u8"Twice", problem).IsEmpty());
    CHECK(problem.IsEmpty());

    // An asset prefers the class it records, else the one named like its file.
    ScriptClassAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"Battle.2.as");
    CHECK(PreferredScriptClass(asset) == u8"Battle.2");
    asset.className = String(u8"Battle");
    CHECK(PreferredScriptClass(asset) == u8"Battle");
}

TEST_CASE("script.pipeline: handler scan finds declared handlers only (comments stripped)")
{
    const Array<String> handlers = ScanScriptHandlers(u8"class A {\n"
                                                      u8"    onStart() {}\n"
                                                      u8"    onUpdate(dt) {}\n"
                                                      u8"    // onDestroy() would be nice\n"
                                                      u8"    /* onEnable() {} */\n"
                                                      u8"}\n");
    auto has = [&handlers](StringView name)
    {
        for (const String& h : handlers)
        {
            if (h.AsView() == name)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has(u8"onStart"));
    CHECK(has(u8"onUpdate"));
    CHECK_FALSE(has(u8"onDestroy"));
    CHECK_FALSE(has(u8"onEnable"));
    CHECK_FALSE(has(u8"onDisable"));
}

TEST_CASE("script.pipeline: handler scan captures the whole on<Upper>(...) convention - "
          "custom message + event handlers, not lookalikes (P2)")
{
    const Array<String> handlers =
        ScanScriptHandlers(u8"class A {\n"
                           u8"    onStart() {}\n"
                           u8"    onHeal(amount) {}\n" // custom message handler (entity.send)
                           u8"    onContactBegin(o, p, n) {}\n" // physics event handler
                           u8"    onlyOnce() {}\n" // lowercase after 'on' - NOT a handler
                           u8"    onFoo {}\n"      // getter (no parens) - NOT a handler
                           u8"    speed=(v) {}\n"  // setter - NOT a handler
                           u8"}\n");
    auto has = [&handlers](StringView name)
    {
        for (const String& h : handlers)
        {
            if (h.AsView() == name)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has(u8"onStart"));
    CHECK(has(u8"onHeal"));
    CHECK(has(u8"onContactBegin"));
    CHECK_FALSE(has(u8"onlyOnce"));
    CHECK_FALSE(has(u8"onFoo"));
    CHECK(handlers.Size() == 3u);
}

TEST_CASE("script.pipeline: the shared startCoroutine( surface is detected neutrally "
          "(comments ignored) - both languages reuse it")
{
    // The shared coroutine-start token (a facade convention, not language syntax).
    CHECK(ScriptReferencesCoroutineStart(
        u8"class Mover {\n    onStart() { startCoroutine(Fn.new {}) }\n}\n"));
    CHECK(ScriptReferencesCoroutineStart(u8"void begin() { startCoroutine(@this.Run); }\n"));
    // A plain behavior does not reference it.
    CHECK_FALSE(ScriptReferencesCoroutineStart(u8"class Mover {\n    onUpdate(dt) {}\n}\n"));
    // Only in real code, not a comment.
    CHECK_FALSE(ScriptReferencesCoroutineStart(u8"// startCoroutine(nope)\nclass Mover {\n}\n"));
}

// The script surface hash a script product's recipe carries: it moves when a runtime type joins
// or leaves, not with the order the types registered in, and not for an editor-only type (which no
// player has), so every host that cooks agrees on it.
TEST_CASE("script.pipeline: the script surface hash follows the runtime types, in any order")
{
    const TypeInfo& asset = ScriptClassAsset::StaticType();
    const TypeInfo& source = Asset::StaticType();
    TypeRegistry one;
    one.Register(asset);
    const u64 alone = ScriptSurfaceHash(one);
    one.Register(source);
    const u64 both = ScriptSurfaceHash(one);
    CHECK(alone != both);

    TypeRegistry other;
    other.Register(source);
    other.Register(asset);
    CHECK(ScriptSurfaceHash(other) == both);

    other.Register(TypeOf<Float3>(), TypeDomain(StringView(u8"Editor")));
    CHECK(ScriptSurfaceHash(other) == both);

    other.Unregister(source.id);
    CHECK(ScriptSurfaceHash(other) == alone);
}

namespace
{
    // A type whose one method's signature the surface hash must see whole.
    class SignatureProbe : public Object
    {
        RTTI_OBJECT(SignatureProbe, Object)
    public:
        [[nodiscard]] f64 Take(f64 value) const { return value; }
    };
}

REFLECT_MEMBERS(SignatureProbe, "rtti::script::pipelinetest")
{
    builder.Constructor();
    builder.Method<&SignatureProbe::Take>("take");
}

TEST_CASE("script.pipeline: the script surface hash follows a signature's types and an enumerator's value")
{
    // The hash covered a method's name and parameter count: a parameter or return type changed at
    // the same count, or an enumerator's value, kept the hash, and the stale bytecode failed at load.
    const TypeInfo& probe = SignatureProbe::StaticType();
    REQUIRE(probe.methodCount == 1u);
    REQUIRE(probe.methods[0].paramCount == 1u);
    TypeRegistry original;
    original.Register(probe);
    const u64 before = ScriptSurfaceHash(original);

    // The same type, its method taking an i32: same name, same count.
    MethodInfo takesInt = probe.methods[0];
    ParamInfo intParam = takesInt.params[0];
    intParam.type = []() -> const TypeInfo* { return &TypeOf<i32>(); };
    takesInt.params = &intParam;
    TypeInfo changedParam = probe;
    changedParam.methods = &takesInt;
    TypeRegistry withParam;
    withParam.Register(changedParam);
    CHECK(ScriptSurfaceHash(withParam) != before);

    // Its method returning an i32 instead.
    MethodInfo returnsInt = probe.methods[0];
    returnsInt.returnType = []() -> const TypeInfo* { return &TypeOf<i32>(); };
    TypeInfo changedReturn = probe;
    changedReturn.methods = &returnsInt;
    TypeRegistry withReturn;
    withReturn.Register(changedReturn);
    CHECK(ScriptSurfaceHash(withReturn) != before);

    // An enumerator with another value under the same name.
    static const EnumValue one[] = {{"A", 1}};
    static const EnumValue two[] = {{"A", 2}};
    TypeInfo first = probe;
    first.methods = nullptr;
    first.methodCount = 0;
    first.enumerators = one;
    first.enumeratorCount = 1;
    TypeInfo second = first;
    second.enumerators = two;
    TypeRegistry firstRegistry;
    firstRegistry.Register(first);
    TypeRegistry secondRegistry;
    secondRegistry.Register(second);
    CHECK(ScriptSurfaceHash(firstRegistry) != ScriptSurfaceHash(secondRegistry));
}
