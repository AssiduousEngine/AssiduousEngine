// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.ScriptScene: one entity behavior written in every language the engine runs, played
// through the same scene and read back through the same BehaviorObject, must come out the same.
// The probe records its lifecycle (enable and start as it goes live, its updates, none while its
// entity is inactive, disable and enable as its switch goes off and on), a message with a number scaled by a property an entity overrides, and a run
// bus event its on<Event> handler hears. AngelScript and Luau (where they are built) and C++ (a
// native behavior, always) are the columns; a difference between them is a behavior a game would
// see change with its language.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import engine.behavior;
#if defined(HAS_PLUGIN_ANGELSCRIPT)
import angelscript.foundation;
#endif
#if defined(HAS_PLUGIN_LUAU)
import luau.foundation;
#endif

using namespace foundation::core;
using namespace foundation::script;
using namespace engine::behavior;
namespace scene = foundation::scene;

namespace conformance
{
    // The probe in C++.
    class Probe final : public EntityBehavior
    {
        RTTI_OBJECT(Probe, EntityBehavior)
    public:
        f32 step = 1.0f;
        f64 pinged = 0.0;
        i32 updates = 0;
        String log;

        void OnStart() { log += StringView(u8"start;"); }
        void OnUpdate(f32) { ++updates; }
        void OnPing(f64 amount) { pinged += amount * step; }
        void OnRoundOver(f64) { log += StringView(u8"round;"); }
        void OnDisable() { log += StringView(u8"disable;"); }
        void OnEnable() { log += StringView(u8"enable;"); }
    };

    REFLECT_MEMBERS(Probe, "rtti::conformance")
    {
        builder.Property<&Probe::step>("step").PropAttribute("behaviorProperty", true);
        builder.Property<&Probe::pinged>("pinged");
        builder.Property<&Probe::updates>("updates");
        builder.Property<&Probe::log>("log");
        builder.Method<&Probe::OnStart>("onStart");
        builder.Method<&Probe::OnUpdate>("onUpdate");
        builder.Method<&Probe::OnPing>("onPing");
        builder.Method<&Probe::OnRoundOver>("onRoundOver");
        builder.Method<&Probe::OnDisable>("onDisable");
        builder.Method<&Probe::OnEnable>("onEnable");
        builder.Creator();
    }

    // The probe in AngelScript.
    constexpr StringView kAngelScriptProbe = u8"class Probe : EntityBehavior {\n"
                                             u8"    private Entity self;\n"
                                             u8"    float step = 1;\n"
                                             u8"    double pinged = 0;\n"
                                             u8"    int updates = 0;\n"
                                             u8"    string log = \"\";\n"
                                             u8"    Probe(Entity entity) { self = entity; }\n"
                                             u8"    void onStart() { log += \"start;\"; }\n"
                                             u8"    void onUpdate(float dt) { updates += 1; }\n"
                                             u8"    void onPing(double amount) { pinged += amount * step; }\n"
                                             u8"    void onRoundOver(double round) { log += \"round;\"; }\n"
                                             u8"    void onDisable() { log += \"disable;\"; }\n"
                                             u8"    void onEnable() { log += \"enable;\"; }\n"
                                             u8"}\n";

    // The probe in Luau.
    constexpr StringView kLuauProbe =
        u8"Probe = EntityBehavior.class()\n"
        u8"function Probe.new(entity)\n"
        u8"    return setmetatable({ entity = entity, step = 1, pinged = 0, updates = 0, log = \"\" }, Probe)\n"
        u8"end\n"
        u8"function Probe:onStart() self.log = self.log .. \"start;\" end\n"
        u8"function Probe:onUpdate(dt) self.updates = self.updates + 1 end\n"
        u8"function Probe:onPing(amount) self.pinged = self.pinged + amount * self.step end\n"
        u8"function Probe:onRoundOver(round) self.log = self.log .. \"round;\" end\n"
        u8"function Probe:onDisable() self.log = self.log .. \"disable;\" end\n"
        u8"function Probe:onEnable() self.log = self.log .. \"enable;\" end\n";

    void RegisterOnce()
    {
        static const bool once = []()
        {
#if defined(HAS_PLUGIN_ANGELSCRIPT)
            angelscript::RegisterAngelScriptBackend();
#endif
#if defined(HAS_PLUGIN_LUAU)
            luau::RegisterLuauScriptBackend();
#endif
            RegisterCoreTypes();
            RegisterBehaviorComponentReflection();
            RegisterScriptFacadeReflection();
            GlobalTypeRegistry().Register(Probe::StaticType());
            return true;
        }();
        (void)once;
    }

    // What a probe came out with.
    struct Outcome
    {
        String log;
        f64 pinged = 0.0;
        f64 updates = 0.0;
        bool faulted = true;
    };

