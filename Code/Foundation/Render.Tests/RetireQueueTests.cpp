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

TEST_CASE("shadows: a new resolution remakes each slot's textures, retiring the old")
{
    // A quality level's shadows tier sizes the cascades and the atlas; a frame slot made at another
    // size remakes its textures when it next prepares, the old ones retired, not freed in flight.
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuRetireQueue queue;
    queue.Initialize(&device, /*framesInFlight*/ 2);
    ShadowSystem shadows(device, /*framesInFlight*/ 2);
    REQUIRE(shadows.Initialize().IsOk());
    shadows.SetRetireQueue(&queue);

    // The sizes before the levels: 1024 cascades, a 2048 atlas of 512 tiles.
    rhi::TextureView* cascades = shadows.PrepareFrame(0, 1);
    REQUIRE(cascades != nullptr);
    CHECK(cascades->texture->desc.width == 1024u);
    rhi::TextureView* atlas = shadows.PrepareAtlas(0);
    REQUIRE(atlas != nullptr);
    CHECK(atlas->texture->desc.width == 2048u);
    CHECK(shadows.AtlasTileCapacity() == 16u);
    const u64 atlasGeneration = shadows.AtlasGeneration();
    CHECK(queue.PendingCount() == 0u);

    // The same sizes again: nothing is remade.
    CHECK(shadows.PrepareFrame(0, 1) == cascades);
    CHECK(shadows.PrepareAtlas(0) == atlas);

    // Low: 512 cascades, a 1024 atlas of 256 tiles, still 16 of them.
    shadows.SetResolutions(512, 256);
    CHECK(shadows.Resolution() == 512u);
    CHECK(shadows.AtlasResolution() == 1024u);
    rhi::TextureView* smaller = shadows.PrepareFrame(0, 1);
    REQUIRE(smaller != nullptr);
    CHECK(smaller->texture->desc.width == 512u);
    rhi::TextureView* smallerAtlas = shadows.PrepareAtlas(0);
    REQUIRE(smallerAtlas != nullptr);
    CHECK(smallerAtlas->texture->desc.width == 1024u);
    CHECK(shadows.AtlasTileCapacity() == 16u);
    CHECK(shadows.AtlasGeneration() != atlasGeneration); // the cached static tiles redraw
    CHECK(queue.PendingCount() == 6u); // each old texture and its two views, retired

    // Only the cascades change: the atlas, at the same tile, stays.
    const u64 kept = shadows.AtlasGeneration();
    shadows.SetResolutions(768, 256);
    CHECK(shadows.PrepareFrame(0, 1)->texture->desc.width == 768u);
    CHECK(shadows.PrepareAtlas(0) == smallerAtlas);
    CHECK(shadows.AtlasGeneration() == kept);
    queue.Flush();
}
