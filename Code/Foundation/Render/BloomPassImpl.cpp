// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:bloom` partition.
///
/// HDR bloom via a downsample/upsample pyramid (Jimenez, "Next Generation Post Processing in Call of
/// Duty": a 13-tap downsample and a 9-tap tent upsample, additive). The first downsample applies a
/// soft-knee threshold and a Karis average to keep fireflies out. The pyramid is built from
/// render-graph transients (sized per view, so no resize handling); the tonemap pass composites the
/// result additively. Runs on the linear HDR scene, before tonemap.

module;
#include "Core/Prelude.h"

module foundation.render;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    Status BloomPass::Initialize()
    {

        rhi::BindGroupLayoutEntry tex =
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry samp =
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry e[] = {tex, samp};
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{e, 2};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutHandle layouts[] = {m_layout};
        rhi::PushConstantRange pc{};
        pc.stages = rhi::ShaderStage::Fragment;
        pc.offset = 0;
        pc.size = sizeof(GpuBloomPush);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Linear;
        ss.magFilter = rhi::FilterMode::Linear;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"bloom.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        m_downPipeline = MakePipeline(u8"bloom_ds", /*additive*/ false);
        m_upPipeline = MakePipeline(u8"bloom_us", /*additive*/ true);
        if (!m_downPipeline.IsValid() || !m_upPipeline.IsValid())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    rendergraph::RGHandle BloomPass::DeclareBloom(rendergraph::RenderGraph& graph,
                                                  rendergraph::RGHandle hdr, u32 vpW, u32 vpH,
                                                  f32 threshold, f32 knee, u32 maxLevels)
    {
        if (vpW < 4 || vpH < 4)
        {
            return {};
        }
        // Hot reload: rebuild both pipelines when the shaders changed (the subsystem
        // idles the GPU on a reload, so immediate destroy is safe).
        const u64 shaderVersion =
            m_shaders->Version(u8"bloom_ds") + m_shaders->Version(u8"bloom_us");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            if (m_downPipeline.IsValid())
            {
                m_device->DestroyRenderPipeline(m_downPipeline);
            }
            if (m_upPipeline.IsValid())
            {
                m_device->DestroyRenderPipeline(m_upPipeline);
            }
            m_downPipeline = MakePipeline(u8"bloom_ds", /*additive*/ false);
            m_upPipeline = MakePipeline(u8"bloom_us", /*additive*/ true);
            m_pipelineShaderVersion = shaderVersion;
        }
        if (!m_downPipeline.IsValid() || !m_upPipeline.IsValid())
        {
            return {};
        }
        // Level count: halve until a small floor, capped by the pass and by the view's budget.
        const u32 cap = Clamp(maxLevels, 1u, kMaxLevels);
        u32 levels = 1;
        for (u32 w = vpW / 2, h = vpH / 2; levels < cap && w > 8 && h > 8; w /= 2, h /= 2)
        {
            ++levels;
        }

        rendergraph::RGHandle chain[kMaxLevels] = {};
        u32 lw[kMaxLevels] = {}, lh[kMaxLevels] = {};
        u32 w = vpW, h = vpH;
        for (u32 i = 0; i < levels; ++i)
        {
            w = Max(1u, w / 2);
            h = Max(1u, h / 2);
            lw[i] = w;
            lh[i] = h;
            chain[i] = graph.CreateTransient(u8"bloom.mip",
                                             rendergraph::RGTextureDesc(kBloomFormat, w, h));
        }

        // Downsample: hdr into mip0 (thresholded), then mip(i-1) into mip(i).
        for (u32 i = 0; i < levels; ++i)
        {
            const rendergraph::RGHandle src = (i == 0) ? hdr : chain[i - 1];
            const u32 srcW = (i == 0) ? vpW : lw[i - 1];
            const u32 srcH = (i == 0) ? vpH : lh[i - 1];
            GpuBloomPush push{};
            push.srcTexel = Float2{1.0f / static_cast<f32>(srcW), 1.0f / static_cast<f32>(srcH)};
            push.threshold = threshold;
            push.knee = knee;
            push.firstPass = (i == 0) ? 1 : 0;
            const rendergraph::RGHandle dst = chain[i];
            const u32 dw = lw[i], dh = lh[i];
            graph.AddRenderPass(u8"bloom.down",
                                [this, &graph, src, dst, dw, dh, push](rendergraph::PassBuilder& b)
                                {
                                    b.SetColorTarget(0, dst, rhi::LoadOp::Clear,
                                                     rhi::StoreOp::Store, rhi::ClearColor::Black());
                                    b.ReadTexture(src);
                                    b.SetViewport(0, 0, dw, dh);
                                    b.NeverCull();
                                    b.SetExecute(
                                        [this, &graph, src, push](rhi::RenderPassEncoder& rp)
                                        {
                                            rhi::BindGroupHandle bg =
                                                EnsureBindGroup(graph.GetTextureView(src),
                                                                graph.GetTextureGeneration(src));
                                            if (!bg.IsValid())
                                            {
                                                return;
                                            }
                                            rp.SetPipeline(m_downPipeline);
                                            rp.SetBindGroup(0, bg, Span<const u32>{});
                                            rp.SetPushConstants(rhi::ShaderStage::Fragment, 0,
                                                                sizeof(GpuBloomPush), &push);
                                            rp.Draw(3, 1, 0, 0);
                                        });
                                });
        }

        // Upsample (additive): add mip(i+1) into mip(i), coarsest to finest.
        for (i32 i = static_cast<i32>(levels) - 2; i >= 0; --i)
        {
            const rendergraph::RGHandle src = chain[i + 1];
            const rendergraph::RGHandle dst = chain[i];
            GpuBloomPush push{};
            push.srcTexel =
                Float2{1.0f / static_cast<f32>(lw[i + 1]), 1.0f / static_cast<f32>(lh[i + 1])};
            const u32 dw = lw[i], dh = lh[i];
            graph.AddRenderPass(u8"bloom.up",
                                [this, &graph, src, dst, dw, dh, push](rendergraph::PassBuilder& b)
                                {
                                    b.SetColorTarget(0, dst, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                                     rhi::ClearColor::Black()); // additive blend
                                    b.ReadTexture(src);
                                    b.SetViewport(0, 0, dw, dh);
                                    b.NeverCull();
                                    b.SetExecute(
                                        [this, &graph, src, push](rhi::RenderPassEncoder& rp)
                                        {
                                            rhi::BindGroupHandle bg =
                                                EnsureBindGroup(graph.GetTextureView(src),
                                                                graph.GetTextureGeneration(src));
                                            if (!bg.IsValid())
                                            {
                                                return;
                                            }
                                            rp.SetPipeline(m_upPipeline);
                                            rp.SetBindGroup(0, bg, Span<const u32>{});
                                            rp.SetPushConstants(rhi::ShaderStage::Fragment, 0,
                                                                sizeof(GpuBloomPush), &push);
                                            rp.Draw(3, 1, 0, 0);
                                        });
                                });
        }
        return chain[0];
    }

    rhi::RenderPipelineHandle BloomPass::MakePipeline(StringView name, bool additive)
    {
        rhi::ShaderModuleHandle vs =
            m_shaders->GetVariant(name, shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModuleHandle ps =
            m_shaders->GetVariant(name, shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (!vs.IsValid() || !ps.IsValid())
        {
            return {};
        }
        rhi::ColorTargetState color{};
        color.format = kBloomFormat;
        if (additive)
        {
            color.blend = rhi::BlendState::Additive();
        }
        rhi::FragmentState frag{};
        frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = name;
        rhi::RenderPipelineHandle p{};
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return {};
        }
        return p;
    }

    rhi::BindGroupHandle BloomPass::EnsureBindGroup(rhi::TextureViewHandle view, u64 generation)
    {
        if (!view.IsValid())
        {
            return {};
        }
        if (Entry* e = m_bindGroups.Find(view))
        {
            if (e->gen == generation && e->bg.IsValid())
            {
                return e->bg;
            }
            if (e->bg.IsValid())
            {
                m_device->DestroyBindGroup(e->bg);
                e->bg = {};
            }
        }
        rhi::BindGroupEntry ent[] = {rhi::BindGroupEntry::TextureEntry(view),
                                     rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
        rhi::BindGroupHandle bg{};
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return {};
        }
        m_bindGroups.InsertOrAssign(view, Entry{bg, generation});
        return bg;
    }

    void BloomPass::Shutdown()
    {
        for (auto& kv : m_bindGroups)
        {
            if (kv.value.bg.IsValid())
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_bindGroups.Clear();
        if (m_downPipeline.IsValid())
        {
            m_device->DestroyRenderPipeline(m_downPipeline);
            m_downPipeline = {};
        }
        if (m_upPipeline.IsValid())
        {
            m_device->DestroyRenderPipeline(m_upPipeline);
            m_upPipeline = {};
        }
        if (m_sampler.IsValid())
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = {};
        }
        if (m_pipelineLayout.IsValid())
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = {};
        }
        if (m_layout.IsValid())
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = {};
        }
    }
}
