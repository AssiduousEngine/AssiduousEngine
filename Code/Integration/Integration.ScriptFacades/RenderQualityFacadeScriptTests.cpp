// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The RenderQuality script facade on both script backends: a script lists the levels its run
// offers, reads the one it plays at and switches to another by its label, through the run's
// RenderQualityChoice, whose choose hook (the host's: the player plays at the level and keeps the
// choice) hears it. A label the run does not offer is refused, and a context with no choice lists
// nothing. It needs the scripting backends, so it lives here rather than in Engine.Render.Tests.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import foundation.script.facades;
import engine.render;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
namespace render = engine::render;

namespace
{
    [[nodiscard]] render::OfferedQualityLevel Level(u8 seed, StringView label, u32 msaa)
    {
        render::OfferedQualityLevel level;
        level.id = Guid{seed, seed};
        level.values.label = String(label);
        level.values.msaaSamples = msaa;
        return level;
    }

    // The script reads Listed, Before and After and sets Switched and Refused; the choice's hook
    // records what it was asked to play at.
    void Check(IScriptManager& manager, StringView script)
    {
        render::RegisterRenderScriptFacade();
        RegisterReflectedTypes(manager);
        render::RenderQualityChoice choice;
        choice.levels.PushBack(Level(1, u8"Low", 1));
        choice.levels.PushBack(Level(2, u8"Medium", 2));
        choice.levels.PushBack(Level(3, u8"High", 4));
        choice.current = choice.levels[2].id;
        Array<String> played;
        choice.choose = [&played](const render::OfferedQualityLevel& level) { played.PushBack(level.values.label); };

        RefPtr<IScriptContext> scriptContext = manager.CreateContext();
        ScriptServices services; // the run's services, as a run holds its choice
        services.Add(choice);
        scriptContext->SetServiceResolver(services.Resolver());
        REQUIRE(scriptContext->Load(script, u8"quality").IsOk());

        CHECK(scriptContext->GetGlobal(u8"Listed").Get<String>() == StringView(u8"Low,Medium,High"));
        CHECK(scriptContext->GetGlobal(u8"Before").Get<String>() == StringView(u8"High"));
        CHECK(scriptContext->GetGlobal(u8"Switched").Get<bool>());
        CHECK_FALSE(scriptContext->GetGlobal(u8"Refused").Get<bool>()); // "Ultra" is not offered
        CHECK(scriptContext->GetGlobal(u8"After").Get<String>() == StringView(u8"Low"));
        CHECK(choice.current == choice.levels[0].id);
        REQUIRE(played.Size() == 1u); // the refused set asked for nothing
        CHECK(played[0] == StringView(u8"Low"));
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("render-quality-facade: AngelScript lists the levels, reads the current one and switches by label")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    Check(*manager,
          u8"string Listed; string Before; string After; bool Switched = false; bool Refused = true;\n"
          u8"void main() {\n"
          u8"  array<string>@ levels = RenderQuality::levels();\n"
          u8"  for (uint i = 0; i < levels.length(); i++) { if (i > 0) Listed += \",\"; Listed += levels[i]; }\n"
          u8"  Before = RenderQuality::current();\n"
          u8"  Switched = RenderQuality::set(\"Low\");\n"
          u8"  Refused = RenderQuality::set(\"Ultra\");\n"
          u8"  After = RenderQuality::current();\n"
          u8"}\n");
}
#endif

#ifdef OPTION_HAS_LUAU
TEST_CASE("render-quality-facade: Luau lists the levels, reads the current one and switches by label")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    Check(*manager,
          u8"Listed = table.concat(RenderQuality.levels(), ',')\n"
          u8"Before = RenderQuality.current()\n"
          u8"Switched = RenderQuality.set('Low')\n"
          u8"Refused = RenderQuality.set('Ultra')\n"
          u8"After = RenderQuality.current()\n");
}
#endif

TEST_CASE("render-quality-facade: a context whose run offers no levels lists none and refuses a set")
{
    RegisterCoreTypes();
    render::RegisterRenderScriptFacade();
#ifdef OPTION_HAS_LUAU
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"Count = #RenderQuality.levels(); Current = RenderQuality.current(); Set = RenderQuality.set('Low')\n",
                                u8"none")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"Count").Get<f64>() == doctest::Approx(0.0));
    CHECK(scriptContext->GetGlobal(u8"Current").Get<String>() == StringView(u8""));
    CHECK_FALSE(scriptContext->GetGlobal(u8"Set").Get<bool>());
#endif
}
