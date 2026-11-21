// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.sequence - the play (begin, advance through holds and loops, finish, release), the
// property and event tracks' runners, serialization and registration.

module;
#include <atomic>
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module foundation.sequence;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.propertyanimation;

using namespace foundation::core;

namespace foundation::sequence
{
    REFLECT_MEMBERS(PropertySequenceTrack, "rtti::sequence")
    {
        builder.Attribute("displayName", String(u8"Property"));
    }
    REFLECT_MEMBERS(EventSequenceTrack, "rtti::sequence")
    {
        builder.Attribute("displayName", String(u8"Event"));
    }
    REFLECT_ENUM(SequenceFinish, "rtti::sequence")
    {
        builder.Value("Release", SequenceFinish::Release);
        builder.Value("Hold", SequenceFinish::Hold);
        builder.Value("Restore", SequenceFinish::Restore);
    }

    // ---- tracks ---------------------------------------------------------------------------------

    void SequenceTrack::Serialize(ISerializer& ar)
    {
        foundation::core::Serialize(ar, "name", name);
        foundation::core::Serialize(ar, "binding", binding);
        foundation::core::Serialize(ar, "muted", muted);
    }

    namespace
    {
        // Writes a property track on its entity each step. The value it found there before its
        // first write is kept, so a Restore finish (and the editor's preview) can put it back.
        class PropertyRunner final : public SequenceTrackRunner
        {
        public:
            PropertyRunner(scene::Scene& scene, scene::EntityHandle entity, const propertyanimation::PropertyTrack& track,
                           propertyanimation::TrackTarget target)
                : m_scene(&scene), m_entity(entity), m_track(&track), m_target(Move(target))
            {
            }

            void Evaluate(const SequenceStep& step) override
            {
                if (!m_scene->IsValid(m_entity))
                {
                    return;
                }
                if (!m_changed)
                {
                    m_original = propertyanimation::ReadTrackTarget(*m_scene, m_target, m_entity);
                    m_changed = true;
                }
                (void)propertyanimation::ApplyTrack(*m_scene, m_target, m_entity, *m_track, step.time);
            }

            void Release(bool restore) override
            {
                if (restore && m_changed && !m_original.IsEmpty() && m_scene->IsValid(m_entity))
                {
                    (void)propertyanimation::WriteTrackTarget(*m_scene, m_target, m_entity, m_original);
                }
                m_changed = false;
            }

        private:
            scene::Scene* m_scene;
            scene::EntityHandle m_entity;
            const propertyanimation::PropertyTrack* m_track;
            propertyanimation::TrackTarget m_target;
            Variant m_original;
            bool m_changed = false;
        };

        // Reports each key the playhead passes while playing.
        class EventRunner final : public SequenceTrackRunner
        {
        public:
            EventRunner(const EventSequenceTrack& track, scene::EntityHandle entity, ISequenceListener* listener)
                : m_track(&track), m_entity(entity), m_listener(listener)
            {
            }

            void Evaluate(const SequenceStep& step) override
            {
                if (step.jumped || step.preview || m_listener == nullptr)
                {
                    return;
                }
                for (const SequenceEventKey& key : m_track->keys)
                {
                    if (key.time > step.previousTime && key.time <= step.time)
                    {
                        m_listener->OnSequenceEvent(m_entity, key.name.AsView(), key.value.AsView());
                    }
                }
            }

            void Release(bool restore) override { (void)restore; }

        private:
            const EventSequenceTrack* m_track;
            scene::EntityHandle m_entity;
            ISequenceListener* m_listener;
        };
    }

    UniquePtr<SequenceTrackRunner> PropertySequenceTrack::Begin(const SequenceTrackContext& context) const
    {
        propertyanimation::TrackTarget target = propertyanimation::ResolveTrackTarget(*context.scene, property);
        switch (target.status)
        {
        case propertyanimation::TrackTargetStatus::Resolved:
            return MakeUnique<PropertyRunner>(*context.allocator, *context.scene, context.entity, property, Move(target));
        case propertyanimation::TrackTargetStatus::UnknownComponent:
            LOG_WARNING(u8"Sequence", u8"track '{}' targets unknown component type '{}' - skipped", name, property.componentType);
            break;
        case propertyanimation::TrackTargetStatus::UnknownProperty:
            LOG_WARNING(u8"Sequence", u8"track '{}': property '{}' not found on '{}' - skipped", name, property.propertyPath,
                        property.componentType);
            break;
        case propertyanimation::TrackTargetStatus::KindMismatch:
            LOG_WARNING(u8"Sequence", u8"track '{}': '{}.{}' is not of the track's kind - skipped", name, property.componentType,
                        property.propertyPath);
            break;
        }
        return {};
    }

