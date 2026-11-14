// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Deferred implementations that break circular dependencies between
/// ValidatedBackend, ValidatedAdapter, and ValidatedDevice.

module;
#include "Core/Prelude.h"

export module foundation.rhi.validation:wiring;

import foundation.core;
import foundation.rhi;
import :validated_backend;
import :validated_adapter;
import :validated_device;
import :validated_queue;
import :validated_swap_chain;
import :validated_fence;

using namespace foundation::core;

namespace foundation::rhi::validation
{

    ValidatedAdapter* ValidatedBackend::CreateValidatedAdapter(Adapter* inner,
                                                               IAllocator& allocator)
    {
        return allocator.New<ValidatedAdapter>(inner, allocator);
    }

    Status ValidatedAdapter::CreateDevice(const DeviceDesc& desc, Device*& out)
    {
        Device* innerDevice = nullptr;
        Status r = m_inner->CreateDevice(desc, innerDevice);
        if (r != ErrorCode::Ok || !innerDevice)
        {
            out = nullptr;
            return r;
        }
        out = m_allocator.New<ValidatedDevice>(innerDevice, m_allocator);
        return ErrorCode::Ok;
    }


    void ValidatedQueue::SubmitFrame(Span<CommandBuffer* const> cmdBufs, SwapChain& swapChain,
                                     Span<Fence* const> waitFences, Span<const u64> waitValues,
                                     Fence* signalFence, u64 signalValue)
    {
        auto& chain = static_cast<ValidatedSwapChain&>(swapChain);
        if (!chain.NoteFrameSubmitted())
            LogError("[Validation] Queue::submitFrame: the swapchain has no image acquired");
        if (waitFences.Size() != waitValues.Size())
            LogError("[Validation] Queue::submitFrame: waitFences and waitValues count mismatch");
        if (!signalFence)
            LogError("[Validation] Queue::submitFrame: signalFence is null");
        Array<Fence*> innerWait(waitFences.Size());
        for (usize i = 0; i < waitFences.Size(); ++i)
        {
            auto* vw = static_cast<ValidatedFence*>(waitFences[i]);
            innerWait[i] = vw ? vw->inner() : waitFences[i];
        }
        auto* vf = static_cast<ValidatedFence*>(signalFence);
        if (vf)
            vf->trackSignal(signalValue);
        m_inner->SubmitFrame(cmdBufs, *chain.inner(), Span<Fence* const>(innerWait.Data(), innerWait.Size()),
                             waitValues, vf ? vf->inner() : signalFence, signalValue);
    }
} // namespace foundation::rhi::validation
