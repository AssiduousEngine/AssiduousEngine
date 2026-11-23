// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// A queue's own timeline: a timeline semaphore every submission on the queue signals with the
/// queue's next submission value (the device's SubmissionTracker hands the values out), so the
/// device knows how far the queue has completed and when a retired resource may be freed. The
/// queue's submits and its transfer batches' submits both go through it.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:queue_timeline;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::vk
{
    class VkQueueTimeline
    {
    public:
        Status init(VkDevice device, SubmissionTracker& tracker)
        {
            VkSemaphoreTypeCreateInfo type{};
            type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue = 0;
            VkSemaphoreCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            ci.pNext = &type;
            if (vkCreateSemaphore(device, &ci, nullptr, &m_semaphore) != VK_SUCCESS)
            {
                m_semaphore = VK_NULL_HANDLE;
                return ErrorCode::Unknown;
            }
            m_tracker = &tracker;
            m_slot = tracker.AddQueue();
            return ErrorCode::Ok;
        }

        void destroy(VkDevice device)
        {
            if (m_semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, m_semaphore, nullptr);
                m_semaphore = VK_NULL_HANDLE;
            }
        }

        [[nodiscard]] bool valid() const noexcept { return m_semaphore != VK_NULL_HANDLE; }
        [[nodiscard]] VkSemaphore semaphore() const noexcept { return m_semaphore; }
        [[nodiscard]] u32 slot() const noexcept { return m_slot; }
        [[nodiscard]] SubmissionTracker* tracker() const noexcept { return m_tracker; }

        /// The value the next submission signals. The caller submits it at once, holding the
        /// queue as Vulkan requires, so values reach the GPU in order.
        [[nodiscard]] u64 next() { return m_tracker->NextSubmission(m_slot); }

        /// How far the queue has completed.
        [[nodiscard]] u64 completed(VkDevice device) const
        {
            u64 value = 0;
            if (m_semaphore == VK_NULL_HANDLE || vkGetSemaphoreCounterValue(device, m_semaphore, &value) != VK_SUCCESS)
            {
                return 0;
            }
            return value;
        }

        /// A submission that failed signals its value from the host, so nothing waits on it for
        /// ever (its work is lost either way).
        void signalFromHost(VkDevice device, u64 value) const
        {
            VkSemaphoreSignalInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
            si.semaphore = m_semaphore;
            si.value = value;
            (void)vkSignalSemaphore(device, &si);
        }

    private:
        VkSemaphore m_semaphore = VK_NULL_HANDLE;
        SubmissionTracker* m_tracker = nullptr;
        u32 m_slot = 0;
    };
}
