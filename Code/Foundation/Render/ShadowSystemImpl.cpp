// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:shadows` partition.
///
/// Shadow mapping, sampled with PCF in the forward shader. The directional light renders
/// kShadowCascadesCount (4) cascades into a depth array with a layer per view's cascade; spot and
/// point lights render into a 2048 depth atlas of 512 tiles with two layers, a realtime one redrawn
/// every frame and a static one redrawn only when its casters change. This partition owns the depth
/// textures (one set per frame-in-flight) and imports them into the frame graph; RenderFrame
/// (:pipeline, which has the renderer registry) declares the depth passes that re-emit the casters
/// and threads the bindings into the forward pass.

module;
#include "Core/Prelude.h"

module foundation.render;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data;  // ShadowCascades
import :views; // ViewCamera

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    rhi::TextureView* ShadowSystem::PrepareFrame(u32 frameIndex, u32 viewCount)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        const u32 views =
            (viewCount < 1) ? 1u : (viewCount > kMaxShadowViews ? kMaxShadowViews : viewCount);
        const u32 layers = views * kCascadeCount;
        return EnsureTexture(slot, layers) ? m_sampleViews[slot] : nullptr;
    }

    rhi::TextureView* ShadowSystem::SampleView(u32 frameIndex) const noexcept
    {
        return m_sampleViews[frameIndex % m_framesInFlight];
    }

    rendergraph::RGHandle ShadowSystem::ImportTarget(rendergraph::RenderGraph& graph,
                                                     u32 frameIndex)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        if (m_textures[slot] == nullptr)
        {
            return {};
        } // PrepareFrame creates it (sized for the views)
        const rendergraph::RGHandle h = graph.ImportTarget(
            u8"shadow.map", m_textures[slot], m_attachViews[slot], m_sampleViews[slot],
            rhi::ResourceState::DepthStencilRead, m_states[slot]);
        m_states[slot] = rhi::ResourceState::DepthStencilRead;
        return h;
    }

    rhi::TextureView* ShadowSystem::PrepareAtlas(u32 frameIndex)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        return EnsureAtlas(slot) ? m_atlasSampleViews[slot] : nullptr;
    }

    rendergraph::RGHandle ShadowSystem::ImportAtlas(rendergraph::RenderGraph& graph, u32 frameIndex)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        if (m_atlasTextures[slot] == nullptr)
        {
            return {};
        }
        const rendergraph::RGHandle h = graph.ImportTarget(
            u8"shadow.atlas", m_atlasTextures[slot], m_atlasAttachViews[slot],
            m_atlasSampleViews[slot], rhi::ResourceState::DepthStencilRead, m_atlasStates[slot]);
        m_atlasStates[slot] = rhi::ResourceState::DepthStencilRead;
        return h;
    }

    void ShadowSystem::Release(rhi::Texture*& texture, rhi::TextureView*& attach, rhi::TextureView*& sample)
    {
        // In-flight frames may still sample them, so retire them through the queue when wired
        // (web-safe: no mid-frame WaitIdle pumping the browser event loop), otherwise drain.
        if (texture == nullptr)
        {
            return;
        }
        if (m_retire != nullptr)
        {
            m_retire->Retire(sample);
            m_retire->Retire(attach);
            m_retire->Retire(texture);
        }
        else
        {
            m_device->WaitIdle();
            if (sample != nullptr)
            {
                m_device->DestroyTextureView(sample);
            }
            if (attach != nullptr)
            {
                m_device->DestroyTextureView(attach);
            }
            m_device->DestroyTexture(texture);
        }
        sample = nullptr;
        attach = nullptr;
        texture = nullptr;
    }

    bool ShadowSystem::EnsureTexture(u32 slot, u32 layerCount)
    {
        if (m_textures[slot] != nullptr && m_layerCounts[slot] >= layerCount &&
            m_slotResolutions[slot] == m_resolution)
        {
            return true;
        }
        // Grow (more views) or resize (another resolution): replace the old array and its views.
        Release(m_textures[slot], m_attachViews[slot], m_sampleViews[slot]);
        rhi::TextureDesc td{};
        td.format = kShadowFormat;
        td.width = m_resolution;
        td.height = m_resolution;
        td.arrayLayerCount = layerCount;
        td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        td.label = u8"shadow.cascades";
        if (!m_device->CreateTexture(td, m_textures[slot]).IsOk())
        {
            m_textures[slot] = nullptr;
            return false;
        }

        // Whole-array views (the import's attachment fallback and the forward's Texture2DArray sample).
        rhi::TextureViewDesc av{};
        av.format = kShadowFormat;
        av.aspect = rhi::TextureAspect::DepthOnly;
        av.dimension = rhi::TextureViewDimension::Texture2DArray;
        av.arrayLayerCount = layerCount;
        if (!m_device->CreateTextureView(m_textures[slot], av, m_attachViews[slot]).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc sv{};
        sv.format = kShadowFormat;
        sv.aspect = rhi::TextureAspect::DepthOnly;
        sv.dimension = rhi::TextureViewDimension::Texture2DArray;
        sv.arrayLayerCount = layerCount;
        if (!m_device->CreateTextureView(m_textures[slot], sv, m_sampleViews[slot]).IsOk())
        {
            return false;
        }
        m_layerCounts[slot] = layerCount;
        m_slotResolutions[slot] = m_resolution;
        m_states[slot] = rhi::ResourceState::Undefined;
        ++m_generation; // a new physical shadow texture exists: invalidate consumer bind-group caches
        return true;
    }

    bool ShadowSystem::EnsureAtlas(u32 slot)
    {
        if (m_atlasTextures[slot] != nullptr && m_atlasSlotTiles[slot] == m_atlasTile)
        {
            return true;
        }
        Release(m_atlasTextures[slot], m_atlasAttachViews[slot], m_atlasSampleViews[slot]);
        rhi::TextureDesc td{};
        td.format = kShadowFormat;
        td.width = AtlasResolution();
        td.height = AtlasResolution();
        td.arrayLayerCount = kAtlasLayers;
        td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        td.label = u8"shadow.atlas";
        if (!m_device->CreateTexture(td, m_atlasTextures[slot]).IsOk())
        {
            m_atlasTextures[slot] = nullptr;
            return false;
        }
        rhi::TextureViewDesc av{};
        av.format = kShadowFormat;
        av.aspect = rhi::TextureAspect::DepthOnly;
        av.dimension = rhi::TextureViewDimension::Texture2DArray;
        av.arrayLayerCount = kAtlasLayers;
        if (!m_device->CreateTextureView(m_atlasTextures[slot], av, m_atlasAttachViews[slot])
                 .IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc sv{};
        sv.format = kShadowFormat;
        sv.aspect = rhi::TextureAspect::DepthOnly;
        sv.dimension = rhi::TextureViewDimension::Texture2DArray;
        sv.arrayLayerCount = kAtlasLayers;
        if (!m_device->CreateTextureView(m_atlasTextures[slot], sv, m_atlasSampleViews[slot])
                 .IsOk())
        {
            return false;
        }
        m_atlasSlotTiles[slot] = m_atlasTile;
        ++m_atlasGeneration; // its static layer starts empty
        m_atlasStates[slot] = rhi::ResourceState::Undefined;
        ++m_generation;
        return true;
    }

    rhi::RenderPipeline* ShadowSystem::TileClearPipeline()
    {
        if (m_shaders == nullptr)
        {
            return nullptr;
        }
        const u64 version = m_shaders->Version(u8"shadow_tile_clear");
        if (version == m_tileClearVersion)
        {
            return m_tileClearPipeline;
        }
        // A reload: the GPU is idle then, as for every engine pass's pipeline rebuild.
        if (m_tileClearPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_tileClearPipeline);
            m_tileClearPipeline = nullptr;
        }
        m_tileClearVersion = version;
        if (m_tileClearLayout == nullptr)
        {
            rhi::PipelineLayoutDesc pld{};
            if (!m_device->CreatePipelineLayout(pld, m_tileClearLayout).IsOk())
            {
                m_tileClearLayout = nullptr;
                return nullptr;
            }
        }
        rhi::ShaderModule* vs =
            m_shaders->GetVariant(u8"shadow_tile_clear", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        if (vs == nullptr)
        {
            return nullptr;
        }
        rhi::DepthStencilState ds{};
        ds.format = kShadowFormat;
        ds.depthTestEnabled = true; // Vulkan writes depth only with the test on
        ds.depthWriteEnabled = true;
        ds.depthCompare = rhi::CompareFunction::Always;
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_tileClearLayout;
        pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
        pd.depthStencil = ds;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"shadow.tile-clear";
        if (!m_device->CreateRenderPipeline(pd, m_tileClearPipeline).IsOk())
        {
            m_tileClearPipeline = nullptr;
        }
        return m_tileClearPipeline;
    }

    void ShadowSystem::Shutdown()
    {
        if (m_tileClearPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_tileClearPipeline);
            m_tileClearPipeline = nullptr;
        }
        if (m_tileClearLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_tileClearLayout);
            m_tileClearLayout = nullptr;
        }
        for (u32 i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (m_sampleViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_sampleViews[i]);
                m_sampleViews[i] = nullptr;
            }
            if (m_attachViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_attachViews[i]);
                m_attachViews[i] = nullptr;
            }
            if (m_textures[i] != nullptr)
            {
                m_device->DestroyTexture(m_textures[i]);
                m_textures[i] = nullptr;
            }
            if (m_atlasSampleViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_atlasSampleViews[i]);
                m_atlasSampleViews[i] = nullptr;
            }
            if (m_atlasAttachViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_atlasAttachViews[i]);
                m_atlasAttachViews[i] = nullptr;
            }
            if (m_atlasTextures[i] != nullptr)
            {
                m_device->DestroyTexture(m_atlasTextures[i]);
                m_atlasTextures[i] = nullptr;
            }
        }
    }
}
