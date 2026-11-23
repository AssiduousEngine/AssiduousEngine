// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::RHI - the `:lifetime` partition: when a destroyed resource may really be freed, and
/// the table of a backend's objects its handles name.
///
/// A resource destroyed through its handle is stale at once, but the GPU may still use it: work
/// already submitted, and work recorded before the destroy and submitted after it (a frame's
/// commands recorded, an upload that destroys a buffer submitted, then the frame's commands). So a
/// retired object waits until every recording begun before its destroy has ended (submitted or
/// discarded), and then until each queue it was submitted on has completed that submission.
/// Backends report the events: a recording's begin, its end (the queue and submission value it
/// went out at, or none), and each queue's completed value when they collect.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

export module foundation.rhi:lifetime;

import foundation.core;

using namespace foundation::core;

export namespace foundation::rhi
{
    class SubmissionTracker
    {
    public:
        /// A recording's number, from Begin; 0 is none.
        using Recording = u64;
        static constexpr u32 kMaxQueues = 8;
        using FreeFn = void (*)(void* context, void* object);

        explicit SubmissionTracker(IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_open(allocator), m_retired(allocator)
        {
        }
        ~SubmissionTracker() { FreeAll(); }
        SubmissionTracker(const SubmissionTracker&) = delete;
        SubmissionTracker& operator=(const SubmissionTracker&) = delete;

        /// A queue's slot, by which it reports its submissions and completions.
        [[nodiscard]] u32 AddQueue() noexcept
        {
            ScopedLock lock(m_mutex);
            DIAGNOSTIC_ASSERT_MSG(m_queueCount < kMaxQueues, "SubmissionTracker: too many queues");
            return m_queueCount < kMaxQueues ? m_queueCount++ : kMaxQueues - 1;
        }

        /// A recording began (a command encoder, a render bundle, a transfer batch's staging).
        [[nodiscard]] Recording Begin()
        {
            ScopedLock lock(m_mutex);
            const Recording recording = m_next++;
            m_open.PushBack(recording);
            return recording;
        }

        /// The value `queue`'s next submission signals. The queue takes it as it submits and ends
        /// the submission's recordings with it.
        [[nodiscard]] u64 NextSubmission(u32 queue) noexcept
        {
            ScopedLock lock(m_mutex);
            return ++m_submitted[queue];
        }

        /// A recording ended: submitted on `queue` at `value`, or discarded (`value` 0). Ending
        /// one twice, or 0, does nothing.
        void End(Recording recording, u32 queue = 0, u64 value = 0)
        {
            if (recording == 0)
            {
                return;
            }
            ScopedLock lock(m_mutex);
            bool found = false;
            for (usize i = 0; i < m_open.Size(); ++i)
            {
                if (m_open[i] == recording)
                {
                    m_open[i] = m_open[m_open.Size() - 1];
                    m_open.PopBack();
                    found = true;
                    break;
                }
            }
            if (!found || value == 0)
            {
                return;
            }
            // Every object retired after this recording began waits for its submission too.
            for (Entry& entry : m_retired)
            {
                if (recording < entry.before && entry.wait[queue] < value)
                {
                    entry.wait[queue] = value;
                }
            }
        }

        /// Hands `object` over to be freed by `free(context, object)` once the GPU can no longer
        /// use it.
        void Retire(void* object, FreeFn free, void* context)
        {
            ScopedLock lock(m_mutex);
            Entry entry{object, free, context, m_next, {}};
            for (u32 q = 0; q < kMaxQueues; ++q)
            {
                entry.wait[q] = m_submitted[q]; // everything submitted so far may use it
            }
            m_retired.PushBack(entry);
        }

        /// Frees what is safe now; `completed[q]` is queue q's completed submission value.
        void Collect(Span<const u64> completed)
        {
            Array<Entry> ready(*m_allocator);
            {
                ScopedLock lock(m_mutex);
                Recording oldestOpen = ~Recording{0};
                for (Recording open : m_open)
                {
                    oldestOpen = open < oldestOpen ? open : oldestOpen;
                }
                for (usize i = m_retired.Size(); i-- > 0;)
                {
                    const Entry& entry = m_retired[i];
                    bool done = oldestOpen >= entry.before; // nothing begun before it still open
                    for (u32 q = 0; done && q < kMaxQueues; ++q)
                    {
                        const u64 have = q < completed.Size() ? completed[q] : ~u64{0};
                        done = entry.wait[q] <= have;
                    }
                    if (done)
                    {
                        ready.PushBack(entry);
                        m_retired[i] = m_retired[m_retired.Size() - 1];
                        m_retired.PopBack();
                    }
                }
            }
            // Freed outside the lock: a free may take the backend's own locks.
            for (const Entry& entry : ready)
            {
                entry.free(entry.context, entry.object);
            }
        }

        /// How much is retired and not freed yet.
        [[nodiscard]] usize RetiredCount()
        {
            ScopedLock lock(m_mutex);
            return m_retired.Size();
        }

        /// The last value `queue` handed out.
        [[nodiscard]] u64 LastSubmission(u32 queue)
        {
            ScopedLock lock(m_mutex);
            return m_submitted[queue];
        }

        /// Frees everything retired: the device is idle and going (teardown), or its owner knows
        /// nothing recorded is left.
        void FreeAll()
        {
            Array<Entry> all(*m_allocator);
            {
                ScopedLock lock(m_mutex);
                all = Move(m_retired);
                m_retired = Array<Entry>(*m_allocator);
            }
            for (const Entry& entry : all)
            {
                entry.free(entry.context, entry.object);
            }
        }

    private:
        struct Entry
        {
            void* object = nullptr;
            FreeFn free = nullptr;
            void* context = nullptr;
            Recording before = 0;     // recordings below this began before the retire
            u64 wait[kMaxQueues] = {}; // per queue, the submission to complete first
        };

        IAllocator* m_allocator;
        Mutex m_mutex;
        Recording m_next = 1;
        u32 m_queueCount = 0;
        u64 m_submitted[kMaxQueues] = {};
        Array<Recording> m_open;
        Array<Entry> m_retired;
    };

