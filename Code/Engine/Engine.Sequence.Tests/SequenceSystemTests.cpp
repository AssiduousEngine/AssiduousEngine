// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.sequence - the scene's player: plays step with the scene, what a play started from script
// reports goes on the run's bus as a notice naming the play, what an owned play reports reaches its
// owner as entity events, an event track bound to an actor posts to that actor, and stopping lets
// go (restoring when asked).

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.messaging;
import foundation.run;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import engine.sequence;

using namespace foundation::core;
using namespace engine::sequence;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;

namespace
{
    // "Actor" walks X from 0 to 10 over one second; a cue at 0.5 (bound to the actor when `boundCue`),
    // a hold named "line" at 0.25 when `hold`.
    RefPtr<sequence::SequenceResource> Walk(bool boundCue = false, bool hold = false)
    {
        RefPtr<sequence::SequenceResource> walk = MakeRef<sequence::SequenceResource>(DefaultAllocator());
        sequence::Sequence& s = walk->sequence;
        s.bindings.PushBack(sequence::SequenceBinding{String(u8"Actor")});
        RefPtr<sequence::PropertySequenceTrack> move = MakeRef<sequence::PropertySequenceTrack>(DefaultAllocator());
        move->binding = 0;
        move->property.componentType = String(propertyanimation::kTransformTarget);
        move->property.propertyPath = String(u8"position");
        move->property.kind = propertyanimation::TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 1.0f;
        b.value = 10.0f;
        move->property.channels[0].AddKey(a);
        move->property.channels[0].AddKey(b);
        s.tracks.PushBack(move);
        RefPtr<sequence::EventSequenceTrack> cues = MakeRef<sequence::EventSequenceTrack>(DefaultAllocator());
        cues->binding = boundCue ? 0 : -1;
        cues->keys.PushBack(sequence::SequenceEventKey{0.5f, String(u8"cue"), String(u8"gate")});
        s.tracks.PushBack(cues);
        if (hold)
        {
            s.markers.PushBack(sequence::SequenceMarker{0.25f, String(u8"line"), true});
        }
        s.duration = 1.0f;
        return walk;
    }

    struct Heard
    {
        String event;
        SequenceNotice notice;
    };

    class EventLog final : public scene::IEntityEventSink
    {
    public:
        struct Entry
        {
            scene::EntityHandle target;
            String name;
            usize args = 0;
            String first;
        };
        Array<Entry> entries;

        void OnEntityEvent(scene::Scene& scene, const scene::EntityEvent& event) override
        {
            (void)scene;
            Entry entry;
            entry.target = event.target;
            entry.name = String(event.name);
            entry.args = event.args.Size();
            if (!event.args.IsEmpty() && event.args[0].Is<String>())
            {
                entry.first = event.args[0].Get<String>();
            }
            entries.PushBack(Move(entry));
        }
    };

    struct Stage
    {
        foundation::run::RunBase run{DefaultAllocator(), u8"sequences"};
        scene::Scene world{DefaultAllocator(), u8"sequences"};
        SequenceSystem* sequences = nullptr;
        scene::EntityHandle actor;
        Array<Heard> heard;

        Stage()
        {
            RegisterCoreTypes(); // Transform's reflection, which the walk's track writes through
            RegisterSequenceReflection();
            world.SetRun(&run);
            AddSequenceSceneSystems(world);
            sequences = world.GetSystem<SequenceSystem>();
            actor = world.CreateEntity(u8"Actor");
            const StringView names[] = {kSequenceEventBusName, kSequenceHeldBusName, kSequenceFinishedBusName};
            for (StringView name : names)
            {
                const String event(name);
                (void)run.Events().Subscribe(StringHash(name),
                                             [this, event](const Variant& payload)
                                             {
                                                 Heard h;
                                                 h.event = event;
                                                 if (payload.Is<SequenceNotice>())
                                                 {
                                                     h.notice = payload.Get<SequenceNotice>();
                                                 }
                                                 heard.PushBack(Move(h));
                                             });
            }
            world.Start();
            world.SetSimulationEnabled(true);
        }

        void Frame(f32 dt)
        {
            world.Update(dt);
            run.Events().Drain();
        }

        [[nodiscard]] f32 X() const { return world.GetLocalTransform(actor).position.x; }
    };
}

