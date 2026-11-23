// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Replacing GPU resources while frames in flight may use them: the renderer destroys what it
// replaces at once, and the device keeps it until the GPU is done, so nothing waits for the GPU
// (a mid-frame wait drops the frame on the web) and no pass keeps a retire list of its own. And a
// cache keyed on view handles never hands back a group built over a destroyed view.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;

namespace
{
    // A Null device that counts the waits for the GPU.
    class WaitCountingDevice final : public rhi::null::NullDevice
    {
    public:
        WaitCountingDevice() : rhi::null::NullDevice(DefaultAllocator()) {}
        void WaitIdle() override
        {
            ++idleWaits;
            rhi::null::NullDevice::WaitIdle();
        }
        u32 idleWaits = 0;
    };

    [[nodiscard]] rhi::TextureViewHandle MakeView(rhi::Device& device, rhi::TextureHandle& texture)
    {
        REQUIRE(device.CreateTexture(rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8Unorm, 4, 4), texture)
                    .IsOk());
        rhi::TextureViewHandle view{};
        REQUIRE(device.CreateTextureView(texture, rhi::TextureViewDesc{}, view).IsOk());
        return view;
    }

    [[nodiscard]] rhi::BindGroupHandle MakeBindGroup(rhi::Device& device)
    {
        rhi::BindGroupLayoutHandle layout{};
        REQUIRE(device.CreateBindGroupLayout(rhi::BindGroupLayoutDesc{}, layout).IsOk());
        rhi::BindGroupDesc desc{};
        desc.layout = layout;
        rhi::BindGroupHandle group{};
        REQUIRE(device.CreateBindGroup(desc, group).IsOk());
        return group;
    }
}

TEST_CASE("render.replace: a uniform ring grows mid-frame without waiting for the GPU, and its old buffer goes")
{
    WaitCountingDevice device;
    DynamicUniformRing ring(device, /*framesInFlight*/ 2, 256, rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
                            u8"test.ring");
    REQUIRE(ring.Reserve(4));
    const rhi::BufferHandle first = ring.Buffer();
    REQUIRE(first.IsValid());
    REQUIRE(ring.Reserve(64)); // a frame asked for more than the ring held
    CHECK(ring.Buffer() != first);
    CHECK(device.Describe(first) == nullptr); // destroyed, the device keeping it for frames in flight
    CHECK(device.idleWaits == 0u);
}

TEST_CASE("render.replace: a bind group cache never hands back a group built over a destroyed view")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::TextureHandle texture{};
    rhi::TextureViewHandle view = MakeView(device, texture);
    BindGroupCache<1> cache;
    BindGroupInputs<1> inputs;
    inputs.Set(0, view, 1);
    cache.Store(view, inputs, MakeBindGroup(device));
    rhi::BindGroupHandle stale{};
    CHECK(cache.Find(view, inputs, stale).IsValid());

    // The view goes and a new one is made in its slot, with the same generation as before.
    const rhi::TextureViewHandle old = view;
    device.DestroyTextureView(view);
    device.DestroyTexture(texture);
    const rhi::TextureViewHandle fresh = MakeView(device, texture);
    CHECK(fresh.index == old.index);
    BindGroupInputs<1> freshInputs;
    freshInputs.Set(0, fresh, 1);
    CHECK_FALSE(cache.Find(fresh, freshInputs, stale).IsValid()); // another handle: no group for it
    CHECK_FALSE(stale.IsValid());
    cache.Release([&device](rhi::BindGroupHandle group) { device.DestroyBindGroup(group); });
}

TEST_CASE("render.replace: a cache keyed on views drops the groups of destroyed views, and only those")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::TextureHandle keptTexture{};
    rhi::TextureHandle goneTexture{};
    rhi::TextureViewHandle kept = MakeView(device, keptTexture);
    rhi::TextureViewHandle gone = MakeView(device, goneTexture);
    HashMap<rhi::TextureViewHandle, rhi::BindGroupHandle> groups;
    groups.InsertOrAssign(kept, MakeBindGroup(device));
    groups.InsertOrAssign(gone, MakeBindGroup(device));
    const rhi::BindGroupHandle goneGroup = *groups.Find(gone);
    const rhi::TextureViewHandle goneKey = gone;
    device.DestroyTextureView(gone);

    DropGroupsOfDestroyedViews(device, groups);
    CHECK(groups.Size() == 1u);
    CHECK(groups.Find(kept) != nullptr);
    CHECK(groups.Find(goneKey) == nullptr);
    CHECK_FALSE(device.LayoutOf(goneGroup).IsValid()); // its group destroyed
    DropGroupsOfDestroyedViews(device, groups);        // nothing more to drop
    CHECK(groups.Size() == 1u);
}

TEST_CASE("shadows: a new resolution remakes each slot's textures, destroying the old")
{
    // A quality level's shadows tier sizes the cascades and the atlas; a frame slot made at another
    // size remakes its textures when it next prepares, the old ones destroyed (the device keeps them
    // while frames in flight sample them).
    rhi::null::NullDevice device{DefaultAllocator()};
    ShadowSystem shadows(device, /*framesInFlight*/ 2);
    REQUIRE(shadows.Initialize().IsOk());

    // The sizes before the levels: 1024 cascades, a 2048 atlas of 512 tiles.
    rhi::TextureViewHandle cascades = shadows.PrepareFrame(0, 1);
    REQUIRE(cascades.IsValid());
    CHECK(device.Describe(device.TextureOf(cascades))->width == 1024u);
    rhi::TextureViewHandle atlas = shadows.PrepareAtlas(0);
    REQUIRE(atlas.IsValid());
    CHECK(device.Describe(device.TextureOf(atlas))->width == 2048u);
    CHECK(shadows.AtlasTileCapacity() == 16u);
    const u64 atlasGeneration = shadows.AtlasGeneration();

    // The same sizes again: nothing is remade.
    CHECK(shadows.PrepareFrame(0, 1) == cascades);
    CHECK(shadows.PrepareAtlas(0) == atlas);

    // Low: 512 cascades, a 1024 atlas of 256 tiles, still 16 of them.
    shadows.SetResolutions(512, 256);
    CHECK(shadows.Resolution() == 512u);
    CHECK(shadows.AtlasResolution() == 1024u);
    rhi::TextureViewHandle smaller = shadows.PrepareFrame(0, 1);
    REQUIRE(smaller.IsValid());
    CHECK(device.Describe(device.TextureOf(smaller))->width == 512u);
    rhi::TextureViewHandle smallerAtlas = shadows.PrepareAtlas(0);
    REQUIRE(smallerAtlas.IsValid());
    CHECK(device.Describe(device.TextureOf(smallerAtlas))->width == 1024u);
    CHECK(shadows.AtlasTileCapacity() == 16u);
    CHECK(shadows.AtlasGeneration() != atlasGeneration); // the cached static tiles redraw
    CHECK_FALSE(device.IsLive(cascades)); // each old texture and its views destroyed
    CHECK_FALSE(device.IsLive(atlas));

    // Only the cascades change: the atlas, at the same tile, stays.
    const u64 kept = shadows.AtlasGeneration();
    shadows.SetResolutions(768, 256);
    CHECK(device.Describe(device.TextureOf(shadows.PrepareFrame(0, 1)))->width == 768u);
    CHECK(shadows.PrepareAtlas(0) == smallerAtlas);
    CHECK(shadows.AtlasGeneration() == kept);
}
