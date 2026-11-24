// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::PropertyAnimation - module `foundation.propertyanimation`.
//
// The data model, evaluation and reflection binding resolver for animating any reflected property
// on any component with keyframe curves, as data. It depends only on foundation.core reflection
// and is kept apart from foundation.animation (skeletal), so headless consumers (the editor page,
// tests, MCP hosts) evaluate a property clip without pulling in bones, poses and skinning.
//
// A clip is a set of tracks; each track targets one property, found by a target source (an entity's
// component, a scene's settings, or what a domain registers), and holds
// either per-component scalar Curves (Float, Float3, Color) or quaternion keys (Quat, slerped,
// never per-component Euler). Evaluation yields a Variant per track at time t; the binding
// resolver walks the reflected property chain (nested paths included) and writes through
// reflection set, so resolve-mode rules and any setter side effects apply exactly as if the
// inspector wrote the value. The resolver takes a reflection Instance; a track target binds a
// track's source, target and path on a scene, once, and reads or writes it there (on any entity, for
// a source that needs one), which is what every player of tracks (the animator, the sequencer, the
// editor's preview) needs.

module;
#include "Core/Prelude.h"

export module foundation.propertyanimation;

import foundation.core;
import foundation.scene;

using namespace foundation::core;

export namespace foundation::propertyanimation
{
    // The value a track animates. Float, Float3 and Color are per-component scalar curves; Quat is
    // slerped keys. The wire stores the kind as a byte, so step tracks (Bool, enums) can be added.
    enum class TrackValueKind : u8
    {
        Float,
        Float3,
        Quat,
        Color,
    };

    // Scalar channels a kind uses (Float=1, Float3=3, Color=4). Quat=0 (it uses quatKeys, not curves).
    [[nodiscard]] inline u32 ChannelCount(TrackValueKind kind) noexcept
    {
        switch (kind)
        {
        case TrackValueKind::Float:
            return 1;
        case TrackValueKind::Float3:
            return 3;
        case TrackValueKind::Color:
            return 4;
        case TrackValueKind::Quat:
            return 0;
        }
        return 0;
    }

    // A quaternion keyframe (Quat tracks). Slerp between neighbours; shortest-arc handled by Slerp.
    struct QuatKey
    {
        f32 time = 0.0f;
        Quaternion value = Quaternion::Identity;
    };

    // Max channel count a scalar track needs (Color = 4).
    inline constexpr u32 kMaxChannels = 4;

    // The two target sources property animation registers itself (any other is a domain's).
    // `component`: a reflected component type of the played entity, or its `Transform`.
    inline constexpr StringView kComponentSource = u8"component";
    // `settings`: a scene system's settings block, by its settings id (`environment`).
    inline constexpr StringView kSettingsSource = u8"settings";

    // One animated property. `source` names the target source that finds it (`component` when
    // unset); `target` is what that source finds (a component type's reflected name, a settings id);
    // `propertyPath` is dot-joined for nested props (e.g. "color" or "light.color"). Channel storage:
    // Float uses channels[0]; Float3 channels[0..2]; Color channels[0..3] (r,g,b,a); Quat uses quatKeys.
    struct PropertyTrack
    {
        String source{kComponentSource};
        String target;
        String propertyPath;
        TrackValueKind kind = TrackValueKind::Float;
        Curve channels[kMaxChannels];
        Array<QuatKey> quatKeys;

        // The track's own time span: the longest of its active channels / quat keys.
        [[nodiscard]] f32 Duration() const noexcept
        {
            f32 d = 0.0f;
            const u32 channelCount = ChannelCount(kind);
            for (u32 i = 0; i < channelCount; ++i)
            {
                d = Max(d, channels[i].Duration());
            }
            if (!quatKeys.IsEmpty())
            {
                d = Max(d, quatKeys[quatKeys.Size() - 1].time);
            }
            return d;
        }

