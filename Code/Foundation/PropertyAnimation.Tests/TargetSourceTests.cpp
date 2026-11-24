// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.propertyanimation - target sources: a scene's settings keyed with no entity, a system
// on a shared profile given the profile's values before the first write and the profile back after
// a restore, each reason a settings target does not resolve, a source registered by a test and
// taken back, and a track read as it was saved before targets had sources.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;

using namespace foundation::core;
using namespace foundation::propertyanimation;
namespace scene = foundation::scene;

void RttiRegisterValue_GlowSettings();

namespace
{
    struct GlowSettings
    {
        f32 glow = 1.0f;
        Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    };

    // A scene system with a settings block that can take its values from a profile, as the
    // environment's does: the profile's values are in effect while it is on one.
    class GlowSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override { return &TypeOf<GlowSettings>(); }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &own; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"glow"; }
        [[nodiscard]] void* EffectiveSettingsInstance() noexcept override { return profile.IsNil() ? &own : &profileValues; }
        [[nodiscard]] Guid SettingsProfile() noexcept override { return profile; }
        void UseSettingsProfile(const Guid& id) override { profile = id; }
        void CopySettingsProfileIntoScene() override
        {
            own = *static_cast<GlowSettings*>(EffectiveSettingsInstance());
            profile = Guid{};
        }

        [[nodiscard]] const GlowSettings& Effective() const { return profile.IsNil() ? own : profileValues; }

        GlowSettings own;
        GlowSettings profileValues;
        Guid profile;
    };

    void EnsureReflected()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            RttiRegisterValue_GlowSettings();
            GlobalTypeRegistry().Register(TypeOf<GlowSettings>());
            return true;
        }();
        (void)once;
    }

    PropertyTrack GlowTrack(f32 from, f32 to)
    {
        PropertyTrack track;
        track.source = String(kSettingsSource);
        track.target = String(u8"glow");
        track.propertyPath = String(u8"glow");
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

    // A source of the test's own: one value of its own type, found under the name "dial".
    struct DialValues
    {
        GlowSettings values;
    };
    DialValues g_dial;

    class DialSource final : public PropertyTargetSource
    {
    public:
        [[nodiscard]] StringView Id() const noexcept override { return u8"dial"; }
        [[nodiscard]] bool NeedsEntity() const noexcept override { return false; }
        [[nodiscard]] bool Resolve(scene::Scene& scene, StringView target, TrackTarget& out) const override
        {
            (void)scene;
            if (target != StringView(u8"dial"))
                return false;
            out.object = &g_dial.values;
            out.type = &TypeOf<GlowSettings>();
            return true;
        }
        [[nodiscard]] Variant Read(scene::Scene& scene, const TrackTarget& target, scene::EntityHandle entity) const override
        {
            (void)scene;
            (void)entity;
            return ReadBinding(target.binding, Instance{target.object, target.type});
        }
        bool Write(scene::Scene& scene, const TrackTarget& target, scene::EntityHandle entity, const Variant& value) const override
        {
            (void)scene;
            (void)entity;
            return WriteBinding(target.binding, Instance{target.object, target.type}, value).IsOk();
        }
        void Targets(scene::Scene& scene, scene::EntityHandle entity, Array<PropertyTargetInfo>& out) const override
        {
            (void)scene;
            (void)entity;
            out.PushBack(PropertyTargetInfo{String(u8"dial"), &TypeOf<GlowSettings>()});
        }
    };

    [[nodiscard]] bool Listed(StringView id)
    {
        for (const PropertyTargetSource* source : PropertyTargetSources())
            if (source->Id() == id)
                return true;
        return false;
    }
}

REFLECT_VALUE(GlowSettings, "rtti::propertyanimation::test")
{
    builder.Property<&GlowSettings::glow>("glow").Property<&GlowSettings::tint>("tint");
}