TEST_CASE("sequence system: a play steps with the scene and reports on the run's bus, naming its play")
{
    Stage stage;
    const Guid id{0x51, 0x52};
    const u32 play = stage.sequences->Play(Walk(), id);
    REQUIRE(play != 0);
    sequence::SequencePlay* running = stage.sequences->Find(play);
    REQUIRE(running != nullptr);
    CHECK(running->Bind(u8"Actor", stage.actor));

    stage.Frame(0.5f);
    CHECK(stage.X() == doctest::Approx(5.0f));
    stage.Frame(0.75f);
    CHECK(stage.X() == doctest::Approx(10.0f));
    CHECK(stage.sequences->Find(play) == nullptr); // finished and let go
    CHECK(stage.sequences->PlayCount() == 0u);

    REQUIRE(stage.heard.Size() == 2u);
    CHECK(stage.heard[0].event.AsView() == kSequenceEventBusName);
    CHECK(stage.heard[0].notice.play == static_cast<i32>(play));
    CHECK(stage.heard[0].notice.sequence == id);
    CHECK(stage.heard[0].notice.name.AsView() == StringView(u8"cue"));
    CHECK(stage.heard[0].notice.value.AsView() == StringView(u8"gate"));
    CHECK(stage.heard[1].event.AsView() == kSequenceFinishedBusName);
    CHECK(stage.heard[1].notice.play == static_cast<i32>(play));
}

TEST_CASE("sequence system: a hold is reported and the play waits there until resumed")
{
    Stage stage;
    const u32 play = stage.sequences->Play(Walk(false, true), Guid{1, 2});
    stage.sequences->Find(play)->Bind(u8"Actor", stage.actor);
    stage.Frame(0.5f);
    REQUIRE(stage.heard.Size() == 1u);
    CHECK(stage.heard[0].event.AsView() == kSequenceHeldBusName);
    CHECK(stage.heard[0].notice.name.AsView() == StringView(u8"line"));
    stage.Frame(1.0f);
    CHECK(stage.X() == doctest::Approx(2.5f));
    stage.sequences->Find(play)->Resume();
    stage.Frame(0.25f);
    CHECK(stage.X() == doctest::Approx(5.0f));
}

TEST_CASE("sequence system: an owned play reports to its owner; a bound event track to its actor")
{
    Stage stage;
    EventLog log;
    stage.world.AddEntityEventSink(log);
    const scene::EntityHandle director = stage.world.CreateEntity(u8"Director");
    const u32 play = stage.sequences->Play(Walk(true), Guid{3, 4}, sequence::SequenceFinish::Release, director);
    stage.sequences->Find(play)->Bind(u8"Actor", stage.actor);
    stage.Frame(1.5f);

    REQUIRE(log.entries.Size() == 2u);
    CHECK(log.entries[0].target == stage.actor); // the cue's track is bound to the actor
    CHECK(log.entries[0].name.AsView() == kSequenceEventName);
    CHECK(log.entries[0].args == 2u);
    CHECK(log.entries[0].first.AsView() == StringView(u8"cue"));
    CHECK(log.entries[1].target == director);
    CHECK(log.entries[1].name.AsView() == kSequenceFinishedName);
    CHECK(stage.heard.IsEmpty()); // nothing on the bus
}

TEST_CASE("sequence system: stopping lets go, restoring when the finish asks, and the scene stopping stops all")
{
    Stage stage;
    const u32 restored = stage.sequences->Play(Walk(), Guid{5, 6}, sequence::SequenceFinish::Restore);
    stage.sequences->Find(restored)->Bind(u8"Actor", stage.actor);
    stage.Frame(0.5f);
    CHECK(stage.X() == doctest::Approx(5.0f));
    stage.sequences->Stop(restored);
    CHECK(stage.X() == doctest::Approx(0.0f));
    CHECK(stage.sequences->Find(restored) == nullptr);

    const u32 kept = stage.sequences->Play(Walk(), Guid{7, 8});
    stage.sequences->Find(kept)->Bind(u8"Actor", stage.actor);
    stage.Frame(0.5f);
    stage.world.Stop();
    CHECK(stage.sequences->PlayCount() == 0u);
    CHECK(stage.X() == doctest::Approx(5.0f)); // released: what moved stays
}

