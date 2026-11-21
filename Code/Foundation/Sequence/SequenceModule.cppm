// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Sequence - module `foundation.sequence`.
//
// A sequence is a timeline that drives several entities at once: tracks of keys or sections, each
// bound by name to one of the sequence's cast, and markers, some of which hold the playhead until
// the game says go on. A play is one running instance of it on a scene: its time, which entity
// fills each name, and a runner per track that applies the track at each step and releases what it
// did when the play ends. The editor's preview and the game's player both go through a play, so a
// sequence looks the same in both.
//
// A track kind is a subclass of SequenceTrack, registered with the type and serializable
// registries (RegisterSequenceTrack); whatever lists the kinds (the editor's add-track menu) asks
// the type registry for SequenceTrack's subclasses, so a domain, a plugin or a game adds its own
// without a list anywhere. This library has the kinds that need no domain: property tracks
// (any reflected property, through property animation's track targets) and event tracks.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT, RTTI_DEFINE_OBJECT

export module foundation.sequence;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.propertyanimation;

using namespace foundation::core;

export namespace foundation::sequence
{
    namespace scene = foundation::scene;
    namespace propertyanimation = foundation::propertyanimation;

    /// A named point on a sequence's timeline. A hold stops the playhead there until the play is
    /// resumed (a line of dialogue waiting for a press).
    struct SequenceMarker
    {
        f32 time = 0.0f;
        String name;
        bool hold = false;
    };

    /// One name in a sequence's cast ("Mentor", "Camera"). Whoever starts a play fills it with an
    /// entity; tracks refer to it by its index.
    struct SequenceBinding
    {
        String name;
    };

    /// What a play reports, for whoever started it to pass on (as entity events, or on the run's
    /// event bus).
    class ISequenceListener
    {
    public:
        virtual ~ISequenceListener() = default;
        /// An event key was passed. `target` is the event track's entity, unassigned when the track
        /// is bound to no one.
        virtual void OnSequenceEvent(scene::EntityHandle target, StringView name, StringView value) = 0;
        /// The playhead reached the hold marker `marker` and stopped there.
        virtual void OnSequenceHeld(StringView marker) = 0;
        /// The playhead reached the end (not on a stop).
        virtual void OnSequenceFinished() = 0;
    };

    /// One step of a play, as each track sees it. Events fire for keys in (previousTime, time]; a
    /// play's first step, and the step after a loop wraps, start before 0 so a key at 0 fires.
    struct SequenceStep
    {
        f32 time = 0.0f;
        f32 previousTime = 0.0f;
        /// The playhead moved without playing what lies between (a seek, a scrub): nothing in
        /// between fires.
        bool jumped = false;
        /// The editor posing the scene: tracks apply their values but send no events and start no
        /// sounds.
        bool preview = false;
    };

    /// One track's work in one play: made when the play begins, told each step, released when the
    /// play ends.
    class SequenceTrackRunner
    {
    public:
        virtual ~SequenceTrackRunner() = default;
        virtual void Evaluate(const SequenceStep& step) = 0;
        /// The play ended or stopped: let go of what this runner holds (a voice, an override).
        /// `restore`: also put back every value it changed, as it was before its first change.
        virtual void Release(bool restore) = 0;
        /// The play was paused or resumed (not held: at a hold the playhead waits while what
        /// plays, a line of dialogue, carries on). Most runners have nothing that runs on its own
        /// between steps; a sound's voice does.
        virtual void SetPaused(bool paused) { (void)paused; }
    };

    class SequencePlay;

    /// What a track is given to begin in a play.
    struct SequenceTrackContext
    {
        scene::Scene* scene = nullptr;
        scene::EntityHandle entity; // the track's bound entity; unassigned when it is bound to no one
        ISequenceListener* listener = nullptr; // may be null: nothing is reported
        IAllocator* allocator = nullptr;
        const SequencePlay* play = nullptr; // the play the track runs in (its cast, its parents)
    };

    /// The base of every track kind. A kind adds its keys or sections, serializes them after the
    /// base's fields, and makes a runner for each play.
    class SequenceTrack : public ISerializable
    {
        RTTI_OBJECT(SequenceTrack, ISerializable)
    public:
        String name;      // shown in the editor
        i32 binding = -1; // index into the sequence's bindings; -1: bound to no one
        bool muted = false;

