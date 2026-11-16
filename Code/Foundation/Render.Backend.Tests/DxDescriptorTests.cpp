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
        rhi::Texture* texture = nullptr;
        REQUIRE(device.CreateTexture(td, texture).IsOk());
        rhi::SamplerDesc sd{};
        rhi::Sampler* sampler = nullptr;
        REQUIRE(device.CreateSampler(sd, sampler).IsOk());
        rhi::BindGroupLayout* layout = TextureAndSampler(device);

        // 4097 views, each written into a group: one more SRV than the first CPU page holds (a full
        // heap handed out a null handle, and the view was written through it). 2049 groups on one
        // sampler: one more than the shader-visible sampler heap has slots (each group took its own).
        constexpr u32 kGroups = 4097;
        Array<rhi::TextureView*> views;
        Array<rhi::BindGroup*> groups;
        bool allMade = true;
        for (u32 i = 0; i < kGroups; ++i)
        {
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* view = nullptr;
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
        for (rhi::TextureView* view : views)
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
#endif // OPTION_HAS_DX12
