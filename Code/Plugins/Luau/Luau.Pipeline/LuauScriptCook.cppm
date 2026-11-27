// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Luau.Pipeline - the `luau.pipeline` module (tooling).
//
// The Luau cook service: compile-checks a Luau behavior in a cooker-owned Luau VM (resolved
// through the backend registry by language), harvests editor properties by constructing the
// class and walking the instance's fields, compiles the source to a bytecode blob for the pack,
// and supplies the New-Asset starters. All Luau-specific cook syntax lives here, not in the
// neutral script.pipeline.
//
// The cook VM is created through the neutral IScriptContext surface
// (CreateScriptManagerForLanguage), so no Luau C header appears in this interface unit.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module luau.pipeline;

import foundation.core;
import foundation.script;         // registry, IScriptManager/IScriptContext, RegisterReflectedTypes
import foundation.script.resource; // BehaviorClassSource and BehaviorProperty
import foundation.script.facades;  // RegisterScriptFacadeReflection (the cook VM's global surface)
import script.pipeline;   // IScriptLanguageCook, registry, shared cook helpers
import luau.foundation;     // ensures the Luau backend is available to the registry

using namespace foundation::core;
using namespace foundation::script;
using namespace pipeline; // its bases

export namespace luau
{
    // The New Asset starters, one per kind of behavior, each class named after its asset. Luau OOP
    // is a global class table made from its kind; the engine makes one with `<Class>.new(...)`.
    // Editor properties are plain fields set in the constructor, which the cook harvests by
    // walking the built instance. Reflected instance methods use a colon (self.entity:position());
    // static facades use a dot (Log.info, Time.delta); Float3 is Luau's native vector.
    inline constexpr StringView kLuauEntityStarter =
        u8"-- An entity behavior: set it on an entity's Behavior component.\n"
        u8"-- A global class table made from its kind; the engine calls {ClassName}.new(entity).\n"
        u8"-- Reflected engine types are globals: Float3 (Luau's vector), Log, Time, Random.\n"
        u8"{ClassName} = EntityBehavior.class()\n"
        u8"\n"
        u8"function {ClassName}.new(entity)\n"
        u8"    local self = setmetatable({}, {ClassName})\n"
        u8"    self.entity = entity\n"
        u8"    -- Properties are plain fields set here; the cook finds them by making one. They can\n"
        u8"    -- be numbers, booleans, strings or Float3s. Keep the constructor to setting fields\n"
        u8"    -- (it runs when the script is cooked).\n"
        u8"    self.speed = 1.0\n"
        u8"    return self\n"
        u8"end\n"
        u8"\n"
        u8"-- Handlers are called when they exist (onStart, onUpdate, on<Event>). Instance methods\n"
        u8"-- use a colon (self.entity:name()); static facades use a dot (Log.info).\n"
        u8"function {ClassName}:onStart()\n"
        u8"    Log.info(\"{ClassName} started on \" .. self.entity:name())\n"
        u8"end\n"
        u8"\n"
        u8"function {ClassName}:onUpdate(dt)\n"
        u8"    -- Drift along +X at `speed` units a second.\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
        u8"end\n"
        u8"\n"
        u8"function {ClassName}:onDestroy()\n"
        u8"end\n";

    // A scene behavior: one object per scene, made with the scene's handle. Every handler is
    // optional; onUpdate and onFixedUpdate run only while the scene simulates.
    inline constexpr StringView kLuauSceneStarter =
        u8"-- A scene behavior: one per scene, made with the scene's handle.\n"
        u8"-- Set it in the scene's Scene Behavior settings.\n"
        u8"{ClassName} = SceneBehavior.class()\n"
        u8"\n"
        u8"function {ClassName}.new(scene)\n"
        u8"    return setmetatable({ scene = scene }, {ClassName})\n"
        u8"end\n"
        u8"\n"
        u8"-- Static facades use a dot (Log.info); the scene's handle a colon: self.scene:find(\"name\").\n"
        u8"function {ClassName}:onStart()\n"
        u8"    Log.info(\"{ClassName} started\")\n"
        u8"end\n"
        u8"-- Every frame while the scene simulates.\n"
        u8"function {ClassName}:onUpdate(dt)\n"
        u8"end\n"
        u8"-- Every fixed physics step while the scene simulates.\n"
        u8"function {ClassName}:onFixedUpdate(dt)\n"
        u8"end\n"
        u8"function {ClassName}:onStop()\n"
        u8"end\n";

    // A run behavior: one per run, for the whole game across its scenes; it may load the scenes
    // itself (through the run facade).
    inline constexpr StringView kLuauRunStarter =
        u8"-- A run behavior: one per run, for the whole game across its scenes.\n"
        u8"-- Set it as the project's Run behavior.\n"
        u8"{ClassName} = RunBehavior.class()\n"
        u8"\n"
        u8"function {ClassName}.new()\n"
        u8"    return setmetatable({}, {ClassName})\n"
        u8"end\n"
        u8"\n"
        u8"-- Runs once, before the default scene loads. With no default scene, load the first here:\n"
        u8"--   run.loadScene(Guid.new(\"<the scene's id>\"))\n"
        u8"function {ClassName}:onStart()\n"
        u8"    Log.info(\"{ClassName} started\")\n"
        u8"end\n"
        u8"-- Every frame, whatever scene is loaded.\n"
        u8"function {ClassName}:onUpdate(dt)\n"
        u8"end\n"
        u8"function {ClassName}:onStop()\n"
        u8"end\n";

    /// Registers the Luau cook (and, idempotently, the Luau backend it needs) so the neutral
    /// ScriptClassAssetBuilder resolves it by language. Entry points call this alongside
    /// RegisterAngelScriptScriptCook(). Idempotent.
    void RegisterLuauScriptCook();
}
