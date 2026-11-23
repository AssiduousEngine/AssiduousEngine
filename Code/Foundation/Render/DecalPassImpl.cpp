// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:decal_pass` partition.
///
/// Screen-space projected decals: reconstruct the world position under each pixel from the scene
/// depth, transform it into a decal's oriented unit box, clip to the box, and alpha-blend the decal
/// texture onto the lit HDR. The result is a "sprayed" sticker that lands on whatever surface is under
/// the box, including animated meshes, since it reads the depth buffer.
///
/// Runs after the forward and sky passes and before AO and TAA, blending into the HDR so decals are
/// TAA-resolved. The shader assumes row-major matrices, D3D-style [0,1] clip depth and a top-origin uv
/// under the negative viewport (the same reconstruction as AoPass).
///
/// Each decal draws a fullscreen triangle, which is robust across split-screen sub-rects and avoids box
/// winding and culling pitfalls; the box test lives in the fragment shader. The receiver normal for the
/// angle fade comes from the reconstructed world-position derivatives (ddx, ddy). A per-decal box mesh,
/// and sampling the normal G-buffer instead, would be cheaper.

module;
#include "Core/Prelude.h"

module foundation.render;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data;
import :resources;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    Status DecalPass::Initialize()
    {
        // set 0: scene depth (t0) and sampler (s0). set 1: per-decal UBO (dynamic). set 2: decal texture.
        rhi::BindGroupLayoutEntry depthEntries[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        // WebGPU annotations (Vulkan and DX12 ignore them): t0 is the scene depth, read as
        // data through the point sampler.
        depthEntries[0].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        depthEntries[1].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc dld{};
        dld.entries = Span<const rhi::BindGroupLayoutEntry>{depthEntries, 2};
        if (!m_device->CreateBindGroupLayout(dld, m_depthLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutEntry uboEntry =
            rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Fragment);
        uboEntry.hasDynamicOffset = true;
        rhi::BindGroupLayoutDesc uld{};
        uld.entries = Span<const rhi::BindGroupLayoutEntry>{&uboEntry, 1};
        if (!m_device->CreateBindGroupLayout(uld, m_uboLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutEntry texEntries[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc tld{};
        tld.entries = Span<const rhi::BindGroupLayoutEntry>{texEntries, 2};
        if (!m_device->CreateBindGroupLayout(tld, m_texLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutHandle layouts[] = {m_depthLayout, m_uboLayout, m_texLayout};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 3};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        auto mkSampler = [this](rhi::FilterMode f, rhi::SamplerHandle& out)
        {
            rhi::SamplerDesc s{};
            s.minFilter = f;
            s.magFilter = f;
            s.mipmapFilter = f == rhi::FilterMode::Nearest
                                 ? rhi::MipmapFilterMode::Nearest
                                 : rhi::MipmapFilterMode::Linear; // WebGPU: see AoPass's sampler
            s.addressU = rhi::AddressMode::ClampToEdge;
            s.addressV = rhi::AddressMode::ClampToEdge;
            s.addressW = rhi::AddressMode::ClampToEdge;
            return m_device->CreateSampler(s, out).IsOk();
        };
        if (!mkSampler(rhi::FilterMode::Nearest, m_depthSampler) ||
            !mkSampler(rhi::FilterMode::Linear, m_texSampler))
        {
            return Status{ErrorCode::Unknown};
        }

        m_pipelineShaderVersion = m_shaders->Version(u8"decal");
        if (!PipelineFor(1).IsValid()) // single-sample up front; other counts as views ask
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void DecalPass::BeginFrame(u32 frameIndex)
    {
        if (!m_reserved)
        {
            m_reserved = m_decalRing.Reserve(kMaxDecalsPerFrame);
        }
        m_decalRing.BeginFrame(frameIndex);
    }

    void DecalPass::DeclareDecals(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                                  rendergraph::RGHandle depth, Span<const DecalInstance> decals,
                                  const Float4x4& viewProj, u32 w, u32 h, i32 vpX, i32 vpY, u32 vpW,
                                  u32 vpH, u32 samples)
    {
        if (decals.IsEmpty() || w == 0 || h == 0 || !m_decalRing.Buffer().IsValid())
        {
            return;
        }
        if (samples == 0)
        {
            samples = 1;
        }
        const rhi::RenderPipelineHandle pipeline = PipelineFor(samples);
        if (!pipeline.IsValid())
        {
            return;
        }

        const Float4x4 invViewProj = Inverse(viewProj);
        const Float2 invSize{1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h)};

        // This view's decals go into fresh ring slots (with its own InvViewProj). The list is local and
        // captured by value into the pass so it survives to execute time; a shared member would be
        // overwritten by the next view.
        Array<Draw> viewDraws;
        for (const DecalInstance& d : decals)
        {
            if (!d.texture.IsValid())
            {
                continue;
            }
            const DynamicUniformRing::Range r = m_decalRing.Allocate();
            if (!r.ok)
            {
                break;
            }
            DecalUniforms u{};
            u.world = d.world;
            u.invWorld = Inverse(d.world);
            u.invViewProj = invViewProj;
            u.color = Float4{d.color.r, d.color.g, d.color.b, d.color.a};
            u.invTargetSize = invSize;
            u.fadeStartCos = Cos(d.fadeStart);
            u.fadeEndCos = Cos(d.fadeEnd);
            // Interpolant Y correction (the sky's SceneYSign convention): Vulkan's negative viewport
            // already makes the emitted NDC land on the right pixel (+1 leaves it as is); Y-flip
            // targets mirror the interpolant, so -1 undoes the mirror.
            u.yFlip = m_device->NeedsClipSpaceYFlip() ? -1.0f : 1.0f;
            MemCopy(r.ptr, &u, sizeof(u));
            viewDraws.PushBack(Draw{r.byteOffset, d.texture});
        }
        if (viewDraws.IsEmpty())
        {
            return;
        }

        rhi::BindGroupHandle uboBg = EnsureUboBindGroup();
        graph.AddRenderPass(
            u8"decal",
            [this, &graph, hdr, depth, uboBg, pipeline, vpX, vpY, vpW, vpH,
             draws = viewDraws](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, hdr, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(depth);
                // The view's sub-rect (matches the forward and sky), so the fullscreen tri's emitted NDC lines up
                // with the scene geometry's NDC at each pixel.
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, depth, uboBg, pipeline, draws](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroupHandle depthBg = EnsureDepthBindGroup(
                            graph.GetTextureView(depth), graph.GetTextureGeneration(depth));
                        if (!depthBg.IsValid() || !uboBg.IsValid())
                        {
                            return;
                        }
                        rp.SetPipeline(pipeline);
                        rp.SetBindGroup(0, depthBg, Span<const u32>{});
                        for (const Draw& d : draws)
                        {
                            rhi::BindGroupHandle texBg = EnsureTextureBindGroup(d.texture);
                            if (!texBg.IsValid())
                            {
                                continue;
                            }
                            rp.SetBindGroup(1, uboBg, Span<const u32>{&d.offset, 1});
                            rp.SetBindGroup(2, texBg, Span<const u32>{});
                            rp.Draw(3, 1, 0, 0);
                        }
                    });
            });
    }

    rhi::RenderPipelineHandle DecalPass::MakePipeline(u32 samples)
    {
        rhi::ShaderModuleHandle vs = m_shaders->GetVariant(u8"decal", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModuleHandle ps = m_shaders->GetVariant(u8"decal", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (!vs.IsValid() || !ps.IsValid())
        {
            return {};
        }
        rhi::ColorTargetState color{};
        color.format = kDecalHdrFormat;
        color.blend = rhi::BlendState::AlphaBlend();
        rhi::FragmentState frag{};
        frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None; // fullscreen tri, no depth attachment
        pd.multisample.count = samples; // scene-pass MSAA: decals draw into the MSAA hdr
        pd.label = u8"decal";
        rhi::RenderPipelineHandle p{};
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return {};
        }
        return p;
    }

    rhi::BindGroupHandle DecalPass::EnsureUboBindGroup()
    {
        const u32 gen = m_decalRing.Generation();
        if (m_uboBg.IsValid() && m_uboBgGen == gen)
        {
            return m_uboBg;
        }
        if (m_uboBg.IsValid())
        {
            m_device->DestroyBindGroup(m_uboBg);
            m_uboBg = {};
        }
        if (!m_decalRing.Buffer().IsValid())
        {
            return {};
        }
        rhi::BindGroupEntry e =
            rhi::BindGroupEntry::BufferEntry(m_decalRing.Buffer(), 0, m_decalRing.SlotSize());
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_uboLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
        if (!m_device->CreateBindGroup(bgd, m_uboBg).IsOk())
        {
            m_uboBg = {};
            return {};
        }
        m_uboBgGen = gen;
        return m_uboBg;
    }

    rhi::BindGroupHandle DecalPass::EnsureDepthBindGroup(rhi::TextureViewHandle depth, u64 generation)
    {
        if (!depth.IsValid())
        {
            return {};
        }
        if (m_depthBg.IsValid() && m_depthView == depth && m_depthGen == generation)
        {
            return m_depthBg;
        }
        if (m_depthBg.IsValid())
        {
            m_device->DestroyBindGroup(m_depthBg);
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::SamplerEntry(m_depthSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_depthLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
        if (!m_device->CreateBindGroup(bgd, m_depthBg).IsOk())
        {
            m_depthBg = {};
            return {};
        }
        m_depthView = depth;
        m_depthGen = generation;
        return m_depthBg;
    }

    rhi::BindGroupHandle DecalPass::EnsureTextureBindGroup(rhi::TextureViewHandle tex)
    {
        if (!tex.IsValid())
        {
            return {};
        }
        if (rhi::BindGroupHandle* found = m_texBindGroups.Find(tex))
        {
            return *found;
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(tex),
            rhi::BindGroupEntry::SamplerEntry(m_texSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_texLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
        rhi::BindGroupHandle bg{};
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return {};
        }
        DropGroupsOfDestroyedViews(*m_device, m_texBindGroups);
        m_texBindGroups.InsertOrAssign(tex, bg);
        return bg;
    }

    rhi::RenderPipelineHandle DecalPass::PipelineFor(u32 samples)
    {
        const u64 shaderVersion = m_shaders->Version(u8"decal");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            // A reload, which lands between frames: each count's pipeline is rebuilt as it is asked
            // for. The device keeps the old ones while frames in flight use them.
            DestroyPipelines();
            m_pipelineShaderVersion = shaderVersion;
        }
        for (const Pipeline& cached : m_pipelines)
        {
            if (cached.samples == samples)
            {
                return cached.pipeline;
            }
        }
        const rhi::RenderPipelineHandle made = MakePipeline(samples);
        if (made.IsValid())
        {
            m_pipelines.PushBack(Pipeline{samples, made});
        }
        return made;
    }

    void DecalPass::DestroyPipelines()
    {
        for (Pipeline& cached : m_pipelines)
        {
            m_device->DestroyRenderPipeline(cached.pipeline);
        }
        m_pipelines.Clear();
    }

    void DecalPass::Shutdown()
    {
        for (auto& kv : m_texBindGroups)
        {
            if (kv.value.IsValid())
            {
                m_device->DestroyBindGroup(kv.value);
            }
        }
        m_texBindGroups.Clear();
        if (m_depthBg.IsValid())
        {
            m_device->DestroyBindGroup(m_depthBg);
            m_depthBg = {};
        }
        if (m_uboBg.IsValid())
        {
            m_device->DestroyBindGroup(m_uboBg);
            m_uboBg = {};
        }
        DestroyPipelines();
        if (m_texSampler.IsValid())
        {
            m_device->DestroySampler(m_texSampler);
            m_texSampler = {};
        }
        if (m_depthSampler.IsValid())
        {
            m_device->DestroySampler(m_depthSampler);
            m_depthSampler = {};
        }
        if (m_pipelineLayout.IsValid())
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = {};
        }
        if (m_texLayout.IsValid())
        {
            m_device->DestroyBindGroupLayout(m_texLayout);
            m_texLayout = {};
        }
        if (m_uboLayout.IsValid())
        {
            m_device->DestroyBindGroupLayout(m_uboLayout);
            m_uboLayout = {};
        }
        if (m_depthLayout.IsValid())
        {
            m_device->DestroyBindGroupLayout(m_depthLayout);
            m_depthLayout = {};
        }
    }
}
