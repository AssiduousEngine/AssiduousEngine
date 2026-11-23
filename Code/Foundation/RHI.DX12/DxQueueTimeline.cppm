// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// A queue's own timeline: a fence every submission on the queue signals with the queue's next
/// submission value (the device's SubmissionTracker hands the values out), so the device knows how
/// far the queue has completed and when a retired resource may be freed. The queue's submits and
/// its transfer batches' submits both go through it. Apart from the queue's WaitIdle fence, whose
/// values count idles, not submissions.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:queue_timeline;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{
    class DxQueueTimeline
    {
    public:
        Status init(ID3D12Device* device, SubmissionTracker& tracker)
        {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
            {
                return ErrorCode::Unknown;
            }
            m_tracker = &tracker;
            m_slot = tracker.AddQueue();
            return ErrorCode::Ok;
        }

        [[nodiscard]] bool valid() const noexcept { return m_fence != nullptr; }
        [[nodiscard]] u32 slot() const noexcept { return m_slot; }
        [[nodiscard]] SubmissionTracker* tracker() const noexcept { return m_tracker; }

        /// Takes the queue's next value and signals it on `queue` after the work just executed
        /// there; 0 when the timeline is not made.
        [[nodiscard]] u64 signal(ID3D12CommandQueue* queue)
        {
            if (m_fence == nullptr)
            {
                return 0;
            }
            const u64 value = m_tracker->NextSubmission(m_slot);
            if (FAILED(queue->Signal(m_fence.Get(), value)))
            {
                (void)m_fence->Signal(value); // from the CPU: nothing waits on it for ever
            }
            return value;
        }

        /// How far the queue has completed.
        [[nodiscard]] u64 completed() const { return m_fence != nullptr ? m_fence->GetCompletedValue() : 0; }

    private:
        ComPtr<ID3D12Fence> m_fence;
        SubmissionTracker* m_tracker = nullptr;
        u32 m_slot = 0;
    };
}
