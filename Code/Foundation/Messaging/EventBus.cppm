// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Messaging - the `foundation.messaging` module.
//
// A native, name-keyed event bus in its own leaf module. It is a run-scope facility (a scene's,
// or a whole run's), with no scripting and no scene dependency: C++ systems Publish and Subscribe
// directly with native callbacks, so a C++-only game is a first-class participant. Script reaches
// it through a bridge (as ScriptPhysicsContactBridge does), never the reverse: the bus never
// delivers to script itself.
//
// Delivery is deferred: Publish enqueues, and Drain (called at the scope tick's top level, with no
// VM call active) delivers every queued event to its subscribers in subscription order. A handler
// may Publish again; those events cascade in the same Drain, bounded to kMaxDrainPasses to break a
// runaway loop. A handler may also subscribe and unsubscribe (itself included): the list changes
// only between events, so no running handler is moved or freed. The payload is a Variant, which also crosses between C++ and script and between
// backends.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.messaging;

import foundation.core;

using namespace foundation::core;

export namespace foundation::messaging
{
    /// EventBus::kMaxDrainPasses, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kEventBusMaxDrainPasses = 8;

    class EventBus
    {
    public:
        // Cascade bound: a handler that keeps emitting cannot spin the drain forever.
        static constexpr u32 kMaxDrainPasses = kEventBusMaxDrainPasses;

        // Publish `payload` under event name `name`. Delivered (deferred) at the next Drain.
        void Publish(StringHash name, Variant payload)
        {
            m_queue.PushBack(PendingEvent{name, Move(payload)});
        }

        // Subscribe a native callback to event `name`; fires (in subscription order) for every
        // matching event at Drain time. Returns a handle for Unsubscribe. Made from a handler
        // during Drain, it joins after the event being delivered, so it hears the ones after.
        [[nodiscard]] u32 Subscribe(StringHash name, Function<void(const Variant&)> callback)
        {
            const u32 handle = m_nextHandle++;
            Subscriber added{name, handle, Move(callback), false};
            if (m_draining)
            {
                m_added.PushBack(Move(added)); // the list may not move under a running handler
            }
            else
            {
                m_subscribers.PushBack(Move(added));
            }
            return handle;
        }

        // Remove a subscription by its handle. Safe if already gone, and from a handler during
        // Drain (its own included): the subscription stops hearing at once, and leaves the list
        // between events, when no handler runs.
        void Unsubscribe(u32 handle)
        {
            for (usize i = 0; i < m_added.Size(); ++i)
            {
                if (m_added[i].handle == handle)
                {
                    m_added.RemoveAt(i);
                    return;
                }
            }
            for (usize i = 0; i < m_subscribers.Size(); ++i)
            {
                if (m_subscribers[i].handle == handle)
                {
                    if (m_draining)
                    {
                        m_subscribers[i].dead = true;
                        m_hasDead = true;
                    }
                    else
                    {
                        m_subscribers.RemoveAt(i);
                    }
                    return;
                }
            }
        }

        // Deliver all queued events. Cascaded (a handler's Publish is delivered in this same call)
        // up to kMaxDrainPasses; within one event, subscribers fire in subscription order. A Drain
        // asked for from a handler does nothing: the one running delivers what was published.
        void Drain()
        {
            if (m_draining)
            {
                return;
            }
            m_draining = true;
            u32 pass = 0;
            while (!m_queue.IsEmpty())
            {
                if (++pass > kMaxDrainPasses)
                {
                    LOG_WARNING(
                        u8"Messaging",
                        u8"event bus drain hit the {} pass cap - dropping the rest (runaway emit?)",
                        kMaxDrainPasses);
                    m_queue.Clear();
                    break;
                }
                // Take the current batch; a handler's Publish appends to a fresh queue (next pass).
                Array<PendingEvent> batch = Move(m_queue);
                m_queue = Array<PendingEvent>{};
                for (const PendingEvent& event : batch)
                {
                    // The list does not change size while handlers run (Subscribe and Unsubscribe
                    // wait for Settle), so no handler's callback moves or dies under it.
                    for (usize i = 0; i < m_subscribers.Size(); ++i)
                    {
                        if (!m_subscribers[i].dead && m_subscribers[i].name == event.name)
                        {
                            m_subscribers[i].callback(event.payload);
                        }
                    }
                    Settle();
                }
            }
            m_draining = false;
            Settle();
        }

        // Drop all subscribers and pending events (scope teardown). From a handler during Drain,
        // every subscription stops hearing at once and leaves when no handler runs.
        void Clear()
        {
            m_queue.Clear();
            m_added.Clear();
            if (!m_draining)
            {
                m_subscribers.Clear();
                return;
            }
            for (Subscriber& s : m_subscribers)
            {
                s.dead = true;
            }
            m_hasDead = !m_subscribers.IsEmpty();
        }

        [[nodiscard]] usize SubscriberCount() const noexcept
        {
            usize live = m_added.Size();
            for (const Subscriber& s : m_subscribers)
            {
                live += s.dead ? 0u : 1u;
            }
            return live;
        }
        [[nodiscard]] usize PendingCount() const noexcept { return m_queue.Size(); }

    private:
        struct Subscriber
        {
            StringHash name;
            u32 handle;
            Function<void(const Variant&)> callback;
            bool dead = false; // unsubscribed during Drain; removed by Settle
        };

        // Between events (no handler running): the unsubscribed leave, the subscribed join.
        void Settle()
        {
            if (m_hasDead)
            {
                for (usize i = m_subscribers.Size(); i-- > 0;)
                {
                    if (m_subscribers[i].dead)
                    {
                        m_subscribers.RemoveAt(i);
                    }
                }
                m_hasDead = false;
            }
            for (Subscriber& added : m_added)
            {
                m_subscribers.PushBack(Move(added));
            }
            m_added.Clear();
        }

        struct PendingEvent
        {
            StringHash name;
            Variant payload;
        };
        Array<Subscriber> m_subscribers;
        Array<Subscriber> m_added; // subscribed during Drain, joining at the next Settle
        Array<PendingEvent> m_queue;
        u32 m_nextHandle = 1;
        bool m_draining = false;
        bool m_hasDead = false;
    };
}