        // Sample the track at `time` into a Variant of the matching type (Float->f32, Float3->Float3,
        // Color->Color, Quat->Quaternion). Callers wrap/clamp `time` (loop/pingpong is the clip's job).
        [[nodiscard]] Variant Sample(f32 time) const
        {
            switch (kind)
            {
            case TrackValueKind::Float:
                return Variant::From(channels[0].Evaluate(time));
            case TrackValueKind::Float3:
                return Variant::From(Float3{channels[0].Evaluate(time), channels[1].Evaluate(time),
                                            channels[2].Evaluate(time)});
            case TrackValueKind::Color:
                return Variant::From(Color{channels[0].Evaluate(time), channels[1].Evaluate(time),
                                           channels[2].Evaluate(time), channels[3].Evaluate(time)});
            case TrackValueKind::Quat:
                return Variant::From(SampleQuat(time));
            }
            return Variant{};
        }

        // Sample, but keep `current`'s value for any channel that has no keys. An empty channel does
        // not drive its component; otherwise it would sample 0 and, say, teleport an entity that
        // only animates X to y = z = 0. `current` is the target property's live value; if it is empty
        // (unreadable) an empty channel falls back to 0. A track with all channels empty returns
        // `current` unchanged (a no-op write). Quat: empty quatKeys keeps `current`.
        [[nodiscard]] Variant SampleMerged(f32 time, const Variant& current) const
        {
            switch (kind)
            {
            case TrackValueKind::Float:
                return channels[0].KeyCount() > 0 ? Variant::From(channels[0].Evaluate(time)) : current;
            case TrackValueKind::Float3:
            {
                const Float3 base = current.Is<Float3>() ? current.Get<Float3>() : Float3{};
                return Variant::From(Float3{
                    channels[0].KeyCount() > 0 ? channels[0].Evaluate(time) : base.x,
                    channels[1].KeyCount() > 0 ? channels[1].Evaluate(time) : base.y,
                    channels[2].KeyCount() > 0 ? channels[2].Evaluate(time) : base.z});
            }
            case TrackValueKind::Color:
            {
                const Color base = current.Is<Color>() ? current.Get<Color>() : Color{};
                return Variant::From(Color{
                    channels[0].KeyCount() > 0 ? channels[0].Evaluate(time) : base.r,
                    channels[1].KeyCount() > 0 ? channels[1].Evaluate(time) : base.g,
                    channels[2].KeyCount() > 0 ? channels[2].Evaluate(time) : base.b,
                    channels[3].KeyCount() > 0 ? channels[3].Evaluate(time) : base.a});
            }
            case TrackValueKind::Quat:
                return quatKeys.IsEmpty() ? current : Variant::From(SampleQuat(time));
            }
            return current;
        }

        // Slerp the quaternion keys at `time` (clamped to the ends; shortest-arc via Slerp).
        [[nodiscard]] Quaternion SampleQuat(f32 time) const noexcept
        {
            const usize count = quatKeys.Size();
            if (count == 0)
            {
                return Quaternion::Identity;
            }
            if (count == 1 || time <= quatKeys[0].time)
            {
                return quatKeys[0].value;
            }
            if (time >= quatKeys[count - 1].time)
            {
                return quatKeys[count - 1].value;
            }
            usize i = 0;
            while (i + 1 < count && quatKeys[i + 1].time <= time)
            {
                ++i;
            }
            const QuatKey& a = quatKeys[i];
            const QuatKey& b = quatKeys[i + 1];
            const f32 segment = b.time - a.time;
            if (segment <= 1e-6f)
            {
                return b.value;
            }
            return Slerp(a.value, b.value, (time - a.time) / segment);
        }
    };

    // A cooked or authored clip: a duration and the tracks. Immutable shared data at runtime (each
    // playing component owns only its time, state and binding cache). Loop behavior is the animator
    // component's loopMode, not a clip field.
    struct PropertyAnimationClip
    {
        f32 duration = 0.0f;
        Array<PropertyTrack> tracks;

        // The clip's duration, its longest track's (call after editing to refresh `duration`).
        [[nodiscard]] f32 ComputeDuration() const noexcept
        {
            f32 d = 0.0f;
            for (const PropertyTrack& t : tracks)
            {
                d = Max(d, t.Duration());
            }
            return d;
        }
    };

    // Reads or writes one track, keyed: its source and target, its kind, each channel's keys and the
    // quaternion keys. For data that holds tracks one by one (a sequence's property tracks); a clip's
    // cooked form is PropertyAnimationClipSource's flat wire. `previousLayout` reads a track saved
    // before its target had a source (its target under `component`, a component's name): the legacy
    // reader of the assets that hold tracks, one data version back.
    void SerializePropertyTrack(ISerializer& ar, PropertyTrack& track, bool previousLayout = false);

