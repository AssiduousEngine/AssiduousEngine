// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Sequence - the `engine.sequence` module.
///
/// A scene's sequences as they play. SequenceSystem is the player: it holds every play running in
/// its scene and steps them each frame, in the order they started. A level script starts one with
/// `scene.sequences.play(sequence)` and gets a SequencePlay handle back; what a play reports goes
/// out on the run's event bus as a SequenceNotice (every scene in the run hears that bus, so the
/// notice names its play). A play an entity owns (a sequence component's) reports to that entity
/// as entity events instead. An event track bound to an actor posts to that actor.

module;
#include "Core/Prelude.h"

export module engine.sequence;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.script.facades;
import foundation.sequence;
import foundation.sequence.resource;
import engine.domain;

using namespace foundation::core;

export namespace engine::sequence
{
    namespace scene = foundation::scene;

    /// The names a play reports under as entity events, as the domains' are named
    /// ("contactBegin"). Behavior API: a behavior's onSequenceEvent, onSequenceHeld and
    /// onSequenceFinished hear them.
    inline constexpr StringView kSequenceEventName = u8"sequenceEvent";
    inline constexpr StringView kSequenceHeldName = u8"sequenceHeld";
    inline constexpr StringView kSequenceFinishedName = u8"sequenceFinished";
    /// The same on the run's bus, where a handler on<Name> hears the event Name: the same handlers.
    inline constexpr StringView kSequenceEventBusName = u8"SequenceEvent";
    inline constexpr StringView kSequenceHeldBusName = u8"SequenceHeld";
    inline constexpr StringView kSequenceFinishedBusName = u8"SequenceFinished";

    class SequenceSystem;

    /// One play in one scene, as a script holds it. Harmless once its play has ended: its calls do
    /// nothing and isPlaying() is false.
    struct SequencePlayHandle
    {
        scene::WeakSceneRef scene; // null once the scene is gone
        u32 play = 0;              // 0: no play

        /// The play's number, as a SequenceNotice carries it.
        [[nodiscard]] i32 id() const noexcept { return static_cast<i32>(play); }
        /// Fills the sequence's name `name` with `entity` (an actor spawned for the scene). False
        /// when the sequence has no such name or the play has ended.
        bool bind(String name, foundation::script::Entity entity) const;
        void stop() const;
        void pause() const;
        /// Continues after a pause or from a hold (a line of dialogue answered).
        void resume() const;
        void seek(f32 time) const;
        void setSpeed(f32 speed) const;
        [[nodiscard]] f32 time() const;
        [[nodiscard]] f32 duration() const;
        [[nodiscard]] bool isPlaying() const;
        [[nodiscard]] bool isHeld() const;

    private:
        [[nodiscard]] foundation::sequence::SequencePlay* Play() const;
    };

    /// What a play started from script reports on the run's event bus: which play (its id), which
    /// sequence, and the event's own name and value (an event key's; a hold's marker name).
    struct SequenceNotice
    {
        i32 play = 0;
        Guid sequence;
        String name;
        String value;
    };

    /// How a play ends, by name for scripts: "release" (the default), "hold", "restore".
    [[nodiscard]] foundation::sequence::SequenceFinish ParseSequenceFinish(StringView name) noexcept;

    /// The scene's player of sequences (the scene system, as Traktor's world component is).
    class SequenceSystem final : public scene::SceneSystem
    {
    public:
        SequenceSystem() = default;
        ~SequenceSystem() override;

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        /// After the behaviors (kScriptUpdateOrder, -110) and the sequence components (-100), so a
        /// play a script starts or resumes this frame steps this frame, and before what reads what
        /// plays write.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -99; }
        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override;
        void OnSceneStopped() override { StopAll(); }

        /// Starts a play of `sequence` (`id` names it in notices). `owner`, when assigned, is told
        /// what the play reports as entity events; otherwise it goes on the run's bus. `prepare`,
        /// when given, sets the play up (its cast, its speed) before it starts, so time 0 is applied
        /// on what it fills. The play is at time 0 when this returns. Its id, or 0 when there is no
        /// sequence.
        u32 Play(RefPtr<foundation::sequence::SequenceResource> sequence, const Guid& id, foundation::sequence::SequenceFinish finish = foundation::sequence::SequenceFinish::Release,
                 scene::EntityHandle owner = {}, const Function<void(foundation::sequence::SequencePlay&)>& prepare = {});
        /// The play numbered `id`, or null once it has ended.
        [[nodiscard]] foundation::sequence::SequencePlay* Find(u32 id) noexcept;
        /// Ends the play numbered `id` (its finish decides what it puts back).
        void Stop(u32 id);
        /// Ends every play (a skip, a level unloading).
        void StopAll();
        [[nodiscard]] usize PlayCount() const noexcept { return m_plays.Size(); }

