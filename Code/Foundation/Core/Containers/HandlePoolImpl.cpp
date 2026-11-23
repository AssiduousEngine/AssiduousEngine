// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The ids of the live HandlePools of each handle type. It lives here, in Core's implementation
// unit, so the process has one table: a static in the HandlePool template would be one per shared
// library, and two pools made in different libraries could take the same id. The table is fixed
// (no allocation, usable from static initialization): a slot per handle type, a bit per id.

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
