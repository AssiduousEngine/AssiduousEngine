// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

export module foundation.core:handle_pool;

import :base;
import :allocator;
import :atomic;
import :type_info;

export namespace foundation::core
{
    // =======================================================================
    // Handle<Tag> - names a value in a HandlePool by its slot and the slot's generation. Plain
    // data: copied, compared and hashed as two integers. The tag makes a buffer's handle a
    // different type from a texture's.
    //
    // The generation's top 8 bits are the id of the pool that made the handle, its low 24 bits
    // the slot's count. A pool bumps a slot's count when it frees the slot, so every copy of the
    // handle goes stale at once, and a handle from another pool of its type fails on the id.
    // =======================================================================
    inline constexpr u32 kHandlePoolIdShift = 24;
    inline constexpr u32 kHandleCountMask = (1u << kHandlePoolIdShift) - 1u;
    inline constexpr u32 kHandlePoolIdCount = 256; // ids 1 to 255; 0 is never a pool's

    template <typename Tag>
    struct Handle
    {
        static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

        u32 index = kInvalidIndex;
        u32 generation = 0;

        [[nodiscard]] static constexpr Handle Invalid() noexcept { return Handle{}; }
        // Assigned by a pool once; says nothing of whether its value is still there (the pool's
        // Get and IsLive answer that).
        [[nodiscard]] constexpr bool IsValid() const noexcept { return index != kInvalidIndex; }
        // As IsValid, so a handle tests as a pointer did (`if (!buffer)`): assigned, not live.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }
        [[nodiscard]] constexpr u32 PoolId() const noexcept { return generation >> kHandlePoolIdShift; }
        [[nodiscard]] constexpr u32 Count() const noexcept { return generation & kHandleCountMask; }

        friend constexpr bool operator==(Handle a, Handle b) noexcept = default;
    };

    namespace detail
    {
        // The ids of the live pools of each handle type, one process-wide table in Core's
        // implementation unit (a static in this template would be one per shared library).
        // Claim gives the lowest id free for `type` (0 when all 255 are taken, which a pool
        // asserts on); Release frees it.
        [[nodiscard]] u32 ClaimHandlePoolId(TypeId type) noexcept;
        void ReleaseHandlePoolId(TypeId type, u32 id) noexcept;
    }

    // =======================================================================
    // HandlePool<Tag, T> - values of T in slots, named by Handle<Tag>.
    //
    //   Allocate stores a value and names it; Free destroys it and makes every copy of its
    //   handle stale; Get answers the value, or null for a handle that is invalid, freed, or
    //   another pool's. A slot whose count would wrap is retired, never reused.
    //
    //   Threads: Allocate and Free are the owner's to serialize (one lock around them, as the
    //   owner already has). Get and IsLive may run on other threads at the same time as Allocate:
    //   slots live in fixed pages that never move, each page published once, and a slot's count
    //   is read with acquire ordering. Freeing a handle another thread is still reading is the
    //   owner's mistake, as with any value it destroys.
    // =======================================================================
    template <typename Tag, typename T>
    class HandlePool
    {
    public:
        static constexpr u32 kPageSize = 256;
        static constexpr u32 kDefaultMaxSlots = 1u << 18; // 262,144

        explicit HandlePool(IAllocator& allocator, u32 maxSlots = kDefaultMaxSlots) noexcept
            : m_allocator(&allocator),
              m_pageCount(((maxSlots > 0 ? maxSlots : 1u) + kPageSize - 1u) / kPageSize),
              m_poolId(detail::ClaimHandlePoolId(detail::SignatureTypeId<Tag>()))
        {
            DIAGNOSTIC_ASSERT_MSG(m_poolId != 0, "HandlePool: 255 pools of this handle type are alive already");
            m_pages = static_cast<Atomic<Slot*>*>(m_allocator->Allocate(sizeof(Atomic<Slot*>) * m_pageCount, alignof(Atomic<Slot*>)));
            DIAGNOSTIC_ASSERT_MSG(m_pages != nullptr, "HandlePool: page table allocation failed");
            for (u32 i = 0; i < m_pageCount; ++i)
            {
                Construct<Atomic<Slot*>>(m_pages + i, nullptr);
            }
        }

        ~HandlePool()
        {
            for (u32 page = 0; m_pages != nullptr && page < m_pageCount; ++page)
            {
                Slot* slots = m_pages[page].load(MemoryOrder::Relaxed);
                if (slots == nullptr)
                {
                    continue;
                }
                for (u32 i = 0; i < kPageSize; ++i)
                {
                    if (slots[i].live)
                    {
                        Destruct(slots[i].Value());
                    }
                    Destruct(&slots[i]);
                }
                m_allocator->Free(slots);
            }
            if (m_pages != nullptr)
            {
                for (u32 i = 0; i < m_pageCount; ++i)
                {
                    Destruct(m_pages + i);
                }
                m_allocator->Free(m_pages);
            }
            if (m_poolId != 0)
            {
                detail::ReleaseHandlePoolId(detail::SignatureTypeId<Tag>(), m_poolId);
            }
        }

        HandlePool(const HandlePool&) = delete;
        HandlePool& operator=(const HandlePool&) = delete;
        HandlePool(HandlePool&&) = delete; // readers on other threads hold its pages
        HandlePool& operator=(HandlePool&&) = delete;

