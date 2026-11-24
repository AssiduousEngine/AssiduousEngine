// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.propertyanimation - the binding resolver, the target sources and track targets. The
// resolver walks the reflected property chain (FindProperty for each dot-separated segment, Nested
// for the intermediates) and writes the leaf through reflection set; a track target binds it on a
// scene through the track's source.
// They live outside the interface because they need the reflection header and are not hot enough
// to be worth inlining.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.propertyanimation;

import foundation.core;
import foundation.scene;

using namespace foundation::core;

namespace foundation::propertyanimation
{
    PropertyBinding ResolveBinding(const TypeInfo& componentType, StringView propertyPath)
    {
        PropertyBinding binding;
        if (propertyPath.IsEmpty())
        {
            return binding;
        }

        const TypeInfo* current = &componentType;
        usize start = 0;
        while (true)
        {
            usize dot = start;
            while (dot < propertyPath.Size() && propertyPath[dot] != utf8char('.'))
            {
                ++dot;
            }
            const StringView segment = propertyPath.SubStr(start, dot - start);
            if (segment.IsEmpty() || current == nullptr)
            {
                binding.chain.Clear();
                return binding;
            }
            // FindProperty keys on a C-string; property names are ASCII, so a null-terminated copy
            // of the (non-terminated) path segment reinterprets cleanly.
            const String segName(segment);
            const PropertyInfo* prop =
                FindProperty(*current, reinterpret_cast<const char*>(segName.CStr()));
            if (prop == nullptr)
            {
                binding.chain.Clear();
                return binding;
            }
            binding.chain.PushBack(prop);

            const bool isLast = (dot >= propertyPath.Size());
            if (isLast)
            {
                break;
            }
            // An intermediate segment must be a Nested struct to keep walking.
            if (!IsNested(*prop) || prop->type == nullptr)
            {
                binding.chain.Clear();
                return binding;
            }
            current = prop->type;
            start = dot + 1;
        }
        return binding;
    }

    Status WriteBinding(const PropertyBinding& binding, const Instance& componentInstance,
                        const Variant& value)
    {
        if (!binding.IsResolved() || componentInstance.IsEmpty())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        // Re-walk the nested sub-instances from the live component instance on every call: a
        // sub-object pointer cached across a structural change would dangle.
        Instance current = componentInstance;
        const usize count = binding.chain.Size();
        for (usize i = 0; i + 1 < count; ++i)
        {
            const PropertyInfo* prop = binding.chain[i];
            if (prop->address == nullptr)
            {
                return Status{ErrorCode::NotFound}; // a computed nested property has no address to walk
            }
            void* sub = prop->address(current);
            if (sub == nullptr)
            {
                return Status{ErrorCode::NotFound}; // null nested member
            }
            current = Instance(sub, prop->type);
        }
        return SetProperty(*binding.chain[count - 1], current, value);
    }

    Variant ReadBinding(const PropertyBinding& binding, const Instance& componentInstance)
    {
        if (!binding.IsResolved() || componentInstance.IsEmpty())
        {
            return Variant{};
        }
        // The same live re-walk as WriteBinding (sub-object pointers are never cached), then a
        // reflection get of the leaf.
        Instance current = componentInstance;
        const usize count = binding.chain.Size();
        for (usize i = 0; i + 1 < count; ++i)
        {
            const PropertyInfo* prop = binding.chain[i];
            if (prop->address == nullptr)
            {
                return Variant{}; // a computed nested property has no address to walk
            }
            void* sub = prop->address(current);
            if (sub == nullptr)
            {
                return Variant{}; // null nested member
            }
            current = Instance(sub, prop->type);
        }
        return GetProperty(*binding.chain[count - 1], current);
    }

    void SerializePropertyTrack(ISerializer& ar, PropertyTrack& track, bool previousLayout)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        if (reading && previousLayout)
        {
            // Before a target had a source, every target was a component's name.
            track.source = String(kComponentSource);
            foundation::core::Serialize(ar, "component", track.target);
        }
        else
        {
            foundation::core::Serialize(ar, "source", track.source);
            foundation::core::Serialize(ar, "target", track.target);
        }
        foundation::core::Serialize(ar, "path", track.propertyPath);
        u8 kind = static_cast<u8>(track.kind);
        foundation::core::Serialize(ar, "kind", kind);