TEST_CASE("target source: a scene's settings are keyed with no entity")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    auto* glow = world.AddSystem<GlowSystem>();
    glow->own.glow = 2.0f;

    const PropertyTrack track = GlowTrack(0.0f, 1.0f);
    const TrackTarget target = ResolveTrackTarget(world, track);
    REQUIRE(target.IsResolved());
    CHECK_FALSE(target.source->NeedsEntity());
    CHECK(target.object == glow);

    // No entity is needed to read or write it.
    CHECK(ReadTrackTarget(world, target, scene::EntityHandle{}).Get<f32>() == doctest::Approx(2.0f));
    CHECK(ApplyTrack(world, target, scene::EntityHandle{}, track, 0.5f));
    CHECK(glow->own.glow == doctest::Approx(0.5f));

    // The editors' list: every system with settings, by its id and type.
    Array<PropertyTargetInfo> targets;
    FindPropertyTargetSource(kSettingsSource)->Targets(world, scene::EntityHandle{}, targets);
    REQUIRE(targets.Size() == 1u);
    CHECK(targets[0].name.AsView() == StringView(u8"glow"));
    CHECK(targets[0].type == &TypeOf<GlowSettings>());
}

TEST_CASE("target source: each reason a settings target does not resolve is told apart")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    world.AddSystem<GlowSystem>();

    CHECK(ResolveTrackTarget(world, kSettingsSource, u8"fog", u8"glow").status == TrackTargetStatus::UnknownTarget);
    CHECK(ResolveTrackTarget(world, kSettingsSource, u8"glow", u8"missing").status == TrackTargetStatus::UnknownProperty);
    CHECK(ResolveTrackTarget(world, u8"no-such-source", u8"glow", u8"glow").status == TrackTargetStatus::UnknownSource);
    PropertyTrack wrongKind = GlowTrack(0.0f, 1.0f);
    wrongKind.propertyPath = String(u8"tint"); // a Color keyed as a Float
    CHECK(ResolveTrackTarget(world, wrongKind).status == TrackTargetStatus::KindMismatch);
}

TEST_CASE("target source: a system on a profile is given the profile's values before the first write, and the profile back")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    auto* glow = world.AddSystem<GlowSystem>();
    glow->own.glow = 9.0f;            // stale: not in effect while on the profile
    glow->profileValues.glow = 4.0f;
    glow->profileValues.tint = Color{0.2f, 0.4f, 0.6f, 1.0f};
    const Guid profile{0x6a7d1c2e0b4f4e8aull, 0x9c1d2f3e4a5b6c7dull};
    glow->profile = profile;

    const PropertyTrack track = GlowTrack(1.0f, 0.0f);
    const TrackTarget target = ResolveTrackTarget(world, track);
    REQUIRE(target.IsResolved());

    // A player keeps what it found, is told of the writes, then writes.
    const Variant original = ReadTrackTarget(world, target, scene::EntityHandle{});
    UniquePtr<PropertyTargetWrites> writes = target.source->BeginWrites(DefaultAllocator(), world, target);
    REQUIRE(writes);
    CHECK(glow->profile.IsNil());                             // the scene's own values are in effect now
    CHECK(glow->own.tint.b == doctest::Approx(0.6f));         // as the profile had them, so the scene looks the same
    CHECK(ApplyTrack(world, target, scene::EntityHandle{}, track, 1.0f));
    CHECK(glow->Effective().glow == doctest::Approx(0.0f));   // the write shows

    // A restore puts the value back, then the profile.
    CHECK(WriteTrackTarget(world, target, scene::EntityHandle{}, original));
    writes->Restored();
    CHECK(glow->profile == profile);
    CHECK(glow->own.glow == doctest::Approx(9.0f));
    CHECK(glow->Effective().glow == doctest::Approx(4.0f));

    // A system on its own values needs nothing given back.
    glow->profile = Guid{};
    CHECK_FALSE(target.source->BeginWrites(DefaultAllocator(), world, target));
}

