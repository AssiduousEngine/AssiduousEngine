// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.sequence - a play on a scene: property tracks over several bound entities, events
// fired only when played through, holds, loops, the three finishes, rebinding, the editor's pose,
// serialization, and the track kinds found through the type registry.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;

using namespace foundation::core;
using namespace foundation::sequence;
namespace scene = foundation::scene;
namespace propertyanimation = foundation::propertyanimation;

void RttiRegisterValue_SeqLamp();

namespace
{
    struct SeqLamp
    {
        f32 brightness = 1.0f;
    };

    inline void Serialize(ISerializer& ar, SeqLamp& c) { foundation::core::Serialize(ar, "brightness", c.brightness); }

    class SeqLampManager final : public scene::SerializableComponentManager<SeqLamp>
    {
    public:
        SeqLampManager() : scene::SerializableComponentManager<SeqLamp>(u8"seq_lamp") {}
    };

    void EnsureRegistered()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            RttiRegisterValue_SeqLamp();
            GlobalTypeRegistry().Register(TypeOf<SeqLamp>());
            RegisterSequenceTypes();
            return true;
        }();
        (void)once;
    }

    // Each report as a line: "event:<name>=<value>@bound|@none", "held:<marker>", "finished".
    class Recorder final : public ISequenceListener
    {
    public:
        Array<String> heard;

        void OnSequenceEvent(scene::EntityHandle target, StringView name, StringView value) override
        {
            String line(u8"event:");
            line.Append(name);
            line.Append(u8"=");
            line.Append(value);
            line.Append(target.IsAssigned() ? StringView(u8"@bound") : StringView(u8"@none"));
            heard.PushBack(Move(line));
        }
        void OnSequenceHeld(StringView marker) override
        {
            String line(u8"held:");
            line.Append(marker);
            heard.PushBack(Move(line));
        }
        void OnSequenceFinished() override { heard.PushBack(String(u8"finished")); }

        [[nodiscard]] bool Has(StringView line) const
        {
            for (const String& h : heard)
            {
                if (h.AsView() == line)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] usize Count(StringView line) const
        {
            usize n = 0;
            for (const String& h : heard)
            {
                n += h.AsView() == line ? 1u : 0u;
            }
            return n;
        }
    };

    // A linear 0 -> `to` property track over one second.
    RefPtr<PropertySequenceTrack> Brightness(i32 binding, f32 from, f32 to)
    {
        RefPtr<PropertySequenceTrack> track = MakeRef<PropertySequenceTrack>(DefaultAllocator());
        track->name = String(u8"brightness");
        track->binding = binding;
        track->property.componentType = String(u8"SeqLamp");
        track->property.propertyPath = String(u8"brightness");
        track->property.kind = propertyanimation::TrackValueKind::Float;
        CurveKey a;
        a.time = 0.0f;
        a.value = from;
        CurveKey b;
        b.time = 1.0f;
        b.value = to;
        track->property.channels[0].AddKey(a);
        track->property.channels[0].AddKey(b);
        return track;
    }

    RefPtr<PropertySequenceTrack> Position(i32 binding, f32 toX)
    {
        RefPtr<PropertySequenceTrack> track = MakeRef<PropertySequenceTrack>(DefaultAllocator());
        track->name = String(u8"position");
        track->binding = binding;
        track->property.componentType = String(propertyanimation::kTransformTarget);
        track->property.propertyPath = String(u8"position");
        track->property.kind = propertyanimation::TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 1.0f;
        b.value = toX;
        track->property.channels[0].AddKey(a);
        track->property.channels[0].AddKey(b);
        return track;
    }

    SequenceEventKey Key(f32 time, StringView name, StringView value = {})
    {
        return SequenceEventKey{time, String(name), String(value)};
    }

    template <typename... Keys>
    RefPtr<EventSequenceTrack> Events(i32 binding, Keys... keys)
    {
        RefPtr<EventSequenceTrack> track = MakeRef<EventSequenceTrack>(DefaultAllocator());
        track->name = String(u8"events");
        track->binding = binding;
        (track->keys.PushBack(Move(keys)), ...);
        return track;
    }

    struct Stage
    {
        scene::Scene world{DefaultAllocator()};
        SeqLampManager* lamps = nullptr;
        scene::EntityHandle mentor;
        scene::EntityHandle lamp;

        Stage()
        {
            EnsureRegistered();
            world.AddSystem<SeqLampManager>();
            lamps = world.GetSystem<SeqLampManager>();
            mentor = world.CreateEntity(u8"Mentor");
            lamp = world.CreateEntity(u8"Lamp");
            lamps->Add(lamp).brightness = 1.0f;
        }
    };

    Sequence TwoActors()
    {
        Sequence sequence;
        sequence.bindings.PushBack(SequenceBinding{String(u8"Mentor")});
        sequence.bindings.PushBack(SequenceBinding{String(u8"Lamp")});
        sequence.tracks.PushBack(Position(0, 10.0f));
        sequence.tracks.PushBack(Brightness(1, 0.0f, 4.0f));
        sequence.duration = sequence.ComputeDuration();
        return sequence;
    }
}