        /// The time of the track's last key or the end of its last section.
        [[nodiscard]] virtual f32 Duration() const noexcept = 0;
        /// When the track runs within a step, lower first: properties, then what reads them
        /// (animation, cameras), events last.
        [[nodiscard]] virtual i32 Order() const noexcept { return 0; }
        /// Whether the track does nothing without an entity (a property track); an event track
        /// reports to the play instead.
        [[nodiscard]] virtual bool NeedsBinding() const noexcept { return true; }
        /// The track's runner for one play; null when it has nothing to do there (a warning says
        /// why when that is a fault).
        [[nodiscard]] virtual UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const = 0;
        /// Binds the assets the track refers to (a clip, a sound, a nested sequence), after a
        /// sequence is loaded.
        virtual void ResolveResources(foundation::resource::ResourceManager& manager) { (void)manager; }

        // === Editing, whatever the kind (the editor's panel and its tools) ===
        // A kind shows what it holds as key channels (a property's x, y and z; an event track's
        // events; a fade's amount) or as sections (a clip, a sound, a shot). Keys stay sorted by
        // time in each channel; a move says where each moved key went. A kind holding neither
        // keeps the defaults.

        [[nodiscard]] virtual usize KeyChannelCount() const { return 0; }
        /// The channel's name under the track ("x", "events", "amount").
        [[nodiscard]] virtual String KeyChannelName(usize channel) const
        {
            (void)channel;
            return String{};
        }
        virtual void GetKeyTimes(usize channel, Array<f32>& out) const
        {
            (void)channel;
            out.Clear();
        }
        /// Moves the keys at `keys` (indices in the channel) by `delta` seconds, never before 0,
        /// sorts the channel again and fills `moved` with each one's new index, in order.
        virtual void MoveKeys(usize channel, Span<const usize> keys, f32 delta, Array<usize>& moved)
        {
            (void)channel;
            (void)keys;
            (void)delta;
            moved.Clear();
        }
        virtual void DeleteKeys(usize channel, Span<const usize> keys)
        {
            (void)channel;
            (void)keys;
        }
        /// Adds a key at `time` holding the channel's value there (or a new key's default);
        /// returns its index, or -1 when the channel takes none.
        virtual i32 InsertKey(usize channel, f32 time)
        {
            (void)channel;
            (void)time;
            return -1;
        }
        /// The channel's keys as a curve, when they are one (a property's part, a fade's amount):
        /// the editor's curve view edits it. Null otherwise.
        [[nodiscard]] virtual Curve* KeyCurve(usize channel)
        {
            (void)channel;
            return nullptr;
        }

        [[nodiscard]] virtual usize SectionCount() const { return 0; }
        /// The section's span and what its bar says.
        virtual void GetSection(usize index, f32& start, f32& length, String& label) const
        {
            (void)index;
            start = 0.0f;
            length = 0.0f;
            label = String{};
        }
        /// Sets the section's span. A start moved later trims what it plays from (a clip's or a
        /// sound's offset moves with it), so what plays stays where it was.
        virtual void SetSection(usize index, f32 start, f32 length)
        {
            (void)index;
            (void)start;
            (void)length;
        }
        virtual void RemoveSection(usize index) { (void)index; }
        /// Adds a section at `time` (what it plays is chosen in the inspector); returns its
        /// index, or -1 when the kind takes no more.
        virtual i32 AddSection(f32 time)
        {
            (void)time;
            return -1;
        }

        void Serialize(ISerializer& ar) override;
    };
    RTTI_DEFINE_OBJECT(SequenceTrack, "rtti::sequence")

    /// Moves the keys at `indices` of `keys` (sorted by their `time`) by `delta`, never before 0,
    /// sorts them again (a moved key keeps its place among equals) and fills `moved` with each
    /// one's new index, in the order of `indices`. The track kinds' MoveKeys share it.
    template <typename Key>
    void MoveKeysInTime(Array<Key>& keys, f32 Key::* time, Span<const usize> indices, f32 delta, Array<usize>& moved)
    {
        struct Entry
        {
            Key key;
            usize from = 0;
        };
        Array<Entry> entries;
        for (usize i = 0; i < keys.Size(); ++i)
        {
            Entry entry{keys[i], i};
            for (const usize at : indices)
                if (at == i)
                    entry.key.*time = Max(0.0f, entry.key.*time + delta);
            entries.PushBack(Move(entry));
        }
        for (usize i = 1; i < entries.Size(); ++i)
        {
            Entry entry = Move(entries[i]);
            usize j = i;
            while (j > 0 && entries[j - 1].key.*time > entry.key.*time)
            {
                entries[j] = Move(entries[j - 1]);
                --j;
            }
            entries[j] = Move(entry);
        }
        moved.Clear();
        for (const usize at : indices)
            for (usize i = 0; i < entries.Size(); ++i)
                if (entries[i].from == at)
                    moved.PushBack(i);
        for (usize i = 0; i < entries.Size(); ++i)
            keys[i] = Move(entries[i].key);
    }

