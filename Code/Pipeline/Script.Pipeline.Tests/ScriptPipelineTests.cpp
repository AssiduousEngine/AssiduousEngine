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
import foundation.script; // BehaviorKind
import pipeline.core;
import foundation.vfs; // SourcePath
import script.pipeline;
import foundation.script.resource; // BehaviorClassSource
import foundation.content; // the cooked database a build writes into

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

// The class a script plays: the one deriving from a kind, of any name; its helpers derive from none.
// Two deriving from a kind is never a guess, but a problem naming them; none is a module of helpers.
TEST_CASE("script.pipeline: the class a script plays is its one class deriving from a kind, never a guess")
{
    using foundation::script::BehaviorKind;
    const DeclaredScriptClass helpers[] = {{String(u8"EffectDef"), BehaviorKind::None},
                                           {String(u8"Battle"), BehaviorKind::Entity},
                                           {String(u8"Fighter"), BehaviorKind::None}};
    BehaviorKind kind = BehaviorKind::None;
    String problem;
    CHECK(ChooseBehaviorClass(Span<const DeclaredScriptClass>(helpers, 3), kind, problem) == u8"Battle");
    CHECK(kind == BehaviorKind::Entity);
    CHECK(problem.IsEmpty());

    const DeclaredScriptClass two[] = {{String(u8"Battle"), BehaviorKind::Entity}, {String(u8"Heist"), BehaviorKind::Run}};
    CHECK(ChooseBehaviorClass(Span<const DeclaredScriptClass>(two, 2), kind, problem).IsEmpty());
    CHECK(kind == BehaviorKind::None);
    CHECK(problem.AsView().Contains(u8"Battle, Heist"));

    const DeclaredScriptClass none[] = {{String(u8"EffectDef"), BehaviorKind::None}};
    CHECK(ChooseBehaviorClass(Span<const DeclaredScriptClass>(none, 1), kind, problem).IsEmpty());
    CHECK(problem.IsEmpty());
    CHECK(ChooseBehaviorClass(Span<const DeclaredScriptClass>{}, kind, problem).IsEmpty());
    CHECK(problem.IsEmpty());
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

TEST_CASE("script.pipeline: an include resolves against the file that has it, never outside the sources")
{
    String out;
    CHECK(ResolveScriptInclude(u8"Game/Boot.as", u8"Rules.as", out));
    CHECK(out == u8"Game/Rules.as");
    CHECK(ResolveScriptInclude(u8"Game/Boot.as", u8"../Shared/Numbers.as", out));
    CHECK(out == u8"Shared/Numbers.as");
    CHECK(ResolveScriptInclude(u8"Boot.as", u8"./Lib\\Steps.as", out));
    CHECK(out == u8"Lib/Steps.as");
    CHECK(ResolveScriptInclude(u8"Game/Deep/A.as", u8"../../B.as", out));
    CHECK(out == u8"B.as");
    CHECK_FALSE(ResolveScriptInclude(u8"Boot.as", u8"../Outside.as", out));
    CHECK_FALSE(ResolveScriptInclude(u8"Game/Boot.as", u8"../../Outside.as", out));
    CHECK_FALSE(ResolveScriptInclude(u8"Boot.as", u8"/etc/passwd", out));
    CHECK_FALSE(ResolveScriptInclude(u8"Boot.as", u8"C:/Windows/x.as", out));
}

namespace
{
    // A cook that does nothing: the registry test needs only its presence.
    struct IdleCook final : IScriptLanguageCook
    {
        [[nodiscard]] String NewAssetTemplate(foundation::script::BehaviorKind, StringView) const override { return {}; }
        [[nodiscard]] bool Cook(StringView, StringView, CookScriptErrorSink&, foundation::script::BehaviorClassSource&,
                                IScriptIncludeReader* = nullptr) override
        {
            return false;
        }
        [[nodiscard]] bool Check(StringView, StringView, CookScriptErrorSink&, String&, Span<const TypeId>,
                                 IScriptIncludeReader* = nullptr) override
        {
            return false;
        }
        void PrepareCheck() override {}
        void ListIncludes(StringView, Array<String>&) const override {}
        [[nodiscard]] Array<foundation::script::ScriptApiType> DescribeApi() override { return {}; }
        [[nodiscard]] u32 CheckManagerBuildCount() const override { return 0; }
    };

    [[nodiscard]] bool Lists(StringView language)
    {
        for (const String& id : ScriptLanguageCookRegistry::Get().Languages())
        {
            if (id.AsView() == language)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("script pipeline: a language's cook is listed while registered and taken back with its plugin")
{
    ScriptLanguageCookRegistry::Get().Register(String(u8"idletest"), MakeUnique<IdleCook>(DefaultAllocator()));
    CHECK(ScriptLanguageCookRegistry::Get().FindByLanguage(u8"idletest") != nullptr);
    CHECK(Lists(u8"idletest"));

    CHECK(ScriptLanguageCookRegistry::Get().Unregister(u8"idletest"));
    CHECK(ScriptLanguageCookRegistry::Get().FindByLanguage(u8"idletest") == nullptr);
    CHECK_FALSE(Lists(u8"idletest"));
    CHECK_FALSE(ScriptLanguageCookRegistry::Get().Unregister(u8"idletest"));
}

namespace
{
    // A project build that does nothing: the registry test needs only its presence.
    struct IdleAssemblyCook final : IScriptAssemblyCook
    {
        [[nodiscard]] StringView ProjectExtension() const override { return u8"idleproj"; }
        [[nodiscard]] StringView SourceExtension() const override { return u8"idle"; }
        [[nodiscard]] bool Build(StringView, StringView, Span<const ScriptAssemblyFile>, CookScriptErrorSink&,
                                 foundation::script::ScriptAssemblySource&) override
        {
            return false;
        }
        [[nodiscard]] String NewProjectTemplate(StringView) const override { return {}; }
    };
}

TEST_CASE("script pipeline: a language's project build is found while registered and taken back with its plugin")
{
    ScriptAssemblyCookRegistry::Get().Register(String(u8"idletest"), MakeUnique<IdleAssemblyCook>(DefaultAllocator()));
    CHECK(ScriptAssemblyCookRegistry::Get().FindByLanguage(u8"idletest") != nullptr);
    CHECK(ScriptAssemblyCookRegistry::Get().LanguageOfProject(u8"idleproj") == StringView(u8"idletest"));

    CHECK(ScriptAssemblyCookRegistry::Get().Unregister(u8"idletest"));
    CHECK(ScriptAssemblyCookRegistry::Get().FindByLanguage(u8"idletest") == nullptr);
    CHECK(ScriptAssemblyCookRegistry::Get().LanguageOfProject(u8"idleproj").IsEmpty());
    CHECK_FALSE(ScriptAssemblyCookRegistry::Get().Unregister(u8"idletest"));
}

// A language is a plugin: a script in one whose cook is not registered checks as one error that
// names the plugin to enable, rather than a compile failure.
TEST_CASE("script pipeline: a script in a language whose plugin is not loaded names the plugin to enable")
{
    ScriptSourceDocument doc;
    doc.Bind(u8".", u8"lost.lost", u8"lostlang");
    doc.SetSource(u8"anything\n");
    CHECK_FALSE(doc.Validate());
    REQUIRE(doc.Errors().Size() == 1u);
    CHECK(doc.Errors()[0].message == StringView(u8"written in lostlang, whose plugin is not loaded here - enable plugin 'lostlang' to check it"));
}

// The cook skips a script whose language's plugin is not loaded instead of failing it: its builder
// answers Unavailable, which the driver counts as skipped.
TEST_CASE("script pipeline: the class builder skips a script whose language's plugin is not loaded")
{
    foundation::script::RegisterScriptResource();
    RegisterScriptAssets();
    const StringView srcDir = u8"scratch_scriptpipe_skip_src";
    const StringView outDir = u8"scratch_scriptpipe_skip_out";
    (void)RemoveDirectoryRecursive(srcDir);
    (void)RemoveDirectoryRecursive(outDir);
    REQUIRE(CreateDirectories(srcDir));
    REQUIRE(CreateDirectories(outDir));
    const StringView text = u8"anything\n";
    REQUIRE(WriteFile(PathJoin(srcDir, u8"lost.lost").AsView(),
                      Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    {
        foundation::vfs::NativeFileSystem sources(srcDir, DefaultAllocator());
        foundation::vfs::NativeFileSystem output(outDir, DefaultAllocator());
        foundation::content::ContentDatabase outputDb(DefaultAllocator(), output, BinarySerializerFactory(), u8".rasset");
        ScriptClassAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"lost.lost");
        asset.language = String(u8"lostlang");
        ScriptClassAssetBuilder builder;
        AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sources;
        ctx.output = outputDb.RootGroup()->CreateInstance(u8"cooked", foundation::script::BehaviorClassSource::StaticType());
        CHECK(builder.Build(asset, ctx).Code() == ErrorCode::Unavailable);
    }
    (void)RemoveDirectoryRecursive(srcDir);
    (void)RemoveDirectoryRecursive(outDir);
}