        // Stores `value` and names it; Invalid when every slot is taken or retired.
        [[nodiscard]] Handle<Tag> Allocate(T value)
        {
            u32 index = Handle<Tag>::kInvalidIndex;
            Slot* slot = nullptr;
            bool fresh = false;
            if (m_freeHead != Handle<Tag>::kInvalidIndex)
            {
                index = m_freeHead;
                slot = SlotAt(index);
                m_freeHead = slot->nextFree;
            }
            else
            {
                if (m_pages == nullptr || m_slotCount >= m_pageCount * kPageSize)
                {
                    return Handle<Tag>::Invalid();
                }
                index = m_slotCount;
                if (index % kPageSize == 0 && !AddPage(index / kPageSize))
                {
                    return Handle<Tag>::Invalid();
                }
                ++m_slotCount;
                slot = SlotAt(index);
                fresh = true;
            }
            Construct<T>(slot->Value(), Move(value));
            slot->live = true;
            ++m_live;
            // A freed slot already holds the count its next handle carries (Free bumped it, and no
            // handle has it yet); a fresh one gets its first, after the value is in place.
            if (fresh)
            {
                slot->count.store(1, MemoryOrder::Release);
            }
            const u32 count = slot->count.load(MemoryOrder::Relaxed);
            return Handle<Tag>{index, (m_poolId << kHandlePoolIdShift) | count};
        }

        // Destroys the value and makes every copy of `handle` stale. False, with nothing done, for
        // a handle that is not live here.
        bool Free(Handle<Tag> handle)
        {
            Slot* slot = LiveSlot(handle);
            if (slot == nullptr)
            {
                return false;
            }
            const u32 count = slot->count.load(MemoryOrder::Relaxed);
            // Stale first, then destroyed: a reader that still sees the old count is the owner's
            // race (see the class comment), but one that sees the new count never reads the value.
            if (count == kHandleCountMask)
            {
                slot->count.store(0, MemoryOrder::Release); // retired: no handle has count 0
            }
            else
            {
                slot->count.store(count + 1u, MemoryOrder::Release);
            }
            Destruct(slot->Value());
            slot->live = false;
            --m_live;
            if (count != kHandleCountMask)
            {
                slot->nextFree = m_freeHead;
                m_freeHead = handle.index;
            }
            return true;
        }

        // The value `handle` names, or null when it is invalid, freed, or another pool's.
        [[nodiscard]] T* Get(Handle<Tag> handle) noexcept
        {
            Slot* slot = LiveSlot(handle);
            return slot != nullptr ? slot->Value() : nullptr;
        }
        [[nodiscard]] const T* Get(Handle<Tag> handle) const noexcept
        {
            const Slot* slot = LiveSlot(handle);
            return slot != nullptr ? slot->Value() : nullptr;
        }

        [[nodiscard]] bool IsLive(Handle<Tag> handle) const noexcept { return LiveSlot(handle) != nullptr; }

        // How many values are stored.
        [[nodiscard]] u32 Size() const noexcept { return m_live; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_live == 0; }
        // The id this pool stamps into its handles (1 to 255).
        [[nodiscard]] u32 PoolId() const noexcept { return m_poolId; }

        // Calls fn(handle, value) for every stored value, in slot order. The owner's thread, with
        // no Allocate or Free from inside fn.
        template <typename Fn>
        void ForEach(Fn&& fn)
        {
            for (u32 index = 0; index < m_slotCount; ++index)
            {
                Slot* slot = SlotAt(index);
                if (slot->live)
                {
                    const u32 count = slot->count.load(MemoryOrder::Relaxed);
                    fn(Handle<Tag>{index, (m_poolId << kHandlePoolIdShift) | count}, *slot->Value());
                }
            }
        }

    private:
        struct Slot
        {
            // The count a live handle of this slot carries; the next one to hand out while the
            // slot is free; 0 before its first use and once retired. `live` is the owner's
            // (Allocate, Free, ForEach, the destructor), never read by a lookup.
            Atomic<u32> count{0};
            u32 nextFree = Handle<Tag>::kInvalidIndex;
            bool live = false;
            alignas(T) byte storage[sizeof(T)];

            [[nodiscard]] T* Value() noexcept { return reinterpret_cast<T*>(storage); }
            [[nodiscard]] const T* Value() const noexcept { return reinterpret_cast<const T*>(storage); }
        };

        [[nodiscard]] bool AddPage(u32 page)
        {
            Slot* slots = static_cast<Slot*>(m_allocator->Allocate(sizeof(Slot) * kPageSize, alignof(Slot)));
            if (slots == nullptr)
            {
                return false;
            }
            for (u32 i = 0; i < kPageSize; ++i)
            {
                Construct<Slot>(slots + i);
            }
            m_pages[page].store(slots, MemoryOrder::Release);
            return true;
        }

        [[nodiscard]] Slot* SlotAt(u32 index) const noexcept
        {
            Slot* slots = m_pages[index / kPageSize].load(MemoryOrder::Acquire);
            return slots != nullptr ? slots + index % kPageSize : nullptr;
        }

        [[nodiscard]] Slot* LiveSlot(Handle<Tag> handle) const noexcept
        {
            if (m_pages == nullptr || !handle.IsValid() || handle.PoolId() != m_poolId || handle.Count() == 0 ||
                handle.index / kPageSize >= m_pageCount)
            {
                return nullptr;
            }
            Slot* slot = SlotAt(handle.index);
            // The count alone decides: a free slot holds the count its next handle will carry,
            // which no handle has yet, and a retired one holds 0, which none ever has.
            if (slot == nullptr || slot->count.load(MemoryOrder::Acquire) != handle.Count())
            {
                return nullptr;
            }
            return slot;
        }

        IAllocator* m_allocator = nullptr;
        Atomic<Slot*>* m_pages = nullptr;
        u32 m_pageCount = 0;
        u32 m_slotCount = 0; // slots ever used: the high-water mark
        u32 m_freeHead = Handle<Tag>::kInvalidIndex;
        u32 m_live = 0;
        u32 m_poolId = 0;
    };
}
