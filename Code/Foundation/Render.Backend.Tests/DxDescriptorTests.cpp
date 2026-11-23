// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DX12's descriptor limits, on a real device (Windows: the MSVC lanes). The CPU-side heaps page, so
// more views than one page holds all get their descriptors; and bind groups of the same samplers
// share one shader-visible sampler table, so more groups than that heap's 2048 slots still bind.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#ifdef OPTION_HAS_DX12
import foundation.core;
import foundation.rhi;
import foundation.rhi.dx12;
import foundation.rhi.testsupport;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;

namespace
{
    // A layout of one sampled texture and one sampler, the shape of a material's table.
    rhi::BindGroupLayout* TextureAndSampler(rhi::Device& device)
    {
        rhi::BindGroupLayoutEntry entries[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 2};
        rhi::BindGroupLayout* layout = nullptr;
        REQUIRE(device.CreateBindGroupLayout(ld, layout).IsOk());
        return layout;
    }

    void RunDescriptorLimits(rhi::Device& device)
    {
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = 4;
        td.height = 4;
        td.usage = rhi::TextureUsage::Sampled;
        td.label = u8"dx.limits.texture";
        rhi::TextureHandle texture{};
        REQUIRE(device.CreateTexture(td, texture).IsOk());
        rhi::SamplerDesc sd{};
        rhi::SamplerHandle sampler{};
        REQUIRE(device.CreateSampler(sd, sampler).IsOk());
        rhi::BindGroupLayout* layout = TextureAndSampler(device);

        // 4097 views, each written into a group: one more SRV than the first CPU page holds (a full
        // heap handed out a null handle, and the view was written through it). 2049 groups on one
        // sampler: one more than the shader-visible sampler heap has slots (each group took its own).
        constexpr u32 kGroups = 4097;
        Array<rhi::TextureViewHandle> views;
        Array<rhi::BindGroup*> groups;
        bool allMade = true;
        for (u32 i = 0; i < kGroups; ++i)
        {
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureViewHandle view{};
            if (!device.CreateTextureView(texture, vd, view).IsOk())
            {
                allMade = false;
                break;
            }
            views.PushBack(view);
            rhi::BindGroupEntry entries[] = {rhi::BindGroupEntry::TextureEntry(view),
                                             rhi::BindGroupEntry::SamplerEntry(sampler)};
            rhi::BindGroupDesc bgd{};
            bgd.layout = layout;
            bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
            rhi::BindGroup* group = nullptr;
            if (!device.CreateBindGroup(bgd, group).IsOk())
            {
                allMade = false;
                break;
            }
            groups.PushBack(group);
        }
        CHECK(allMade);
        CHECK(groups.Size() == kGroups);

        for (rhi::BindGroup* group : groups)
            device.DestroyBindGroup(group);
        for (rhi::TextureViewHandle view : views)
            device.DestroyTextureView(view);
        device.DestroyBindGroupLayout(layout);
        device.DestroySampler(sampler);
        device.DestroyTexture(texture);
    }
}

TEST_CASE("dx12: more views than a descriptor page, and more groups than sampler slots, all bind")
{
    rhi::Backend* dx12 = nullptr;
    (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
    if (rhi::Device* device = dx12 != nullptr ? testsupport::MakeTestDevice(dx12) : nullptr)
    {
        RunDescriptorLimits(*device);
        device->Destroy();
    }
    else
    {
        MESSAGE("DX12 unavailable - skipped");
    }
    if (dx12 != nullptr)
        dx12->Destroy();
}

// A bindless update of a sampler in a group whose sampler table is shared: the update stopped at
// the group's CPU copy and never reached the shader-visible table (or, written there, would have
// changed every group sharing it). The group takes a table of its own first; its neighbour keeps
// the shared one.
TEST_CASE("dx12: a sampler updated in a group with a shared table moves it to its own")
{
    rhi::Backend* dx12 = nullptr;
    (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
    rhi::Device* device = dx12 != nullptr ? testsupport::MakeTestDevice(dx12) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("DX12 unavailable - skipped");
        if (dx12 != nullptr)
            dx12->Destroy();
        return;
    }
    rhi::TextureDesc td{};
    td.format = rhi::TextureFormat::RGBA8Unorm;
    td.width = 4;
    td.height = 4;
    td.usage = rhi::TextureUsage::Sampled;
    rhi::TextureHandle texture{};
    REQUIRE(device->CreateTexture(td, texture).IsOk());
    rhi::TextureViewDesc vd{};
    vd.format = rhi::TextureFormat::RGBA8Unorm;
    rhi::TextureViewHandle view{};
    REQUIRE(device->CreateTextureView(texture, vd, view).IsOk());
    rhi::SamplerDesc linear{};
    rhi::SamplerHandle sampler{};
    REQUIRE(device->CreateSampler(linear, sampler).IsOk());
    rhi::SamplerDesc point{};
    point.minFilter = rhi::FilterMode::Nearest;
    point.magFilter = rhi::FilterMode::Nearest;
    rhi::SamplerHandle other{};
    REQUIRE(device->CreateSampler(point, other).IsOk());
    rhi::BindGroupLayout* layout = TextureAndSampler(*device);
    rhi::BindGroupEntry entries[] = {rhi::BindGroupEntry::TextureEntry(view), rhi::BindGroupEntry::SamplerEntry(sampler)};
    rhi::BindGroupDesc bgd{};
    bgd.layout = layout;
    bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
    rhi::BindGroup* a = nullptr;
    rhi::BindGroup* b = nullptr;
    REQUIRE(device->CreateBindGroup(bgd, a).IsOk());
    REQUIRE(device->CreateBindGroup(bgd, b).IsOk());
    auto* da = static_cast<rhi::dx12::DxBindGroupImpl*>(a);
    auto* db = static_cast<rhi::dx12::DxBindGroupImpl*>(b);
    CHECK(da->gpuSamplerOffset() == db->gpuSamplerOffset()); // shared

    rhi::BindlessUpdateEntry update{};
    update.layoutIndex = 1; // the sampler
    update.sampler = other;
    a->UpdateBindless(Span<const rhi::BindlessUpdateEntry>{&update, 1});
    CHECK(da->gpuSamplerOffset() >= 0);
    CHECK(da->gpuSamplerOffset() != db->gpuSamplerOffset()); // its own now; b keeps the shared one

    device->DestroyBindGroup(a);
    device->DestroyBindGroup(b);
    device->DestroyBindGroupLayout(layout);
    device->DestroySampler(other);
    device->DestroySampler(sampler);
    device->DestroyTextureView(view);
    device->DestroyTexture(texture);
    device->Destroy();
    dx12->Destroy();
}
#endif // OPTION_HAS_DX12