    // Plays the scenario with the probe written in `language` (`className` its class, `source` its
    // text; none for a native one).
    Outcome Play(StringView language, StringView className, StringView source)
    {
        RegisterOnce();
        foundation::run::RunBase run{DefaultAllocator(), u8"conformance"};
        scene::Scene world{DefaultAllocator(), u8"conformance"};
        BehaviorRunHost host{DefaultAllocator()};
        world.SetRun(&run);
        auto* components = world.AddSystem<BehaviorComponentManager>();
        auto* behaviors = world.AddSystem<EntityBehaviorSystem>();
        components->SetScriptSystem(behaviors);
        behaviors->SetRunHost(&host);
        EntityBehaviorSystem* system = behaviors;
        host.Binding().dispatchMessage = Function<void(scene::Scene*, scene::EntityHandle, StringView, Span<const Variant>)>{
            [system](scene::Scene*, scene::EntityHandle target, StringView message, Span<const Variant> args)
            { system->EnqueueMessage(target, message, args); }};

        RefPtr<BehaviorClass> probe = MakeRef<BehaviorClass>(DefaultAllocator());
        probe->language = String(language);
        probe->className = String(className);
        probe->kind = BehaviorKind::Entity;
        probe->source = String(source);
        const StringView handlers[] = {u8"onStart", u8"onUpdate", u8"onPing", u8"onRoundOver", u8"onDisable", u8"onEnable"};
        for (StringView handler : handlers)
        {
            probe->handlers.PushBack(String(handler));
        }
        BehaviorProperty step;
        step.name = String(u8"step");
        step.hash = BehaviorPropertyNameHash(u8"step");
        step.type = BehaviorPropertyType::Float;
        step.defaultValue.kind = BehaviorPropertyType::Float;
        step.defaultValue.number = 1.0;
        probe->properties.PushBack(step);
        probe->BuildProfileName();

        const scene::EntityHandle e = world.CreateEntity(u8"probe");
        AttachedBehavior attached;
        attached.behaviorClass = probe;
        BehaviorPropertyValue two;
        two.kind = BehaviorPropertyType::Float;
        two.number = 2.0;
        attached.SetOverride(BehaviorPropertyNameHash(u8"step"), two);
        components->Add(e).behaviors.PushBack(Move(attached));

        const auto frame = [&]()
        {
            run.Events().Drain();
            world.Update(1.0f / 60.0f);
        };
        world.Start();
        world.SetSimulationEnabled(true);
        frame(); // it starts and updates
        const Variant three[1] = {Variant::From<f64>(3.0)};
        behaviors->EnqueueMessage(e, u8"Ping", Span<const Variant>{three, 1});
        frame(); // the message: 3 scaled by the overridden step
        run.Events().Publish(StringHash(StringView(u8"RoundOver")), Variant::From<i32>(7));
        frame(); // the bus event
        world.SetActive(e, false);
        frame(); // its entity inactive: suspended, no update and no edge
        world.SetActive(e, true);
        frame(); // active again: updates
        components->Get(e)->behaviors[0].enabled = false;
        frame(); // the behavior switched off: onDisable
        components->Get(e)->behaviors[0].enabled = true;
        frame(); // and on: onEnable, and an update

        Outcome outcome;
        const AttachedBehavior& behavior = components->Get(e)->behaviors[0];
        outcome.faulted = behavior.faulted;
        if (BehaviorObject* object = behavior.instance.Get())
        {
            bool ok = false;
            outcome.log = object->GetProperty(u8"log").Value().Get<String>();
            outcome.pinged = ScriptNumbers::Of(object->GetProperty(u8"pinged").Value(), ok);
            outcome.updates = ScriptNumbers::Of(object->GetProperty(u8"updates").Value(), ok);
        }
        return outcome;
    }

    // What every column must come out with.
    void CheckOutcome(const Outcome& outcome)
    {
        CHECK_FALSE(outcome.faulted);
        // onEnable as it first goes live, then onStart; the behavior's switch, not its entity's
        // activity, gives onDisable and onEnable.
        CHECK(outcome.log == String(u8"enable;start;round;disable;enable;"));
        CHECK(outcome.pinged == doctest::Approx(6.0)); // 3 * the overridden step, 2
        CHECK(outcome.updates == doctest::Approx(5.0)); // every running frame: not the suspended or switched-off one
    }
}

TEST_CASE("behavior conformance: a native behavior plays the probe")
{
    conformance::CheckOutcome(conformance::Play(kNativeLanguage, u8"rtti::conformance::Probe", u8""));
}

#if defined(HAS_PLUGIN_ANGELSCRIPT)
TEST_CASE("behavior conformance: an AngelScript behavior plays the probe as the native one does")
{
    conformance::CheckOutcome(conformance::Play(u8"angelscript", u8"Probe", conformance::kAngelScriptProbe));
}
#endif

#if defined(HAS_PLUGIN_LUAU)
TEST_CASE("behavior conformance: a Luau behavior plays the probe as the native one does")
{
    conformance::CheckOutcome(conformance::Play(u8"luau", u8"Probe", conformance::kLuauProbe));
}
#endif
