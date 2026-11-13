// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The render-side PSO cache: build a pipeline from a PipelineConfig pulling variants
// from the ShaderSystem, verify it caches, and verify the version-polling hot-reload
// path, where invalidating the shader makes GetPipeline rebuild and retire the stale PSO.
// Nothing here compiles shaders: the cache needs a shader module to point at and a
// version to poll, so the ShaderSystem runs without a compiler over a cooked pack of
// placeholder bytes (the Null device accepts any bytecode). That lets these tests run on
// every machine, with or without DXC.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.shaders;
import foundation.shaders.system;
import foundation.materials;
import foundation.materials.pipelinecache;

using namespace foundation::core;
using namespace foundation::materials;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

namespace
{
    // One placeholder variant per stage, under every cooked format so the pack answers
    // whatever format the device reports, with the entry point the cook records.
    void AddPlaceholder(shaders::CookedShaderPack& pack, StringView name,
                        shaders::ShaderStage stage)
    {
        static constexpr byte kBlob[4] = {byte{1}, byte{2}, byte{3}, byte{4}};
        static constexpr shaders::CookedShaderFormat kFormats[] = {
            shaders::CookedShaderFormat::SpirV, shaders::CookedShaderFormat::Dxil,
            shaders::CookedShaderFormat::Wgsl};
        for (const shaders::CookedShaderFormat format : kFormats)
        {
            pack.Add(name, stage, shaders::ShaderFlags::None, format, Span<const byte>(kBlob, 4));
        }
        pack.AddEntryPoint(name, stage, shaders::kCorpusEntryPoint); // as the cook records it
    }
}

TEST_CASE("pso cache: builds + caches a pipeline; shader reload rebuilds via version poll")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::CookedShaderPack pack;
    AddPlaceholder(pack, u8"forward", shaders::ShaderStage::Vertex);
    AddPlaceholder(pack, u8"forward", shaders::ShaderStage::Fragment);
    AddPlaceholder(pack, u8"other", shaders::ShaderStage::Vertex);
    AddPlaceholder(pack, u8"other", shaders::ShaderStage::Fragment);
    shaders::ShaderSystem shaderSystem(device);
    shaderSystem.SetCookedPack(&pack);

    rhi::PipelineLayout* layout = nullptr;
    REQUIRE(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{}, layout).IsOk());

    PipelineStateCache cache(shaderSystem, device);
    const PipelineConfig config = PipelineConfig::ForOpaqueMesh(u8"forward");

    rhi::RenderPipeline* p0 = cache.GetPipeline(config, layout);
    REQUIRE(p0 != nullptr);
    CHECK(cache.Size() == 1);

    // the same request gives the same pipeline (cache hit, no rebuild)
    rhi::RenderPipeline* p1 = cache.GetPipeline(config, layout);
    CHECK(p1 == p0);
    CHECK(cache.Size() == 1);
    CHECK(cache.RetiredCount() == 0);

    // reload the shader: the version bumps, so the next GetPipeline rebuilds and retires the old PSO
    shaderSystem.InvalidateShader(u8"forward");
    rhi::RenderPipeline* p2 = cache.GetPipeline(config, layout);
    REQUIRE(p2 != nullptr);
    CHECK(p2 != p0);                  // rebuilt against the new shader
    CHECK(cache.Size() == 1);         // same key, replaced in place
    CHECK(cache.RetiredCount() == 1); // stale pipeline retired, awaiting GPU-safe free

    cache.ReleaseRetired();
    CHECK(cache.RetiredCount() == 0);

    // versions are per shader: invalidating another shader rebuilds nothing here
    rhi::RenderPipeline* pOther =
        cache.GetPipeline(PipelineConfig::ForOpaqueMesh(u8"other"), layout);
    REQUIRE(pOther != nullptr);
    shaderSystem.InvalidateShader(u8"other");
    CHECK(cache.GetPipeline(config, layout) == p2);
    CHECK(cache.GetPipeline(PipelineConfig::ForOpaqueMesh(u8"other"), layout) != pOther);
    cache.ReleaseRetired();

    // a distinct render state is a distinct cache entry
    rhi::RenderPipeline* pT =
        cache.GetPipeline(PipelineConfig::ForTransparentMesh(u8"forward"), layout);
    REQUIRE(pT != nullptr);
    CHECK(cache.Size() == 3);

    cache.Clear();
    CHECK(cache.Size() == 0);

    device.DestroyPipelineLayout(layout);
}

