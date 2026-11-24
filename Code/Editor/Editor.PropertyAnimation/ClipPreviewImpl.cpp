// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_preview` partition (implementation).

module;
#include "Core/Prelude.h"

module editor.propertyanimation;

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;

using namespace foundation::core;
namespace core = foundation::core; // so core::Transform reads plainly beside the scene's types
namespace propanim = foundation::propertyanimation;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        // A component type is offered only when reflected: an unreflected type has no name, or has
        // the "<value>" fallback. The inspector uses the same rule.
        [[nodiscard]] bool IsReflectedType(const TypeInfo* type)
        {
            return type != nullptr && type->name != nullptr && type->name[0] != '<';
        }

        void CollectInto(StringView source, StringView target, const TypeInfo& type, const TypeInfo& current, const String& prefix,
                         Array<AnimatablePropertyInfo>& out)
        {
            for (const PropertyInfo& property : Properties(current))
            {
                if (property.type == nullptr || IsContainer(*property.type))
                    continue;
                String path = prefix;
                if (!path.IsEmpty())
                    path += u8".";
                path += StringView(reinterpret_cast<const utf8char*>(property.name));
                if (IsNested(property))
                {
                    CollectInto(source, target, type, *property.type, path, out);
                    continue;
                }
                const Optional<propanim::TrackValueKind> kind = InferTrackKind(property.type);
                if (!kind.HasValue() || !propanim::ResolveBinding(type, path.AsView()).IsResolved())
                    continue; // only what the runtime can drive
                out.PushBack(AnimatablePropertyInfo{String(source), String(target), Move(path), kind.Value()});
            }
        }

        [[nodiscard]] String TrackKey(const propanim::PropertyTrack& track)
        {
            return Format(u8"{}|{}|{}", track.source, track.target, track.propertyPath);
        }
    }

    Optional<propanim::TrackValueKind> InferTrackKind(const TypeInfo* leafType)
    {
        if (leafType == &TypeOf<f32>())
            return propanim::TrackValueKind::Float;
        if (leafType == &TypeOf<Float3>())
            return propanim::TrackValueKind::Float3;
        if (leafType == &TypeOf<Color>())
            return propanim::TrackValueKind::Color;
        if (leafType == &TypeOf<Quaternion>())
            return propanim::TrackValueKind::Quat;
        return {};
    }

    void CollectAnimatableProperties(StringView source, StringView target, const TypeInfo& type, Array<AnimatablePropertyInfo>& out)
    {
        CollectInto(source, target, type, type, String{}, out);
    }

    Array<AnimatablePropertyInfo> ClipPreview::TrackSeeds(const Guid& entity) const
    {
        Array<AnimatablePropertyInfo> seeds;
        const scene::EntityHandle handle = m_scene->FindEntity(entity);
        if (!handle.IsAssigned())
            return seeds;
        // Every entity has a transform (the scene's, not a reflected component), so its position,
        // rotation and scale come first.
        const String component(propanim::kComponentSource);
        seeds.PushBack(AnimatablePropertyInfo{component, String(propanim::kTransformTarget), String(u8"position"), propanim::TrackValueKind::Float3});
        seeds.PushBack(AnimatablePropertyInfo{component, String(propanim::kTransformTarget), String(u8"rotation"), propanim::TrackValueKind::Quat});
        seeds.PushBack(AnimatablePropertyInfo{component, String(propanim::kTransformTarget), String(u8"scale"), propanim::TrackValueKind::Float3});
        m_scene->ForEachManager(
            [&](scene::ComponentManagerBase& manager)
            {
                const TypeInfo* type = manager.ComponentType();
                if (manager.HasComponent(handle) && IsReflectedType(type))
                    CollectAnimatableProperties(propanim::kComponentSource, StringView(reinterpret_cast<const utf8char*>(type->name)), *type,
                                                seeds);
            });
        return seeds;
    }

    Variant ClipPreview::Read(const Guid& entity, StringView source, StringView target, StringView propertyPath) const
    {
        const scene::EntityHandle handle = m_scene->FindEntity(entity);
        return handle.IsAssigned()
                   ? propanim::ReadTrackTarget(*m_scene, propanim::ResolveTrackTarget(*m_scene, source, target, propertyPath), handle)
                   : Variant{};
    }

    void ClipPreview::Pose(const Guid& entity, const propanim::PropertyAnimationClip& clip, f32 time)
    {
        const scene::EntityHandle handle = m_scene->FindEntity(entity);
        if (!handle.IsAssigned())
        {
            Stop();
            return;
        }
        // Save the values again when the entity or the tracks changed, since the saved ones may name
        // targets that are gone.
        Array<String> tracks;
        for (const propanim::PropertyTrack& track : clip.tracks)
            tracks.PushBack(TrackKey(track));
        bool same = m_posing && m_entity == entity && tracks.Size() == m_tracks.Size();
        for (usize i = 0; same && i < tracks.Size(); ++i)
            same = tracks[i].AsView() == m_tracks[i].AsView();
        if (!same)
        {
            Stop();
            for (const propanim::PropertyTrack& track : clip.tracks)
            {
                propanim::TrackTarget target = propanim::ResolveTrackTarget(*m_scene, track);
                Variant value = propanim::ReadTrackTarget(*m_scene, target, handle);
                if (!value.IsEmpty())
                    m_kept.PushBack(Kept{Move(target), Move(value)});
            }
            m_tracks = Move(tracks);
            m_entity = entity;
            m_posing = true;
        }
        for (const propanim::PropertyTrack& track : clip.tracks)
        {
            // A channel with no keys keeps the entity's current value (an X-only track leaves Y and
            // Z alone).
            const propanim::TrackTarget target = propanim::ResolveTrackTarget(*m_scene, track);
            (void)propanim::ApplyTrack(*m_scene, target, handle, track, time);
        }
    }

    void ClipPreview::Stop()
    {
        if (!m_posing)
            return;
        const scene::EntityHandle handle = m_scene->FindEntity(m_entity);
        if (handle.IsAssigned())
            for (const Kept& kept : m_kept)
                (void)propanim::WriteTrackTarget(*m_scene, kept.target, handle, kept.value);
        m_kept.Clear();
        m_tracks.Clear();
        m_entity = Guid{};
        m_posing = false;
    }
}
