// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.animation:propertyanimator
//
// PropertyAnimatorComponent and its per-scene manager. One animator per entity plays one clip
// whose tracks drive reflected properties on the owning entity's components. The manager ticks in
// PostUpdate (before render extraction, the skeletal phase): it advances the play clock
// (Once/Loop/PingPong), then writes each track through its target (propertyanimation's
// TrackTarget, bound once per clip), which finds the live component again on every write. A track
// that fails to resolve is disabled with one warning; the clip keeps playing its other tracks.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.animation:propertyanimator;

import foundation.core;
import foundation.profiler;
import foundation.resource;
import foundation.scene;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;

using namespace foundation::core;

export namespace engine::animation
{
    namespace scene = foundation::scene;
    namespace propanim = foundation::propertyanimation;

    enum class PropertyLoopMode : u8
    {
        Once,     // play through once, then stop
        Loop,     // wrap to the start
        PingPong, // bounce between ends
    };

    // Physics: the animator writes the transform like any other property. On an entity with a
    // dynamic rigid body, physics is authoritative: its per-frame pose sync overwrites the animated
    // transform (they run in different scene phases and the body integrates independently).
    // Animate the transform only of kinematic or non-physics entities; a dynamic body's
    // non-transform properties animate fine. The test for this lives with the physics manager.

    // Plays one property-animation clip on the owning entity. Serialized fields are the clip ref and
    // the playback tunables; the play clock, bindings and cached clip are runtime-only.
    struct PropertyAnimatorComponent
    {
        foundation::resource::Ref<propanim::PropertyAnimationClipResource> clip;
        bool autoplay = true;
        f32 speed = 1.0f;
        PropertyLoopMode loopMode = PropertyLoopMode::Loop;

        // runtime state
        bool playing = false;
        f32 time = 0.0f;
        i8 pingPongDir = 1;
        propanim::PropertyAnimationClipResource* boundClip = nullptr; // bindings built for this clip
        Array<propanim::TrackTarget> bindings; // one per track; an unresolved one is skipped

        // Reflected playback ops for scripting. They reach scripts through the reflected-component
        // `.of(entity)` surface, with natural numeric types, so no facade library is needed. They
        // mutate only the runtime clock and state; the manager tick applies it.
        void play()
        {
            playing = true;
            time = 0.0f;
            pingPongDir = 1;
        }
        void stop()
        {
            playing = false;
            time = 0.0f;
        }
        void pause() { playing = false; }
        void resume() { playing = true; }
        [[nodiscard]] bool isPlaying() const { return playing; }
        [[nodiscard]] f32 currentTime() const { return time; }
        void setTime(f32 seconds) { time = seconds; }
    };

    inline void Serialize(ISerializer& ar, PropertyAnimatorComponent& c)
    {
        foundation::core::Serialize(ar, "clip", c.clip);
        foundation::core::Serialize(ar, "autoplay", c.autoplay);
        foundation::core::Serialize(ar, "speed", c.speed);
        foundation::core::Serialize(ar, "loopMode", c.loopMode);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 PropertyAnimatorComponent& c)
    {
        c.clip.Bind(manager);
    }

