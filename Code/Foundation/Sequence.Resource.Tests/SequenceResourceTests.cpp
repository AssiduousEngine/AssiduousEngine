// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.sequence.resource - the factory loads a cooked sequence and binds what its tracks
// refer to; a nested sequence plays inside another on the same cast, over its section, clamped or
// looped, and a sequence never plays inside itself.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;

using namespace foundation::core;
using namespace foundation::sequence;
namespace scene = foundation::scene;
namespace propertyanimation = foundation::propertyanimation;

namespace
{
    void EnsureRegistered()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            RegisterSequenceResource();
            return true;
        }();
        (void)once;
    }

    // Moves "Actor" along X from 0 to `toX` over one second.
    RefPtr<SequenceResource> Walk(f32 toX)
    {
        RefPtr<SequenceResource> walk = MakeRef<SequenceResource>(DefaultAllocator());
        walk->sequence.bindings.PushBack(SequenceBinding{String(u8"Actor")});
        RefPtr<PropertySequenceTrack> track = MakeRef<PropertySequenceTrack>(DefaultAllocator());
        track->binding = 0;
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
        walk->sequence.tracks.PushBack(track);
        RefPtr<EventSequenceTrack> step = MakeRef<EventSequenceTrack>(DefaultAllocator());
        step->keys.PushBack(SequenceEventKey{0.5f, String(u8"step"), String()});
        walk->sequence.tracks.PushBack(step);
        walk->sequence.duration = 1.0f;
        return walk;
    }

    RefPtr<NestedSequenceTrack> Nested(const RefPtr<SequenceResource>& inner, f32 start, f32 length, bool loop)
    {
        RefPtr<NestedSequenceTrack> track = MakeRef<NestedSequenceTrack>(DefaultAllocator());
        track->name = String(u8"nested");
        track->sequence = inner;
        track->start = start;
        track->length = length;
        track->loop = loop;
        return track;
    }

    class StepCounter final : public ISequenceListener
    {
    public:
        u32 steps = 0;
        void OnSequenceEvent(scene::EntityHandle, StringView name, StringView) override
        {
            steps += name == StringView(u8"step") ? 1u : 0u;
        }
        void OnSequenceHeld(StringView) override {}
        void OnSequenceFinished() override {}
    };
}

TEST_CASE("sequence resource: a nested sequence plays over its section, on the outer cast")
{
    EnsureRegistered();
    scene::Scene world{DefaultAllocator()};
    const scene::EntityHandle actor = world.CreateEntity(u8"Actor");

    Sequence outer;
    outer.bindings.PushBack(SequenceBinding{String(u8"Actor")});
    outer.tracks.PushBack(Nested(Walk(10.0f), 1.0f, 0.0f, false));
    outer.duration = outer.ComputeDuration();
    CHECK(outer.duration == doctest::Approx(2.0f));

    StepCounter counter;
    SequencePlay play(DefaultAllocator(), world, outer);
    play.SetListener(&counter);
    play.Bind(u8"Actor", actor);
    play.Play();
    play.Advance(0.5f); // before the section: nothing moves
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(0.0f));
    play.Advance(1.0f); // 1.5: halfway through the nested walk
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(5.0f));
    CHECK(counter.steps == 1u); // its events reach the outer listener
    play.Advance(1.0f); // past the end: it stays at its end
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(10.0f));
    CHECK(counter.steps == 1u);
}

TEST_CASE("sequence resource: a looped nested sequence repeats over its section")
{
    EnsureRegistered();
    scene::Scene world{DefaultAllocator()};
    const scene::EntityHandle actor = world.CreateEntity(u8"Actor");

    Sequence outer;
    outer.bindings.PushBack(SequenceBinding{String(u8"Actor")});
    outer.tracks.PushBack(Nested(Walk(10.0f), 0.0f, 3.0f, true));
    outer.duration = outer.ComputeDuration();

    StepCounter counter;
    SequencePlay play(DefaultAllocator(), world, outer);
    play.SetListener(&counter);
    play.Bind(u8"Actor", actor);
    play.Play();
    play.Advance(1.25f); // a quarter into the second pass
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(2.5f));
    play.Advance(1.0f); // 2.25: a quarter into the third pass
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(2.5f));
    CHECK(counter.steps == 2u); // once in each pass played through so far
}