    /// Deletes the keys at `indices` of `keys`.
    template <typename Key>
    void DeleteKeysAt(Array<Key>& keys, Span<const usize> indices)
    {
        for (usize i = keys.Size(); i-- > 0;)
            for (const usize at : indices)
                if (at == i)
                {
                    keys.RemoveAt(i);
                    break;
                }
    }

    /// Evaluation orders of the kinds here; a domain's kind picks its own between them.
    inline constexpr i32 kPropertyTrackOrder = 0;
    inline constexpr i32 kEventTrackOrder = 1000;

    /// Any reflected property of a component on the bound entity, or its transform, as property
    /// animation's track (its keys, its target, its sampling).
    class PropertySequenceTrack final : public SequenceTrack
    {
        RTTI_OBJECT(PropertySequenceTrack, SequenceTrack)
    public:
        propertyanimation::PropertyTrack property;

        [[nodiscard]] f32 Duration() const noexcept override { return property.Duration(); }
        [[nodiscard]] i32 Order() const noexcept override { return kPropertyTrackOrder; }
        [[nodiscard]] UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const override;
        void Serialize(ISerializer& ar) override;

        [[nodiscard]] usize KeyChannelCount() const override;
        [[nodiscard]] String KeyChannelName(usize channel) const override;
        void GetKeyTimes(usize channel, Array<f32>& out) const override;
        void MoveKeys(usize channel, Span<const usize> keys, f32 delta, Array<usize>& moved) override;
        void DeleteKeys(usize channel, Span<const usize> keys) override;
        i32 InsertKey(usize channel, f32 time) override;
        [[nodiscard]] Curve* KeyCurve(usize channel) override;
    };

    /// A named event at a time, with a text value (a cue the game hears).
    struct SequenceEventKey
    {
        f32 time = 0.0f;
        String name;
        String value;
    };

    /// Events at times, reported to the play's listener with the track's entity when it is bound.
    /// Fired only when played through: never on a seek, a scrub or in the editor's preview.
    class EventSequenceTrack final : public SequenceTrack
    {
        RTTI_OBJECT(EventSequenceTrack, SequenceTrack)
    public:
        Array<SequenceEventKey> keys; // by time

        [[nodiscard]] f32 Duration() const noexcept override;
        [[nodiscard]] i32 Order() const noexcept override { return kEventTrackOrder; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const override;
        void Serialize(ISerializer& ar) override;

        [[nodiscard]] usize KeyChannelCount() const override { return 1; }
        [[nodiscard]] String KeyChannelName(usize channel) const override;
        void GetKeyTimes(usize channel, Array<f32>& out) const override;
        void MoveKeys(usize channel, Span<const usize> keys, f32 delta, Array<usize>& moved) override;
        void DeleteKeys(usize channel, Span<const usize> keys) override;
        /// A new event is named "event", with no value; the inspector names it.
        i32 InsertKey(usize channel, f32 time) override;
    };

    /// A sequence, authored or cooked. Shared, unchanged, by every play of it.
    struct Sequence
    {
        f32 duration = 0.0f;
        f32 frameRate = 30.0f; // the editor's snapping and display only; evaluation is continuous
        Array<SequenceBinding> bindings;
        Array<RefPtr<SequenceTrack>> tracks;
        Array<SequenceMarker> markers; // by time

        /// The binding named `name`, or -1.
        [[nodiscard]] i32 FindBinding(StringView name) const noexcept;
        /// The longest of its tracks and its last marker.
        [[nodiscard]] f32 ComputeDuration() const noexcept;
    };

    /// Reads or writes a sequence: its duration, frame rate, bindings, markers, and each track as
    /// its kind's type id followed by its fields. A track kind this build cannot make fails the
    /// payload (its fields cannot be skipped).
    void SerializeSequence(ISerializer& ar, Sequence& sequence);

    /// How a play ends when it reaches the end.
    enum class SequenceFinish : u8
    {
        Release, // let go of side effects (camera, sounds, fades); what moved stays where it is
        Hold,    // keep everything as the last frame left it, until Stop
        Restore, // put every value back as it was before the play, and let go
    };

    /// One running instance of a sequence on a scene. The sequence must outlive the play. Stop a
    /// play to release what it changed; destroying one releases nothing (as Abandon).
    class SequencePlay
    {
    public:
        SequencePlay(IAllocator& allocator, scene::Scene& scene, const Sequence& sequence);
        ~SequencePlay();
        SequencePlay(const SequencePlay&) = delete;
        SequencePlay& operator=(const SequencePlay&) = delete;