        // Every channel's keys in one pool, channel after channel, with each channel's count.
        Array<u32> keyCount;
        Array<f32> keyTime;
        Array<f32> keyValue;
        Array<f32> keyTangentIn;
        Array<f32> keyTangentOut;
        Array<u8> keyInterpolation;
        Array<u8> keyTangentMode;
        Array<f32> quatTime;
        Array<Float4> quatValue;
        if (!reading)
        {
            for (const Curve& channel : track.channels)
            {
                keyCount.PushBack(static_cast<u32>(channel.KeyCount()));
                for (const CurveKey& key : channel.Keys())
                {
                    keyTime.PushBack(key.time);
                    keyValue.PushBack(key.value);
                    keyTangentIn.PushBack(key.tangentIn);
                    keyTangentOut.PushBack(key.tangentOut);
                    keyInterpolation.PushBack(static_cast<u8>(key.interpolation));
                    keyTangentMode.PushBack(static_cast<u8>(key.tangentMode));
                }
            }
            for (const QuatKey& key : track.quatKeys)
            {
                quatTime.PushBack(key.time);
                quatValue.PushBack(Float4{key.value.x, key.value.y, key.value.z, key.value.w});
            }
        }
        foundation::core::Serialize(ar, "keyCount", keyCount);
        foundation::core::Serialize(ar, "keyTime", keyTime);
        foundation::core::Serialize(ar, "keyValue", keyValue);
        foundation::core::Serialize(ar, "keyTangentIn", keyTangentIn);
        foundation::core::Serialize(ar, "keyTangentOut", keyTangentOut);
        foundation::core::Serialize(ar, "keyInterpolation", keyInterpolation);
        foundation::core::Serialize(ar, "keyTangentMode", keyTangentMode);
        foundation::core::Serialize(ar, "quatTime", quatTime);
        foundation::core::Serialize(ar, "quatValue", quatValue);
        if (!reading)
        {
            return;
        }

