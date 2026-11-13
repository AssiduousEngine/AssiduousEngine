// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Run - the `foundation.run` module.
///
/// A run is one running game: a player's game, each editor Game tab's game, an in-editor
/// dedicated server. `IRun` is how anything reaches one (its id, name, state, event bus and the
/// services it owns); `RunBase` implements it once, for a run to derive from (the engine's
/// GameInstance does). `IRunManager` lists the live runs, holds the focused one and announces each
/// run's lifecycle to its `IRunListener`s; `RunRegistry` implements it, owned by whatever owns the
/// runs (the engine's DefaultApplication), and the subsystem manager holds it for every subsystem.
///
/// A run is not a scheduler: it holds services and runs their lifecycle hooks; its owner ticks
/// what it ticks. Plays are sessions inside a run: Idle -> Running <-> Paused -> Stopping -> Idle,
/// as many times as the host plays, then Disposed.
///
/// Layering: Core and Messaging only. The runtime and the scenes depend on this module; it never
/// depends on them.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.run;

import foundation.core;
import foundation.messaging;

using namespace foundation::core;

export namespace foundation::run
{
    namespace messaging = foundation::messaging;

    /// A run's id: never reused in the process; 0 is no run.
    using RunId = u64;

    enum class RunState : u8
    {
        Idle,     ///< made, or between sessions
        Running,  ///< a session is playing
        Paused,   ///< the host paused it (the toolbar, a debugger break): not the game's own pause
        Stopping, ///< a session is ending: its services' and listeners' stop hooks are running
        Disposed, ///< gone; nothing reaches it after its destruction is announced
    };

    class IRun;

    /// What a run defers while its scripts run. A script may destroy an entity, remove a
    /// component, destroy or replace a scene, or stop its networking from anywhere it runs: an
    /// update, the fixed step, an event handler, a network message, a destroy hook. Done at once,
    /// that frees what the code that called the script is still walking. So whoever runs a run's
    /// scripts enters a Scope first, and what is asked for meanwhile waits here until Flush, which
    /// the same caller runs once the scope is left, between the frame's phases.
    ///
    /// Each piece of work names its owner (the scene, the manager it acts on); an owner that goes
    /// away first cancels its work (Cancel), so the order the work was queued in can never reach
    /// a freed owner.
    class RunMutations
    {
    public:
        RunMutations() = default;
        RunMutations(const RunMutations&) = delete;
        RunMutations& operator=(const RunMutations&) = delete;

        /// While one is alive (they nest), work is queued rather than done.
        class Scope
        {
        public:
            explicit Scope(RunMutations& queue) noexcept : m_queue(&queue) { ++m_queue->m_depth; }
            ~Scope() { --m_queue->m_depth; }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            RunMutations* m_queue;
        };

        /// Whether work asked for now must wait for Flush.
        [[nodiscard]] bool IsDeferring() const noexcept { return m_depth > 0; }
        /// Queues `work` for `owner` until the next Flush.
        void Enqueue(const void* owner, Function<void()> work) { m_items.PushBack(Item{owner, Move(work)}); }
        /// Drops what `owner` queued: it is going away. Safe during Flush.
        void Cancel(const void* owner) noexcept
        {
            for (Item& item : m_items)
                if (item.owner == owner)
                    item.owner = nullptr;
        }
        /// Runs what was queued, in order, once no scope is open; work queued by work runs in the
        /// same flush. Nothing while a scope is open.
        void Flush()
        {
            if (m_depth > 0 || m_flushing)
                return;
            m_flushing = true;
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                if (m_items[i].owner == nullptr)
                    continue;
                // Moved out: the list may grow while it runs.
                Function<void()> work = Move(m_items[i].work);
                m_items[i].owner = nullptr;
                work();
            }
            m_items.Clear();
            m_flushing = false;
        }
        [[nodiscard]] usize PendingCount() const noexcept
        {
            usize count = 0;
            for (const Item& item : m_items)
                count += item.owner != nullptr ? 1u : 0u;
            return count;
        }

