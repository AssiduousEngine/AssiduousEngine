// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The graph's core: resource handles, passes, output size, imports and Reset.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::rendergraph;
namespace rhi = foundation::rhi;

TEST_CASE("rg.graph: create transient returns a valid handle")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle handle = graph.CreateTransient(
        u8"Test", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm, SizeMode::FullSize));
    CHECK(handle.IsValid());
    CHECK(graph.ResourceCount() == 1u);
}

TEST_CASE("rg.graph: multiple resources get unique handles")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle h1 = graph.CreateTransient(u8"A", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    const RGHandle h2 =
        graph.CreateTransient(u8"B", RGTextureDesc(rhi::TextureFormat::Depth32Float));
    CHECK(h1 != h2);
    CHECK(graph.ResourceCount() == 2u);
}

TEST_CASE("rg.graph: get resource by name")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle h1 =
        graph.CreateTransient(u8"SceneColor", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    CHECK(graph.GetResource(u8"SceneColor") == h1);
    CHECK_FALSE(graph.GetResource(u8"NonExistent").IsValid());
}

TEST_CASE("rg.graph: pass count is correct")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Pass1",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddComputePass(u8"Pass2", [](PassBuilder& b) { b.HasSideEffects(); });

    CHECK(graph.PassCount() == 2u);
}

TEST_CASE("rg.graph: set output size affects resolution")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.SetOutputSize(1920, 1080);
    CHECK(graph.OutputWidth() == 1920u);
    CHECK(graph.OutputHeight() == 1080u);
}

TEST_CASE("rg.graph: import target with final state")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle handle =
        graph.ImportTarget(u8"Backbuffer", rhi::TextureHandle{}, rhi::TextureViewHandle{}, rhi::ResourceState::Present);
    CHECK(handle.IsValid());
}

TEST_CASE("rg.graph: reset keeps persistent, drops transient")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    graph.RegisterPersistent(u8"Shadow", {}, {});
    graph.CreateTransient(u8"Temp", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.Reset();

    CHECK(graph.GetResource(u8"Shadow").IsValid());
    CHECK_FALSE(graph.GetResource(u8"Temp").IsValid());
}

TEST_CASE("rg.graph: SetViewport records a per-pass viewport override")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"ViewportPass",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.SetViewport(10, 20, 100, 200);
                            b.NeverCull();
                        });

    REQUIRE(graph.Passes().Size() == 1u);
    const RenderGraphPass* pass = graph.Passes()[0];
    CHECK(pass->hasViewport);
    CHECK(pass->viewportX == 10);
    CHECK(pass->viewportY == 20);
    CHECK(pass->viewportW == 100u);
    CHECK(pass->viewportH == 200u);
}

TEST_CASE("rg.graph: a pass without SetViewport has no viewport override (full attachment)")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"FullPass",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Passes().Size() == 1u);
    CHECK_FALSE(graph.Passes()[0]->hasViewport);
}

TEST_CASE("rg.graph: a slot reused after Reset gives its new resource a new generation")
{
    // Every resource was generation 1, so a handle kept from an earlier frame matched whatever
    // later took its slot: the stale handle resolved to the wrong texture.
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle first = graph.CreateTransient(u8"First", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    graph.Reset();
    graph.BeginFrame(1);
    const RGHandle second = graph.CreateTransient(u8"Second", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    REQUIRE(second.index == first.index); // the slot was reused
    CHECK(second != first);               // and the old handle does not name the new resource
    CHECK(graph.GetResource(u8"Second") == second);
}

TEST_CASE("rg.graph: a handle reaches nothing once its resource is recycled, nor in another graph")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    RenderGraph other(DefaultAllocator(), nullptr);
    const rhi::BufferHandle bufferA{3, 1};
    const rhi::BufferHandle bufferB{5, 1};
    graph.BeginFrame(0);
    other.BeginFrame(0);
    const RGHandle mine = graph.ImportBuffer(u8"Mine", bufferA);
    const RGHandle theirs = other.ImportBuffer(u8"Theirs", bufferB);
    REQUIRE(mine.index == theirs.index); // the same slot in each graph
    CHECK(graph.GetBuffer(mine) == bufferA);
    CHECK_FALSE(graph.GetBuffer(theirs).IsValid()); // another graph's handle: its pool differs
    graph.Reset();
    graph.BeginFrame(1);
    const RGHandle next = graph.ImportBuffer(u8"Next", bufferB);
    REQUIRE(next.index == mine.index);
    CHECK_FALSE(graph.GetBuffer(mine).IsValid()); // kept past the recycle: names nothing
    CHECK(graph.GetBuffer(next) == bufferB);
}
