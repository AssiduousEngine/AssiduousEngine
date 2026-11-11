// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Imgui.Engine - the `:renderer` partition.
///
/// A Dear ImGui renderer built on our RHI rather than a stock imgui backend. It owns the GPU
/// pipeline, the font atlas and the per-frame dynamic vertex, index and uniform buffers. Prepare readies
/// a frame's ImDrawData before a pass and Draw records it into one (the render subsystem's shared
/// overlay pass, the window turned onto the target when the swapchain is pre-rotated); Render does
/// both in a pass of its own. The font atlas upload waits for the first Prepare, since a command
/// encoder is available there and not at init.

module;
#include "Core/Prelude.h"
#include "imgui.h"
#include <cstring>

export module imgui.engine:renderer;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system;

using namespace foundation::core;
using namespace foundation;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace imgui
{

    // The ImGui vertex and pixel shaders ship with the engine shaders (imgui.vs and imgui.ps) and
    // are resolved through ShaderSystem::GetVariant("imgui", ...): cooked WGSL from the pack on
    // web, compiled from Data/Engine/Shaders on desktop. There is no inline HLSL here.
    class ImguiRenderer
    {
    public:
        ImguiRenderer(rhi::Device& device, shaders::ShaderSystem& shaders,
                      u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_fif(framesInFlight < 1 ? 1 : (framesInFlight > kMaxFIF ? kMaxFIF : framesInFlight))
        {
        }
        ~ImguiRenderer() { Shutdown(); }
        ImguiRenderer(const ImguiRenderer&) = delete;
        ImguiRenderer& operator=(const ImguiRenderer&) = delete;

        Status Initialize()
        {
            // The "imgui" shaders come from the ShaderSystem (the cooked pack or the development
            // file provider), as part of the engine shaders.

            // set 0: projection UBO (b0, Vertex), font texture (t0) and sampler (s0).
            rhi::BindGroupLayoutEntry projE =
                rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
            rhi::BindGroupLayoutEntry texE =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry sampE =
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry set0[] = {projE, texE, sampE};
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{set0, 3};
            if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            rhi::BindGroupLayout* layouts[] = {m_layout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            if (!CreateFontAtlas())
            {
                return Status{ErrorCode::Unknown};
            }

            // A projection UBO and bind group per frame in flight. The font and sampler are shared;
            // the UBO is per slot so an in-flight frame's projection is not overwritten.
            for (u32 i = 0; i < m_fif; ++i)
            {
                rhi::BufferDesc ud{};
                ud.size = sizeof(Float4x4);
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                ud.label = u8"imgui.proj";
                if (!m_device->CreateBuffer(ud, m_frames[i].ubo).IsOk())
                {
                    return Status{ErrorCode::Unknown};
                }
                rhi::BindGroupEntry be[] = {
                    rhi::BindGroupEntry::BufferEntry(m_frames[i].ubo, 0, sizeof(Float4x4)),
                    rhi::BindGroupEntry::TextureEntry(m_fontView),
                    rhi::BindGroupEntry::SamplerEntry(m_sampler),
                };
                rhi::BindGroupDesc bgd{};
                bgd.layout = m_layout;
                bgd.entries = Span<const rhi::BindGroupEntry>{be, 3};
                if (!m_device->CreateBindGroup(bgd, m_frames[i].bindGroup).IsOk())
                {
                    return Status{ErrorCode::Unknown};
                }
            }
            return Status{};
        }

        /// The projection a vertex in window pixels takes to clip space on a target of `target`
        /// pixels that the window is drawn into turned (`windowToTarget`: ScreenOverlayView::
        /// WindowToTargetTransform, the identity when nothing is turned): the turn, then the
        /// target's orthographic map (0..width, 0..height, y down). Row vectors.
        [[nodiscard]] static Float4x4 Projection(Float2 target, const Float4x4& windowToTarget) noexcept
        {
            Float4x4 ortho{};
            ortho.m[0][0] = 2.0f / target.x;
            ortho.m[1][1] = -2.0f / target.y;
            ortho.m[2][2] = 1.0f;
            ortho.m[3][3] = 1.0f;
            ortho.m[3][0] = -1.0f;
            ortho.m[3][1] = 1.0f;
            return windowToTarget * ortho;
        }

        /// The scissor for a draw's clip rectangle, in window pixels, on the target: turned with the
        /// window (`turn`, the window `window` pixels) and kept inside the target, which a scissor
        /// outside the attachment is not on WebGPU. False when nothing of it is left.
        [[nodiscard]] static bool Scissor(Rectangle clip, ContentRotation turn, Float2 window, u32 targetWidth,
                                          u32 targetHeight, u32& x, u32& y, u32& width, u32& height) noexcept
        {
            const Rectangle r = TurnRectangle(clip, turn, window);
            const f32 x0 = r.x < 0.0f ? 0.0f : r.x;
            const f32 y0 = r.y < 0.0f ? 0.0f : r.y;
            const f32 right = r.x + r.width;
            const f32 bottom = r.y + r.height;
            const f32 x1 = right > static_cast<f32>(targetWidth) ? static_cast<f32>(targetWidth) : right;
            const f32 y1 = bottom > static_cast<f32>(targetHeight) ? static_cast<f32>(targetHeight) : bottom;
            if (x1 <= x0 || y1 <= y0)
            {
                return false;
            }
            x = static_cast<u32>(x0);
            y = static_cast<u32>(y0);
            width = static_cast<u32>(x1) - x;
            height = static_cast<u32>(y1) - y;
            return width > 0 && height > 0;
        }

        /// Readies `drawData` for a pass on a target of `targetFormat` (with `depthStencilFormat`'s
        /// attachment, Undefined for none) and `target` pixels: the font, the geometry and the
        /// projection, recorded on `enc` before the pass opens. False when there is nothing to draw;
        /// Draw then draws nothing.
        bool Prepare(rhi::CommandEncoder& enc, rhi::TextureFormat targetFormat, rhi::TextureFormat depthStencilFormat,
                     Float2 target, const Float4x4& windowToTarget, const ImDrawData* drawData, u32 frameIndex)
        {
            m_prepared = nullptr;
            if (drawData == nullptr || drawData->TotalVtxCount <= 0 || drawData->CmdListsCount <= 0)
            {
                return false;
            }
            EnsureFontUploaded(enc);
            rhi::RenderPipeline* pipeline = EnsurePipeline(targetFormat, depthStencilFormat);
            if (pipeline == nullptr)
            {
                return false;
            }
            FrameSlot& slot = m_frames[frameIndex % m_fif];
            if (!UploadGeometry(slot, drawData))
            {
                return false;
            }
            const Float4x4 proj = Projection(target, windowToTarget);
            if (void* p = slot.ubo->Map())
            {
                MemCopy(p, &proj, sizeof(Float4x4));
                slot.ubo->Unmap();
            }
            m_prepared = &slot;
            return true;
        }

        /// Draws what Prepare readied into the open `pass` on a `targetWidth` x `targetHeight`
        /// target, the window (`window` pixels) drawn into it turned by `turn`.
        void Draw(rhi::RenderPassEncoder& pass, const ImDrawData* drawData, ContentRotation turn, Float2 window, u32 targetWidth,
                  u32 targetHeight)
        {
            FrameSlot* slot = m_prepared;
            m_prepared = nullptr;
            if (slot == nullptr || drawData == nullptr)
            {
                return;
            }
            pass.SetViewport(0.0f, 0.0f, static_cast<f32>(targetWidth), static_cast<f32>(targetHeight));
            pass.SetPipeline(m_pipeline);
            pass.SetVertexBuffer(0, slot->vtx, 0);
            pass.SetIndexBuffer(slot->idx, rhi::IndexFormat::UInt16, 0);
            const ImVec2 clipOff = drawData->DisplayPos;
            u32 globalVtx = 0, globalIdx = 0;
            for (i32 n = 0; n < drawData->CmdListsCount; ++n)
            {
                const ImDrawList* cmdList = drawData->CmdLists[n];
                for (i32 c = 0; c < cmdList->CmdBuffer.Size; ++c)
                {
                    const ImDrawCmd& cmd = cmdList->CmdBuffer[c];
                    if (cmd.ElemCount == 0 || cmd.UserCallback != nullptr)
                    {
                        continue;
                    }
                    const Rectangle clip{cmd.ClipRect.x - clipOff.x, cmd.ClipRect.y - clipOff.y, cmd.ClipRect.z - cmd.ClipRect.x,
                                         cmd.ClipRect.w - cmd.ClipRect.y};
                    u32 x = 0, y = 0, w = 0, h = 0;
                    if (!Scissor(clip, turn, window, targetWidth, targetHeight, x, y, w, h))
                    {
                        continue;
                    }
                    pass.SetScissor(x, y, w, h);
                    pass.SetBindGroup(0, slot->bindGroup, Span<const u32>{});
                    pass.DrawIndexed(cmd.ElemCount, 1, cmd.IdxOffset + globalIdx, static_cast<i32>(cmd.VtxOffset + globalVtx), 0);
                }
                globalVtx += static_cast<u32>(cmdList->VtxBuffer.Size);
                globalIdx += static_cast<u32>(cmdList->IdxBuffer.Size);
            }
        }

        // Records ImDrawData in a render pass of its own on `target`, loaded and drawn on top of the
        // scene, for an application with no render subsystem whose overlays would draw it. `enc` is
        // the frame's primary encoder; the font upload and buffer fills happen on it before the pass.
        void Render(rhi::CommandEncoder& enc, rhi::TextureView* target, rhi::TextureFormat targetFormat, u32 width, u32 height,
                    const ImDrawData* drawData, u32 frameIndex)
        {
            if (target == nullptr || width == 0 || height == 0)
            {
                return;
            }
            // The projection spans the display size ImGui laid out for, which trails the target by a
            // frame across a resize (it is set at NewFrame); the scissors keep to the target.
            const Float2 size{static_cast<f32>(width), static_cast<f32>(height)};
            const Float2 display = drawData != nullptr && drawData->DisplaySize.x > 0.0f && drawData->DisplaySize.y > 0.0f
                                       ? Float2{drawData->DisplaySize.x, drawData->DisplaySize.y}
                                       : size;
            if (!Prepare(enc, targetFormat, rhi::TextureFormat::Undefined, display, Float4x4::Identity(), drawData, frameIndex))
            {
                return;
            }
            rhi::ColorAttachment ca{};
            ca.view = target;
            ca.loadOp = rhi::LoadOp::Load;
            ca.storeOp = rhi::StoreOp::Store;
            rhi::RenderPassDesc rp{};
            rp.colorAttachments.Add(ca);
            rp.label = u8"imgui";
            rhi::RenderPassEncoder* pass = enc.BeginRenderPass(rp);
            if (pass == nullptr)
            {
                m_prepared = nullptr;
                return;
            }
            Draw(*pass, drawData, ContentRotation::None, display, width, height);
            pass->End();
        }

    private:
        static constexpr u32 kMaxFIF = 4;
        struct FrameSlot
        {
            rhi::Buffer* vtx = nullptr;
            u64 vtxCap = 0;
            rhi::Buffer* idx = nullptr;
            u64 idxCap = 0;
            rhi::Buffer* ubo = nullptr;
            rhi::BindGroup* bindGroup = nullptr;
        };

        bool CreateFontAtlas()
        {
            ImGuiIO& io = ImGui::GetIO();
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            if (pixels == nullptr || w <= 0 || h <= 0)
            {
                return false;
            }
            const u64 bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4u;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = static_cast<u32>(w);
            td.height = static_cast<u32>(h);
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"imgui.font";
            if (!m_device->CreateTexture(td, m_fontTex).IsOk())
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_fontTex, vd, m_fontView).IsOk())
            {
                return false;
            }

            rhi::BufferDesc sd{};
            sd.size = bytes;
            sd.usage = rhi::BufferUsage::CopySrc;
            sd.memory = rhi::MemoryLocation::CpuToGpu;
            sd.label = u8"imgui.fontStaging";
            if (!m_device->CreateBuffer(sd, m_fontStaging).IsOk())
            {
                return false;
            }
            if (void* p = m_fontStaging->Map())
            {
                MemCopy(p, pixels, bytes);
                m_fontStaging->Unmap();
            }
            m_fontW = static_cast<u32>(w);
            m_fontH = static_cast<u32>(h);

            rhi::SamplerDesc ss{};
            ss.minFilter = rhi::FilterMode::Linear;
            ss.magFilter = rhi::FilterMode::Linear;
            ss.mipmapFilter = rhi::MipmapFilterMode::Linear;
            ss.addressU = rhi::AddressMode::ClampToEdge;
            ss.addressV = rhi::AddressMode::ClampToEdge;
            ss.addressW = rhi::AddressMode::ClampToEdge;
            ss.label = u8"imgui.sampler";
            if (!m_device->CreateSampler(ss, m_sampler).IsOk())
            {
                return false;
            }

            io.Fonts->SetTexID(
                static_cast<ImTextureID>(1)); // single font texture; we always bind it
            return true;
        }

        void EnsureFontUploaded(rhi::CommandEncoder& enc)
        {
            if (m_fontUploaded || m_fontTex == nullptr || m_fontStaging == nullptr)
            {
                return;
            }
            enc.TransitionTexture(m_fontTex, rhi::ResourceState::Undefined,
                                  rhi::ResourceState::CopyDst);
            rhi::BufferTextureCopyRegion region{};
            region.bufferOffset = 0;
            region.bytesPerRow = m_fontW * 4u;
            region.rowsPerImage = m_fontH;
            region.textureExtent = rhi::Extent3D{m_fontW, m_fontH, 1};
            enc.CopyBufferToTexture(m_fontStaging, m_fontTex, region);
            enc.TransitionTexture(m_fontTex, rhi::ResourceState::CopyDst,
                                  rhi::ResourceState::ShaderRead);
#if PLATFORM_WEB
            // A submit during web startup can be dropped (the canvas texture expires if the frame
            // yields), which would lose this one-shot copy while the latch says it is done, and
            // ImGui would never be visible. Re-record the small copy for the first frames, the
            // same startup window the IBL environment bake and probe captures use.
            m_fontUploaded = ++m_fontUploadFrames >= 20;
#else
            m_fontUploaded = true;
#endif
        }

        bool UploadGeometry(FrameSlot& slot, const ImDrawData* drawData)
        {
            const u64 vtxBytes = static_cast<u64>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
            const u64 idxBytes = static_cast<u64>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);
            if (vtxBytes == 0 || idxBytes == 0)
            {
                return false;
            }
            if (!EnsureBuffer(slot.vtx, slot.vtxCap, vtxBytes, rhi::BufferUsage::Vertex))
            {
                return false;
            }
            if (!EnsureBuffer(slot.idx, slot.idxCap, idxBytes, rhi::BufferUsage::Index))
            {
                return false;
            }

            auto* vdst = static_cast<u8*>(slot.vtx->Map());
            auto* idst = static_cast<u8*>(slot.idx->Map());
            if (vdst == nullptr || idst == nullptr)
            {
                if (vdst)
                    slot.vtx->Unmap();
                if (idst)
                    slot.idx->Unmap();
                return false;
            }
            u64 vo = 0, io_ = 0;
            for (i32 n = 0; n < drawData->CmdListsCount; ++n)
            {
                const ImDrawList* cl = drawData->CmdLists[n];
                const u64 vb = static_cast<u64>(cl->VtxBuffer.Size) * sizeof(ImDrawVert);
                const u64 ib = static_cast<u64>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx);
                MemCopy(vdst + vo, cl->VtxBuffer.Data, vb);
                vo += vb;
                MemCopy(idst + io_, cl->IdxBuffer.Data, ib);
                io_ += ib;
            }
            slot.vtx->Unmap();
            slot.idx->Unmap();
            return true;
        }

        // Grow a per-slot dynamic buffer to at least `bytes`, with 1.5x headroom, rounded to 64 KiB.
        bool EnsureBuffer(rhi::Buffer*& buf, u64& cap, u64 bytes, rhi::BufferUsage usage)
        {
            if (buf != nullptr && cap >= bytes)
            {
                return true;
            }
            if (buf != nullptr)
            {
                m_device->DestroyBuffer(buf);
                buf = nullptr;
            }
            u64 want = (bytes + bytes / 2u + 0xFFFFu) & ~static_cast<u64>(0xFFFFu);
            rhi::BufferDesc bd{};
            bd.size = want;
            bd.usage = usage;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"imgui.geom";
            if (!m_device->CreateBuffer(bd, buf).IsOk())
            {
                buf = nullptr;
                cap = 0;
                return false;
            }
            cap = want;
            return true;
        }

        // The pipeline for a pass on a `fmt` target with `depthStencilFormat`'s attachment (Undefined
        // for none: a pass of its own); ImGui neither tests nor writes it.
        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat fmt, rhi::TextureFormat depthStencilFormat)
        {
            if (m_pipeline != nullptr && m_pipelineFormat == fmt && m_pipelineDepthStencil == depthStencilFormat)
            {
                return m_pipeline;
            }
            rhi::ShaderModule* vs = m_shaders->GetVariant(u8"imgui", shaders::ShaderStage::Vertex,
                                                          shaders::ShaderFlags::None);
            rhi::ShaderModule* ps = m_shaders->GetVariant(u8"imgui", shaders::ShaderStage::Fragment,
                                                          shaders::ShaderFlags::None);
            if (vs == nullptr || ps == nullptr)
            {
                return nullptr;
            }
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }

            // ImDrawVert is { ImVec2 pos; ImVec2 uv; ImU32 col }: byte offsets 0, 8 and 16, stride 20.
            rhi::VertexAttribute attrs[] = {
                {rhi::VertexFormat::Float32x2, 0, 0},
                {rhi::VertexFormat::Float32x2, 8, 1},
                {rhi::VertexFormat::Unorm8x4, 16, 2},
            };
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(ImDrawVert);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 3};

            rhi::ColorTargetState color{};
            color.format = fmt;
            color.blend = rhi::BlendState::AlphaBlend();
            rhi::FragmentState frag{};
            frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
            frag.targets = Span<const rhi::ColorTargetState>{&color, 1};

            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            if (depthStencilFormat != rhi::TextureFormat::Undefined)
            {
                rhi::DepthStencilState ds{};
                ds.format = depthStencilFormat;
                ds.depthTestEnabled = false;
                ds.depthWriteEnabled = false;
                ds.depthCompare = rhi::CompareFunction::Always;
                pd.depthStencil = ds;
            }
            pd.label = u8"imgui";
            if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
            {
                m_pipeline = nullptr;
                return nullptr;
            }
            m_pipelineFormat = fmt;
            m_pipelineDepthStencil = depthStencilFormat;
            return m_pipeline;
        }

        void Shutdown()
        {
            for (u32 i = 0; i < kMaxFIF; ++i)
            {
                if (m_frames[i].bindGroup)
                {
                    m_device->DestroyBindGroup(m_frames[i].bindGroup);
                    m_frames[i].bindGroup = nullptr;
                }
                if (m_frames[i].ubo)
                {
                    m_device->DestroyBuffer(m_frames[i].ubo);
                    m_frames[i].ubo = nullptr;
                }
                if (m_frames[i].vtx)
                {
                    m_device->DestroyBuffer(m_frames[i].vtx);
                    m_frames[i].vtx = nullptr;
                }
                if (m_frames[i].idx)
                {
                    m_device->DestroyBuffer(m_frames[i].idx);
                    m_frames[i].idx = nullptr;
                }
            }
            if (m_pipeline)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }
            if (m_sampler)
            {
                m_device->DestroySampler(m_sampler);
                m_sampler = nullptr;
            }
            if (m_fontStaging)
            {
                m_device->DestroyBuffer(m_fontStaging);
                m_fontStaging = nullptr;
            }
            if (m_fontView)
            {
                m_device->DestroyTextureView(m_fontView);
                m_fontView = nullptr;
            }
            if (m_fontTex)
            {
                m_device->DestroyTexture(m_fontTex);
                m_fontTex = nullptr;
            }
            if (m_pipelineLayout)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_layout)
            {
                m_device->DestroyBindGroupLayout(m_layout);
                m_layout = nullptr;
            }
        }

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_fif = 2;

        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::TextureFormat m_pipelineFormat = rhi::TextureFormat::Undefined;
        rhi::TextureFormat m_pipelineDepthStencil = rhi::TextureFormat::Undefined;

        rhi::Texture* m_fontTex = nullptr;
        rhi::TextureView* m_fontView = nullptr;
        rhi::Buffer* m_fontStaging = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        u32 m_fontW = 0, m_fontH = 0;
        bool m_fontUploaded = false;
#if PLATFORM_WEB
        u32 m_fontUploadFrames = 0; // web startup re-record window (see EnsureFontUploaded)
#endif

        FrameSlot m_frames[kMaxFIF] = {};
        FrameSlot* m_prepared = nullptr; // what Prepare readied for the next Draw
    };

} // namespace imgui
