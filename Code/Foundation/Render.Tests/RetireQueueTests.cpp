// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GpuRetireQueue: frames-in-flight deferred GPU destruction, which is safe on the web where a
// WaitIdle on the grow path is not. On the Null RHI: verifies aging (freed only after
// framesInFlight+1 ticks), typed retire, and Flush.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;

TEST_CASE("render.retire: entries age out after framesInFlight + 1 ticks")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuRetireQueue queue;
    queue.Initialize(&device, /*framesInFlight*/ 2);

    rhi::BufferDesc bd{};
    bd.size = 256;
    bd.usage = rhi::BufferUsage::Storage;
    rhi::Buffer* buffer = nullptr;
    REQUIRE(device.CreateBuffer(bd, buffer).IsOk());
    rhi::TextureDesc td = rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8Unorm, 4, 4);
    rhi::Texture* texture = nullptr;
    REQUIRE(device.CreateTexture(td, texture).IsOk());
    rhi::TextureView* view = nullptr;
    REQUIRE(device.CreateTextureView(texture, rhi::TextureViewDesc{}, view).IsOk());

    queue.Retire(view);
    queue.Retire(texture);
    queue.Retire(buffer);
    CHECK(queue.PendingCount() == 3u);

    // framesInFlight=2 gives age 3: two ticks keep everything, the third frees.
    queue.Tick();
    CHECK(queue.PendingCount() == 3u);
    queue.Tick();
    CHECK(queue.PendingCount() == 3u);
    queue.Tick();
    CHECK(queue.PendingCount() == 0u);

    // Null retires are no-ops; Flush drains immediately.
    queue.Retire(static_cast<rhi::Buffer*>(nullptr));
    CHECK(queue.PendingCount() == 0u);
    rhi::Buffer* another = nullptr;
    REQUIRE(device.CreateBuffer(bd, another).IsOk());
    queue.Retire(another);
    CHECK(queue.PendingCount() == 1u);
    queue.Flush();
    CHECK(queue.PendingCount() == 0u);
}

namespace
{
    // A Null device that counts what disposal does to it: bind groups destroyed and waits for idle.
    class CountingDevice final : public rhi::null::NullDevice
    {
    public:
        CountingDevice() : rhi::null::NullDevice(DefaultAllocator()) {}
        void DestroyBindGroup(rhi::BindGroup*& group) override
        {
            ++destroyed;
            rhi::null::NullDevice::DestroyBindGroup(group);
        }
        void WaitIdle() override
        {
            ++idleWaits;
            rhi::null::NullDevice::WaitIdle();
        }
        u32 destroyed = 0;
        u32 idleWaits = 0;
    };
}

TEST_CASE("render.retire: a replaced bind group outlives the frames in flight that may use it")
{
    // A pass that replaces a cached bind group (its inputs changed) must not free it at once: the
    // previous frame's command buffer may still be executing with it (VUID-vkFreeDescriptorSets-00309,
    // seen from the MSAA resolve on the first frames of a scene).
    CountingDevice device;
    GpuRetireQueue queue;
    queue.Initialize(&device, /*framesInFlight*/ 2);
    rhi::BindGroup* group = nullptr;
    REQUIRE(device.CreateBindGroup(rhi::BindGroupDesc{}, group).IsOk());

    RetireBindGroup(device, &queue, group);
    CHECK(device.destroyed == 0u);
    CHECK(device.idleWaits == 0u);
    CHECK(queue.PendingCount() == 1u);
    queue.Tick();
    queue.Tick();
    CHECK(device.destroyed == 0u);
    queue.Tick(); // framesInFlight + 1 frames on
    CHECK(device.destroyed == 1u);
    CHECK(queue.PendingCount() == 0u);
}

TEST_CASE("render.retire: with no queue, a replaced bind group is destroyed once the GPU is idle")
{
    CountingDevice device;
    rhi::BindGroup* group = nullptr;
    REQUIRE(device.CreateBindGroup(rhi::BindGroupDesc{}, group).IsOk());
    RetireBindGroup(device, nullptr, group);
    CHECK(device.idleWaits == 1u);
    CHECK(device.destroyed == 1u);

    RetireBindGroup(device, nullptr, nullptr); // nothing to dispose of
    CHECK(device.idleWaits == 1u);
    CHECK(device.destroyed == 1u);
}