    // ===================================================================================
    // Binding resolver (reflection only, no scene dependency)
    // ===================================================================================

    // A resolved property path: the chain of PropertyInfo from the component type down to the leaf.
    // PropertyInfo pointers are stable static type metadata, so caching the chain is safe; the nested
    // sub-instance addresses are re-walked on every write, because a raw component or sub-object
    // pointer does not survive a structural change. An empty chain means unresolved.
    struct PropertyBinding
    {
        Array<const PropertyInfo*> chain;

        [[nodiscard]] bool IsResolved() const noexcept { return !chain.IsEmpty(); }
    };

    // Resolve a dot-joined `propertyPath` against `componentType`, walking Nested properties for the
    // intermediate segments. Returns an unresolved binding (empty chain) if any segment is missing or
    // an intermediate segment is not a Nested struct. Reads only static type metadata, no instance.
    [[nodiscard]] PropertyBinding ResolveBinding(const TypeInfo& componentType, StringView propertyPath);

    // Write `value` through a resolved binding into `componentInstance`. Re-derives each nested
    // sub-instance address from the live instance (never cached), then reflection-sets the leaf.
    // Returns InvalidArgument for an unresolved binding, NotFound if a nested address is null, or the
    // leaf setter's status. The value's type must match the leaf property (the caller's Sample does).
    [[nodiscard]] Status WriteBinding(const PropertyBinding& binding, const Instance& componentInstance,
                                      const Variant& value);

    // Read the current leaf value through a resolved binding (the symmetric inverse of WriteBinding).
    // Re-derives each nested sub-instance address from the live instance, then reflection-gets the
    // leaf. Returns an empty Variant for an unresolved binding, an empty instance, or a null nested
    // address. Used to snapshot a property before a transient write (the in-scene animation preview).
    [[nodiscard]] Variant ReadBinding(const PropertyBinding& binding, const Instance& componentInstance);

    // ===================================================================================
    // Track targets on a scene
    // ===================================================================================

    // The component type under which a track animates the entity's own transform (position,
    // rotation, scale). The transform belongs to the scene, not to a component, so it is read and
    // written through Get/SetLocalTransform, which keeps the world matrix in step. Reserved: a real
    // component may not take this name.
    inline constexpr StringView kTransformTarget = u8"Transform";

    // Why a track target is not resolved.
    enum class TrackTargetStatus : u8
    {
        Resolved,
        UnknownSource, // no target source of the track's source id is registered (a plugin's, not loaded)
        UnknownTarget, // the source finds nothing of that name on the scene
        UnknownProperty, // the path names no property of it
        KindMismatch,    // the property's type is not the one the track's kind writes
    };

    class PropertyTargetSource;

    // A track's target bound on one scene by its source: what the source found (`object`, stable for
    // the scene's life: a component's manager, a scene system; null for the transform), the type its
    // path walks and the property chain. A component itself is found again on every read and write,
    // never kept, since its pool moves components.
    struct TrackTarget
    {
        const PropertyTargetSource* source = nullptr;
        void* object = nullptr;
        const TypeInfo* type = nullptr;
        PropertyBinding binding;
        TrackTargetStatus status = TrackTargetStatus::UnknownProperty;

        [[nodiscard]] bool IsResolved() const noexcept { return status == TrackTargetStatus::Resolved; }
    };

    // What a source does around a player's writes to one target, made before its first write. The
    // player keeps it for the play and calls Restored once it has put back the values it changed (a
    // restoring finish). Dropping it without Restored leaves things as the writes left them.
    class PropertyTargetWrites
    {
    public:
        virtual ~PropertyTargetWrites() = default;
        virtual void Restored() = 0;
    };

    // A target a source offers on a scene, for the editors: its name (a track's `target`) and the
    // reflected type whose animatable leaves its tracks can key.
    struct PropertyTargetInfo
    {
        String name;
        const TypeInfo* type = nullptr;
    };