TEST_CASE("sequence resource: a sequence never plays inside itself")
{
    EnsureRegistered();
    scene::Scene world{DefaultAllocator()};
    RefPtr<SequenceResource> loop = MakeRef<SequenceResource>(DefaultAllocator());
    loop->sequence.tracks.PushBack(Nested(loop, 0.0f, 1.0f, false));
    loop->sequence.duration = 1.0f;

    SequencePlay play(DefaultAllocator(), world, loop->sequence);
    play.Play(); // the nested track is refused with a warning; nothing recurses
    play.Advance(0.5f);
    CHECK(play.IsPlaying());
    // Break the cycle so the resource frees.
    loop->sequence.tracks.Clear();
}

TEST_CASE("sequence resource: the factory loads a cooked sequence and binds what its tracks refer to")
{
    EnsureRegistered();
    using namespace foundation::vfs;
    using namespace foundation::resource;

    FileDelete(u8"scratch_sequence_resource_db/walk.rasset");
    FileDelete(u8"scratch_sequence_resource_db/outer.rasset");
    RemoveDirectory(u8"scratch_sequence_resource_db");
    NativeFileSystem mount(u8"scratch_sequence_resource_db", DefaultAllocator());

    Guid walkId;
    Guid outerId;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        auto* walkInstance = db.RootGroup()->CreateInstance(u8"walk", SequenceSource::StaticType());
        auto* outerInstance = db.RootGroup()->CreateInstance(u8"outer", SequenceSource::StaticType());
        walkId = walkInstance->Id();
        outerId = outerInstance->Id();

        SequenceSource walk;
        walk.sequence = Walk(10.0f)->sequence;
        REQUIRE(walkInstance->WriteObject(walk).IsOk());

        SequenceSource outer;
        outer.sequence.bindings.PushBack(SequenceBinding{String(u8"Actor")});
        RefPtr<NestedSequenceTrack> nested = MakeRef<NestedSequenceTrack>(DefaultAllocator());
        nested->sequence.SetId(walkId);
        nested->start = 0.5f;
        outer.sequence.tracks.PushBack(nested);
        outer.sequence.duration = 1.5f;
        REQUIRE(outerInstance->WriteObject(outer).IsOk());
    }

    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    SequenceFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<SequenceResource> outer = manager.Bind<SequenceResource>(outerId);
    REQUIRE(outer);
    REQUIRE(outer->sequence.tracks.Size() == 1u);
    auto* nested = Cast<NestedSequenceTrack>(outer->sequence.tracks[0].Get());
    REQUIRE(nested != nullptr);
    CHECK(nested->start == doctest::Approx(0.5f));
    REQUIRE(nested->sequence.Get() != nullptr); // bound by the factory, through the manager
    CHECK(nested->sequence->sequence.tracks.Size() == 2u);
    CHECK(nested->SectionLength() == doctest::Approx(1.0f));

    FileDelete(u8"scratch_sequence_resource_db/walk.rasset");
    FileDelete(u8"scratch_sequence_resource_db/outer.rasset");
    RemoveDirectory(u8"scratch_sequence_resource_db");
}

namespace
{
    // A kind whose runner says whether it was last told the play paused.
    class PauseFlagTrack final : public SequenceTrack
    {
        RTTI_OBJECT(PauseFlagTrack, SequenceTrack)
    public:
        bool* paused = nullptr;

        [[nodiscard]] f32 Duration() const noexcept override { return 1.0f; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const override
        {
            class Runner final : public SequenceTrackRunner
            {
            public:
                explicit Runner(bool* paused) : m_paused(paused) {}
                void Evaluate(const SequenceStep&) override {}
                void Release(bool) override {}
                void SetPaused(bool paused) override { *m_paused = paused; }

            private:
                bool* m_paused;
            };
            return MakeUnique<Runner>(*context.allocator, paused);
        }
    };
}
RTTI_DEFINE_OBJECT(PauseFlagTrack, "rtti::sequence::tests")

TEST_CASE("sequence resource: a nested sequence's runners pause with the outer play")
{
    EnsureRegistered();
    scene::Scene world{DefaultAllocator()};
    bool paused = false;
    RefPtr<SequenceResource> inner = MakeRef<SequenceResource>(DefaultAllocator());
    RefPtr<PauseFlagTrack> flag = MakeRef<PauseFlagTrack>(DefaultAllocator());
    flag->paused = &paused;
    inner->sequence.tracks.PushBack(flag);
    inner->sequence.duration = 1.0f;

    Sequence outer;
    outer.tracks.PushBack(Nested(inner, 0.0f, 0.0f, false));
    outer.duration = outer.ComputeDuration();
    SequencePlay play(DefaultAllocator(), world, outer);
    play.Play();
    play.Advance(0.25f);
    play.Pause();
    CHECK(paused);
    play.Resume();
    CHECK_FALSE(paused);
}
