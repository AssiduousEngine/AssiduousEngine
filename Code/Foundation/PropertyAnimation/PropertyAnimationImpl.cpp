// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.propertyanimation - the binding resolver and track targets. The resolver walks the
// reflected property chain (FindProperty for each dot-separated segment, Nested for the
// intermediates) and writes the leaf through reflection set; a track target binds it on a scene.
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

    void SerializePropertyTrack(ISerializer& ar, PropertyTrack& track)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        foundation::core::Serialize(ar, "component", track.componentType);
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

    TrackTarget ResolveTrackTarget(foundation::scene::Scene& scene, StringView componentType, StringView propertyPath)
    {
        TrackTarget target;
        if (componentType == kTransformTarget)
        {
            target.transform = true;
            target.binding = ResolveBinding(TypeOf<Transform>(), propertyPath);
        }
        else
        {
            target.manager = scene.FindManagerByComponentName(componentType);
            if (target.manager == nullptr || target.manager->ComponentType() == nullptr)
            {
                target.manager = nullptr;
                target.status = TrackTargetStatus::UnknownComponent;
                return target;
            }
            target.binding = ResolveBinding(*target.manager->ComponentType(), propertyPath);
        }
        target.status = target.binding.IsResolved() ? TrackTargetStatus::Resolved : TrackTargetStatus::UnknownProperty;
        return target;
    }

    TrackTarget ResolveTrackTarget(foundation::scene::Scene& scene, const PropertyTrack& track)
    {
        TrackTarget target = ResolveTrackTarget(scene, track.componentType.AsView(), track.propertyPath.AsView());
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
        if (target.transform)
        {
            Transform local = scene.GetLocalTransform(entity);
            return ReadBinding(target.binding, Instance{&local, &TypeOf<Transform>()});
        }
        if (!target.manager->HasComponent(entity))
        {
            return Variant{};
        }
        return ReadBinding(target.binding, target.manager->GetComponentInstance(entity));
    }

    bool WriteTrackTarget(foundation::scene::Scene& scene, const TrackTarget& target, foundation::scene::EntityHandle entity,
                          const Variant& value)
    {
        if (!target.IsResolved())
        {
            return false;
        }
        if (target.transform)
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
        if (!target.manager->HasComponent(entity))
        {
            return false;
        }
        return WriteBinding(target.binding, target.manager->GetComponentInstance(entity), value).IsOk();
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
