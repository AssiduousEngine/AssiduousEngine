// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A script's writes to a component reach its manager's marks, in every language built: a property
// set, a field of a value member (`offset.x`), and a non-const method mark the component; a read
// and a const method do not. A component's consumer (a retained render scene) relies on it: a
// script writes components through handles, never through Edit.

#include <doctest/doctest.h>
#include <cstdio>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

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

namespace marking
{
    // A struct member of a component, whose own fields a script can set.
    struct GaugeRange
    {
        f32 low = 0.0f;
        f32 high = 1.0f;
    };

    REFLECT_VALUE(GaugeRange, "rtti::marking")
    {
        builder.Property<&GaugeRange::low>("low").Property<&GaugeRange::high>("high");
    }

    // A component with a value, a struct member, a method that changes it and one that does not.
    struct Gauge
    {
        f32 level = 1.0f;
        GaugeRange range;

        void Nudge(f32 by) { level += by; }
        [[nodiscard]] f32 Total() const { return level + range.low + range.high; }
    };

    REFLECT_VALUE(Gauge, "rtti::marking")
    {
        builder.Property<&Gauge::level>("level")
            .Property<&Gauge::range>("range")
            .Method<&Gauge::Nudge>("nudge")
            .Method<&Gauge::Total>("total");
        builder.Apply(&foundation::script::ComponentMethods<Gauge>);
    }

    class GaugeManager final : public scene::ComponentManager<Gauge>
    {
    };

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
            RttiRegisterValue_GaugeRange();
            RttiRegisterValue_Gauge();
            GlobalTypeRegistry().Register(TypeOf<GaugeRange>());
            GlobalTypeRegistry().Register(TypeOf<Gauge>());
            // Scripts reach it as Gauge, as a game's own component is reached (its domain's facade).
            RegisterExtraScriptRootType(&TypeOf<Gauge>());
            RegisterExtraFacadeName(u8"Gauge");
            return true;
        }();
        (void)once;
    }

    // Runs `source`'s onStart once on an entity holding a Gauge, with marking on, and answers
    // whether the Gauge was marked, and its level after.
    struct Outcome
    {
        bool marked = false;
        f32 level = 0.0f;
        f32 high = 0.0f; // the struct member's field
        bool faulted = true;
        String said; // the warnings and errors logged while it ran: why a script faulted
    };

    // Collects the warnings and errors logged while a case runs.
    class Said final : public ILogSink
    {
    public:
        String lines;
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (level >= LogLevel::Warning)
                lines += Format(u8"{}: {}\n", category, message);
        }
    };

    Outcome Run(StringView language, StringView source)
    {
        RegisterOnce();
        Said said;
        GlobalLogger().AddSink(&said);
        foundation::run::RunBase run{DefaultAllocator(), u8"marking"};
        scene::Scene world{DefaultAllocator(), u8"marking"};
        BehaviorRunHost host{DefaultAllocator()};
        world.SetRun(&run);
        auto* gauges = world.AddSystem<GaugeManager>();
        auto* components = world.AddSystem<BehaviorComponentManager>();
        auto* behaviors = world.AddSystem<EntityBehaviorSystem>();
        components->SetScriptSystem(behaviors);
        behaviors->SetRunHost(&host);

        RefPtr<BehaviorClass> writer = MakeRef<BehaviorClass>(DefaultAllocator());
        writer->language = String(language);
        writer->className = String(u8"Writer");
        writer->kind = BehaviorKind::Entity;
        writer->source = String(source);
        writer->handlers.PushBack(String(u8"onStart"));
        writer->BuildProfileName();

        const scene::EntityHandle e = world.CreateEntity(u8"gauge");
        (void)gauges->Add(e);
        AttachedBehavior attached;
        attached.behaviorClass = writer;
        components->Add(e).behaviors.PushBack(Move(attached));

        world.Start();
        world.SetSimulationEnabled(true);
        gauges->SetMarking(true);
        gauges->ClearMarked(); // the Add is not what is asked about
        run.Events().Drain();
        world.Update(1.0f / 60.0f); // onStart: the script's one write, or read

        Outcome outcome;
        for (const scene::EntityHandle listed : gauges->Marked())
            outcome.marked = outcome.marked || listed == e;
        outcome.level = gauges->Get(e)->level;
        outcome.high = gauges->Get(e)->range.high;
        outcome.faulted = components->Get(e)->behaviors[0].faulted;
        GlobalLogger().RemoveSink(&said);
        outcome.said = Move(said.lines);
        return outcome;
    }

    // One script per case: its onStart body, in a language.
    struct Case
    {
        const char* name;
        StringView body;
        bool marks;
        f32 level;
    };
}

