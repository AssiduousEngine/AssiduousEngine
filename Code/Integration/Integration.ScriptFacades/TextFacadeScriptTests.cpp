// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Text script facade on both script backends: Text.parseFloat reads a decimal from a string
// (trimmed, the whole text), and gives `otherwise`, or 0 without it, for text that is not one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import foundation.script.facades;
#ifdef HAS_PLUGIN_ANGELSCRIPT
import angelscript.foundation;
#endif
#ifdef HAS_PLUGIN_LUAU
import luau.foundation;
#endif

using namespace foundation::core;
using namespace foundation::script;

namespace
{
    // The script sets A to E: "2.5", " -0.25 ", "strong", "2.5s" otherwise -1, "" otherwise 7.
    void Check(IScriptManager& manager, StringView script)
    {
        RegisterScriptFacadeReflection();
        RegisterReflectedTypes(manager);
        RefPtr<IScriptContext> scriptContext = manager.CreateContext();
        REQUIRE(scriptContext->Load(script, u8"text").IsOk());
        CHECK(scriptContext->GetGlobal(u8"A").Get<f64>() == doctest::Approx(2.5));
        CHECK(scriptContext->GetGlobal(u8"B").Get<f64>() == doctest::Approx(-0.25));
        CHECK(scriptContext->GetGlobal(u8"C").Get<f64>() == doctest::Approx(0.0));
        CHECK(scriptContext->GetGlobal(u8"D").Get<f64>() == doctest::Approx(-1.0));
        CHECK(scriptContext->GetGlobal(u8"E").Get<f64>() == doctest::Approx(7.0));
    }
}

#ifdef HAS_PLUGIN_ANGELSCRIPT
TEST_CASE("text-facade: AngelScript reads a decimal from a string, and `otherwise` from text that is not one")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    Check(*manager,
          u8"double A; double B; double C; double D; double E;\n"
          u8"void main() {\n"
          u8"  A = Text::parseFloat(\"2.5\");\n"
          u8"  B = Text::parseFloat(\" -0.25 \");\n"
          u8"  C = Text::parseFloat(\"strong\");\n"
          u8"  D = Text::parseFloat(\"2.5s\", -1);\n"
          u8"  E = Text::parseFloat(\"\", 7);\n"
          u8"}\n");
}
#endif

#ifdef HAS_PLUGIN_LUAU
TEST_CASE("text-facade: Luau reads a decimal from a string, and `otherwise` from text that is not one")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = luau::CreateLuauScriptManager(DefaultAllocator());
    Check(*manager,
          u8"A = Text.parseFloat('2.5')\n"
          u8"B = Text.parseFloat(' -0.25 ')\n"
          u8"C = Text.parseFloat('strong')\n"
          u8"D = Text.parseFloat('2.5s', -1)\n"
          u8"E = Text.parseFloat('', 7)\n");
}
#endif

// Text.formatDate shows a Unix time as the local date by a pattern; Time.unixNow is the wall clock.
TEST_CASE("text-facade: formatDate fills a pattern with the local date and time; unixNow is the wall clock")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    const f64 when = 1795467854.0; // 2026-11-24, in every zone within a day of it
    const i64 local = static_cast<i64>(when) + GetLocalTimeOffset(static_cast<i64>(when));
    const CivilTime civil = CivilTimeOf(local);
    const String expected = Format(u8"{}-{}{}-{}{} {}{}:{}{}:{}{} at", civil.year, civil.month < 10 ? u8"0" : u8"", civil.month,
                                   civil.day < 10 ? u8"0" : u8"", civil.day, civil.hour < 10 ? u8"0" : u8"", civil.hour,
                                   civil.minute < 10 ? u8"0" : u8"", civil.minute, civil.second < 10 ? u8"0" : u8"", civil.second);
    CHECK(TextFacade::formatDate(when, String(u8"YYYY-MM-DD hh:mm:ss at")) == expected.AsView());
    CHECK(TextFacade::formatDate(when, String(u8"no tokens")) == StringView(u8"no tokens"));
#ifdef HAS_PLUGIN_LUAU
    RefPtr<IScriptManager> manager = luau::CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"Year = Text.formatDate(1795467854, 'YYYY')\nNow = Time.unixNow()\n", u8"date").IsOk());
    CHECK(scriptContext->GetGlobal(u8"Year").Get<String>() == Format(u8"{}", civil.year).AsView());
    CHECK(scriptContext->GetGlobal(u8"Now").Get<f64>() > 1.7e9);
#endif
}