TEST_CASE("sequence system: a finish is named for scripts")
{
    CHECK(ParseSequenceFinish(u8"hold") == sequence::SequenceFinish::Hold);
    CHECK(ParseSequenceFinish(u8"restore") == sequence::SequenceFinish::Restore);
    CHECK(ParseSequenceFinish(u8"release") == sequence::SequenceFinish::Release);
    CHECK(ParseSequenceFinish(u8"anything else") == sequence::SequenceFinish::Release);
}

TEST_CASE("sequence component: autoplays with its cast and reports to its entity")
{
    Stage stage;
    EventLog log;
    stage.world.AddEntityEventSink(log);
    auto* components = stage.world.GetSystem<SequenceComponentManager>();
    REQUIRE(components != nullptr);
    const scene::EntityHandle stageEntity = stage.world.CreateEntity(u8"Stage");
    SequenceComponent& c = components->Add(stageEntity);
    c.sequence = Walk();
    c.cast.PushBack(SequenceCastEntry{String(u8"Actor"), scene::EntityRef{stage.world.GetEntityId(stage.actor)}, false});

    stage.Frame(0.5f); // started and stepped in the same frame
    CHECK(c.playId != 0);
    CHECK(stage.X() == doctest::Approx(5.0f));
    stage.Frame(1.0f);
    CHECK(stage.X() == doctest::Approx(10.0f));
    CHECK(c.playId != 0); // forgotten at the next update, once the play has left
    stage.Frame(0.1f);
    CHECK(c.playId == 0);
    REQUIRE(log.entries.Size() == 2u); // the unbound cue and the end, to the component's entity
    CHECK(log.entries[0].target == stageEntity);
    CHECK(log.entries[1].name.AsView() == kSequenceFinishedName);
    CHECK(stage.heard.IsEmpty());
}

TEST_CASE("sequence component: plays on its own entity, restarts and stops on request, and stops with its entity")
{
    Stage stage;
    auto* components = stage.world.GetSystem<SequenceComponentManager>();
    SequenceComponent& c = components->Add(stage.actor);
    c.sequence = Walk();
    c.autoplay = false;
    c.finish = sequence::SequenceFinish::Restore;
    c.cast.PushBack(SequenceCastEntry{String(u8"Actor"), scene::EntityRef{}, true}); // this entity

    stage.Frame(0.5f);
    CHECK(c.playId == 0); // no autoplay
    c.play();
    stage.Frame(0.25f);
    CHECK(stage.X() == doctest::Approx(2.5f));
    c.play(); // again: from the start
    stage.Frame(0.25f);
    CHECK(stage.X() == doctest::Approx(2.5f));
    c.stop();
    stage.Frame(0.1f);
    CHECK(c.playId == 0);
    CHECK(stage.X() == doctest::Approx(0.0f)); // restored

    c.play();
    stage.Frame(0.25f);
    const u32 running = c.playId;
    REQUIRE(stage.sequences->Find(running) != nullptr);
    stage.world.DestroyEntity(stage.actor);
    CHECK(stage.sequences->Find(running) == nullptr);
}

TEST_CASE("sequence component: its settings and cast read back as written")
{
    RegisterCoreTypes();
    RegisterSequenceReflection();
    SequenceComponent written;
    written.sequence.SetId(Guid{9, 10});
    written.cast.PushBack(SequenceCastEntry{String(u8"Mentor"), scene::EntityRef{Guid{11, 12}}, false});
    written.cast.PushBack(SequenceCastEntry{String(u8"Stage"), scene::EntityRef{}, true});
    written.autoplay = false;
    written.speed = 0.5f;
    written.loop = true;
    written.finish = sequence::SequenceFinish::Hold;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        Serialize(writer, written);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    SequenceComponent read;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        Serialize(reader, read);
    }
    CHECK(read.sequence.id == Guid{9, 10});
    REQUIRE(read.cast.Size() == 2u);
    CHECK(read.cast[0].entity.id == Guid{11, 12});
    CHECK(read.cast[1].self);
    CHECK_FALSE(read.autoplay);
    CHECK(read.speed == doctest::Approx(0.5f));
    CHECK(read.loop);
    CHECK(read.finish == sequence::SequenceFinish::Hold);
}
