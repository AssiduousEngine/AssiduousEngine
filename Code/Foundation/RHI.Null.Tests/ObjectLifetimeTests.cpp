// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// RHI.Null.Tests: shaders, layouts, bind groups, pipelines and query sets by handle. A destroyed
// one's handle is stale at once; the object goes through the same table and tracker as a buffer
// (BufferLifetimeTests covers the wait for recordings and submissions).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;

using namespace foundation::core;
using namespace foundation::rhi;

namespace
{
    struct NullDeviceFixture
    {
        Backend* backend = nullptr;
        Device* device = nullptr;
        NullDeviceFixture()
        {
            REQUIRE(null::CreateNullBackend(backend).IsOk());
            REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
        }
        ~NullDeviceFixture()
        {
            device->Destroy();
            backend->Destroy();
        }
        [[nodiscard]] BindGroupLayoutHandle MakeLayout()
        {
            const BindGroupLayoutEntry entries[] = {
                BindGroupLayoutEntry::UniformBuffer(0, ShaderStage::Vertex),
                BindGroupLayoutEntry::Sampler(0, ShaderStage::Fragment),
            };
            BindGroupLayoutDesc desc{};
            desc.entries = Span<const BindGroupLayoutEntry>(entries, 2);
            BindGroupLayoutHandle layout{};
            REQUIRE(device->CreateBindGroupLayout(desc, layout).IsOk());
            return layout;
        }
        [[nodiscard]] BindGroupHandle MakeGroup(BindGroupLayoutHandle layout)
        {
            BindGroupDesc desc{};
            desc.layout = layout;
            BindGroupHandle group{};
            REQUIRE(device->CreateBindGroup(desc, group).IsOk());
            return group;
        }
    };
}

TEST_CASE("object lifetime: a bind group names its layout, and a layout its entries, until they are destroyed")
{
    NullDeviceFixture f;
    BindGroupLayoutHandle layout = f.MakeLayout();
    BindGroupHandle group = f.MakeGroup(layout);
    CHECK(f.device->LayoutOf(group) == layout);
    REQUIRE(f.device->LayoutEntries(layout).Size() == 2u);
    CHECK(f.device->LayoutEntries(layout)[1].type == BindingType::Sampler);

    const BindGroupHandle keptGroup = group;
    const BindGroupLayoutHandle keptLayout = layout;
    f.device->DestroyBindGroup(group);
    f.device->DestroyBindGroupLayout(layout);
    CHECK_FALSE(group.IsValid()); // the caller's own copies are cleared
    CHECK_FALSE(layout.IsValid());
    CHECK_FALSE(f.device->LayoutOf(keptGroup).IsValid());
    CHECK(f.device->LayoutEntries(keptLayout).IsEmpty());

    // A group made against a destroyed layout is refused.
    BindGroupDesc desc{};
    desc.layout = keptLayout;
    BindGroupHandle refused{};
    CHECK_FALSE(f.device->CreateBindGroup(desc, refused).IsOk());
    CHECK_FALSE(refused.IsValid());

    // A new group in the freed slot is another handle.
    const BindGroupHandle next = f.MakeGroup(f.MakeLayout());
    CHECK(next.index == keptGroup.index);
    CHECK(next != keptGroup);
    CHECK_FALSE(f.device->LayoutOf(keptGroup).IsValid());
}

TEST_CASE("object lifetime: shaders, pipelines, caches and query sets are stale at once when destroyed")
{
    NullDeviceFixture f;
    const u8 code[4] = {1, 2, 3, 4};
    ShaderModuleDesc md{};
    md.code = Span<const u8>(code, 4);
    md.entryPoint = u8"VSMain";
    ShaderModuleHandle module{};
    REQUIRE(f.device->CreateShaderModule(md, module).IsOk());
    CHECK(f.device->EntryPointOf(module) == StringView(u8"VSMain"));

    PipelineLayoutHandle pipelineLayout{};
    REQUIRE(f.device->CreatePipelineLayout(PipelineLayoutDesc{}, pipelineLayout).IsOk());
    RenderPipelineDesc rd{};
    rd.layout = pipelineLayout;
    rd.vertex.shader = StageOf(module, ShaderStage::Vertex);
    RenderPipelineHandle pipeline{};
    REQUIRE(f.device->CreateRenderPipeline(rd, pipeline).IsOk());
    PipelineCacheHandle cache{};
    REQUIRE(f.device->CreatePipelineCache(PipelineCacheDesc{}, cache).IsOk());
    QuerySetDesc qd{};
    qd.type = QueryType::Occlusion;
    qd.count = 8;
    QuerySetHandle queries{};
    REQUIRE(f.device->CreateQuerySet(qd, queries).IsOk());
    REQUIRE(f.device->Describe(queries) != nullptr);
    CHECK(f.device->Describe(queries)->type == QueryType::Occlusion);
    CHECK(f.device->Describe(queries)->count == 8u);
    CHECK(f.device->PipelineCacheData(cache, Span<u8>{}).IsOk());

    const ShaderModuleHandle keptModule = module;
    const QuerySetHandle keptQueries = queries;
    const PipelineCacheHandle keptCache = cache;
    f.device->DestroyRenderPipeline(pipeline);
    f.device->DestroyPipelineLayout(pipelineLayout);
    f.device->DestroyShaderModule(module);
    f.device->DestroyPipelineCache(cache);
    f.device->DestroyQuerySet(queries);
    CHECK_FALSE(pipeline.IsValid());
    CHECK_FALSE(pipelineLayout.IsValid());
    CHECK(f.device->EntryPointOf(keptModule).IsEmpty());
    CHECK(f.device->Describe(keptQueries) == nullptr);
    CHECK_FALSE(f.device->PipelineCacheData(keptCache, Span<u8>{}).IsOk());
}

TEST_CASE("object lifetime: a bind group handle from one device is refused by another")
{
    NullDeviceFixture a;
    NullDeviceFixture b;
    const BindGroupHandle fromA = a.MakeGroup(a.MakeLayout());
    const BindGroupHandle fromB = b.MakeGroup(b.MakeLayout());
    CHECK(fromA.index == fromB.index); // the same slot in each device's table
    CHECK_FALSE(b.device->LayoutOf(fromA).IsValid());
    CHECK_FALSE(a.device->LayoutOf(fromB).IsValid());
    CHECK(a.device->LayoutOf(fromA).IsValid());
}