        // Every count and index is checked against the pools before use: a short or damaged
        // payload leaves the track empty rather than reading past an array.
        track.kind = kind <= static_cast<u8>(TrackValueKind::Color) ? static_cast<TrackValueKind>(kind) : TrackValueKind::Float;
        const usize pool = keyTime.Size();
        const bool pooled = keyValue.Size() == pool && keyTangentIn.Size() == pool && keyTangentOut.Size() == pool &&
                            keyInterpolation.Size() == pool && keyTangentMode.Size() == pool;
        usize cursor = 0;
        for (usize c = 0; c < kMaxChannels; ++c)
        {
            track.channels[c].Clear();
            const usize count = (pooled && c < keyCount.Size()) ? keyCount[c] : 0;
            if (count > pool - cursor)
            {
                break;
            }
            for (usize k = 0; k < count; ++k, ++cursor)
            {
                CurveKey key;
                key.time = keyTime[cursor];
                key.value = keyValue[cursor];
                key.tangentIn = keyTangentIn[cursor];
                key.tangentOut = keyTangentOut[cursor];
                key.interpolation = keyInterpolation[cursor] <= static_cast<u8>(CurveKeyInterpolation::Cubic)
                                        ? static_cast<CurveKeyInterpolation>(keyInterpolation[cursor])
                                        : CurveKeyInterpolation::Linear;
                key.tangentMode = keyTangentMode[cursor] <= static_cast<u8>(CurveTangentMode::Flat)
                                      ? static_cast<CurveTangentMode>(keyTangentMode[cursor])
                                      : CurveTangentMode::Broken;
                track.channels[c].AddKey(key);
            }
        }
        track.quatKeys.Clear();
        const usize quats = Min(quatTime.Size(), quatValue.Size());
        for (usize i = 0; i < quats; ++i)
        {
            QuatKey key;
            key.time = quatTime[i];
            key.value = Quaternion{quatValue[i].x, quatValue[i].y, quatValue[i].z, quatValue[i].w};
            track.quatKeys.PushBack(key);
        }
    }

    const TypeInfo* LeafTypeOf(TrackValueKind kind) noexcept
    {
        switch (kind)
        {
        case TrackValueKind::Float:
            return &TypeOf<f32>();
        case TrackValueKind::Float3:
            return &TypeOf<Float3>();
        case TrackValueKind::Color:
            return &TypeOf<Color>();
        case TrackValueKind::Quat:
            return &TypeOf<Quaternion>();
        }
        return nullptr;
    }

    namespace
    {
        // A component of the played entity, or its transform (kTransformTarget): today's targets.
        class ComponentTargetSource final : public PropertyTargetSource
        {
        public:
            [[nodiscard]] StringView Id() const noexcept override { return kComponentSource; }
            [[nodiscard]] bool NeedsEntity() const noexcept override { return true; }

            [[nodiscard]] bool Resolve(foundation::scene::Scene& scene, StringView target, TrackTarget& out) const override
            {
                if (target == kTransformTarget)
                {
                    out.object = nullptr;
                    out.type = &TypeOf<Transform>();
                    return true;
                }
                foundation::scene::ComponentManagerBase* manager = scene.FindManagerByComponentName(target);
                if (manager == nullptr || manager->ComponentType() == nullptr)
                {
                    return false;
                }
                out.object = manager;
                out.type = manager->ComponentType();
                return true;
            }

            [[nodiscard]] Variant Read(foundation::scene::Scene& scene, const TrackTarget& target,
                                       foundation::scene::EntityHandle entity) const override
            {
                if (target.object == nullptr)
                {
                    Transform local = scene.GetLocalTransform(entity);
                    return ReadBinding(target.binding, Instance{&local, &TypeOf<Transform>()});
                }
                auto* manager = static_cast<foundation::scene::ComponentManagerBase*>(target.object);
                if (!manager->HasComponent(entity))
                {
                    return Variant{};
                }
                return ReadBinding(target.binding, manager->GetComponentInstance(entity));
            }

            bool Write(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                       const Variant& value) const override
            {
                if (target.object == nullptr)
                {
                    // Read, change, write back: SetLocalTransform marks the world matrix dirty, which a
                    // write into the field would not.
                    Transform local = scene.GetLocalTransform(entity);
                    if (!WriteBinding(target.binding, Instance{&local, &TypeOf<Transform>()}, value).IsOk())
                    {
                        return false;
                    }
                    scene.SetLocalTransform(entity, local);
                    return true;
                }
                auto* manager = static_cast<foundation::scene::ComponentManagerBase*>(target.object);
                if (!manager->HasComponent(entity))
                {
                    return false;
                }
                return WriteBinding(target.binding, manager->GetComponentInstance(entity), value).IsOk();
            }

            void Targets(foundation::scene::Scene& scene, foundation::scene::EntityHandle entity,
                         Array<PropertyTargetInfo>& out) const override
            {
                out.PushBack(PropertyTargetInfo{String(kTransformTarget), &TypeOf<Transform>()});
                scene.ForEachManager(
                    [&](foundation::scene::ComponentManagerBase& manager)
                    {
                        const TypeInfo* type = manager.ComponentType();
                        if (type != nullptr && type->name != nullptr && manager.HasComponent(entity))
                        {
                            out.PushBack(PropertyTargetInfo{String(reinterpret_cast<const utf8char*>(type->name)), type});
                        }
                    });
            }
        };

        // A scene system's settings block, by its settings id. Writes go to the scene's own block,
        // never a shared profile; a system on a profile is given the profile's values as its own
        // before the first write (BeginWrites), and the profile back when the player restores.
        class SettingsTargetSource final : public PropertyTargetSource
        {
        public:
            [[nodiscard]] StringView Id() const noexcept override { return kSettingsSource; }
            [[nodiscard]] bool NeedsEntity() const noexcept override { return false; }

            [[nodiscard]] bool Resolve(foundation::scene::Scene& scene, StringView target, TrackTarget& out) const override
            {
                foundation::scene::SceneSystem* system = scene.FindSystemBySettingsId(target);
                if (system == nullptr)
                {
                    return false;
                }
                out.object = system;
                out.type = system->SettingsType();
                return true;
            }

            [[nodiscard]] Variant Read(foundation::scene::Scene& scene, const TrackTarget& target,
                                       foundation::scene::EntityHandle entity) const override
            {
                (void)scene;
                (void)entity;
                auto* system = static_cast<foundation::scene::SceneSystem*>(target.object);
                return ReadBinding(target.binding, Instance{system->SettingsInstance(), target.type});
            }

            bool Write(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                       const Variant& value) const override
            {
                (void)scene;
                (void)entity;
                auto* system = static_cast<foundation::scene::SceneSystem*>(target.object);
                return WriteBinding(target.binding, Instance{system->SettingsInstance(), target.type}, value).IsOk();
            }

            [[nodiscard]] UniquePtr<PropertyTargetWrites> BeginWrites(IAllocator& allocator, foundation::scene::Scene& scene,
                                                                      const TrackTarget& target) const override
            {
                (void)scene;
                auto* system = static_cast<foundation::scene::SceneSystem*>(target.object);
                const Guid profile = system->SettingsProfile();
                if (profile.IsNil())
                {
                    return {}; // the scene's own values already: nothing to give back
                }
                system->CopySettingsProfileIntoScene();
                return MakeUnique<ProfileGivenBack>(allocator, *system, profile);
            }

            void Targets(foundation::scene::Scene& scene, foundation::scene::EntityHandle entity,
                         Array<PropertyTargetInfo>& out) const override
            {
                (void)entity;
                scene.ForEachSystem(
                    [&](foundation::scene::SceneSystem& system)
                    {
                        if (system.SettingsType() != nullptr && !system.SettingsId().IsEmpty())
                        {
                            out.PushBack(PropertyTargetInfo{String(system.SettingsId()), system.SettingsType()});
                        }
                    });
            }

        private:
            // The profile a system was on before a player's first write, put back once it restores.
            class ProfileGivenBack final : public PropertyTargetWrites
            {
            public:
                ProfileGivenBack(foundation::scene::SceneSystem& system, const Guid& profile) : m_system(&system), m_profile(profile) {}
                void Restored() override { m_system->UseSettingsProfile(m_profile); }

            private:
                foundation::scene::SceneSystem* m_system;
                Guid m_profile;
            };
        };

        // The registered sources: the two built in (never unregistered) and those domains and
        // plugins add. Registration runs as hosts and plugins load, on any thread.
        class SourceRegistry
        {
        public:
            void Register(UniquePtr<PropertyTargetSource> source)
            {
                if (!source || IsBuiltIn(source->Id()))
                {
                    return;
                }
                ScopedLock lock(m_lock);
                for (UniquePtr<PropertyTargetSource>& held : m_added)
                {
                    if (held->Id() == source->Id())
                    {
                        held = Move(source);
                        return;
                    }
                }
                m_added.PushBack(Move(source));
            }

            void Unregister(StringView id)
            {
                ScopedLock lock(m_lock);
                for (usize i = 0; i < m_added.Size(); ++i)
                {
                    if (m_added[i]->Id() == id)
                    {
                        m_added.RemoveAt(i);
                        return;
                    }
                }
            }

            [[nodiscard]] const PropertyTargetSource* Find(StringView id)
            {
                if (id == kComponentSource)
                {
                    return &m_component;
                }
                if (id == kSettingsSource)
                {
                    return &m_settings;
                }
                ScopedLock lock(m_lock);
                for (const UniquePtr<PropertyTargetSource>& held : m_added)
                {
                    if (held->Id() == id)
                    {
                        return held.Get();
                    }
                }
                return nullptr;
            }

            [[nodiscard]] Array<const PropertyTargetSource*> All()
            {
                Array<const PropertyTargetSource*> all;
                all.PushBack(&m_component);
                all.PushBack(&m_settings);
                ScopedLock lock(m_lock);
                for (const UniquePtr<PropertyTargetSource>& held : m_added)
                {
                    all.PushBack(held.Get());
                }
                return all;
            }

        private:
            [[nodiscard]] static bool IsBuiltIn(StringView id) noexcept { return id == kComponentSource || id == kSettingsSource; }

            ComponentTargetSource m_component;
            SettingsTargetSource m_settings;
            Mutex m_lock;
            Array<UniquePtr<PropertyTargetSource>> m_added;
        };

        [[nodiscard]] SourceRegistry& Sources()
        {
            static SourceRegistry registry;
            return registry;
        }
    }

    void RegisterPropertyTargetSource(UniquePtr<PropertyTargetSource> source) { Sources().Register(Move(source)); }
    void UnregisterPropertyTargetSource(StringView id) { Sources().Unregister(id); }
    const PropertyTargetSource* FindPropertyTargetSource(StringView id) { return Sources().Find(id); }
    Array<const PropertyTargetSource*> PropertyTargetSources() { return Sources().All(); }

    TrackTarget ResolveTrackTarget(foundation::scene::Scene& scene, StringView source, StringView target, StringView propertyPath)
    {
        TrackTarget resolved;
        resolved.source = FindPropertyTargetSource(source.IsEmpty() ? kComponentSource : source);
        if (resolved.source == nullptr)
        {
            resolved.status = TrackTargetStatus::UnknownSource;
            return resolved;
        }
        if (!resolved.source->Resolve(scene, target, resolved) || resolved.type == nullptr)
        {
            resolved.object = nullptr;
            resolved.type = nullptr;
            resolved.status = TrackTargetStatus::UnknownTarget;
            return resolved;
        }
        resolved.binding = ResolveBinding(*resolved.type, propertyPath);
        resolved.status = resolved.binding.IsResolved() ? TrackTargetStatus::Resolved : TrackTargetStatus::UnknownProperty;
        return resolved;
    }

    TrackTarget ResolveTrackTarget(foundation::scene::Scene& scene, const PropertyTrack& track)
    {
        TrackTarget target = ResolveTrackTarget(scene, track.source.AsView(), track.target.AsView(), track.propertyPath.AsView());
        if (target.IsResolved() && target.binding.chain[target.binding.chain.Size() - 1]->type != LeafTypeOf(track.kind))
        {
            target.status = TrackTargetStatus::KindMismatch;
        }
        return target;
    }

    Variant ReadTrackTarget(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity)
    {
        if (!target.IsResolved())
        {
            return Variant{};
        }
        return target.source->Read(scene, target, entity);
    }

    bool WriteTrackTarget(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                          const Variant& value)
    {
        if (!target.IsResolved())
        {
            return false;
        }
        return target.source->Write(scene, target, entity, value);
    }

    bool ApplyTrack(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                    const PropertyTrack& track, f32 time)
    {
        if (!target.IsResolved())
        {
            return false;
        }
        return WriteTrackTarget(scene, target, entity, track.SampleMerged(time, ReadTrackTarget(scene, target, entity)));
    }
}
