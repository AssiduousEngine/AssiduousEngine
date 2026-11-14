// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script manager a language's checks share (CachedCheckManager), through each language's cook: made
// once, made again when the registered types change, made ahead by PrepareChecks, and the API
// it describes following the types. The same checks for every language, so the cooks cannot
// drift apart.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.script;
import script.pipeline;
#ifdef OPTION_HAS_ANGELSCRIPT
import script.angelscript.pipeline;
#endif
#ifdef OPTION_HAS_LUAU
import script.luau.pipeline;
#endif

using namespace foundation::core;
using namespace foundation::script;
using namespace pipeline;

namespace
{
    // A type registered while the editor runs, as a plugin's types are.
    class CheckLateType : public Object
    {
        RTTI_OBJECT(CheckLateType, Object)
    public:
        int count = 0;
    };

    struct Language
    {
        const char8_t* id;
        const char8_t* gamma;        // a script declaring the class Gamma
        const char8_t* usesLateType; // a script whose compile needs CheckLateType, or null
    };

    [[nodiscard]] bool Describes(const Array<ScriptApiType>& api, StringView name)
    {
        for (const ScriptApiType& type : api)
            if (type.scriptName.AsView() == name)
                return true;
        return false;
    }

    [[nodiscard]] bool CheckOf(IScriptLanguageCook& cook, const char8_t* source)
    {
        String className;
        CookScriptErrorSink sink;
        return cook.Check(StringView(source), u8"Gamma.script", u8"Gamma", sink, className, Span<const TypeId>{});
    }

    void CheckSharedEngine(const Language& language)
    {
        IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(StringView(language.id));
        REQUIRE(cook != nullptr);

        // Checks share the manager: a second makes none.
        REQUIRE(CheckOf(*cook, language.gamma));
        const u32 builds = cook->CheckManagerBuildCount();
        CHECK(builds >= 1u);
        CHECK(CheckOf(*cook, language.gamma));
        CHECK(cook->CheckManagerBuildCount() == builds);

        // A type registered (a plugin loaded) is in the next check's API, which is made again
        // once; removed (the plugin unloaded), it is out, made again once more.
        CHECK_FALSE(Describes(cook->DescribeApi(), u8"CheckLateType"));
        if (language.usesLateType != nullptr)
            CHECK_FALSE(CheckOf(*cook, language.usesLateType));
        GlobalTypeRegistry().Register(CheckLateType::StaticType());
        if (language.usesLateType != nullptr)
            CHECK(CheckOf(*cook, language.usesLateType));
        CHECK(Describes(cook->DescribeApi(), u8"CheckLateType"));
        CHECK(cook->CheckManagerBuildCount() == builds + 1);
        GlobalTypeRegistry().Unregister(CheckLateType::StaticType().id);
        CHECK_FALSE(Describes(cook->DescribeApi(), u8"CheckLateType"));
        if (language.usesLateType != nullptr)
            CHECK_FALSE(CheckOf(*cook, language.usesLateType));
        CHECK(cook->CheckManagerBuildCount() == builds + 2);

        // Prepared ahead (as a project opens), the manager is made once, and the next check uses it.
        GlobalTypeRegistry().Register(CheckLateType::StaticType());
        GlobalTypeRegistry().Unregister(CheckLateType::StaticType().id);
        ScriptLanguageCookRegistry::Get().PrepareChecks();
        CHECK(cook->CheckManagerBuildCount() == builds + 3);
        CHECK(CheckOf(*cook, language.gamma));
        CHECK(cook->CheckManagerBuildCount() == builds + 3);
    }
}

REFLECT_MEMBERS(CheckLateType, "rtti::script::checktest")
{
    builder.Constructor();
    builder.Property<&CheckLateType::count>("count");
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("cached check manager: made once, again when the types change, and ahead (angelscript)")
{
    RegisterAngelScriptScriptCook(); // and its backend
    CheckSharedEngine(Language{u8"angelscript", u8"class Gamma { int z; }\n", u8"class Gamma { CheckLateType@ late; }\n"});
}
#endif

#ifdef OPTION_HAS_LUAU
TEST_CASE("cached check manager: made once, again when the types change, and ahead (luau)")
{
    RegisterLuauScriptCook(); // and its backend
    // A Luau compile resolves no globals, so a name the API lacks is not an error there.
    CheckSharedEngine(Language{u8"luau",
                               u8"Gamma = {}\nGamma.__index = Gamma\nfunction Gamma.new() return setmetatable({}, Gamma) end\n",
                               nullptr});
}
#endif