TEST_CASE("target source: a source registered by a domain resolves, is listed, and is taken back")
{
    EnsureReflected();
    scene::Scene world{DefaultAllocator()};
    CHECK(Listed(kComponentSource));
    CHECK(Listed(kSettingsSource));
    CHECK_FALSE(Listed(u8"dial"));

    RegisterPropertyTargetSource(MakeUnique<DialSource>(DefaultAllocator()));
    CHECK(Listed(u8"dial"));
    g_dial.values.glow = 0.0f;
    const TrackTarget target = ResolveTrackTarget(world, u8"dial", u8"dial", u8"glow");
    REQUIRE(target.IsResolved());
    CHECK(WriteTrackTarget(world, target, scene::EntityHandle{}, Variant::From(0.75f)));
    CHECK(g_dial.values.glow == doctest::Approx(0.75f));

    UnregisterPropertyTargetSource(u8"dial");
    CHECK_FALSE(Listed(u8"dial"));
    CHECK(ResolveTrackTarget(world, u8"dial", u8"dial", u8"glow").status == TrackTargetStatus::UnknownSource);

    // The two built in can be neither replaced nor taken back.
    UnregisterPropertyTargetSource(kSettingsSource);
    CHECK(FindPropertyTargetSource(kSettingsSource) != nullptr);
}

TEST_CASE("target source: a track keeps its source and target, and one saved before sources reads as a component")
{
    PropertyTrack track = GlowTrack(0.0f, 1.0f);
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializePropertyTrack(writer, track);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    PropertyTrack read;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializePropertyTrack(reader, read);
    }
    CHECK(read.source.AsView() == kSettingsSource);
    CHECK(read.target.AsView() == StringView(u8"glow"));
    CHECK(read.channels[0].KeyCount() == 2u);

    // The previous layout: the target under `component`, then the rest as now.
    MemoryStream old;
    {
        BinarySerializer writer(old, SerializeMode::Write);
        String component(u8"Light");
        String path(u8"intensity");
        u8 kind = static_cast<u8>(TrackValueKind::Float);
        Serialize(writer, "component", component);
        Serialize(writer, "path", path);
        Serialize(writer, "kind", kind);
        Array<u32> counts;
        counts.PushBack(1u);
        Array<f32> times;
        times.PushBack(0.5f);
        Array<f32> values;
        values.PushBack(3.0f);
        Array<f32> zeros;
        zeros.PushBack(0.0f);
        Array<u8> linear;
        linear.PushBack(static_cast<u8>(CurveKeyInterpolation::Linear));
        Array<u8> broken;
        broken.PushBack(static_cast<u8>(CurveTangentMode::Broken));
        Array<f32> quatTime;
        Array<Float4> quatValue;
        Serialize(writer, "keyCount", counts);
        Serialize(writer, "keyTime", times);
        Serialize(writer, "keyValue", values);
        Serialize(writer, "keyTangentIn", zeros);
        Serialize(writer, "keyTangentOut", zeros);
        Serialize(writer, "keyInterpolation", linear);
        Serialize(writer, "keyTangentMode", broken);
        Serialize(writer, "quatTime", quatTime);
        Serialize(writer, "quatValue", quatValue);
    }
    (void)old.Seek(0, SeekOrigin::Begin);
    PropertyTrack legacy;
    {
        BinarySerializer reader(old, SerializeMode::Read);
        SerializePropertyTrack(reader, legacy, /*previousLayout*/ true);
    }
    CHECK(legacy.source.AsView() == kComponentSource);
    CHECK(legacy.target.AsView() == StringView(u8"Light"));
    CHECK(legacy.propertyPath.AsView() == StringView(u8"intensity"));
    REQUIRE(legacy.channels[0].KeyCount() == 1u);
    CHECK(legacy.channels[0].Evaluate(0.5f) == doctest::Approx(3.0f));
}
