// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AngelScript.Pipeline (Plugins::AngelScript.Pipeline) - the `script.angelscript.pipeline` module.
//
// The AngelScript cook service: a compile-check in a cooker-owned AngelScript VM (resolved
// through the backend registry by language), the shared on<Upper>(...) handler scan, the
// property harvest and the AngelScript starter templates.
//
// AngelScript's editor-property surface is typed member fields annotated with the
// language's own `[metadata]`: `[default, "description"]` before a field declares that
// field an inspector property (a field with no metadata is not one). The cook builds the
// behavior through the vendored CScriptBuilder add-on (which pre-processes `[metadata]`)
// and walks the class's fields, mapping each metadata'd field's declared type, default and
// description into the same BehaviorProperty metadata other backends produce. All the
// AngelScript and scriptbuilder contact lives in the implementation unit.
//
// No AngelScript SDK header appears in this interface unit; the implementation unit reaches the
// cook VM's engine through the backend's AngelScriptEngineHandle.

module;
#include "Core/Prelude.h"

export module script.angelscript.pipeline;

import foundation.core;

using namespace foundation::core;

export namespace pipeline{
    // The New Asset starters for AngelScript, one per kind of behavior, each class named after its
    // asset and deriving from its kind, with the handlers stubbed.
    inline constexpr StringView kAngelScriptEntityStarter =
        u8"// An entity behavior: set it on an entity's Behavior component.\n"
        u8"// Reflected engine facades are global (no import). Static facades are namespaced and\n"
        u8"// lowercase (Log::info, Time::delta, Random::value), and the entity handle uses '.'\n"
        u8"// (self.setName). The scene is reached through the entity: self.scene.find(\"name\").\n"
        u8"class {ClassName} : EntityBehavior\n"
        u8"{\n"
        u8"    private Entity self;\n"
        u8"\n"
        u8"    {ClassName}(Entity entity) { self = entity; }\n"
        u8"\n"
        u8"    void onStart()\n"
        u8"    {\n"
        u8"        Log::info(\"{ClassName} started on '\" + self.name() + \"'\");\n"
        u8"    }\n"
        u8"\n"
        u8"    void onUpdate(double dt)\n"
        u8"    {\n"
        u8"        // Drift along +X at 1 unit a second. A Float3 is a value, as an int is:\n"
        u8"        // p is a copy of the entity's position.\n"
        u8"        Float3 p = self.position();\n"
        u8"        self.setPosition(p.x + float(dt), p.y, p.z);\n"
        u8"        // Also there: Time::delta(), Random::value(), self.scene.find(\"name\").\n"
        u8"    }\n"
        u8"\n"
        u8"    void onDestroy() {}\n"
        u8"}\n";

    // A scene behavior: one object per scene, made with the scene's handle. Every handler is
    // optional; onUpdate and onFixedUpdate run only while the scene simulates.
    inline constexpr StringView kAngelScriptSceneStarter =
        u8"// A scene behavior: one per scene, made with the scene's handle.\n"
        u8"// Set it in the scene's Scene Behavior settings. Static facades are namespaced and\n"
        u8"// lowercase (Log::info, Time::delta), and the scene's handle uses '.': scene.find(\"name\").\n"
        u8"class {ClassName} : SceneBehavior\n"
        u8"{\n"
        u8"    private Scene scene;\n"
        u8"\n"
        u8"    {ClassName}(Scene s) { scene = s; }\n"
        u8"\n"
        u8"    void onStart() { Log::info(\"{ClassName} started\"); }\n"
        u8"    // Every frame while the scene simulates.\n"
        u8"    void onUpdate(double dt) {}\n"
        u8"    // Every fixed physics step while the scene simulates.\n"
        u8"    void onFixedUpdate(double dt) {}\n"
        u8"    void onStop() {}\n"
        u8"}\n";

    // A run behavior: one per run, for the whole game across its scenes; it may load the scenes
    // itself (through the run facade).
    inline constexpr StringView kAngelScriptRunStarter =
        u8"// A run behavior: one per run, for the whole game across its scenes.\n"
        u8"// Set it as the project's Run behavior.\n"
        u8"class {ClassName} : RunBehavior\n"
        u8"{\n"
        u8"    {ClassName}() {}\n"
        u8"\n"
        u8"    // Runs once, before the default scene loads. With no default scene, load the first here:\n"
        u8"    //   run::loadScene(Guid(\"<the scene's id>\"));\n"
        u8"    void onStart() { Log::info(\"{ClassName} started\"); }\n"
        u8"    // Every frame, whatever scene is loaded.\n"
        u8"    void onUpdate(double dt) {}\n"
        u8"    void onStop() {}\n"
        u8"}\n";

    /// Registers the AngelScript cook (and, idempotently, the AngelScript backend it
    /// needs) so the neutral ScriptClassAssetBuilder resolves it by language. Entry points
    /// call this alongside the other script cook registrations. Idempotent.
    void RegisterAngelScriptScriptCook();
}