REFLECT_VALUE(SeqLamp, "rtti::sequence::test")
{
    builder.Property<&SeqLamp::brightness>("brightness");
}

TEST_CASE("sequence: one play drives several bound entities at once")
{
    Stage stage;
    const Sequence sequence = TwoActors();
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    CHECK(play.Bind(u8"Mentor", stage.mentor));
    CHECK(play.Bind(u8"Lamp", stage.lamp));
    CHECK_FALSE(play.Bind(u8"Nobody", stage.lamp));

    play.Play();
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(0.0f)); // time 0 applied at once
    play.Advance(0.5f);
    CHECK(stage.world.GetLocalTransform(stage.mentor).position.x == doctest::Approx(5.0f));
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(2.0f));
    play.Advance(0.75f);
    CHECK(play.IsFinished());
    CHECK(stage.world.GetLocalTransform(stage.mentor).position.x == doctest::Approx(10.0f));
}

TEST_CASE("sequence: a track whose name is not filled is skipped, the others play")
{
    Stage stage;
    const Sequence sequence = TwoActors();
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    CHECK(play.Bind(u8"Lamp", stage.lamp)); // Mentor left empty
    play.Play();
    play.Advance(0.5f);
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(2.0f));
    CHECK(stage.world.GetLocalTransform(stage.mentor).position.x == doctest::Approx(0.0f));
}

TEST_CASE("sequence: events fire once when played through, never on a seek or a pose")
{
    Stage stage;
    Sequence sequence;
    sequence.bindings.PushBack(SequenceBinding{String(u8"Mentor")});
    sequence.tracks.PushBack(Events(-1, Key(0.0f, u8"start"), Key(0.5f, u8"cue", u8"gate")));
    sequence.tracks.PushBack(Events(0, Key(0.75f, u8"wave")));
    sequence.duration = 1.0f;
    Recorder recorder;
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    play.SetListener(&recorder);
    play.Bind(u8"Mentor", stage.mentor);

    play.Play();
    CHECK(recorder.Has(u8"event:start=@none")); // a key at 0 fires on the first step
    play.Advance(0.5f);
    CHECK(recorder.Has(u8"event:cue=gate@none"));
    play.Advance(0.1f);
    CHECK(recorder.Count(u8"event:cue=gate@none") == 1u);
    play.Advance(0.2f);
    CHECK(recorder.Has(u8"event:wave=@bound")); // a bound track reports its entity

    recorder.heard.Clear();
    play.Seek(0.0f);
    play.Seek(0.9f); // jumps over every key
    play.Pose(0.5f);
    CHECK(recorder.heard.IsEmpty());
}

TEST_CASE("sequence: a hold stops the playhead until the play is resumed")
{
    Stage stage;
    Sequence sequence = TwoActors();
    sequence.markers.PushBack(SequenceMarker{0.25f, String(u8"line1"), true});
    sequence.markers.PushBack(SequenceMarker{0.5f, String(u8"note"), false}); // not a hold
    Recorder recorder;
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    play.SetListener(&recorder);
    play.Bind(u8"Lamp", stage.lamp);
    play.Play();

    play.Advance(0.6f); // past the hold: stops at it
    CHECK(play.IsHeld());
    CHECK(play.Time() == doctest::Approx(0.25f));
    CHECK(recorder.Has(u8"held:line1"));
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f));
    play.Advance(1.0f); // held: nothing moves
    CHECK(play.Time() == doctest::Approx(0.25f));

    play.Resume();
    play.Advance(0.25f);
    CHECK(play.Time() == doctest::Approx(0.5f));
    CHECK(recorder.Count(u8"held:line1") == 1u); // the hold it left does not stop it again
}

TEST_CASE("sequence: a loop wraps, firing what is at its start again")
{
    Stage stage;
    Sequence sequence;
    sequence.tracks.PushBack(Events(-1, Key(0.0f, u8"top")));
    sequence.duration = 1.0f;
    Recorder recorder;
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    play.SetListener(&recorder);
    play.loop = true;
    play.Play();
    play.Advance(1.25f);
    CHECK(play.Time() == doctest::Approx(0.25f));
    CHECK_FALSE(play.IsFinished());
    CHECK(recorder.Count(u8"event:top=@none") == 2u);
    CHECK_FALSE(recorder.Has(u8"finished"));
}

