// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:debug_blit` partition.
///
/// The editor's debug-view pass: visualize any named render-graph texture in the viewport.
/// A fullscreen pass declared after the compose chain (tonemap, FXAA) and before overlays,
/// it overwrites the view's sub-rect with the selected resource run through channel select,
/// range remap and optional depth linearization; gizmos and overlays still draw on top.
/// Reading the source is a real graph dependency, so transient aliasing keeps the resource
/// alive to this pass with no special machinery. The source binds UnfilterableFloat and the
/// shader reads via Load, so every texture format works (WebGPU included); depth resources
/// bind through their depth-only view.

module;
#include "Core/Prelude.h"

export module foundation.render:debug_blit;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.render.api;
import foundation.shaders;
import foundation.shaders.system;
import :bind_group_cache; // ViewSlots

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // The debug blit's push block (DebugBlitPush, debug_blit.ps.hlsl); Integration.ShaderBlocks
    // checks it against the compiled shader, member by member.
    struct GpuDebugBlitPush
    {
        f32 uvScaleX, uvScaleY;   // the shader's UvScale: fullscreen uv to the view's sub-rect
        f32 uvOffsetX, uvOffsetY; // UvOffset
        f32 sourceW, sourceH;     // SourceSize, in texels
        f32 rangeMin, rangeMax;
        f32 nearZ, farZ;
        u32 mode; // bits 0-2 the channel, bit 4 linearize depth
        u32 pad;
    };
    static_assert(sizeof(GpuDebugBlitPush) == 48, "push DebugBlitPush layout drift");

    // Blits a named graph texture into the view's final LDR target for inspection.
    class DebugBlitPass
    {
    public:
        DebugBlitPass(rhi::Device& device, shaders::ShaderSystem& shaders,
                      u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
            m_slots.SetFramesInFlight(m_framesInFlight);
        }
        ~DebugBlitPass() { Shutdown(); }
        DebugBlitPass(const DebugBlitPass&) = delete;
        DebugBlitPass& operator=(const DebugBlitPass&) = delete;

        Status Initialize()
        {
            // Load-read (no sampler): UnfilterableFloat accepts every color format, and
            // depth binds through its depth-only view (as in the MSAA resolve pass).
            rhi::BindGroupLayoutEntry texEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            texEntry.textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{&texEntry, 1};
            if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            rhi::BindGroupLayoutHandle layouts[] = {m_layout};
            rhi::PushConstantRange pc{};
            pc.stages = rhi::ShaderStage::Fragment;
            pc.offset = 0;
            pc.size = sizeof(GpuDebugBlitPush);
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 1};
            pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            return Status{};
        }

        // Declare the blit: read `src` (any 1-sample graph texture), overwrite `ldr`'s view
        // sub-rect with its visualization. `srcIsDepth` selects the depth-only view and the
        // linearize path from `config`.
        void DeclareDebugBlit(rendergraph::RenderGraph& graph, rendergraph::RGHandle src,
                              rendergraph::RGHandle ldr, rhi::TextureFormat ldrFormat, i32 vpX,
                              i32 vpY, u32 vpW, u32 vpH, u32 frameIndex, u32 viewIndex,
                              f32 srcWidth, f32 srcHeight, bool srcIsDepth,
                              const ViewDebugView& config)
        {
            rhi::RenderPipelineHandle pipeline = EnsurePipeline(ldrFormat);
            if (!pipeline.IsValid())
            {
                return;
            }

            GpuDebugBlitPush push{};
            push.uvScaleX = 1.0f;
            push.uvScaleY = 1.0f;
            push.uvOffsetX = 0.0f;
            push.uvOffsetY = 0.0f;
            push.sourceW = srcWidth;
            push.sourceH = srcHeight;
            push.rangeMin = config.rangeMin;
            push.rangeMax = config.rangeMax;
            push.nearZ = config.nearZ;
            push.farZ = config.farZ;
            push.mode = static_cast<u32>(config.channel & 7u);
            if (srcIsDepth && config.linearizeDepth)
            {
                push.mode |= 16u;
            }

            graph.AddRenderPass(
                u8"debug.blit",
                [this, &graph, src, ldr, vpX, vpY, vpW, vpH, pipeline, viewIndex, frameIndex, push,
                 srcIsDepth](rendergraph::PassBuilder& b)
                {
                    b.SetColorTarget(0, ldr, rhi::LoadOp::Load, rhi::StoreOp::Store);
                    b.ReadTexture(src);
                    b.SetViewport(vpX, vpY, vpW, vpH);
                    b.NeverCull();
                    b.SetExecute(
                        [this, &graph, src, pipeline, viewIndex, frameIndex, push,
                         srcIsDepth](rhi::RenderPassEncoder& rp)
                        {
                            rhi::TextureViewHandle view = srcIsDepth ? graph.GetDepthOnlyTextureView(src)
                                                                : graph.GetTextureView(src);
                            if (!view.IsValid())
                            {
                                view = graph.GetTextureView(src);
                            }
                            rhi::BindGroupHandle bg =
                                EnsureBindGroup(viewIndex, frameIndex, view, graph.GetTextureGeneration(src));
                            if (!bg.IsValid())
                            {
                                return;
                            }
                            rp.SetPipeline(pipeline);
                            rp.SetBindGroup(0, bg, Span<const u32>{});
                            rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(push),
                                                &push);
                            rp.Draw(3, 1, 0, 0);
                        });
                });
        }

    private:
        static constexpr usize kMaxPipelineFormats = 4;
        struct PipelineEntry
        {
            rhi::RenderPipelineHandle pipeline{};
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0;
        };

        rhi::RenderPipelineHandle EnsurePipeline(rhi::TextureFormat fmt)
        {
            const u64 shaderVersion = m_shaders->Version(u8"debug_blit"); // hot reload rebuilds
            PipelineEntry* entry = nullptr;
            for (PipelineEntry& candidate : m_pipelines)
            {
                if (candidate.pipeline.IsValid() && candidate.format == fmt)
                {
                    entry = &candidate;
                    break;
                }
            }
            if (entry != nullptr && entry->shaderVersion == shaderVersion)
            {
                return entry->pipeline;
            }
            rhi::ShaderModuleHandle vs = m_shaders->GetVariant(
                u8"debug_blit", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            rhi::ShaderModuleHandle ps = m_shaders->GetVariant(
                u8"debug_blit", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            if (!vs.IsValid() || !ps.IsValid())
            {
                return {};
            }
            if (entry == nullptr)
            {
                for (PipelineEntry& candidate : m_pipelines)
                {
                    if (!candidate.pipeline.IsValid())
                    {
                        entry = &candidate;
                        break;
                    }
                }
            }
            if (entry == nullptr)
            {
                entry = &m_pipelines[0];
            }
            if (entry->pipeline.IsValid())
            {
                m_device->DestroyRenderPipeline(entry->pipeline);
                entry->pipeline = {};
            }
            rhi::ColorTargetState color{};
            color.format = fmt;
            rhi::FragmentState frag{};
            frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
            frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
            pd.fragment = frag;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = u8"debug.blit";
            if (!m_device->CreateRenderPipeline(pd, entry->pipeline).IsOk())
            {
                entry->pipeline = {};
                return {};
            }
            entry->format = fmt;
            entry->shaderVersion = shaderVersion;
            return entry->pipeline;
        }

        rhi::BindGroupHandle EnsureBindGroup(u32 viewIndex, u32 frameIndex, rhi::TextureViewHandle srcView, u64 generation)
        {
            if (!srcView.IsValid())
            {
                return {};
            }
            Slot& slot = m_slots.At(viewIndex, frameIndex);
            if (slot.group.IsValid() && slot.view == srcView &&
                slot.generation == generation)
            {
                return slot.group;
            }
            if (slot.group.IsValid())
            {
                m_device->DestroyBindGroup(slot.group);
                slot.group = {};
            }
            rhi::BindGroupEntry entries[] = {rhi::BindGroupEntry::TextureEntry(srcView)};
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_layout;
            bgd.entries = Span<const rhi::BindGroupEntry>{entries, 1};
            if (!m_device->CreateBindGroup(bgd, slot.group).IsOk())
            {
                slot.group = {};
                return {};
            }
            slot.view = srcView;
            slot.generation = generation;
            return slot.group;
        }

        void Shutdown()
        {
            for (Slot& slot : m_slots.All())
            {
                if (slot.group.IsValid())
                {
                    m_device->DestroyBindGroup(slot.group);
                    slot.group = {};
                }
            }
            for (PipelineEntry& entry : m_pipelines)
            {
                if (entry.pipeline.IsValid())
                {
                    m_device->DestroyRenderPipeline(entry.pipeline);
                    entry.pipeline = {};
                }
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

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;
        rhi::BindGroupLayoutHandle m_layout{};
        rhi::PipelineLayoutHandle m_pipelineLayout{};
        PipelineEntry m_pipelines[kMaxPipelineFormats] = {};
        // Per (view, frame in flight), however many views a frame has: one bind group and the source
        // view and generation it was built from.
        struct Slot
        {
            rhi::BindGroupHandle group{};
            rhi::TextureViewHandle view{};
            u64 generation = 0;
        };
        ViewSlots<Slot> m_slots;
    };

} // namespace foundation::render