    /// The objects a backend's handles of one type name. Add stores an object and names it; Get
    /// answers it, or null for a stale handle; Retire makes the handle stale at once and gives the
    /// object to the tracker, which frees it with `free` when the GPU is done with it.
    template <typename Tag, typename Impl>
    class ResourceTable
    {
    public:
        ResourceTable(IAllocator& allocator, SubmissionTracker& tracker, SubmissionTracker::FreeFn free,
                      void* context) noexcept
            : m_allocator(&allocator), m_pool(allocator), m_tracker(&tracker), m_free(free), m_context(context)
        {
        }

        ~ResourceTable() { FreeAll(); }

        /// Frees every object still in the table and makes its handles stale: the device is idle
        /// and going. A backend whose objects need the native device to free them calls it before
        /// the native device goes.
        void FreeAll()
        {
            Array<Handle<Tag>> live(*m_allocator);
            {
                ScopedLock lock(m_mutex);
                m_pool.ForEach([&live](Handle<Tag> handle, Impl*&) { live.PushBack(handle); });
            }
            for (Handle<Tag> handle : live)
            {
                Impl* object = nullptr;
                {
                    ScopedLock lock(m_mutex);
                    if (Impl* const* held = m_pool.Get(handle))
                    {
                        object = *held;
                        (void)m_pool.Free(handle);
                    }
                }
                if (object != nullptr)
                {
                    m_free(m_context, object);
                }
            }
        }

        ResourceTable(const ResourceTable&) = delete;
        ResourceTable& operator=(const ResourceTable&) = delete;

        [[nodiscard]] Handle<Tag> Add(Impl* object)
        {
            ScopedLock lock(m_mutex);
            return m_pool.Allocate(object);
        }

        [[nodiscard]] Impl* Get(Handle<Tag> handle) const noexcept
        {
            Impl* const* object = m_pool.Get(handle);
            return object != nullptr ? *object : nullptr;
        }

        /// Stale at once; freed when the GPU is done. False for a handle that is not live, and the
        /// caller's copy is made invalid either way.
        bool Retire(Handle<Tag>& handle)
        {
            Impl* object = nullptr;
            {
                ScopedLock lock(m_mutex);
                if (Impl* const* held = m_pool.Get(handle))
                {
                    object = *held;
                    (void)m_pool.Free(handle);
                }
            }
            handle = Handle<Tag>::Invalid();
            if (object == nullptr)
            {
                return false;
            }
            m_tracker->Retire(object, m_free, m_context);
            return true;
        }

        [[nodiscard]] u32 Size() const noexcept { return m_pool.Size(); }

    private:
        IAllocator* m_allocator;
        mutable Mutex m_mutex; // Add and Retire; Get takes none (the pool's lookups are lock-free)
        HandlePool<Tag, Impl*> m_pool;
        SubmissionTracker* m_tracker;
        SubmissionTracker::FreeFn m_free;
        void* m_context;
    };
}
