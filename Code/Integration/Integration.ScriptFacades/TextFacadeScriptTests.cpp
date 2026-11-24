// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Text script facade on both script backends: Text.parseFloat reads a decimal from a string
// (trimmed, the whole text), and gives `otherwise`, or 0 without it, for text that is not one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import foundation.script.facades;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
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

#ifdef OPTION_HAS_ANGELSCRIPT
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

#ifdef OPTION_HAS_LUAU
TEST_CASE("text-facade: Luau reads a decimal from a string, and `otherwise` from text that is not one")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    Check(*manager,
          u8"A = Text.parseFloat('2.5')\n"
          u8"B = Text.parseFloat(' -0.25 ')\n"
          u8"C = Text.parseFloat('strong')\n"
          u8"D = Text.parseFloat('2.5s', -1)\n"
          u8"E = Text.parseFloat('', 7)\n");
}
#endif