    void PropertySequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        propertyanimation::SerializePropertyTrack(ar, property);
    }

    // === Editing a property track: a channel per curve (x, y, z; r, g, b, a), or the rotation's keys ===

    namespace
    {
        [[nodiscard]] bool IsRotation(const propertyanimation::PropertyTrack& track) noexcept
        {
            return track.kind == propertyanimation::TrackValueKind::Quat;
        }

        // The index of the key at `time` in sorted keys (the first at that time).
        template <typename Key>
        [[nodiscard]] i32 IndexAt(const Array<Key>& keys, f32 Key::* time, f32 at)
        {
            for (usize i = 0; i < keys.Size(); ++i)
                if (Abs(keys[i].*time - at) < 1e-5f)
                    return static_cast<i32>(i);
            return -1;
        }
    }

    usize PropertySequenceTrack::KeyChannelCount() const
    {
        return IsRotation(property) ? 1u : propertyanimation::ChannelCount(property.kind);
    }

    String PropertySequenceTrack::KeyChannelName(usize channel) const
    {
        static constexpr StringView kXyz[] = {u8"x", u8"y", u8"z"};
        static constexpr StringView kRgba[] = {u8"r", u8"g", u8"b", u8"a"};
        switch (property.kind)
        {
        case propertyanimation::TrackValueKind::Float3:
            return String(channel < 3 ? kXyz[channel] : StringView{});
        case propertyanimation::TrackValueKind::Color:
            return String(channel < 4 ? kRgba[channel] : StringView{});
        case propertyanimation::TrackValueKind::Quat:
            return String(u8"rotation");
        default:
            return String(u8"value");
        }
    }

    void PropertySequenceTrack::GetKeyTimes(usize channel, Array<f32>& out) const
    {
        out.Clear();
        if (IsRotation(property))
        {
            for (const propertyanimation::QuatKey& key : property.quatKeys)
                out.PushBack(key.time);
            return;
        }
        if (channel < KeyChannelCount())
            for (const CurveKey& key : property.channels[channel].Keys())
                out.PushBack(key.time);
    }

    void PropertySequenceTrack::MoveKeys(usize channel, Span<const usize> keys, f32 delta, Array<usize>& moved)
    {
        if (IsRotation(property))
        {
            MoveKeysInTime(property.quatKeys, &propertyanimation::QuatKey::time, keys, delta, moved);
            return;
        }
        if (channel >= KeyChannelCount())
        {
            moved.Clear();
            return;
        }
        MoveKeysInTime(property.channels[channel].Keys(), &CurveKey::time, keys, delta, moved);
        property.channels[channel].ApplyTangentModes();
    }

    void PropertySequenceTrack::DeleteKeys(usize channel, Span<const usize> keys)
    {
        if (IsRotation(property))
        {
            DeleteKeysAt(property.quatKeys, keys);
            return;
        }
        if (channel < KeyChannelCount())
        {
            DeleteKeysAt(property.channels[channel].Keys(), keys);
            property.channels[channel].ApplyTangentModes();
        }
    }

    i32 PropertySequenceTrack::InsertKey(usize channel, f32 time)
    {
        time = Max(0.0f, time);
        if (IsRotation(property))
        {
            const Quaternion value = property.quatKeys.IsEmpty() ? Quaternion::Identity : property.Sample(time).Get<Quaternion>();
            usize at = 0;
            while (at < property.quatKeys.Size() && property.quatKeys[at].time < time)
                ++at;
            property.quatKeys.Insert(at, propertyanimation::QuatKey{time, value});
            return static_cast<i32>(at);
        }
        if (channel >= KeyChannelCount())
            return -1;
        Curve& curve = property.channels[channel];
        if (const i32 existing = IndexAt(curve.Keys(), &CurveKey::time, time); existing >= 0)
            return existing; // a key is already there
        const f32 value = curve.IsEmpty() ? 0.0f : curve.Evaluate(time);
        curve.AddKey(CurveKey{time, value, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Auto});
        curve.ApplyTangentModes();
        return IndexAt(curve.Keys(), &CurveKey::time, time);
    }

    Curve* PropertySequenceTrack::KeyCurve(usize channel)
    {
        return !IsRotation(property) && channel < KeyChannelCount() ? &property.channels[channel] : nullptr;
    }

    // === Editing an event track: one channel of events ===

    String EventSequenceTrack::KeyChannelName(usize channel) const
    {
        (void)channel;
        return String(u8"events");
    }

    void EventSequenceTrack::GetKeyTimes(usize channel, Array<f32>& out) const
    {
        (void)channel;
        out.Clear();
        for (const SequenceEventKey& key : keys)
            out.PushBack(key.time);
    }

    void EventSequenceTrack::MoveKeys(usize channel, Span<const usize> indices, f32 delta, Array<usize>& moved)
    {
        (void)channel;
        MoveKeysInTime(keys, &SequenceEventKey::time, indices, delta, moved);
    }

    void EventSequenceTrack::DeleteKeys(usize channel, Span<const usize> indices)
    {
        (void)channel;
        DeleteKeysAt(keys, indices);
    }

    i32 EventSequenceTrack::InsertKey(usize channel, f32 time)
    {
        (void)channel;
        time = Max(0.0f, time);
        usize at = 0;
        while (at < keys.Size() && keys[at].time <= time)
            ++at;
        keys.Insert(at, SequenceEventKey{time, String(u8"event"), String{}});
        return static_cast<i32>(at);
    }

    f32 EventSequenceTrack::Duration() const noexcept
    {
        f32 last = 0.0f;
        for (const SequenceEventKey& key : keys)
        {
            last = Max(last, key.time);
        }
        return last;
    }

    UniquePtr<SequenceTrackRunner> EventSequenceTrack::Begin(const SequenceTrackContext& context) const
    {
        return MakeUnique<EventRunner>(*context.allocator, *this, context.entity, context.listener);
    }

    void EventSequenceTrack::Serialize(ISerializer& ar)
    {
        SequenceTrack::Serialize(ar);
        const bool reading = ar.Mode() == SerializeMode::Read;
        Array<f32> times;
        Array<String> names;
        Array<String> values;
        if (!reading)
        {
            for (const SequenceEventKey& key : keys)
            {
                times.PushBack(key.time);
                names.PushBack(key.name);
                values.PushBack(key.value);
            }
        }
        foundation::core::Serialize(ar, "keyTime", times);
        foundation::core::Serialize(ar, "keyName", names);
        foundation::core::Serialize(ar, "keyValue", values);
        if (reading)
        {
            keys.Clear();
            const usize count = Min(times.Size(), Min(names.Size(), values.Size()));
            for (usize i = 0; i < count; ++i)
            {
                keys.PushBack(SequenceEventKey{times[i], Move(names[i]), Move(values[i])});
            }
        }
    }

    // ---- the sequence ---------------------------------------------------------------------------

    i32 Sequence::FindBinding(StringView name) const noexcept
    {
        for (usize i = 0; i < bindings.Size(); ++i)
        {
            if (bindings[i].name.AsView() == name)
            {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }

    f32 Sequence::ComputeDuration() const noexcept
    {
        f32 d = 0.0f;
        for (const RefPtr<SequenceTrack>& track : tracks)
        {
            if (track)
            {
                d = Max(d, track->Duration());
            }
        }
        for (const SequenceMarker& marker : markers)
        {
            d = Max(d, marker.time);
        }
        return d;
    }

    void SerializeSequence(ISerializer& ar, Sequence& sequence)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        foundation::core::Serialize(ar, "duration", sequence.duration);
        foundation::core::Serialize(ar, "frameRate", sequence.frameRate);

        Array<String> bindingNames;
        Array<f32> markerTime;
        Array<String> markerName;
        Array<u8> markerHold;
        if (!reading)
        {
            for (const SequenceBinding& binding : sequence.bindings)
            {
                bindingNames.PushBack(binding.name);
            }
            for (const SequenceMarker& marker : sequence.markers)
            {
                markerTime.PushBack(marker.time);
                markerName.PushBack(marker.name);
                markerHold.PushBack(marker.hold ? 1u : 0u);
            }
        }
        foundation::core::Serialize(ar, "bindings", bindingNames);
        foundation::core::Serialize(ar, "markerTime", markerTime);
        foundation::core::Serialize(ar, "markerName", markerName);
        foundation::core::Serialize(ar, "markerHold", markerHold);
        if (reading)
        {
            sequence.bindings.Clear();
            for (String& bindingName : bindingNames)
            {
                sequence.bindings.PushBack(SequenceBinding{Move(bindingName)});
            }
            sequence.markers.Clear();
            const usize count = Min(markerTime.Size(), Min(markerName.Size(), markerHold.Size()));
            for (usize i = 0; i < count; ++i)
            {
                sequence.markers.PushBack(SequenceMarker{markerTime[i], Move(markerName[i]), markerHold[i] != 0});
            }
        }

        u32 count = reading ? 0u : static_cast<u32>(sequence.tracks.Size());
        ar.Key("tracks");
        ar.BeginArray(count);
        if (reading)
        {
            sequence.tracks.Clear();
        }
        for (u32 i = 0; i < count; ++i)
        {
            ar.BeginObject();
            RefPtr<SequenceTrack> track = reading ? RefPtr<SequenceTrack>{} : sequence.tracks[i];
            u64 typeId = reading ? 0ull : track->GetType()->id;
            foundation::core::Serialize(ar, "type", typeId);
            if (reading)
            {
                RefPtr<ISerializable> made = GlobalSerializableRegistry().Create(typeId);
                track = made ? RefPtr<SequenceTrack>{Cast<SequenceTrack>(made.Get())} : RefPtr<SequenceTrack>{};
                if (!track)
                {
                    // A kind this build cannot make: its fields cannot be skipped, so the whole
                    // payload is refused rather than read out of step.
                    ar.FailPayload(ErrorCode::NotSupported);
                    return;
                }
            }
            track->Serialize(ar);
            ar.EndObject();
            if (reading)
            {
                // A binding index outside the cast is bound to no one.
                if (track->binding >= static_cast<i32>(sequence.bindings.Size()))
                {
                    track->binding = -1;
                }
                sequence.tracks.PushBack(Move(track));
            }
        }
        ar.EndArray();
    }

    // ---- the play -------------------------------------------------------------------------------

    SequencePlay::SequencePlay(IAllocator& allocator, scene::Scene& scene, const Sequence& sequence)
        : m_allocator(&allocator), m_scene(&scene), m_sequence(&sequence)
    {
        m_bound.Resize(sequence.bindings.Size());
        m_warned.Resize(sequence.tracks.Size());
    }

    // Destroying a play lets go of nothing in the scene (it may be the scene going away): whoever
    // owns a play stops it first when what it changed should be released.
    SequencePlay::~SequencePlay() = default;

    bool SequencePlay::Bind(StringView name, scene::EntityHandle entity)
    {
        const i32 index = m_sequence->FindBinding(name);
        if (index < 0)
        {
            return false;
        }
        m_bound[static_cast<usize>(index)] = entity;
        if (!m_begun)
        {
            return true;
        }
        // The tracks bound to it begin again on the new entity; what they did to the old one stays.
        for (usize t = 0; t < m_sequence->tracks.Size(); ++t)
        {
            const SequenceTrack* track = m_sequence->tracks[t].Get();
            if (track == nullptr || track->binding != index)
            {
                continue;
            }
            for (usize r = 0; r < m_runners.Size(); ++r)
            {
                if (m_runners[r].track == t)
                {
                    m_runners[r].runner->Release(false);
                    m_runners.RemoveAt(r);
                    break;
                }
            }
            m_warned[t] = 0;
            BeginTrack(t);
        }
        return true;
    }

    namespace
    {
        std::atomic<u64> s_nextStartOrder{1}; // plays begin on any scene's thread
    }

    u64 SequencePlay::StartOrder() const noexcept
    {
        const SequencePlay* outer = this;
        while (outer->m_parent != nullptr)
        {
            outer = outer->m_parent;
        }
        return outer->m_startOrder;
    }

    void SequencePlay::Begin()
    {
        m_startOrder = s_nextStartOrder.fetch_add(1, std::memory_order_relaxed);
        m_runners.Clear();
        for (u8& warned : m_warned)
        {
            warned = 0;
        }
        for (usize t = 0; t < m_sequence->tracks.Size(); ++t)
        {
            BeginTrack(t);
        }
        m_begun = true;
    }

    void SequencePlay::BeginTrack(usize index)
    {
        const SequenceTrack* track = m_sequence->tracks[index].Get();
        if (track == nullptr || track->muted)
        {
            return;
        }
        scene::EntityHandle entity;
        if (track->binding >= 0 && static_cast<usize>(track->binding) < m_bound.Size())
        {
            entity = m_bound[static_cast<usize>(track->binding)];
        }
        if (track->NeedsBinding() && !m_scene->IsValid(entity))
        {
            if (m_warned[index] == 0)
            {
                m_warned[index] = 1;
                const StringView bindingName = track->binding >= 0 && static_cast<usize>(track->binding) < m_bound.Size()
                                                   ? m_sequence->bindings[static_cast<usize>(track->binding)].name.AsView()
                                                   : StringView(u8"(none)");
                LOG_WARNING(u8"Sequence", u8"track '{}' has no entity for '{}' - skipped", track->name, bindingName);
            }
            return;
        }
        SequenceTrackContext context;
        context.scene = m_scene;
        context.entity = entity;
        context.listener = m_listener;
        context.allocator = m_allocator;
        context.play = this;
        UniquePtr<SequenceTrackRunner> runner = track->Begin(context);
        if (!runner)
        {
            return;
        }
        // Kept in evaluation order: by the kind's order, then by the track's place.
        usize at = 0;
        while (at < m_runners.Size())
        {
            const SequenceTrack* other = m_sequence->tracks[m_runners[at].track].Get();
            if (other->Order() > track->Order() || (other->Order() == track->Order() && m_runners[at].track > index))
            {
                break;
            }
            ++at;
        }
        m_runners.Insert(at, Running{index, Move(runner)});
    }

    void SequencePlay::Release(bool restore)
    {
        // Last first, so a value two tracks changed is restored to what was there before both.
        for (usize r = m_runners.Size(); r > 0; --r)
        {
            m_runners[r - 1].runner->Release(restore);
        }
        m_runners.Clear();
        m_begun = false;
    }

    void SequencePlay::Evaluate(const SequenceStep& step)
    {
        for (Running& running : m_runners)
        {
            running.runner->Evaluate(step);
        }
    }

    void SequencePlay::Play()
    {
        if (m_begun)
        {
            Release(false);
        }
        Begin();
        m_running = true;
        m_paused = false;
        m_held = false;
        m_finished = false;
        m_time = 0.0f;
        m_fromStart = true;
        if (!PlayTo(0.0f) && m_sequence->duration <= 0.0f)
        {
            Finish();
        }
    }

    void SequencePlay::Stop()
    {
        if (m_begun)
        {
            Release(finish == SequenceFinish::Restore);
        }
        m_running = false;
        m_paused = false;
        m_held = false;
        m_finished = false;
    }

    void SequencePlay::Abandon() noexcept
    {
        m_runners.Clear();
        m_begun = false;
        m_running = false;
        m_paused = false;
        m_held = false;
        m_finished = false;
    }

    void SequencePlay::Pause()
    {
        if (m_paused)
        {
            return;
        }
        m_paused = true;
        for (Running& running : m_runners)
        {
            running.runner->SetPaused(true);
        }
    }

    void SequencePlay::Resume()
    {
        m_held = false;
        if (!m_paused)
        {
            return;
        }
        m_paused = false;
        for (Running& running : m_runners)
        {
            running.runner->SetPaused(false);
        }
    }

    void SequencePlay::Seek(f32 time)
    {
        if (!m_begun)
        {
            Begin();
        }
        m_running = true;
        m_held = false;
        m_finished = false;
        m_fromStart = false;
        m_time = Clamp(time, 0.0f, m_sequence->duration);
        SequenceStep step;
        step.time = m_time;
        step.previousTime = m_time;
        step.jumped = true;
        Evaluate(step);
    }

    void SequencePlay::Pose(f32 time)
    {
        if (!m_begun)
        {
            Begin();
        }
        m_running = true;
        m_held = false;
        m_finished = false;
        m_fromStart = false;
        m_time = Clamp(time, 0.0f, m_sequence->duration);
        SequenceStep step;
        step.time = m_time;
        step.previousTime = m_time;
        step.jumped = true;
        step.preview = true;
        Evaluate(step);
    }

    bool SequencePlay::PlayTo(f32 to)
    {
        const f32 from = m_time;
        const f32 previous = m_fromStart ? -1.0f : from;
        // The first hold on the way, if any, ends the step there.
        const SequenceMarker* hold = nullptr;
        for (const SequenceMarker& marker : m_sequence->markers)
        {
            if (marker.hold && marker.time > previous && marker.time <= to && (hold == nullptr || marker.time < hold->time))
            {
                hold = &marker;
            }
        }
        if (hold != nullptr)
        {
            to = hold->time;
        }
        SequenceStep step;
        step.time = to;
        step.previousTime = previous;
        m_time = to;
        m_fromStart = false;
        Evaluate(step);
        if (hold == nullptr)
        {
            return false;
        }
        m_held = true;
        if (m_listener != nullptr)
        {
            m_listener->OnSequenceHeld(hold->name.AsView());
        }
        return true;
    }

    void SequencePlay::Advance(f32 deltaTime)
    {
        if (!m_running || m_finished || m_paused || m_held)
        {
            return;
        }
        const f32 duration = m_sequence->duration;
        f32 target = m_time + Max(0.0f, deltaTime * speed);
        // A step longer than the sequence wraps at most a few times; what it would skip beyond
        // that is not played.
        for (u32 wraps = 0;; ++wraps)
        {
            if (PlayTo(Min(target, duration)))
            {
                return; // held
            }
            if (target < duration)
            {
                return;
            }
            if (!loop || duration <= 0.0f)
            {
                Finish();
                return;
            }
            target -= duration;
            if (wraps >= 3)
            {
                while (target >= duration)
                {
                    target -= duration;
                }
            }
            m_time = 0.0f;
            m_fromStart = true;
        }
    }

    void SequencePlay::EvaluateStep(const SequenceStep& step)
    {
        if (!m_begun)
        {
            Begin();
        }
        m_running = true;
        m_time = step.time;
        Evaluate(step);
    }

    bool SequencePlay::Plays(const Sequence& sequence) const noexcept
    {
        for (const SequencePlay* play = this; play != nullptr; play = play->m_parent)
        {
            if (play->m_sequence == &sequence)
            {
                return true;
            }
        }
        return false;
    }

    scene::EntityHandle SequencePlay::BoundEntity(StringView name) const noexcept
    {
        const i32 index = m_sequence->FindBinding(name);
        return index >= 0 ? m_bound[static_cast<usize>(index)] : scene::EntityHandle{};
    }

    void SequencePlay::Finish()
    {
        m_finished = true;
        if (m_listener != nullptr)
        {
            m_listener->OnSequenceFinished();
        }
        switch (finish)
        {
        case SequenceFinish::Release:
            Release(false);
            break;
        case SequenceFinish::Restore:
            Release(true);
            break;
        case SequenceFinish::Hold:
            break;
        }
    }

    // ---- registration ---------------------------------------------------------------------------

    void RegisterSequenceTypes()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(SequenceTrack::StaticType());
            RttiRegisterEnum_SequenceFinish();
            RegisterSequenceTrack<PropertySequenceTrack>();
            RegisterSequenceTrack<EventSequenceTrack>();
            return true;
        }();
        (void)once;
    }

    Array<const TypeInfo*> SequenceTrackKinds()
    {
        Array<const TypeInfo*> kinds;
        for (const TypeInfo* type : GlobalTypeRegistry().All())
        {
            if (type != &SequenceTrack::StaticType() && IsDerivedFrom(type, &SequenceTrack::StaticType()) &&
                GlobalSerializableRegistry().Contains(type->id))
            {
                kinds.PushBack(type);
            }
        }
        return kinds;
    }
}