    private:
        struct Item
        {
            const void* owner; // null: done or cancelled
            Function<void()> work;
        };
        Array<Item> m_items;
        u32 m_depth = 0;
        bool m_flushing = false;
    };

    /// What a run's lifecycle is told to: a subsystem keeping per-run state (through the run
    /// manager), and the run manager itself (from each run).
    class IRunListener
    {
    public:
        virtual ~IRunListener() = default;
        virtual void OnRunCreated(IRun&) {}
        virtual void OnRunStarted(IRun&) {}
        virtual void OnRunPaused(IRun&, bool /*paused*/) {}
        virtual void OnRunStopping(IRun&) {}
        virtual void OnRunDestroyed(IRun&) {}
        virtual void OnFocusChanged(IRun* /*focused*/) {}
    };

    /// A service a run owns may implement this to hear its run pause and stop (in reverse order
    /// of registration).
    class IRunService
    {
    public:
        virtual ~IRunService() = default;
        virtual void OnRunPaused(IRun&, bool /*paused*/) {}
        virtual void OnRunStopping(IRun&) {}
    };

    /// A running game, however it is implemented.
    class IRun
    {
    public:
        virtual ~IRun() = default;
        [[nodiscard]] virtual RunId Id() const noexcept = 0;
        [[nodiscard]] virtual StringView Name() const noexcept = 0;
        [[nodiscard]] virtual RunState State() const noexcept = 0;
        /// The run's event bus: its scenes' and its scripts' (`run.events()`).
        [[nodiscard]] virtual messaging::EventBus& Events() noexcept = 0;
        /// What waits while its scripts run (RunMutations).
        [[nodiscard]] virtual RunMutations& Mutations() noexcept = 0;

        /// The service of type `T` the run holds, or null.
        template <typename T>
        [[nodiscard]] T* Get() const noexcept
        {
            return static_cast<T*>(FindService(TypeOf<T>().id));
        }
        template <typename T>
        [[nodiscard]] bool Has() const noexcept
        {
            return FindService(TypeOf<T>().id) != nullptr;
        }

        /// The service the run holds under `type` (a TypeOf<T>().id), or null: Get without the
        /// type, for a lookup that has only the id (a script context's service resolver).
        [[nodiscard]] virtual void* FindService(TypeId type) const noexcept = 0;
    };

    /// The live runs, the focused one, and every run's lifecycle announced.
    class IRunManager
    {
    public:
        virtual ~IRunManager() = default;
        virtual void ForEachRun(const Function<void(IRun&)>& visit) = 0;
        [[nodiscard]] virtual IRun* Find(RunId id) noexcept = 0;
        /// The run the user is playing; null is no preference.
        [[nodiscard]] virtual IRun* Focused() noexcept = 0;
        virtual void SetFocused(IRun* run) = 0;
        /// A listener subscribing after runs exist finds them with ForEachRun.
        virtual void Subscribe(IRunListener& listener) = 0;
        virtual void Unsubscribe(IRunListener& listener) = 0;
    };

    /// IRun implemented once: the id, name, state machine, event bus and service table. A run
    /// derives from it. One that registers its own members as services calls Dispose() in its own
    /// destructor, so their stop hooks run while they still exist.
    class RunBase : public IRun
    {
    public:
        RunBase(IAllocator& allocator, StringView name);
        ~RunBase() override;
        RunBase(const RunBase&) = delete;
        RunBase& operator=(const RunBase&) = delete;

        [[nodiscard]] RunId Id() const noexcept override { return m_id; }
        [[nodiscard]] StringView Name() const noexcept override { return m_name.AsView(); }
        [[nodiscard]] RunState State() const noexcept override { return m_state; }
        [[nodiscard]] messaging::EventBus& Events() noexcept override { return m_events; }
        [[nodiscard]] RunMutations& Mutations() noexcept override { return m_mutations; }
        /// Delivers the run bus's events with its mutations deferred, then flushes them.
        void DrainEvents()
        {
            {
                RunMutations::Scope scripts(m_mutations);
                m_events.Drain();
            }
            m_mutations.Flush();
        }
        void SetName(StringView name) { m_name = String(name, *m_allocator); }

        /// Constructs a service the run owns, for the run's life. A type held already is
        /// replaced (an owned predecessor destroyed).
        template <typename T, typename... Args>
        T& Add(Args&&... args)
        {
            return AddOwned<T>(false, Forward<Args>(args)...);
        }
        /// As Add, for this session only: removed (and destroyed) at Stop.
        template <typename T, typename... Args>
        T& AddForSession(Args&&... args)
        {
            return AddOwned<T>(true, Forward<Args>(args)...);
        }
        /// Holds a service someone else owns (never destroyed by the run).
        template <typename T>
        T& Register(T& service)
        {
            Insert(Entry{TypeOf<T>().id, &service, HooksOf(&service), nullptr, false});
            return service;
        }
        template <typename T>
        void Remove()
        {
            RemoveType(TypeOf<T>().id);
        }

        /// A session starts (Idle -> Running). Nothing when one is under way.
        void Start();
        /// The host's pause (Running <-> Paused); repeating the same value does nothing.
        void SetPaused(bool paused);
        /// The session ends: services' stop hooks (reverse order), then the listener's, then the
        /// per-session services go. Back to Idle. Nothing when no session is under way.
        void Stop();
        /// Stops a session under way, destroys the owned services, announces the destruction.
        /// Idempotent; the destructor calls it.
        void Dispose();

        /// Who hears this run's transitions (its manager); null for none.
        void SetLifecycleListener(IRunListener* listener) noexcept { m_listener = listener; }

        [[nodiscard]] void* FindService(TypeId type) const noexcept override;

    private:
        struct Entry
        {
            TypeId type = 0;
            void* object = nullptr;
            IRunService* hooks = nullptr;
            void (*destroy)(IAllocator&, void*) = nullptr; // null: borrowed
            bool session = false;
        };

        template <typename T>
        static IRunService* HooksOf(T* service) noexcept
        {
            if constexpr (std::is_base_of_v<IRunService, T>)
                return static_cast<IRunService*>(service);
            else
                return nullptr;
        }

        template <typename T, typename... Args>
        T& AddOwned(bool session, Args&&... args)
        {
            T* service = m_allocator->New<T>(Forward<Args>(args)...);
            Insert(Entry{TypeOf<T>().id, service, HooksOf(service),
                         [](IAllocator& allocator, void* object) { allocator.Delete(static_cast<T*>(object)); }, session});
            return *service;
        }

        void Insert(Entry entry);
        void RemoveType(TypeId type);
        void Destroy(Entry& entry);

        IAllocator* m_allocator;
        RunId m_id;
        String m_name;
        RunState m_state = RunState::Idle;
        messaging::EventBus m_events;
        RunMutations m_mutations;
        Array<Entry> m_services; // registration order
        IRunListener* m_listener = nullptr;
    };

    /// IRunManager implemented once: the runs added to it, the focus, the subscribers. Each run
    /// added announces its transitions here, and they go to every subscriber, in subscription
    /// order; a run's destruction removes it (clearing the focus when it held it).
    class RunRegistry final : public IRunManager, private IRunListener
    {
    public:
        explicit RunRegistry(IAllocator& allocator) : m_runs(allocator), m_listeners(allocator) {}
        ~RunRegistry() override;
        RunRegistry(const RunRegistry&) = delete;
        RunRegistry& operator=(const RunRegistry&) = delete;

        /// `run` joins the live runs (announced as created); it leaves when it is disposed.
        void Add(RunBase& run);
        [[nodiscard]] usize Count() const noexcept { return m_runs.Size(); }

        void ForEachRun(const Function<void(IRun&)>& visit) override;
        [[nodiscard]] IRun* Find(RunId id) noexcept override;
        [[nodiscard]] IRun* Focused() noexcept override { return m_focused; }
        void SetFocused(IRun* run) override;
        void Subscribe(IRunListener& listener) override;
        void Unsubscribe(IRunListener& listener) override;

    private:
        void OnRunStarted(IRun& run) override;
        void OnRunPaused(IRun& run, bool paused) override;
        void OnRunStopping(IRun& run) override;
        void OnRunDestroyed(IRun& run) override;

        // The listeners as they are now: one may unsubscribe while it is being told.
        template <typename Fn>
        void Announce(Fn&& tell)
        {
            Array<IRunListener*> listeners = m_listeners;
            for (IRunListener* listener : listeners)
                tell(*listener);
        }

        Array<RunBase*> m_runs;
        Array<IRunListener*> m_listeners;
        IRun* m_focused = nullptr;
    };
}
