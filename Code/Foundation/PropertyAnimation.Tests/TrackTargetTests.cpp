// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.propertyanimation - track targets on a scene: a track's component and path bound
// once, read and written on any entity, the transform through the scene, and each reason a target
// does not resolve.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;

using namespace foundation::core;
using namespace foundation::propertyanimation;
namespace scene = foundation::scene;

void RttiRegisterValue_TargetLamp();

namespace
{
    struct TargetLamp
    {
        Float3 offset{0.0f, 0.0f, 0.0f};
        f32 brightness = 1.0f;
    };

    inline void Serialize(ISerializer& ar, TargetLamp& c)
    {
        foundation::core::Serialize(ar, "offset", c.offset);
        foundation::core::Serialize(ar, "brightness", c.brightness);
    }

    class TargetLampManager final : public scene::SerializableComponentManager<TargetLamp>
    {
    public:
        TargetLampManager() : scene::SerializableComponentManager<TargetLamp>(u8"target_lamp") {}
    };

    void EnsureReflected()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            RttiRegisterValue_TargetLamp();
            GlobalTypeRegistry().Register(TypeOf<TargetLamp>());
            return true;
        }();
        (void)once;
    }

    PropertyTrack FloatTrack(StringView component, StringView path, f32 from, f32 to)
    {
        PropertyTrack track;
        track.componentType = String(component);
        track.propertyPath = String(path);
        track.kind = TrackValueKind::Float;
        CurveKey a;
        a.time = 0.0f;
        a.value = from;
        CurveKey b;
        b.time = 1.0f;
        b.value = to;
        track.channels[0].AddKey(a);
        track.channels[0].AddKey(b);
        return track;
    }
}

REFLECT_VALUE(TargetLamp, "rtti::propertyanimation::test")
{
    builder.Property<&TargetLamp::offset>("offset").Property<&TargetLamp::brightness>("brightness");
}

TEST_CASE("track target: a component's property reads and writes on any entity that has it")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    world.AddSystem<TargetLampManager>();
    auto* lamps = world.GetSystem<TargetLampManager>();
    const scene::EntityHandle a = world.CreateEntity(u8"A");
    const scene::EntityHandle b = world.CreateEntity(u8"B");
    const scene::EntityHandle bare = world.CreateEntity(u8"Bare");
    lamps->Add(a).brightness = 2.0f;
    lamps->Add(b).brightness = 3.0f;

    const TrackTarget target = ResolveTrackTarget(world, u8"TargetLamp", u8"brightness");
    REQUIRE(target.IsResolved());
    CHECK(ReadTrackTarget(world, target, a).Get<f32>() == doctest::Approx(2.0f));
    CHECK(ReadTrackTarget(world, target, b).Get<f32>() == doctest::Approx(3.0f));

    CHECK(WriteTrackTarget(world, target, b, Variant::From(5.0f)));
    CHECK(lamps->Get(b)->brightness == doctest::Approx(5.0f));
    CHECK(lamps->Get(a)->brightness == doctest::Approx(2.0f));

    // An entity without the component: nothing to read, nothing written.
    CHECK(ReadTrackTarget(world, target, bare).IsEmpty());
    CHECK_FALSE(WriteTrackTarget(world, target, bare, Variant::From(1.0f)));
}

TEST_CASE("track target: the transform is written through the scene, so the world matrix follows")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    const scene::EntityHandle e = world.CreateEntity(u8"Mover");

    const TrackTarget target = ResolveTrackTarget(world, kTransformTarget, u8"position");
    REQUIRE(target.IsResolved());
    CHECK(target.transform);
    CHECK(WriteTrackTarget(world, target, e, Variant::From(Float3{4.0f, 5.0f, 6.0f})));
    CHECK(world.GetLocalTransform(e).position.x == doctest::Approx(4.0f));
    const Float3 worldPosition = TransformPoint(Float3{0.0f, 0.0f, 0.0f}, world.GetWorldMatrix(e));
    CHECK(worldPosition.y == doctest::Approx(5.0f));
    CHECK(ReadTrackTarget(world, target, e).Get<Float3>().z == doctest::Approx(6.0f));
}

TEST_CASE("track target: each reason a target does not resolve is told apart")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    world.AddSystem<TargetLampManager>();

    CHECK(ResolveTrackTarget(world, u8"NoSuchComponent", u8"brightness").status == TrackTargetStatus::UnknownComponent);
    CHECK(ResolveTrackTarget(world, u8"TargetLamp", u8"missing").status == TrackTargetStatus::UnknownProperty);
    CHECK(ResolveTrackTarget(world, kTransformTarget, u8"missing").status == TrackTargetStatus::UnknownProperty);

    // A Float3 track on a scalar property: every write would fail, so it does not resolve.
    PropertyTrack wrongKind = FloatTrack(u8"TargetLamp", u8"brightness", 0.0f, 1.0f);
    wrongKind.kind = TrackValueKind::Float3;
    CHECK(ResolveTrackTarget(world, wrongKind).status == TrackTargetStatus::KindMismatch);
    CHECK(ResolveTrackTarget(world, FloatTrack(u8"TargetLamp", u8"brightness", 0.0f, 1.0f)).IsResolved());
}

TEST_CASE("track target: applying a track keeps the live value in channels without keys")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    world.AddSystem<TargetLampManager>();
    auto* lamps = world.GetSystem<TargetLampManager>();
    const scene::EntityHandle e = world.CreateEntity(u8"Lamp");
    lamps->Add(e).offset = Float3{1.0f, 2.0f, 3.0f};

    // Only X is keyed: Y and Z stay where they are.
    PropertyTrack track;
    track.componentType = String(u8"TargetLamp");
    track.propertyPath = String(u8"offset");
    track.kind = TrackValueKind::Float3;
    CurveKey a;
    a.time = 0.0f;
    a.value = 0.0f;
    CurveKey b;
    b.time = 1.0f;
    b.value = 10.0f;
    track.channels[0].AddKey(a);
    track.channels[0].AddKey(b);

    const TrackTarget target = ResolveTrackTarget(world, track);
    REQUIRE(target.IsResolved());
    CHECK(ApplyTrack(world, target, e, track, 0.5f));
    const Float3 offset = lamps->Get(e)->offset;
    CHECK(offset.x == doctest::Approx(5.0f));
    CHECK(offset.y == doctest::Approx(2.0f));
    CHECK(offset.z == doctest::Approx(3.0f));
}