    class PropertyAnimatorComponentManager final
        : public scene::SerializableComponentManager<PropertyAnimatorComponent>
    {
    public:
        PropertyAnimatorComponentManager()
            : scene::SerializableComponentManager<PropertyAnimatorComponent>(u8"property_animator")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

        // Simulation-gated: autoplay clips do not animate in the editor's non-simulating edit mode.
        // This does not affect the in-scene animation panel, whose preview and transport write
        // property values directly (ReadTrackTarget/WriteTrackTarget), never through this manager's
        // tick. Scenes default to simulating, so players, play in editor and tests are unaffected;
        // only a scene that disables simulation (the editor's edit mode) freezes.
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("PropertyAnimation.Update");
            ForEach(
                [&](PropertyAnimatorComponent& a, scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return; // frozen: no advance, no writes
                    }
                    propanim::PropertyAnimationClipResource* clipRes = a.clip.Get();
                    if (clipRes == nullptr)
                    {
                        return;
                    }
                    // Rebuild the bindings when the clip object changes (first tick, editor pick,
                    // hot reload). autoplay starts the new clip from t=0.
                    if (clipRes != a.boundClip)
                    {
                        a.boundClip = clipRes;
                        BuildBindings(a, clipRes->clip);
                        if (a.autoplay)
                        {
                            a.playing = true;
                            a.time = 0.0f;
                            a.pingPongDir = 1;
                        }
                    }
                    if (!a.playing)
                    {
                        return;
                    }

                    const propanim::PropertyAnimationClip& clip = clipRes->clip;
                    const f32 evalTime = Advance(a, clip.duration, deltaTime);

                    const usize trackCount =
                        Min(a.bindings.Size(), clip.tracks.Size());
                    for (usize i = 0; i < trackCount; ++i)
                    {
                        (void)propanim::ApplyTrack(*m_scene, a.bindings[i], owner, clip.tracks[i], evalTime);
                    }
                });
        }

    private:
        // Binds each track's target once. A track that does not resolve is skipped with one warning
        // saying why; the rest of the clip plays on.
        void BuildBindings(PropertyAnimatorComponent& a, const propanim::PropertyAnimationClip& clip)
        {
            a.bindings.Clear();
            for (const propanim::PropertyTrack& track : clip.tracks)
            {
                propanim::TrackTarget target = propanim::ResolveTrackTarget(*m_scene, track);
                switch (target.status)
                {
                case propanim::TrackTargetStatus::Resolved:
                    break;
                case propanim::TrackTargetStatus::UnknownComponent:
                    LOG_WARNING(u8"PropertyAnimation", u8"track targets unknown component type '{}' - disabled",
                                track.componentType);
                    break;
                case propanim::TrackTargetStatus::UnknownProperty:
                    LOG_WARNING(u8"PropertyAnimation", u8"track property '{}' not found on '{}' - disabled",
                                track.propertyPath, track.componentType);
                    break;
                case propanim::TrackTargetStatus::KindMismatch:
                    LOG_WARNING(u8"PropertyAnimation", u8"track '{}.{}' kind does not match the property type - disabled",
                                track.componentType, track.propertyPath);
                    break;
                }
                a.bindings.PushBack(Move(target));
            }
        }

        // Advance the play clock by `dt` under the loop mode; returns the evaluation time and updates
        // playing/pingPongDir. `speed` may be negative.
        [[nodiscard]] static f32 Advance(PropertyAnimatorComponent& a, f32 duration, f32 dt)
        {
            if (duration <= 0.0f)
            {
                return 0.0f;
            }
            const f32 dir = (a.loopMode == PropertyLoopMode::PingPong)
                                ? static_cast<f32>(a.pingPongDir)
                                : 1.0f;
            a.time += dt * a.speed * dir;

            switch (a.loopMode)
            {
            case PropertyLoopMode::Once:
                if (a.time >= duration)
                {
                    a.time = duration;
                    a.playing = false;
                }
                else if (a.time < 0.0f)
                {
                    a.time = 0.0f;
                    a.playing = false;
                }
                break;
            case PropertyLoopMode::Loop:
                a.time -= duration * Floor(a.time / duration); // wraps negatives too
                break;
            case PropertyLoopMode::PingPong:
                // Fold into [0, duration], flipping direction at each end (bounded loop for big dt).
                for (int guard = 0; (a.time > duration || a.time < 0.0f) && guard < 64; ++guard)
                {
                    if (a.time > duration)
                    {
                        a.time = 2.0f * duration - a.time;
                        a.pingPongDir = -1;
                    }
                    else if (a.time < 0.0f)
                    {
                        a.time = -a.time;
                        a.pingPongDir = 1;
                    }
                }
                break;
            }
            return a.time;
        }

        scene::Scene* m_scene = nullptr;
    };
}
