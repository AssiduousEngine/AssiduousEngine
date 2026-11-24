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
void RttiRegisterValue_SeqSky();

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

    // A scene's settings block that can take its values from a profile, as the environment's does.
    struct SeqSky
    {
        f32 light = 1.0f;
        f32 sky = 1.0f;
    };

    class SeqSkySystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override { return &TypeOf<SeqSky>(); }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &own; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"seqsky"; }
        [[nodiscard]] void* EffectiveSettingsInstance() noexcept override { return profile.IsNil() ? &own : &shared; }
        [[nodiscard]] Guid SettingsProfile() noexcept override { return profile; }
        void UseSettingsProfile(const Guid& id) override
        {
            profile = id;
            ++profilesGiven;
        }
        void CopySettingsProfileIntoScene() override
        {
            own = *static_cast<SeqSky*>(EffectiveSettingsInstance());
            profile = Guid{};
        }
        [[nodiscard]] const SeqSky& Effective() const { return profile.IsNil() ? own : shared; }

        SeqSky own;
        SeqSky shared; // the profile's values
        Guid profile;
        u32 profilesGiven = 0;
    };

    void EnsureRegistered()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            RttiRegisterValue_SeqLamp();
            GlobalTypeRegistry().Register(TypeOf<SeqLamp>());
            RttiRegisterValue_SeqSky();
            GlobalTypeRegistry().Register(TypeOf<SeqSky>());
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
        track->property.target = String(u8"SeqLamp");
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

    // A scene's setting keyed from `from` to `to` over one second, bound to no one.
    RefPtr<PropertySequenceTrack> SkySetting(StringView field, f32 from, f32 to)
    {
        RefPtr<PropertySequenceTrack> track = MakeRef<PropertySequenceTrack>(DefaultAllocator());
        track->property.source = String(propertyanimation::kSettingsSource);
        track->property.target = String(u8"seqsky");
        track->property.propertyPath = String(field);
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
        track->property.target = String(propertyanimation::kTransformTarget);
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

REFLECT_VALUE(SeqSky, "rtti::sequence::test")
{
    builder.Property<&SeqSky::light>("light").Property<&SeqSky::sky>("sky");
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

TEST_CASE("sequence: a scene's settings are keyed by a track bound to no one, under each finish")
{
    EnsureRegistered();
    Sequence sequence;
    sequence.tracks.PushBack(SkySetting(u8"light", 1.0f, 0.25f));
    sequence.duration = sequence.ComputeDuration();
    CHECK_FALSE(sequence.tracks[0]->NeedsBinding());

    SUBCASE("release keeps the light where it went")
    {
        scene::Scene world{DefaultAllocator()};
        auto* sky = world.AddSystem<SeqSkySystem>();
        SequencePlay play(DefaultAllocator(), world, sequence);
        play.Play();
        play.Advance(0.5f);
        CHECK(sky->own.light == doctest::Approx(0.625f));
        play.Advance(1.0f);
        CHECK(sky->own.light == doctest::Approx(0.25f));
    }
    SUBCASE("restore puts it back")
    {
        scene::Scene world{DefaultAllocator()};
        auto* sky = world.AddSystem<SeqSkySystem>();
        sky->own.light = 0.8f;
        SequencePlay play(DefaultAllocator(), world, sequence);
        play.finish = SequenceFinish::Restore;
        play.Play();
        play.Advance(2.0f);
        CHECK(sky->own.light == doctest::Approx(0.8f));
    }
    SUBCASE("hold keeps it until stopped")
    {
        scene::Scene world{DefaultAllocator()};
        auto* sky = world.AddSystem<SeqSkySystem>();
        SequencePlay play(DefaultAllocator(), world, sequence);
        play.finish = SequenceFinish::Hold;
        play.Play();
        play.Advance(2.0f);
        play.Stop();
        CHECK(sky->own.light == doctest::Approx(0.25f));
    }
}

TEST_CASE("sequence: a scene on a profile takes its values for the play, and gets the profile back once after two tracks")
{
    EnsureRegistered();
    Sequence sequence;
    sequence.tracks.PushBack(SkySetting(u8"light", 1.0f, 0.0f));
    sequence.tracks.PushBack(SkySetting(u8"sky", 1.0f, 0.5f));
    sequence.duration = sequence.ComputeDuration();
    const Guid profile{0x1122334455667788ull, 0x99aabbccddeeff00ull};

    SUBCASE("release leaves the scene on its own values, as the play left them")
    {
        scene::Scene world{DefaultAllocator()};
        auto* sky = world.AddSystem<SeqSkySystem>();
        sky->shared = SeqSky{0.6f, 0.9f};
        sky->profile = profile;
        SequencePlay play(DefaultAllocator(), world, sequence);
        play.Play();
        play.Advance(2.0f);
        CHECK(sky->profile.IsNil());
        CHECK(sky->Effective().light == doctest::Approx(0.0f));
        CHECK(sky->Effective().sky == doctest::Approx(0.5f));
        CHECK(sky->profilesGiven == 0u);
    }
    SUBCASE("restore gives the profile back, once")
    {
        scene::Scene world{DefaultAllocator()};
        auto* sky = world.AddSystem<SeqSkySystem>();
        sky->own = SeqSky{0.1f, 0.2f}; // stale: not in effect while on the profile
        sky->shared = SeqSky{0.6f, 0.9f};
        sky->profile = profile;
        SequencePlay play(DefaultAllocator(), world, sequence);
        play.finish = SequenceFinish::Restore;
        play.Play();
        play.Advance(0.5f);
        CHECK(sky->profile.IsNil());
        CHECK(sky->Effective().light == doctest::Approx(0.5f));
        play.Advance(2.0f);
        CHECK(sky->profile == profile);
        CHECK(sky->profilesGiven == 1u);
        CHECK(sky->Effective().light == doctest::Approx(0.6f));
        CHECK(sky->own.light == doctest::Approx(0.1f)); // the scene's own block as it was
    }
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
    CHECK(position->property.target.AsView() == propertyanimation::kTransformTarget);
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

namespace
{
    // A kind whose runner counts what the play tells it of pauses.
    struct PauseLog
    {
        i32 paused = 0;
        i32 resumed = 0;
    };

    class PauseProbeTrack final : public SequenceTrack
    {
        RTTI_OBJECT(PauseProbeTrack, SequenceTrack)
    public:
        PauseLog* log = nullptr;

        [[nodiscard]] f32 Duration() const noexcept override { return 1.0f; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const override
        {
            class Runner final : public SequenceTrackRunner
            {
            public:
                explicit Runner(PauseLog* log) : m_log(log) {}
                void Evaluate(const SequenceStep&) override {}
                void Release(bool) override {}
                void SetPaused(bool paused) override { ++(paused ? m_log->paused : m_log->resumed); }

            private:
                PauseLog* m_log;
            };
            return MakeUnique<Runner>(*context.allocator, log);
        }
    };
}
RTTI_DEFINE_OBJECT(PauseProbeTrack, "rtti::sequence::tests")

TEST_CASE("sequence: pausing and resuming a play tells its runners; a hold does not")
{
    EnsureRegistered();
    scene::Scene world{DefaultAllocator()};
    PauseLog log;
    RefPtr<PauseProbeTrack> probe = MakeRef<PauseProbeTrack>(DefaultAllocator());
    probe->log = &log;
    Sequence sequence;
    sequence.tracks.PushBack(probe);
    sequence.markers.PushBack(SequenceMarker{0.5f, String(u8"line"), true});
    sequence.duration = 1.0f;
    SequencePlay play(DefaultAllocator(), world, sequence);

    play.Play();
    play.Pause();
    play.Pause(); // already paused: told once
    CHECK(log.paused == 1);
    play.Resume();
    CHECK(log.resumed == 1);
    play.Advance(0.75f); // stops at the hold: the runners carry on
    REQUIRE(play.IsHeld());
    play.Resume(); // from a hold, not a pause
    CHECK(log.paused == 1);
    CHECK(log.resumed == 1);
}

TEST_CASE("sequence editing: a property track's channels are its parts; keys move, sort, delete and insert")
{
    EnsureRegistered();
    PropertySequenceTrack track;
    track.property.target = String(propertyanimation::kTransformTarget);
    track.property.propertyPath = String(u8"position");
    track.property.kind = propertyanimation::TrackValueKind::Float3;
    REQUIRE(track.KeyChannelCount() == 3u);
    CHECK(track.KeyChannelName(1).AsView() == StringView(u8"y"));
    CHECK(track.KeyCurve(0) == &track.property.channels[0]);

    // x: 0 at 0, 10 at 1. A key inserted at 0.5 takes the curve's value there.
    track.property.channels[0].AddKey(CurveKey{0.0f, 0.0f, 0.0f, 0.0f, CurveKeyInterpolation::Linear, CurveTangentMode::Broken});
    track.property.channels[0].AddKey(CurveKey{1.0f, 10.0f, 0.0f, 0.0f, CurveKeyInterpolation::Linear, CurveTangentMode::Broken});
    CHECK(track.InsertKey(0, 0.5f) == 1);
    CHECK(track.property.channels[0].Keys()[1].value == doctest::Approx(5.0f));
    CHECK(track.InsertKey(0, 0.5f) == 1); // already there: no second key
    CHECK(track.property.channels[0].KeyCount() == 3u);

    // The first key moved past the others lands last; where it went is reported.
    const usize first[] = {0};
    Array<usize> moved;
    track.MoveKeys(0, Span<const usize>(first, 1), 1.5f, moved);
    REQUIRE(moved.Size() == 1u);
    CHECK(moved[0] == 2u);
    Array<f32> times;
    track.GetKeyTimes(0, times);
    REQUIRE(times.Size() == 3u);
    CHECK(times[0] == doctest::Approx(0.5f));
    CHECK(times[2] == doctest::Approx(1.5f));
    track.MoveKeys(0, Span<const usize>(first, 1), -5.0f, moved); // never before 0
    track.GetKeyTimes(0, times);
    CHECK(times[0] == doctest::Approx(0.0f));

    track.DeleteKeys(0, Span<const usize>(first, 1));
    CHECK(track.property.channels[0].KeyCount() == 2u);

    // A rotation is one channel of its keys, and no curve.
    PropertySequenceTrack turn;
    turn.property.kind = propertyanimation::TrackValueKind::Quat;
    CHECK(turn.KeyChannelCount() == 1u);
    CHECK(turn.KeyCurve(0) == nullptr);
    CHECK(turn.InsertKey(0, 0.25f) == 0);
    REQUIRE(turn.property.quatKeys.Size() == 1u);
}

TEST_CASE("sequence editing: an event track is one channel; a new event is named for the inspector to rename")
{
    EventSequenceTrack track;
    track.keys.PushBack(SequenceEventKey{1.0f, String(u8"cue"), String(u8"gate")});
    CHECK(track.InsertKey(0, 0.5f) == 0);
    CHECK(track.keys[0].name.AsView() == StringView(u8"event"));
    const usize second[] = {1};
    Array<usize> moved;
    track.MoveKeys(0, Span<const usize>(second, 1), -0.75f, moved);
    REQUIRE(moved.Size() == 1u);
    CHECK(moved[0] == 0u);
    CHECK(track.keys[0].name.AsView() == StringView(u8"cue")); // the cue now first, at 0.25
    CHECK(track.KeyCurve(0) == nullptr);
    CHECK(track.SectionCount() == 0u);
}

namespace
{
    // A kind whose runner holds the play at `holdAt` until the test reads the line (`read`).
    struct HoldLine
    {
        f32 holdAt = 0.5f;
        bool read = false;
        i32 evaluated = 0;
    };

    class HoldProbeTrack final : public SequenceTrack
    {
        RTTI_OBJECT(HoldProbeTrack, SequenceTrack)
    public:
        HoldLine* line = nullptr;

        [[nodiscard]] f32 Duration() const noexcept override { return 1.0f; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const override
        {
            class Runner final : public SequenceTrackRunner
            {
            public:
                explicit Runner(HoldLine* line) : m_line(line) {}
                void Evaluate(const SequenceStep&) override { ++m_line->evaluated; }
                void Release(bool) override {}
                [[nodiscard]] f32 HoldTime(f32 after, f32 upTo) const override
                {
                    return m_line->holdAt > after && m_line->holdAt <= upTo ? m_line->holdAt : -1.0f;
                }
                [[nodiscard]] bool IsHolding() const override { return !m_line->read; }

            private:
                HoldLine* m_line;
            };
            return MakeUnique<Runner>(*context.allocator, line);
        }
    };
}
RTTI_DEFINE_OBJECT(HoldProbeTrack, "rtti::sequence::tests")

namespace
{
    struct HoldStage
    {
        HoldStage() { EnsureRegistered(); }
        scene::Scene world{DefaultAllocator()};
        HoldLine line;
        Sequence sequence;

        void Build()
        {
            RefPtr<HoldProbeTrack> probe = MakeRef<HoldProbeTrack>(DefaultAllocator());
            probe->line = &line;
            sequence.tracks.PushBack(probe);
            sequence.duration = 1.0f;
        }
    };
}

TEST_CASE("sequence: a runner holds the play at its time until it lets go")
{
    HoldStage stage;
    stage.Build();
    Recorder recorder;
    SequencePlay play(DefaultAllocator(), stage.world, stage.sequence);
    play.SetListener(&recorder);
    play.Play();

    play.Advance(0.75f); // past the runner's hold: stops at it
    CHECK(play.IsHeld());
    CHECK(play.Time() == doctest::Approx(0.5f));
    CHECK(recorder.heard.IsEmpty()); // a runner's hold is its own to show, not a marker's
    const i32 evaluated = stage.line.evaluated;
    play.Advance(1.0f); // still held: nothing moves, nothing is evaluated
    CHECK(play.Time() == doctest::Approx(0.5f));
    CHECK(stage.line.evaluated == evaluated);

    stage.line.read = true; // the runner lets go: the next step goes on
    play.Advance(0.25f);
    CHECK_FALSE(play.IsHeld());
    CHECK(play.Time() == doctest::Approx(0.75f));
    play.Advance(0.5f); // the hold it left does not stop it again
    CHECK(play.IsFinished());
}

TEST_CASE("sequence: a runner's hold already let go is passed in the same step")
{
    HoldStage stage;
    stage.Build();
    stage.line.read = true; // read before the playhead reaches it
    SequencePlay play(DefaultAllocator(), stage.world, stage.sequence);
    play.Play();
    play.Advance(0.75f);
    CHECK_FALSE(play.IsHeld());
    CHECK(play.Time() == doctest::Approx(0.75f)); // the whole step was played
}

TEST_CASE("sequence: a marker and a runner's hold: the earlier wins, Resume lets go of both")
{
    HoldStage stage;
    stage.Build();
    SUBCASE("the marker first")
    {
        stage.sequence.markers.PushBack(SequenceMarker{0.25f, String(u8"line"), true});
        SequencePlay play(DefaultAllocator(), stage.world, stage.sequence);
        play.Play();
        play.Advance(0.75f);
        CHECK(play.Time() == doctest::Approx(0.25f));
        play.Resume();
        play.Advance(0.75f); // then the runner's
        CHECK(play.IsHeld());
        CHECK(play.Time() == doctest::Approx(0.5f));
    }
    SUBCASE("at one time, both hold; Resume lets go of both")
    {
        stage.sequence.markers.PushBack(SequenceMarker{0.5f, String(u8"line"), true});
        SequencePlay play(DefaultAllocator(), stage.world, stage.sequence);
        play.Play();
        play.Advance(0.75f);
        CHECK(play.Time() == doctest::Approx(0.5f));
        stage.line.read = true;
        play.Advance(0.1f); // the marker still holds
        CHECK(play.IsHeld());
        play.Resume();
        play.Advance(0.1f);
        CHECK(play.Time() == doctest::Approx(0.6f));
    }
    SUBCASE("Resume overrides a runner still holding")
    {
        SequencePlay play(DefaultAllocator(), stage.world, stage.sequence);
        play.Play();
        play.Advance(0.75f);
        REQUIRE(play.IsHeld());
        play.Resume();
        play.Advance(0.25f);
        CHECK(play.Time() == doctest::Approx(0.75f));
    }
}

TEST_CASE("sequence: a seek or a stop lets go of a runner's hold")
{
    HoldStage stage;
    stage.Build();
    SequencePlay play(DefaultAllocator(), stage.world, stage.sequence);
    play.Play();
    play.Advance(0.75f);
    REQUIRE(play.IsHeld());
    play.Seek(0.6f);
    CHECK_FALSE(play.IsHeld());
    play.Advance(0.1f);
    CHECK(play.Time() == doctest::Approx(0.7f));
    play.Seek(0.0f);
    play.Advance(0.75f); // reaching it again holds again
    CHECK(play.IsHeld());
    play.Stop();
    CHECK_FALSE(play.IsHeld());
}
