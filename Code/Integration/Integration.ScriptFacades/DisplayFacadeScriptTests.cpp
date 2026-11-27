// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Display script facade on both script backends: a script lists the present modes its run
// offers, reads the one the window presents in and switches to another by its name, through the
// run's DisplayChoice, whose hook (the host's: the player presents in it and keeps the choice)
// hears it. A mode the run does not offer is refused, and so is one the window refused; a context
// with no choice lists nothing. It needs the scripting backends, so it lives here rather than in
// Engine.Render.Tests.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import foundation.script.facades;
import engine.render;
#ifdef HAS_PLUGIN_ANGELSCRIPT
import angelscript.foundation;
#endif
#ifdef HAS_PLUGIN_LUAU
import luau.foundation;
#endif

using namespace foundation::core;
using namespace foundation::script;
namespace render = engine::render;

namespace
{
    // The script reads Listed, Before and After and sets Switched, Unknown and Refused; the
    // choice's hook records what it was asked to present in, and refuses mailbox, as a window
    // whose swapchain could not be made again would.
    void Check(IScriptManager& manager, StringView script)
    {
        render::RegisterRenderScriptFacade();
        RegisterReflectedTypes(manager);
        render::DisplayChoice choice;
        choice.presentModes.PushBack(String(u8"vsync"));
        choice.presentModes.PushBack(String(u8"mailbox"));
        choice.presentModes.PushBack(String(u8"immediate"));
        choice.presentMode = String(u8"vsync");
        Array<String> presented;
        choice.choosePresentMode = [&presented](StringView name)
        {
            presented.PushBack(String(name));
            return name != StringView(u8"mailbox");
        };

        RefPtr<IScriptContext> scriptContext = manager.CreateContext();
        ScriptServices services; // the run's services, as a run holds its choice
        services.Add(choice);
        scriptContext->SetServiceResolver(services.Resolver());
        REQUIRE(scriptContext->Load(script, u8"display").IsOk());

        CHECK(scriptContext->GetGlobal(u8"Listed").Get<String>() == StringView(u8"vsync,mailbox,immediate"));
        CHECK(scriptContext->GetGlobal(u8"Before").Get<String>() == StringView(u8"vsync"));
        CHECK(scriptContext->GetGlobal(u8"Switched").Get<bool>());
        CHECK_FALSE(scriptContext->GetGlobal(u8"Unknown").Get<bool>()); // "uncapped" is not offered
        CHECK_FALSE(scriptContext->GetGlobal(u8"Refused").Get<bool>()); // the window refused mailbox
        CHECK(scriptContext->GetGlobal(u8"After").Get<String>() == StringView(u8"immediate"));
        CHECK(choice.presentMode == StringView(u8"immediate"));
        REQUIRE(presented.Size() == 2u); // the unknown name asked for nothing
        CHECK(presented[0] == StringView(u8"immediate"));
        CHECK(presented[1] == StringView(u8"mailbox"));
    }
}

#ifdef HAS_PLUGIN_ANGELSCRIPT
TEST_CASE("display-facade: AngelScript lists the present modes, reads the current one and switches by name")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    Check(*manager,
          u8"string Listed; string Before; string After; bool Switched = false; bool Unknown = true; bool Refused = true;\n"
          u8"void main() {\n"
          u8"  array<string>@ modes = Display::presentModes();\n"
          u8"  for (uint i = 0; i < modes.length(); i++) { if (i > 0) Listed += \",\"; Listed += modes[i]; }\n"
          u8"  Before = Display::presentMode();\n"
          u8"  Switched = Display::setPresentMode(\"immediate\");\n"
          u8"  Unknown = Display::setPresentMode(\"uncapped\");\n"
          u8"  Refused = Display::setPresentMode(\"mailbox\");\n"
          u8"  After = Display::presentMode();\n"
          u8"}\n");
}
#endif

#ifdef HAS_PLUGIN_LUAU
TEST_CASE("display-facade: Luau lists the present modes, reads the current one and switches by name")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = luau::CreateLuauScriptManager(DefaultAllocator());
    Check(*manager,
          u8"Listed = table.concat(Display.presentModes(), ',')\n"
          u8"Before = Display.presentMode()\n"
          u8"Switched = Display.setPresentMode('immediate')\n"
          u8"Unknown = Display.setPresentMode('uncapped')\n"
          u8"Refused = Display.setPresentMode('mailbox')\n"
          u8"After = Display.presentMode()\n");
}
#endif

TEST_CASE("display-facade: a context whose run offers no present modes lists none and refuses a set")
{
    RegisterCoreTypes();
    render::RegisterRenderScriptFacade();
#ifdef HAS_PLUGIN_LUAU
    RefPtr<IScriptManager> manager = luau::CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"Count = #Display.presentModes(); Current = Display.presentMode(); Set = Display.setPresentMode('vsync')\n",
                                u8"none")
                .IsOk());
    CHECK(scriptContext->GetGlobal(u8"Count").Get<f64>() == doctest::Approx(0.0));
    CHECK(scriptContext->GetGlobal(u8"Current").Get<String>() == StringView(u8""));
    CHECK_FALSE(scriptContext->GetGlobal(u8"Set").Get<bool>());
#endif
}
