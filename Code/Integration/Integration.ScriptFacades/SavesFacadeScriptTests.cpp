// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Saves facade and the SaveStore a script holds, on both script backends, as a title menu uses
// them: New Game opens a slot and writes it, a second slot is written only at its save point,
// Continue is the slot saved last, Load reads a slot's summary without opening it, a store held
// past its slot's closing reaches nothing, and the global store keeps what every slot shares. The
// run's saves keep a clock the test counts, so the order does not hang on the wall clock.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import foundation.script.facades;
import engine.run;
#ifdef OPTION_HAS_ANGELSCRIPT
import angelscript.foundation;
#endif
#ifdef OPTION_HAS_LUAU
import luau.foundation;
#endif

using namespace foundation::core;

namespace
{
    i64 g_tick = 0;
    i64 Tick() { return ++g_tick; } // each write one second after the last

    // Runs the game script `source` once on a fresh saves folder, then checks what it wrote: the
    // script records what it saw in the global store, and the files hold what was saved.
    // `name` is the script's file name: its extension picks the language.
    void Check(StringView source, StringView name)
    {
        RegisterCoreTypes();
        foundation::script::RegisterScriptFacadeReflection();
        engine::runtime::RegisterRunScriptFacade();
        engine::runtime::RegisterSaveScriptFacade();
        const String folder = PathJoin(u8"scratch_saves_facade", PathStem(name));
        (void)RemoveDirectoryRecursive(folder.AsView());
        g_tick = 0;
        {
            engine::runtime::ApplicationRun run;
            run.SetSavesFolder(folder.AsView());
            run.Saves().SetClock(&Tick);
            REQUIRE(run.StartRunBehavior(source, name, u8"Game"));
            run.StopRunBehavior();
        }

        engine::runtime::SaveFile global(DefaultAllocator());
        global.Open(PathJoin(folder.AsView(), u8"global.save.xml").AsView());
        const auto& seen = global.Values();
        CHECK(seen.GetInt(u8"count", -1) == 2);
        CHECK(seen.GetText(u8"continue", u8"") == StringView(u8"2")); // saved last
        CHECK(seen.GetText(u8"place", u8"") == StringView(u8"mill")); // a summary read without opening
        CHECK(seen.GetInt(u8"errands", -1) == 2);
        CHECK(seen.GetInt(u8"unsaved", -1) == 0);    // the save point's slot kept what its flush wrote
        CHECK(seen.GetBool(u8"stale", false));       // a store held past its slot's closing
        CHECK(seen.GetInt(u8"staleRead", -1) == 7);  // reads its fallback
        CHECK(seen.GetText(u8"globalName", u8"x") == StringView(u8""));
        CHECK(seen.GetText(u8"slotName", u8"") == StringView(u8"1"));
        CHECK(seen.GetBool(u8"time", false));        // slotTime is the time inside the slot

        // The slots hold their own values, never the global store's.
        engine::runtime::SaveFile one(DefaultAllocator());
        one.Open(PathJoin(folder.AsView(), u8"slot-1.save.xml").AsView());
        CHECK(one.Values().GetText(u8"place", u8"") == StringView(u8"mill"));
        CHECK_FALSE(one.Values().Has(u8"count"));
        CHECK_FALSE(one.Values().Has(u8"x")); // the stale write went nowhere
        engine::runtime::SaveFile two(DefaultAllocator());
        two.Open(PathJoin(folder.AsView(), u8"slot-2.save.xml").AsView());
        CHECK(two.Values().GetText(u8"place", u8"") == StringView(u8"square"));
        CHECK_FALSE(two.Values().Has(u8"errands"));
        (void)RemoveDirectoryRecursive(folder.AsView());
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("saves-facade: AngelScript plays a title menu's New Game, save point, Continue and Load")
{
    angelscript::RegisterAngelScriptBackend();
    Check(u8"class Game : RunBehavior {\n"
          u8"  Game() {}\n"
          u8"  void onStart() {\n"
          u8"    SaveStore one = Saves::openSlot(\"1\");\n"
          u8"    one.setString(\"place\", \"mill\");\n"
          u8"    one.setInt(\"errands\", 2);\n"
          u8"    Saves::flush();\n"
          u8"    SaveStore two = Saves::openSlot(\"2\", SlotWrites::AtSavePoints);\n"
          u8"    two.setString(\"place\", \"square\");\n"
          u8"    two.flush();\n"
          u8"    two.setInt(\"errands\", 9);\n"
          u8"    Saves::closeSlot();\n"
          u8"    SaveStore global = Saves::global();\n"
          u8"    global.setBool(\"stale\", !one.isValid() && !two.isValid());\n"
          u8"    one.setInt(\"x\", 1);\n"
          u8"    global.setInt(\"staleRead\", one.getInt(\"place\", 7));\n"
          u8"    array<string>@ names = Saves::slots();\n"
          u8"    global.setInt(\"count\", names.length());\n"
          u8"    global.setString(\"continue\", names[0]);\n"
          u8"    SaveStore read = Saves::readSlot(\"1\");\n"
          u8"    global.setString(\"place\", read.getString(\"place\", \"?\"));\n"
          u8"    global.setInt(\"errands\", read.getInt(\"errands\", 0));\n"
          u8"    global.setInt(\"unsaved\", Saves::readSlot(\"2\").getInt(\"errands\", 0));\n"
          u8"    global.setString(\"globalName\", global.name());\n"
          u8"    global.setString(\"slotName\", read.name());\n"
          u8"    global.setBool(\"time\", Saves::slotTime(\"2\") > Saves::slotTime(\"1\") && Saves::slotTime(\"1\") > 0);\n"
          u8"  }\n"
          u8"  void onUpdate(double dt) {}\n"
          u8"  void onStop() {}\n"
          u8"}\n",
          u8"title.as");
}
#endif

#ifdef OPTION_HAS_LUAU
TEST_CASE("saves-facade: Luau plays a title menu's New Game, save point, Continue and Load")
{
    luau::RegisterLuauScriptBackend();
    Check(u8"Game = {}\n"
          u8"Game.__index = Game\n"
          u8"function Game.new() return setmetatable({}, Game) end\n"
          u8"function Game:onStart()\n"
          u8"  local one = Saves.openSlot('1')\n"
          u8"  one:setString('place', 'mill')\n"
          u8"  one:setInt('errands', 2)\n"
          u8"  Saves.flush()\n"
          u8"  local two = Saves.openSlot('2', SlotWrites.AtSavePoints)\n"
          u8"  two:setString('place', 'square')\n"
          u8"  two:flush()\n"
          u8"  two:setInt('errands', 9)\n"
          u8"  Saves.closeSlot()\n"
          u8"  local global = Saves.global()\n"
          u8"  global:setBool('stale', not one:isValid() and not two:isValid())\n"
          u8"  one:setInt('x', 1)\n"
          u8"  global:setInt('staleRead', one:getInt('place', 7))\n"
          u8"  local names = Saves.slots()\n"
          u8"  global:setInt('count', #names)\n"
          u8"  global:setString('continue', names[1])\n"
          u8"  local read = Saves.readSlot('1')\n"
          u8"  global:setString('place', read:getString('place', '?'))\n"
          u8"  global:setInt('errands', read:getInt('errands', 0))\n"
          u8"  global:setInt('unsaved', Saves.readSlot('2'):getInt('errands', 0))\n"
          u8"  global:setString('globalName', global:name())\n"
          u8"  global:setString('slotName', read:name())\n"
          u8"  global:setBool('time', Saves.slotTime('2') > Saves.slotTime('1') and Saves.slotTime('1') > 0)\n"
          u8"end\n"
          u8"function Game:onUpdate(dt) end\n"
          u8"function Game:onStop() end\n",
          u8"title.luau");
}
#endif
