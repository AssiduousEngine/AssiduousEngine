// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The ids of the live HandlePools of each handle type. It lives here, in Core's implementation
// unit, so the process has one table: a static in the HandlePool template would be one per shared
// library, and two pools made in different libraries could take the same id. The table is fixed
// (no allocation, usable from static initialization): a slot per handle type, a bit per id.
//
// Also the table a debugger reads a handle's label through: each owner of a pool registers a
// function that describes a handle of it, and DebugHandleLabel, callable from gdb, lldb or a
// debugger's immediate window in a live process, finds the one for a handle's noun and pool id.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

module foundation.core;

namespace foundation::core::detail
{
    namespace
    {
        constexpr u32 kHandleTypeCapacity = 128;
        constexpr u32 kIdWords = kHandlePoolIdCount / 64;

        struct HandleTypeIds
        {
            TypeId type = 0;
            u64 taken[kIdWords] = {};
        };

        struct HandleIdTable
        {
            Mutex mutex;
            HandleTypeIds types[kHandleTypeCapacity];
        };

        HandleIdTable& Table() noexcept
        {
            static HandleIdTable table;
            return table;
        }

        HandleTypeIds* Find(HandleIdTable& table, TypeId type, bool add) noexcept
        {
            for (HandleTypeIds& entry : table.types)
            {
                if (entry.type == type)
                {
                    return &entry;
                }
            }
            if (!add)
            {
                return nullptr;
            }
            for (HandleTypeIds& entry : table.types)
            {
                if (entry.type == 0)
                {
                    entry.type = type;
                    return &entry;
                }
            }
            return nullptr;
        }
    }

    u32 ClaimHandlePoolId(TypeId type) noexcept
    {
        HandleIdTable& table = Table();
        ScopedLock lock(table.mutex);
        HandleTypeIds* entry = Find(table, type, true);
        DIAGNOSTIC_ASSERT_MSG(entry != nullptr, "HandlePool: more handle types than the id table holds");
        if (entry == nullptr)
        {
            return 0;
        }
        for (u32 id = 1; id < kHandlePoolIdCount; ++id)
        {
            const u64 bit = u64{1} << (id % 64);
            if ((entry->taken[id / 64] & bit) == 0)
            {
                entry->taken[id / 64] |= bit;
                return id;
            }
        }
        return 0;
    }

    void ReleaseHandlePoolId(TypeId type, u32 id) noexcept
    {
        if (id == 0 || id >= kHandlePoolIdCount)
        {
            return;
        }
        HandleIdTable& table = Table();
        ScopedLock lock(table.mutex);
        if (HandleTypeIds* entry = Find(table, type, false))
        {
            entry->taken[id / 64] &= ~(u64{1} << (id % 64));
        }
    }
}

namespace foundation::core
{
    namespace
    {
        constexpr u32 kLabelOwnerCapacity = 512;

        struct LabelOwner
        {
            const char* noun = nullptr; // a string literal: compared by content
            u32 poolId = 0;
            HandleLabelFn describe = nullptr;
            void* context = nullptr;
        };

        struct LabelTable
        {
            Mutex mutex;
            LabelOwner owners[kLabelOwnerCapacity];
        };

        LabelTable& Labels() noexcept
        {
            static LabelTable table;
            return table;
        }

        bool SameNoun(const char* a, const char* b) noexcept
        {
            if (a == nullptr || b == nullptr)
                return false;
            while (*a != 0 && *a == *b)
            {
                ++a;
                ++b;
            }
            return *a == *b;
        }
    }

    void RegisterHandleLabels(const char* noun, u32 poolId, HandleLabelFn describe, void* context) noexcept
    {
        LabelTable& table = Labels();
        ScopedLock lock(table.mutex);
        for (LabelOwner& owner : table.owners)
        {
            if (owner.describe == nullptr)
            {
                owner = LabelOwner{noun, poolId, describe, context};
                return;
            }
        }
        // Full: that pool's handles show without a label.
    }

    void UnregisterHandleLabels(const char* noun, u32 poolId) noexcept
    {
        LabelTable& table = Labels();
        ScopedLock lock(table.mutex);
        for (LabelOwner& owner : table.owners)
        {
            if (owner.describe != nullptr && owner.poolId == poolId && SameNoun(owner.noun, noun))
            {
                owner = LabelOwner{};
                return;
            }
        }
    }
}

// For debuggers: the label of the handle of type `noun` ("Buffer", "TextureView", ...) with this
// index and generation, as its owner describes it; empty when no owner of its pool is registered.
// Not for code: the answer is in one buffer the next call overwrites.
extern "C" const char* DebugHandleLabel(const char* noun, foundation::core::u32 index,
                                            foundation::core::u32 generation)
{
    using namespace foundation::core;
    static char answer[256];
    answer[0] = 0;
    const u32 poolId = generation >> kHandlePoolIdShift;
    LabelTable& table = Labels();
    ScopedLock lock(table.mutex);
    for (const LabelOwner& owner : table.owners)
    {
        if (owner.describe != nullptr && owner.poolId == poolId && SameNoun(owner.noun, noun))
        {
            owner.describe(owner.context, index, generation, answer, sizeof(answer));
            break;
        }
    }
    return answer;
}