    private:
        struct Running;
        class Reporter;

        scene::Scene* m_scene = nullptr;
        Array<UniquePtr<Running>> m_plays; // in the order they started
        u32 m_nextId = 1;
    };

    /// One name of a sequence component's cast and the entity that fills it: `entity`, or the
    /// component's own entity when `self` is set (a prefab's staged moment played by its root).
    struct SequenceCastEntry
    {
        String name;
        scene::EntityRef entity;
        bool self = false;
    };

    /// Plays a sequence from an entity: the cast picked in the inspector, what the play reports
    /// told to this entity's behaviors as entity events, and the play stopped when the entity is
    /// destroyed or disabled. The play runs in the scene's SequenceSystem like any other.
    struct SequenceComponent
    {
        foundation::resource::Ref<foundation::sequence::SequenceResource> sequence;
        Array<SequenceCastEntry> cast;
        bool autoplay = true;
        f32 speed = 1.0f;
        bool loop = false;
        foundation::sequence::SequenceFinish finish = foundation::sequence::SequenceFinish::Release;

        // runtime
        u32 playId = 0;          // its play in the scene's SequenceSystem; 0: none
        bool startPending = false; // play() asked; started at the next update
        bool stopPending = false;
        bool autoplayed = false;   // autoplay has started it this run

        /// Starts the sequence from the beginning at the next update (again, if it is playing).
        void play()
        {
            startPending = true;
            stopPending = false;
        }
        /// Stops it at the next update.
        void stop()
        {
            stopPending = true;
            startPending = false;
        }
    };

    void Serialize(ISerializer& ar, SequenceComponent& c);
    inline void ResolveResources(foundation::resource::ResourceManager& manager, SequenceComponent& c) { c.sequence.Bind(manager); }

    /// The sequence components of a scene: starts and stops their plays in the scene's
    /// SequenceSystem, just before it steps them.
    class SequenceComponentManager final : public scene::SerializableComponentManager<SequenceComponent>
    {
    public:
        SequenceComponentManager() : scene::SerializableComponentManager<SequenceComponent>(u8"sequence") {}

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        /// Just before the SequenceSystem (-99), so a play started here steps this frame; with the
        /// first gameplay systems after the behaviors (the crowd's -100).
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -100; }
        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override;
        void OnEntityDestroyed(scene::EntityHandle entity) override;
        void OnEntityActiveChanged(scene::EntityHandle entity, bool active) override;
        void OnSceneStopped() override;

    private:
        void Start(SequenceComponent& component, scene::EntityHandle owner);
        void StopPlay(SequenceComponent& component);

        scene::Scene* m_scene = nullptr;
    };

    /// The scene's sequences for script (scene.sequences).
    struct SceneSequencesFacade
    {
        scene::WeakSceneRef scene;

        /// Starts `sequence` (an asset id) and returns its play; ends as "release".
        [[nodiscard]] SequencePlayHandle play(Guid sequence) const;
        /// The same, ending as `finish`: "release", "hold" or "restore".
        [[nodiscard]] SequencePlayHandle play(Guid sequence, String finish) const;
        void stopAll() const;
        /// The play `entity`'s sequence component is running, or an ended one when it runs none.
        [[nodiscard]] SequencePlayHandle of(foundation::script::Entity entity) const;

        [[nodiscard]] static SceneSequencesFacade ForScene(const foundation::script::Scene& sceneHandle)
        {
            return SceneSequencesFacade{sceneHandle.scene};
        }
    };

    /// Scene-composition install (the domain module entry).
    void AddSequenceSceneSystems(scene::Scene& scene);
    /// Reflection of the sequence types. Idempotent.
    void RegisterSequenceReflection();
    /// Script surface registration. Idempotent.
    void RegisterSequenceScriptFacade();

    /// This domain's declaration (Engine.Composition lists it once).
    [[nodiscard]] const engine::DomainModule& SequenceDomain() noexcept;
}
