// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pass culling: an unused pass is culled unless a flag, a final-state import or a live reader keeps it.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::rendergraph;
namespace rhi = foundation::rhi;

TEST_CASE("rg.cull: unused pass is culled")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Unused", [&](PassBuilder& b)
                        { b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store); });

    REQUIRE(graph.Compile().IsOk());
    CHECK(graph.CulledPassCount() == 1u);
}

TEST_CASE("rg.cull: NeverCull prevents culling")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Important",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    CHECK(graph.CulledPassCount() == 0u);
}

TEST_CASE("rg.cull: HasSideEffects prevents culling")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    graph.AddComputePass(u8"SideEffect", [](PassBuilder& b) { b.HasSideEffects(); });

    REQUIRE(graph.Compile().IsOk());
    CHECK(graph.CulledPassCount() == 0u);
}

TEST_CASE("rg.cull: backward propagation keeps dependencies alive")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle depth =
        graph.CreateTransient(u8"Depth", RGTextureDesc(rhi::TextureFormat::Depth32Float));
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"DepthPrepass", [&](PassBuilder& b)
                        { b.SetDepthTarget(depth, rhi::LoadOp::Clear, rhi::StoreOp::Store); });
    graph.AddRenderPass(u8"ForwardOpaque",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(depth);
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    CHECK(graph.CulledPassCount() == 0u); // DepthPrepass kept alive by ForwardOpaque
}

TEST_CASE("rg.cull: imported with final state prevents culling")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle backbuffer =
        graph.ImportTarget(u8"BB", nullptr, nullptr, rhi::ResourceState::Present);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Render", [&](PassBuilder& b)
                        { b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store); });
    graph.AddRenderPass(u8"Blit",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(color);
                            b.SetColorTarget(0, backbuffer, rhi::LoadOp::Clear,
                                             rhi::StoreOp::Store);
                        });

    REQUIRE(graph.Compile().IsOk());
    CHECK(graph.CulledPassCount() == 0u);
}

TEST_CASE("rg.cull: a reader of mip 2 to the end keeps the writer of mip 2")
{
    // Culling read a count of 0 ("to the end") against one mip, so a read from mip 2 onward met no
    // writer past mip 0 and the pass that wrote mip 2 was culled.
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    RGTextureDesc desc(rhi::TextureFormat::RGBA16Float, 64, 64);
    desc.mipLevelCount = 4;
    const RGHandle pyramid = graph.CreateTransient(u8"Pyramid", desc);
    const RGHandle out = graph.CreateTransient(u8"Out", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddComputePass(u8"WriteMip2",
                         [&](PassBuilder& b) { b.WriteStorage(pyramid, RGSubresourceRange{2, 1, 0, 1}); });
    graph.AddRenderPass(u8"ReadFromMip2",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(pyramid, RGSubresourceRange{2, 0, 0, 1});
                            b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    CHECK(graph.CulledPassCount() == 0u);
}