TEST_CASE("pso cache: depth-only config builds without a fragment shader")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::CookedShaderPack pack;
    AddPlaceholder(pack, u8"shadow", shaders::ShaderStage::Vertex);
    // no fragment variant in the pack
    shaders::ShaderSystem shaderSystem(device);
    shaderSystem.SetCookedPack(&pack);

    rhi::PipelineLayout* layout = nullptr;
    REQUIRE(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{}, layout).IsOk());

    PipelineStateCache cache(shaderSystem, device);
    PipelineConfig config = PipelineConfig::ForOpaqueMesh(u8"shadow");
    config.depthOnly = true;
    config.vertexLayout = VertexLayoutType::PositionOnly;

    rhi::RenderPipeline* p = cache.GetPipeline(config, layout);
    CHECK(p != nullptr); // vertex-only pipeline, no fragment fetched

    cache.Clear();
    device.DestroyPipelineLayout(layout);
}

TEST_CASE("pso cache: configs apart in any field are apart; a failed build waits for the shader to change")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::CookedShaderPack pack;
    AddPlaceholder(pack, u8"shadow", shaders::ShaderStage::Vertex);
    AddPlaceholder(pack, u8"shadow", shaders::ShaderStage::Fragment);
    shaders::ShaderSystem shaderSystem(device);
    shaderSystem.SetCookedPack(&pack);
    rhi::PipelineLayout* layout = nullptr;
    REQUIRE(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{}, layout).IsOk());
    PipelineStateCache cache(shaderSystem, device, /*framesInFlight*/ 2);

    // Two depth passes apart only in their slope bias are two pipelines (the hash left it out, so
    // they were one).
    PipelineConfig caster = PipelineConfig::ForOpaqueMesh(u8"shadow");
    caster.depthBiasSlopeScale = 1.5f;
    PipelineConfig prepass = caster;
    prepass.depthBiasSlopeScale = 0.0f;
    rhi::RenderPipeline* a = cache.GetPipeline(caster, layout);
    rhi::RenderPipeline* b = cache.GetPipeline(prepass, layout);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    CHECK(cache.Size() == 2u);

    // A name held elsewhere (a view into other storage) finds the same entry: the name is matched.
    String sameName(u8"shadow");
    PipelineConfig again = caster;
    again.shaderName = sameName.AsView();
    CHECK(cache.GetPipeline(again, layout) == a);

    // A shader with no variants fails once, and is not built again on every draw after.
    const PipelineConfig broken = PipelineConfig::ForOpaqueMesh(u8"missing");
    CHECK(cache.GetPipeline(broken, layout) == nullptr);
    const u64 tried = cache.BuildAttempts();
    for (int draw = 0; draw < 10; ++draw)
    {
        CHECK(cache.GetPipeline(broken, layout) == nullptr);
    }
    CHECK(cache.BuildAttempts() == tried);
    shaderSystem.InvalidateShader(u8"missing"); // an edit: worth trying again
    (void)cache.GetPipeline(broken, layout);
    CHECK(cache.BuildAttempts() == tried + 1u);

    // A pipeline a reload superseded lives through the frames in flight, then goes.
    shaderSystem.InvalidateShader(u8"shadow");
    REQUIRE(cache.GetPipeline(caster, layout) != a);
    CHECK(cache.RetiredCount() == 1u);
    cache.TickRetired();
    CHECK(cache.RetiredCount() == 1u); // one frame in flight may still draw with it
    cache.TickRetired();
    CHECK(cache.RetiredCount() == 0u);

    cache.Clear();
    device.DestroyPipelineLayout(layout);
}