        /// Fills the binding `name` with `entity`. False when the sequence has no such binding.
        /// A play running takes it at once: the tracks bound to it begin again on the new entity.
        bool Bind(StringView name, scene::EntityHandle entity);
        void SetListener(ISequenceListener* listener) noexcept { m_listener = listener; }

        SequenceFinish finish = SequenceFinish::Release;
        bool loop = false;
        f32 speed = 1.0f; // 0 and up

        /// Starts from the beginning and applies time 0 at once.
        void Play();
        /// Ends the play: every runner lets go, and puts values back when the finish is Restore.
        void Stop();
        /// Ends the play without letting the runners touch the scene: for a scene being torn down,
        /// whose other systems may already be gone.
        void Abandon() noexcept;
        /// Stops the playhead, and tells the runners (a sound's voice pauses with it).
        void Pause();
        /// Continues after a pause, or from a hold.
        void Resume();
        /// Moves the playhead to `time` without playing what lies between; a hold it was at is let
        /// go. Starts the play first when it is not running.
        void Seek(f32 time);
        /// The editor's preview: applies `time` with no events and no sounds.
        void Pose(f32 time);
        /// Advances by `deltaTime` seconds (times speed): stops at a hold, wraps or ends at the end.
        void Advance(f32 deltaTime);
        /// Applies `step` as given, holds and the end left to whoever steps it: a nested sequence
        /// follows its parent's playhead this way. Begins the runners first when needed.
        void EvaluateStep(const SequenceStep& step);

        /// The play this one is nested in, or null.
        [[nodiscard]] const SequencePlay* Parent() const noexcept { return m_parent; }
        void SetParent(const SequencePlay* parent) noexcept { m_parent = parent; }
        /// Whether this play, or one it is nested in, plays `sequence`.
        [[nodiscard]] bool Plays(const Sequence& sequence) const noexcept;
        /// The entity that fills the binding `name`; unassigned when there is none.
        [[nodiscard]] scene::EntityHandle BoundEntity(StringView name) const noexcept;
        /// When the play began, against every other play: of two plays claiming what plays share
        /// (the view's camera, its fade), the one begun later wins. A nested play answers its
        /// outermost play's.
        [[nodiscard]] u64 StartOrder() const noexcept;

        [[nodiscard]] bool IsPlaying() const noexcept { return m_running && !m_finished; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }
        [[nodiscard]] bool IsHeld() const noexcept { return m_held; }
        [[nodiscard]] bool IsFinished() const noexcept { return m_finished; }
        [[nodiscard]] f32 Time() const noexcept { return m_time; }
        [[nodiscard]] f32 Duration() const noexcept { return m_sequence->duration; }
        [[nodiscard]] const Sequence& GetSequence() const noexcept { return *m_sequence; }

    private:
        struct Running
        {
            usize track = 0;
            UniquePtr<SequenceTrackRunner> runner;
        };

        void Begin();
        void BeginTrack(usize index);
        void Release(bool restore);
        void Evaluate(const SequenceStep& step);
        /// Plays from the playhead to `to`, stopping at the first hold on the way. True when it
        /// stopped at one.
        bool PlayTo(f32 to);
        void Finish();

        IAllocator* m_allocator;
        scene::Scene* m_scene;
        const Sequence* m_sequence;
        ISequenceListener* m_listener = nullptr;
        const SequencePlay* m_parent = nullptr;
        Array<scene::EntityHandle> m_bound; // per binding
        Array<Running> m_runners;           // in evaluation order
        Array<u8> m_warned;                 // per track: its missing binding was told this play
        f32 m_time = 0.0f;
        u64 m_startOrder = 0;     // see StartOrder
        bool m_begun = false;     // the runners exist
        bool m_running = false;   // between Play (or Seek, Pose) and Stop
        bool m_paused = false;
        bool m_held = false;
        bool m_finished = false;
        bool m_fromStart = false; // the next step starts before 0, so what is at 0 fires
    };

    /// Registers SequenceTrack and the kinds here with the type and serializable registries.
    /// Idempotent; call before reading a sequence.
    void RegisterSequenceTypes();

    /// Registers a track kind: its type (so the editor lists it) and its factory (so a sequence
    /// that holds it reads). A domain calls it for each kind it adds.
    template <typename T>
    void RegisterSequenceTrack()
    {
        GlobalTypeRegistry().Register(T::StaticType());
        RegisterSerializable<T>();
    }

    /// Every registered track kind (SequenceTrack's concrete subclasses).
    [[nodiscard]] Array<const TypeInfo*> SequenceTrackKinds();
}
