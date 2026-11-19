// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Entity events reach behaviors in every language: a domain posts to the scene ("contactBegin",
// "animationFinished"), the scene's behavior system queues on<Name> for the target's behaviors, and
// an entity among the arguments arrives as the Entity a behavior is given.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import engine.behavior;
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
import foundation.script.angelscript;
#endif
#if defined(INTEGRATION_HAS_LUAU)
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
using namespace engine::behavior;
namespace scene = foundation::scene;

namespace
{
    void RegisterOnce()
    {
        static const bool once = []()
        {
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
            foundation::script::angelscript::RegisterAngelScriptBackend();
#endif
#if defined(INTEGRATION_HAS_LUAU)
            foundation::script::RegisterLuauScriptBackend();
#endif
            RegisterCoreTypes();
            RegisterBehaviorComponentReflection();
            RegisterScriptFacadeReflection();
            return true;
        }();
        (void)once;
    }

    // A scene with the behavior systems, as a run has them.
    struct EventScene
    {
        foundation::run::RunBase run{DefaultAllocator(), u8"entity-events"};
        scene::Scene scene{DefaultAllocator(), u8"entity-events"};
        BehaviorRunHost host{DefaultAllocator()};
        BehaviorComponentManager* components = nullptr;

        EventScene()
        {
            RegisterOnce();
            scene.SetRun(&run);
            components = scene.AddSystem<BehaviorComponentManager>();
            EntityBehaviorSystem* behaviors = scene.AddSystem<EntityBehaviorSystem>();
            components->SetScriptSystem(behaviors);
            behaviors->SetRunHost(&host);
        }

        void Attach(scene::EntityHandle e, StringView language, StringView className, StringView source)
        {
            RefPtr<BehaviorClass> cls = MakeRef<BehaviorClass>(DefaultAllocator());
            cls->language = String(language);
            cls->className = String(className);
            cls->kind = BehaviorKind::Entity;
            cls->source = String(source);
            cls->handlers.PushBack(String(u8"onHit"));
            cls->BuildProfileName();
            AttachedBehavior behavior;
            behavior.behaviorClass = cls;
            components->Add(e).behaviors.PushBack(Move(behavior));
        }

        void Frame()
        {
            host.Binding().deltaSeconds = 1.0f / 60.0f;
            scene.Update(1.0f / 60.0f);
        }

        // Starts the scene, posts "hit" to `target` from `from` with a strength, and runs a frame.
        void PostHit(scene::EntityHandle target, scene::EntityHandle from)
        {
            scene.Start();
            scene.SetSimulationEnabled(true);
            Frame(); // the behaviors start
            REQUIRE(scene.WantsEntityEvents());
            const Variant args[] = {Variant::From<scene::EntityHandle>(from), Variant::From<f64>(2.5)};
            scene.PostEntityEvent(scene::EntityEvent{target, u8"hit", Span<const Variant>(args, 2)});
            Frame(); // delivered at the behavior tick
        }
    };
}

#if defined(INTEGRATION_HAS_ANGELSCRIPT)
TEST_CASE("entity events: a posted event calls on<Name> on an AngelScript behavior, the entity argument an Entity")
{
    EventScene s;
    const scene::EntityHandle target = s.scene.CreateEntity(u8"target");
    const scene::EntityHandle from = s.scene.CreateEntity(u8"rock");
    s.Attach(target, u8"angelscript", u8"Target",
             u8"class Target : EntityBehavior {\n"
             u8"    private Entity self;\n"
             u8"    Target(Entity entity) { self = entity; }\n"
             u8"    void onHit(Entity other, double strength) {\n"
             u8"        if (strength == 2.5) { self.setName(\"hit by \" + other.name()); }\n"
             u8"    }\n"
             u8"}\n");
    s.PostHit(target, from);
    CHECK(s.scene.GetEntityName(target) == StringView(u8"hit by rock"));
}
#endif

#if defined(INTEGRATION_HAS_LUAU)
TEST_CASE("entity events: a posted event calls on<Name> on a Luau behavior, the entity argument an Entity")
{
    EventScene s;
    const scene::EntityHandle target = s.scene.CreateEntity(u8"target");
    const scene::EntityHandle from = s.scene.CreateEntity(u8"rock");
    s.Attach(target, u8"luau", u8"Target",
             u8"Target = EntityBehavior.class()\n"
             u8"function Target.new(entity) return setmetatable({ entity = entity }, Target) end\n"
             u8"function Target:onHit(other, strength)\n"
             u8"    if strength == 2.5 then self.entity:setName(\"hit by \" .. other:name()) end\n"
             u8"end\n");
    s.PostHit(target, from);
    CHECK(s.scene.GetEntityName(target) == StringView(u8"hit by rock"));
}
#endif