#if defined(HAS_PLUGIN_ANGELSCRIPT)
TEST_CASE("marking: AngelScript's writes to a component mark it, and its reads do not")
{
    const marking::Case cases[] = {
        {"a property set", u8"Gauge@ g = Gauge::of(self); g.level = 4.0f;", true, 4.0f},
        // A struct member is a value in AngelScript: copied out, changed, set back whole.
        {"a struct member, set back", u8"Gauge@ g = Gauge::of(self); GaugeRange r = g.range; r.high = 3.0f; g.range = r;", true, 1.0f},
        {"a non-const method", u8"Gauge@ g = Gauge::of(self); g.nudge(0.5f);", true, 1.5f},
        {"a const method", u8"Gauge@ g = Gauge::of(self); float t = g.total();", false, 1.0f},
        {"a read", u8"Gauge@ g = Gauge::of(self); float l = g.level;", false, 1.0f},
    };
    for (const marking::Case& c : cases)
    {
        CAPTURE(c.name);
        String source(u8"class Writer : EntityBehavior {\n"
                      u8"    private Entity self;\n"
                      u8"    Writer(Entity entity) { self = entity; }\n"
                      u8"    void onStart() { ");
        source += c.body;
        source += StringView(u8" }\n}\n");
        const marking::Outcome outcome = marking::Run(u8"angelscript", source.AsView());
        if (outcome.faulted)
            std::fprintf(stderr, "%s faulted:\n%s", c.name, reinterpret_cast<const char*>(outcome.said.CStr()));
        CHECK_FALSE(outcome.faulted);
        CHECK(outcome.marked == c.marks);
        CHECK(outcome.level == doctest::Approx(c.level));
        if (c.marks && StringView(reinterpret_cast<const utf8char*>(c.name)).Contains(u8"struct member"))
            CHECK(outcome.high == doctest::Approx(3.0f)); // written in the component, not a copy
    }
}
#endif

#if defined(HAS_PLUGIN_LUAU)
TEST_CASE("marking: Luau's writes to a component mark it, and its reads do not")
{
    const marking::Case cases[] = {
        {"a property set", u8"local g = Gauge.of(self.entity); g.level = 4", true, 4.0f},
        // A struct member is a value in Luau too: a field set on it changes a copy, so the script sets
        // it back whole.
        {"a struct member, set back", u8"local g = Gauge.of(self.entity); local r = g.range; r.high = 3; g.range = r", true, 1.0f},
        {"a struct member's field (a copy)", u8"local g = Gauge.of(self.entity); g.range.high = 3", false, 1.0f},
        {"a non-const method", u8"local g = Gauge.of(self.entity); g:nudge(0.5)", true, 1.5f},
        {"a const method", u8"local g = Gauge.of(self.entity); local t = g:total()", false, 1.0f},
        {"a read", u8"local g = Gauge.of(self.entity); local l = g.level", false, 1.0f},
    };
    for (const marking::Case& c : cases)
    {
        CAPTURE(c.name);
        String source(u8"Writer = EntityBehavior.class()\n"
                      u8"function Writer.new(entity) return setmetatable({ entity = entity }, Writer) end\n"
                      u8"function Writer:onStart() ");
        source += c.body;
        source += StringView(u8" end\n");
        const marking::Outcome outcome = marking::Run(u8"luau", source.AsView());
        if (outcome.faulted)
            std::fprintf(stderr, "%s faulted:\n%s", c.name, reinterpret_cast<const char*>(outcome.said.CStr()));
        CHECK_FALSE(outcome.faulted);
        CHECK(outcome.marked == c.marks);
        CHECK(outcome.level == doctest::Approx(c.level));
        if (c.marks && StringView(reinterpret_cast<const utf8char*>(c.name)).Contains(u8"struct member"))
            CHECK(outcome.high == doctest::Approx(3.0f)); // written in the component, not a copy
    }
}
#endif