    // Finds the targets of tracks that name it. Property animation registers `component` and
    // `settings`; a domain or a plugin registers its own (RegisterPropertyTargetSource) for what it
    // owns, and takes it back when it unloads.
    class PropertyTargetSource
    {
    public:
        virtual ~PropertyTargetSource() = default;

        // The stable name a track saves as its `source`.
        [[nodiscard]] virtual StringView Id() const noexcept = 0;
        // Whether a track of it is played on an entity (a sequence binds it to one of the cast).
        [[nodiscard]] virtual bool NeedsEntity() const noexcept = 0;
        // Finds `target` on `scene`: sets `out.object` (what the source keeps to find the instance
        // again, null when it needs nothing) and `out.type` (what the path walks). False when the
        // scene has no such target.
        [[nodiscard]] virtual bool Resolve(foundation::scene::Scene& scene, StringView target, TrackTarget& out) const = 0;
        // The target's value, through `target.binding`; empty when there is none to read (the
        // entity lacks the component).
        [[nodiscard]] virtual Variant Read(foundation::scene::Scene& scene, const TrackTarget& target,
                                           foundation::scene::EntityHandle entity) const = 0;
        // The value as the scene shows it, where that is not what Read gives (a settings block on a
        // shared profile shows the profile's): what an editor keys a new track with. Read by default.
        [[nodiscard]] virtual Variant ReadShown(foundation::scene::Scene& scene, const TrackTarget& target,
                                                foundation::scene::EntityHandle entity) const
        {
            return Read(scene, target, entity);
        }
        // Writes `value` to the target; false when nothing was written.
        virtual bool Write(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                           const Variant& value) const = 0;
        // What happens around a player's writes to `target` (see PropertyTargetWrites); none by
        // default.
        [[nodiscard]] virtual UniquePtr<PropertyTargetWrites> BeginWrites(IAllocator& allocator, foundation::scene::Scene& scene,
                                                                          const TrackTarget& target) const
        {
            (void)allocator;
            (void)scene;
            (void)target;
            return {};
        }
        // The targets it offers on `scene` (for `entity`, when it needs one), for the editors.
        virtual void Targets(foundation::scene::Scene& scene, foundation::scene::EntityHandle entity,
                             Array<PropertyTargetInfo>& out) const = 0;
    };

    // Registers `source`, replacing one of the same id. A domain or a plugin calls it as it loads.
    void RegisterPropertyTargetSource(UniquePtr<PropertyTargetSource> source);
    // Takes back the source of `id` (as its domain or plugin unloads). The two built in stay.
    void UnregisterPropertyTargetSource(StringView id);
    // The source registered under `id`, or null.
    [[nodiscard]] const PropertyTargetSource* FindPropertyTargetSource(StringView id);
    // Every registered source, the two built in first.
    [[nodiscard]] Array<const PropertyTargetSource*> PropertyTargetSources();

    // The reflected type a track of `kind` writes (f32, Float3, Color, Quaternion).
    [[nodiscard]] const TypeInfo* LeafTypeOf(TrackValueKind kind) noexcept;

    // Binds `target` and `propertyPath` through the source `source` on `scene`. Not resolved when
    // the source, the target or the path is unknown.
    [[nodiscard]] TrackTarget ResolveTrackTarget(foundation::scene::Scene& scene, StringView source, StringView target,
                                                 StringView propertyPath);
    // Binds `track`'s target on `scene`, and checks that its property's type is the one its kind
    // writes (KindMismatch otherwise, since every write would fail).
    [[nodiscard]] TrackTarget ResolveTrackTarget(foundation::scene::Scene& scene, const PropertyTrack& track);

    // The target's value (on `entity`, for a source that needs one); empty when it is not resolved,
    // or there is nothing to read (the entity has no such component).
    [[nodiscard]] Variant ReadTrackTarget(foundation::scene::Scene& scene, const TrackTarget& target,
                                          foundation::scene::EntityHandle entity);
    // Writes `value` to the target on `entity`; false when nothing was written.
    bool WriteTrackTarget(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                          const Variant& value);
    // Samples `track` at `time` over the target's live value (channels without keys keep it) and
    // writes it: one track's step for a player. False when nothing was written.
    bool ApplyTrack(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                    const PropertyTrack& track, f32 time);
}