TEST_CASE("sequence: the finish lets go, holds, or puts every value back")
{
    SUBCASE("release keeps what moved")
    {
        Stage stage;
        const Sequence sequence = TwoActors();
        Recorder recorder;
        SequencePlay play(DefaultAllocator(), stage.world, sequence);
        play.SetListener(&recorder);
        play.Bind(u8"Lamp", stage.lamp);
        play.Play();
        play.Advance(2.0f);
        CHECK(recorder.Has(u8"finished"));
        CHECK_FALSE(play.IsPlaying());
        CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(4.0f));
    }
    SUBCASE("restore puts back what was there before the first write")
    {
        Stage stage;
        const Sequence sequence = TwoActors();
        SequencePlay play(DefaultAllocator(), stage.world, sequence);
        play.finish = SequenceFinish::Restore;
        play.Bind(u8"Mentor", stage.mentor);
        play.Bind(u8"Lamp", stage.lamp);
        play.Play();
        play.Advance(2.0f);
        CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f));
        CHECK(stage.world.GetLocalTransform(stage.mentor).position.x == doctest::Approx(0.0f));
    }
    SUBCASE("hold keeps the last frame until stopped")
    {
        Stage stage;
        const Sequence sequence = TwoActors();
        SequencePlay play(DefaultAllocator(), stage.world, sequence);
        play.finish = SequenceFinish::Hold;
        play.Bind(u8"Lamp", stage.lamp);
        play.Play();
        play.Advance(2.0f);
        CHECK(play.IsFinished());
        CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(4.0f));
        play.Stop();
        CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(4.0f));
    }
}

TEST_CASE("sequence: two tracks on one property restore to the value before both")
{
    Stage stage;
    Sequence sequence;
    sequence.bindings.PushBack(SequenceBinding{String(u8"Lamp")});
    sequence.tracks.PushBack(Brightness(0, 0.0f, 4.0f));
    sequence.tracks.PushBack(Brightness(0, 10.0f, 20.0f)); // later track, written last
    sequence.duration = sequence.ComputeDuration();
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    play.finish = SequenceFinish::Restore;
    play.Bind(u8"Lamp", stage.lamp);
    play.Play();
    play.Advance(0.5f);
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(15.0f));
    play.Stop();
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f));
}

TEST_CASE("sequence: binding a name during a play moves its tracks to the new entity")
{
    Stage stage;
    const scene::EntityHandle other = stage.world.CreateEntity(u8"Other");
    stage.lamps->Add(other).brightness = 7.0f;
    const Sequence sequence = TwoActors();
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    play.Bind(u8"Lamp", stage.lamp);
    play.Play();
    play.Advance(0.25f);
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f));
    play.Bind(u8"Lamp", other);
    play.Advance(0.25f);
    CHECK(stage.lamps->Get(other)->brightness == doctest::Approx(2.0f));
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f)); // left where it was
}

TEST_CASE("sequence: the editor's pose applies values and restores them on stop")
{
    Stage stage;
    const Sequence sequence = TwoActors();
    SequencePlay play(DefaultAllocator(), stage.world, sequence);
    play.finish = SequenceFinish::Restore;
    play.Bind(u8"Lamp", stage.lamp);
    play.Pose(0.75f);
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(3.0f));
    play.Pose(0.25f);
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f));
    play.Stop();
    CHECK(stage.lamps->Get(stage.lamp)->brightness == doctest::Approx(1.0f));
}

TEST_CASE("sequence: a sequence reads back as it was written, every track by its kind")
{
    EnsureRegistered();
    Sequence original = TwoActors();
    original.frameRate = 24.0f;
    original.markers.PushBack(SequenceMarker{0.5f, String(u8"line"), true});
    original.tracks.PushBack(Events(0, Key(0.25f, u8"cue", u8"gate")));
    original.tracks[1]->muted = true;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeSequence(writer, original);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    Sequence read;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeSequence(reader, read);
    }

    CHECK(read.duration == doctest::Approx(original.duration));
    CHECK(read.frameRate == doctest::Approx(24.0f));
    REQUIRE(read.bindings.Size() == 2u);
    CHECK(read.bindings[1].name.AsView() == StringView(u8"Lamp"));
    REQUIRE(read.markers.Size() == 1u);
    CHECK(read.markers[0].hold);
    REQUIRE(read.tracks.Size() == 3u);
    auto* position = Cast<PropertySequenceTrack>(read.tracks[0].Get());
    REQUIRE(position != nullptr);
    CHECK(position->property.componentType.AsView() == propertyanimation::kTransformTarget);
    CHECK(position->property.Sample(0.5f).Get<Float3>().x == doctest::Approx(5.0f));
    CHECK(read.tracks[1]->muted);
    CHECK(read.tracks[1]->binding == 1);
    auto* events = Cast<EventSequenceTrack>(read.tracks[2].Get());
    REQUIRE(events != nullptr);
    REQUIRE(events->keys.Size() == 1u);
    CHECK(events->keys[0].value.AsView() == StringView(u8"gate"));
}

TEST_CASE("sequence: the track kinds are found through the type registry")
{
    EnsureRegistered();
    const Array<const TypeInfo*> kinds = SequenceTrackKinds();
    bool property = false;
    bool event = false;
    for (const TypeInfo* kind : kinds)
    {
        property = property || kind == &PropertySequenceTrack::StaticType();
        event = event || kind == &EventSequenceTrack::StaticType();
        CHECK(kind != &SequenceTrack::StaticType());
    }
    CHECK(property);
    CHECK(event);
}
