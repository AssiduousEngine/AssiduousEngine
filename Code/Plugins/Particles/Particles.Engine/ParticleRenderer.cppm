// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.particles:renderer - the dedicated billboard particle Renderer.
//
// Modeled on SpriteRenderer (SV_VertexID quad, hardware instancing, a per-frame ring and a blended
// forward pass), but with a richer per-instance record and vertex shader: per-particle rotation and
// a velocity-stretched billboard mode. Each DrawItem is a ParticleBillboardRenderData batch (N
// instances), so a whole system draws as one instanced call and the particle count never reaches
// the draw-list sort. Registered with RenderSubsystem through RegisterRenderer (rides Transparent).

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof

export module particles.engine:renderer;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render; // Renderer, RenderRecordContext, ResolvedDraw, DrawItem, DynamicUniformRing, categories
import particles.foundation; // ParticleBlendMode
import :renderdata;
import foundation.materials;

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace render = foundation::render;


export namespace particles
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace render = foundation::render;

    // The particle passes' view (cbuffer ParticleView, particle.vs/.ps and particletrail.vs .hlsl);
    // Integration.ShaderBlocks checks it against the compiled shaders, member by member.
    struct GpuParticleView
    {
        Float4x4 viewProj;
        Float4x4 view;
        // The projection terms that turn a sampled NDC depth back into view depth (soft particles):
        // Proj[2][2], Proj[3][2] and Proj[2][3] (0 for an orthographic projection). The soft
        // distance itself rides each instance (ParticleBillboardInstance::softDistance).
        f32 projZScale = 0;
        f32 projZOffset = 0;
        f32 projWScale = 0;
        f32 _pad0 = 0;
    };
    static_assert(sizeof(GpuParticleView) == 144, "cbuffer ParticleView layout drift");
    static_assert(offsetof(GpuParticleView, projZScale) == 128);
    static_assert(offsetof(GpuParticleView, projZOffset) == 132);
    static_assert(offsetof(GpuParticleView, projWScale) == 136);

    class ParticleRenderer final : public render::Renderer
    {
    public:
        ParticleRenderer(rhi::Device& device, shaders::ShaderSystem& shaders,
                         u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_instanceRing(device, framesInFlight, sizeof(ParticleBillboardInstance),
                             rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                             u8"particle.instances"),
              m_trailRing(device, framesInFlight, sizeof(TrailVertex),
                          rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                          u8"particle.trailverts"),
              m_viewRing(device, framesInFlight, kViewSlotSize, kMinViews, u8"particle.view")
        {
        }
        ~ParticleRenderer() override { Shutdown(); }
        ParticleRenderer(const ParticleRenderer&) = delete;
        ParticleRenderer& operator=(const ParticleRenderer&) = delete;

        core::Status Initialize()
        {
            rhi::BindGroupLayoutEntry viewEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            viewEntry.hasDynamicOffset = true;
            rhi::BindGroupLayoutDesc vld{};
            vld.entries = Span<const rhi::BindGroupLayoutEntry>{&viewEntry, 1};
            if (!m_device->CreateBindGroupLayout(vld, m_viewLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            rhi::BindGroupLayoutEntry texEntries[] = {
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            };
            rhi::BindGroupLayoutDesc tld{};
            tld.entries = Span<const rhi::BindGroupLayoutEntry>{texEntries, 2};
            if (!m_device->CreateBindGroupLayout(tld, m_texLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            // set 2: the opaque scene depth (sampled via Load, no sampler) for soft particles.
            rhi::BindGroupLayoutEntry depthEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            // WebGPU annotation (Vulkan/DX12 ignore it): the depth view read as data.
            depthEntry.textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
            rhi::BindGroupLayoutDesc dld{};
            dld.entries = Span<const rhi::BindGroupLayoutEntry>{&depthEntry, 1};
            if (!m_device->CreateBindGroupLayout(dld, m_depthLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            rhi::BindGroupLayoutHandle layouts[] = {m_viewLayout, m_texLayout, m_depthLayout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 3};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            rhi::SamplerDesc sd{};
            sd.minFilter = rhi::FilterMode::Linear;
            sd.magFilter = rhi::FilterMode::Linear;
            sd.addressU = rhi::AddressMode::ClampToEdge;
            sd.addressV = rhi::AddressMode::ClampToEdge;
            sd.addressW = rhi::AddressMode::ClampToEdge;
            if (!m_device->CreateSampler(sd, m_sampler).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            const u16 indices[6] = {0, 1, 2, 3, 4, 5};
            rhi::BufferDesc ibd{};
            ibd.size = sizeof(indices);
            ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
            ibd.memory = rhi::MemoryLocation::CpuToGpu;
            ibd.label = u8"particle.indices";
            if (!m_device->CreateBuffer(ibd, m_indexBuffer).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            if (void* p = m_device->Map(m_indexBuffer))
            {
                MemCopy(p, indices, sizeof(indices));
                m_device->Unmap(m_indexBuffer);
            }

            // Default texture: a soft radial dot (white RGB, smooth alpha falloff to the edge) so
            // untextured particles read as glowing sparks rather than hard squares. Generated on the CPU
            // and uploaded once. A component can still supply its own atlas to override this.
            constexpr u32 kDot = 64;
            u8 dot[kDot * kDot * 4];
            for (u32 y = 0; y < kDot; ++y)
            {
                for (u32 x = 0; x < kDot; ++x)
                {
                    const f32 fx = (static_cast<f32>(x) + 0.5f) / kDot * 2.0f - 1.0f;
                    const f32 fy = (static_cast<f32>(y) + 0.5f) / kDot * 2.0f - 1.0f;
                    const f32 d = Sqrt(fx * fx + fy * fy); // 0 centre .. 1 edge
                    f32 a = 1.0f - d;
                    a = (a < 0.0f) ? 0.0f : a * a; // squared falloff (soft)
                    const usize i = (static_cast<usize>(y) * kDot + x) * 4;
                    dot[i + 0] = 255;
                    dot[i + 1] = 255;
                    dot[i + 2] = 255;
                    dot[i + 3] = static_cast<u8>(a * 255.0f);
                }
            }
            rhi::TextureDesc wtd{};
            wtd.format = rhi::TextureFormat::RGBA8Unorm;
            wtd.width = kDot;
            wtd.height = kDot;
            wtd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            wtd.label = u8"particle.softdot";
            if (!m_device->CreateTexture(wtd, m_whiteTex).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc wvd{};
            wvd.format = rhi::TextureFormat::RGBA8Unorm;
            wvd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_whiteTex, wvd, m_whiteView).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            if (rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* tb = nullptr;
                if (q->CreateTransferBatch(tb).IsOk() && tb != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = kDot * 4;
                    layout.rowsPerImage = kDot;
                    tb->WriteTexture(m_whiteTex, Span<const u8>{dot, sizeof(dot)}, layout,
                                     rhi::Extent3D{kDot, kDot, 1});
                    (void)tb->Submit();
                    q->DestroyTransferBatch(tb);
                }
            }

            // Trails: a 2-set layout (view and texture, no depth) and a large identity index buffer
            // so the ribbon triangle list draws through DrawIndexed (ResolvedDraw is always
            // indexed).
            rhi::BindGroupLayoutHandle trailLayouts[] = {m_viewLayout, m_texLayout};
            rhi::PipelineLayoutDesc tpld{};
            tpld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{trailLayouts, 2};
            if (!m_device->CreatePipelineLayout(tpld, m_trailPipelineLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::BufferDesc tibd{};
            tibd.size = static_cast<u64>(kTrailMaxIndices) * sizeof(u32);
            tibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
            tibd.memory = rhi::MemoryLocation::CpuToGpu;
            tibd.label = u8"particle.trailindices";
            if (!m_device->CreateBuffer(tibd, m_trailIndexBuffer).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            if (void* p = m_device->Map(m_trailIndexBuffer))
            {
                u32* ix = static_cast<u32*>(p);
                for (u32 k = 0; k < kTrailMaxIndices; ++k)
                {
                    ix[k] = k;
                }
                m_device->Unmap(m_trailIndexBuffer);
            }
            return core::Status{};
        }

        [[nodiscard]] Span<const render::RenderCategory> SupportedCategories() const override
        {
            static const render::RenderCategory cats[] = {render::RenderCategories::Transparent};
            return Span<const render::RenderCategory>{cats, 1};
        }

        void PrepareFrame(u32 maxDraws, u32 frameIndex) override
        {
            // maxDraws is the frame's draw-item count, but a particle batch is one item holding
            // many instances, and every billboard system allocates from this one ring within a
            // frame (the cursor only resets at EndFrame). So size by the sum of instances across
            // all systems in a frame (m_maxInstancesSeen, accumulated in Resolve, finalized in
            // FinishFrame), not the largest single system, or later systems in the sorted draw list
            // fail to allocate and vanish. Rounded up in 8k chunks. A frame that needs more than
            // this grows the ring before its first write (Resolve's MakeRoom).
            const u32 chunk = 8192u;
            const u32 want = Max(maxDraws, ((m_maxInstancesSeen + chunk - 1u) / chunk) * chunk);
            m_instanceRing.Reserve(want == 0 ? 1u : want);
            const u32 twant = ((m_maxTrailVertsSeen + chunk - 1u) / chunk) * chunk;
            m_trailRing.Reserve(twant == 0 ? 1u : twant);
            m_instanceRing.BeginFrame(frameIndex);
            m_trailRing.BeginFrame(frameIndex);
            (void)m_viewRing.BeginFrame(frameIndex); // a slot per view, grown to the views a frame has
            m_frameInstances = 0; // sum of instances allocated this frame (all systems/views)
            m_frameTrailVerts = 0;

            // Last frame's scene-depth bind groups: the device keeps them while that frame runs.
            for (rhi::BindGroupHandle& group : m_depthGroups)
            {
                m_device->DestroyBindGroup(group);
            }
            m_depthGroups.Clear();
        }

        void Resolve(const render::RenderRecordContext& ctx, Span<const render::DrawItem> items,
                     Array<render::ResolvedDraw>& out) override
        {
            if (items.IsEmpty())
            {
                return;
            }
            m_depthFormat = ctx.depthFormat;
            rhi::BindGroupHandle viewBg = EnsureViewBindGroup();
            if (!viewBg.IsValid())
            {
                return;
            }

            // The projection terms that reconstruct view-space depth from a sampled NDC depth
            // (Proj[2][2], Proj[3][2], Proj[2][3]: 0 for an orthographic projection, which the
            // shader linearizes as such).
            const Float4x4 proj =
                (ctx.view != nullptr) ? ctx.view->Camera().projection : Float4x4::Identity();
            GpuParticleView ubo{};
            ubo.viewProj = ctx.viewProj;
            ubo.view = ctx.viewMatrix;
            ubo.projZScale = proj(2, 2);
            ubo.projZOffset = proj(3, 2);
            ubo.projWScale = proj(2, 3);
            // Once a frame per view, however many runs it resolves.
            u32 viewOffset = 0;
            if (!m_viewRing.SlotFor(&ubo, sizeof(ubo), viewOffset))
            {
                return;
            }

            // Scene-depth bind group (set 2) for soft particles. The transparent pass hands us the
            // opaque depth (already DepthStencilRead). Fresh per Resolve, retired after
            // framesInFlight frames.
            rhi::BindGroupHandle depthBg = AcquireDepthBindGroup(
                ctx.sceneDepthView.IsValid() ? ctx.sceneDepthView : m_whiteView);
            if (!depthBg.IsValid())
            {
                return;
            }

            // Room for this view's instances and trail vertices before the first write: the rings
            // are sized from earlier frames, so the frame a scene first shows (or grows) its
            // particles would otherwise lose them while the rings catch up. Only a ring nothing
            // has written this frame can grow (a second view's need waits for the next frame).
            {
                u32 instances = 0;
                u32 trailVerts = 0;
                for (const render::DrawItem& item : items)
                {
                    const auto* b = static_cast<const ParticleRenderDataBase*>(item.data);
                    if (b->particleKind == 1)
                        trailVerts += Min(static_cast<const ParticleTrailRenderData*>(b)->vertexCount, kTrailMaxIndices);
                    else
                        instances += static_cast<const ParticleBillboardRenderData*>(b)->count;
                }
                const u32 chunk = 8192u;
                if (instances > 0)
                    (void)m_instanceRing.MakeRoom(((instances + chunk - 1u) / chunk) * chunk);
                if (trailVerts > 0)
                    (void)m_trailRing.MakeRoom(((trailVerts + chunk - 1u) / chunk) * chunk);
            }

            // Walk the items; dispatch each by kind (both ride the particle rendererId). Trails draw one at
            // a time; consecutive billboards sharing (texture, blend) fuse into one instanced draw.
            usize i = 0;
            while (i < items.Size())
            {
                const auto* base = static_cast<const ParticleRenderDataBase*>(items[i].data);
                if (base->particleKind == 1) // trail ribbon
                {
                    EmitTrailDraw(ctx, static_cast<const ParticleTrailRenderData*>(base), viewBg,
                                  viewOffset, out);
                    ++i;
                    continue;
                }
                const auto* head = static_cast<const ParticleBillboardRenderData*>(base);
                usize j = i + 1;
                u32 total = head->count;
                while (j < items.Size())
                {
                    const auto* nb = static_cast<const ParticleRenderDataBase*>(items[j].data);
                    if (nb->particleKind != 0)
                    {
                        break;
                    }
                    const auto* nd = static_cast<const ParticleBillboardRenderData*>(nb);
                    if (nd->texture != head->texture || nd->blend != head->blend)
                    {
                        break;
                    }
                    total += nd->count;
                    ++j;
                }
                if (total > 0)
                {
                    m_frameInstances +=
                        total; // sum across all systems; sizes the ring next frame
                    const render::DynamicUniformRing::Range ir =
                        m_instanceRing.AllocateRange(total);
                    if (ir.ok)
                    {
                        auto* dst = static_cast<ParticleBillboardInstance*>(ir.ptr);
                        u32 off = 0;
                        for (usize k = i; k < j; ++k)
                        {
                            const auto* b =
                                static_cast<const ParticleBillboardRenderData*>(items[k].data);
                            if (b->count > 0 && b->instances != nullptr)
                            {
                                MemCopy(dst + off, b->instances,
                                        static_cast<usize>(b->count) *
                                            sizeof(ParticleBillboardInstance));
                                off += b->count;
                            }
                        }
                        rhi::RenderPipelineHandle pso = EnsurePipeline(ctx.colorFormat, head->blend);
                        rhi::BindGroupHandle texBg = EnsureTextureBindGroup(
                            head->texture.IsValid() ? head->texture : m_whiteView);
                        if (pso.IsValid() && texBg.IsValid() && off > 0)
                        {
                            render::ResolvedDraw d{};
                            d.pso = pso;
                            d.viewSet = viewBg;
                            d.viewDynamic = true;
                            d.viewOffset = viewOffset;
                            d.drawSet = texBg;
                            d.materialSet = depthBg; // set 2: scene depth (soft particles)
                            d.vertexBuffer0 = m_instanceRing.Buffer();
                            d.vertexOffset0 = ir.byteOffset;
                            d.indexBuffer = m_indexBuffer;
                            d.indexFormat = rhi::IndexFormat::UInt16;
                            d.indexCount = 6;
                            d.instanceCount = off;
                            out.PushBack(d);
                        }
                    }
                }
                i = j;
            }
        }

        void FinishFrame() override
        {
            m_maxInstancesSeen =
                Max(m_maxInstancesSeen, m_frameInstances); // sized to the frame's sum
            m_maxTrailVertsSeen = Max(m_maxTrailVertsSeen, m_frameTrailVerts);
            m_instanceRing.EndFrame();
            m_trailRing.EndFrame();
            m_viewRing.EndFrame();
        }

        /// View slots the ring holds per frame, and those this frame took (tests pin them).
        [[nodiscard]] u32 ViewSlotsPerFrame() const noexcept { return m_viewRing.SlotsPerFrame(); }
        [[nodiscard]] u32 ViewSlotsUsed() const noexcept { return m_viewRing.FrameSlots(); }

    private:
        static constexpr u32 kMinViews = 8; // the view ring's start; it grows to the views a frame has
        static constexpr u64 kViewSlotSize =
            256; // GpuParticleView (2x mat4 + a float4), padded to the dynamic-uniform alignment
        static constexpr u32 kTrailMaxIndices =
            262144u; // identity index buffer cap (largest single trail batch)

        rhi::BindGroupHandle EnsureViewBindGroup()
        {
            const u32 gen = m_viewRing.Generation();
            if (m_viewBg.IsValid() && m_viewBgGen == gen)
            {
                return m_viewBg;
            }
            if (m_viewBg.IsValid())
            {
                m_device->DestroyBindGroup(m_viewBg); // kept by the device while a frame in flight uses it
                m_viewBg = {};
            }
            if (!m_viewRing.Buffer().IsValid())
            {
                return {};
            }
            rhi::BindGroupEntry e =
                rhi::BindGroupEntry::BufferEntry(m_viewRing.Buffer(), 0, kViewSlotSize);
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_viewLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
            if (!m_device->CreateBindGroup(bgd, m_viewBg).IsOk())
            {
                m_viewBg = {};
                return {};
            }
            m_viewBgGen = gen;
            return m_viewBg;
        }

        // A fresh scene-depth bind group over `depth` (set 2). Tracked for deferred destruction so an
        // in-flight frame never references a freed set. Cheap (one bind group per Resolve).
        rhi::BindGroupHandle AcquireDepthBindGroup(rhi::TextureViewHandle depth)
        {
            if (!depth.IsValid())
            {
                return {};
            }
            rhi::BindGroupEntry e = rhi::BindGroupEntry::TextureEntry(depth);
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_depthLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
            rhi::BindGroupHandle bg{};
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return {};
            }
            m_depthGroups.PushBack(bg);
            return bg;
        }

        rhi::BindGroupHandle EnsureTextureBindGroup(rhi::TextureViewHandle tex)
        {
            if (!tex.IsValid())
            {
                return {};
            }
            if (rhi::BindGroupHandle* found = m_texBindGroups.Find(tex))
            {
                return *found;
            }
            rhi::BindGroupEntry ent[] = {rhi::BindGroupEntry::TextureEntry(tex),
                                         rhi::BindGroupEntry::SamplerEntry(m_sampler)};
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_texLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
            rhi::BindGroupHandle bg{};
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return {};
            }
            render::DropGroupsOfDestroyedViews(*m_device, m_texBindGroups);
            m_texBindGroups.InsertOrAssign(tex, bg);
            return bg;
        }

        // The blend state for each particle mode. Additive keeps the particle-appropriate {SrcAlpha, One}
        // (scales the source by its alpha before accumulating) rather than the RHI's plain {One, One}, so
        // an un-premultiplied soft dot still reads as a glow instead of blowing out.
        static rhi::BlendState BlendFor(ParticleBlendMode mode)
        {
            switch (mode)
            {
            case ParticleBlendMode::Additive:
                return {
                    {rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::One, rhi::BlendOperation::Add},
                    {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOperation::Add}};
            case ParticleBlendMode::Premultiplied:
                return rhi::BlendState::PremultipliedAlpha();
            case ParticleBlendMode::Multiply:
                return rhi::BlendState::Multiply();
            case ParticleBlendMode::Alpha:
            default:
                return rhi::BlendState::AlphaBlend();
            }
        }

        static const char8_t* BlendLabel(ParticleBlendMode mode)
        {
            switch (mode)
            {
            case ParticleBlendMode::Additive:
                return u8"particle.additive";
            case ParticleBlendMode::Premultiplied:
                return u8"particle.premultiplied";
            case ParticleBlendMode::Multiply:
                return u8"particle.multiply";
            case ParticleBlendMode::Alpha:
            default:
                return u8"particle.alpha";
            }
        }

        rhi::RenderPipelineHandle EnsurePipeline(rhi::TextureFormat colorFormat, ParticleBlendMode mode)
        {
            Pipelines& p = m_billboard[static_cast<u32>(mode)];
            const u64 shaderVersion = m_shaders->Version(u8"particle"); // hot reload rebuilds
            if (p.pso.IsValid() && p.format == colorFormat && p.shaderVersion == shaderVersion)
            {
                return p.pso;
            }
            if (p.pso.IsValid())
            {
                m_device->DestroyRenderPipeline(p.pso);
                p.pso = {};
            }

            rhi::ShaderModuleHandle vs = m_shaders->GetVariant(
                u8"particle", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            rhi::ShaderModuleHandle ps = m_shaders->GetVariant(
                u8"particle", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            if (!vs.IsValid() || !ps.IsValid())
            {
                if (!m_loggedBillboardPipelineMiss)
                {
                    m_loggedBillboardPipelineMiss = true;
                    rhi::LogErrorf("ParticleRenderer: 'particle' shaders unresolved - billboard "
                                   "particles will not draw");
                }
                return {};
            }

            // Five float4s, each carrying four of ParticleBillboardInstance's named fields (its offset
            // asserts); particle.vs.hlsl unpacks them by name.
            const rhi::VertexAttribute attrs[] = {
                {rhi::VertexFormat::Float32x4, 0, 0},  {rhi::VertexFormat::Float32x4, 16, 1},
                {rhi::VertexFormat::Float32x4, 32, 2}, {rhi::VertexFormat::Float32x4, 48, 3},
                {rhi::VertexFormat::Float32x4, 64, 4},
            };
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(ParticleBillboardInstance);
            vbl.stepMode = rhi::VertexStepMode::Instance;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 5};

            rhi::ColorTargetState target{};
            target.format = colorFormat;
            target.blend = BlendFor(mode);

            rhi::FragmentState frag{};
            frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
            frag.targets = Span<const rhi::ColorTargetState>{&target, 1};

            rhi::DepthStencilState ds{};
            ds.format = m_depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = false;
            ds.depthCompare = rhi::depth::NearerOrEqual();

            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = BlendLabel(mode);
            rhi::RenderPipelineHandle pso{};
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return {};
            }
            p.pso = pso;
            p.format = colorFormat;
            p.shaderVersion = shaderVersion; // never recorded, every frame after a reload rebuilt it
            return pso;
        }

        // Draw one system's trail ribbon: upload its (already camera-facing) vertices to the trail
        // ring and emit a triangle-list draw through the identity index buffer. Reuses the view UBO
        // and a texture.
        void EmitTrailDraw(const render::RenderRecordContext& ctx, const ParticleTrailRenderData* b,
                           rhi::BindGroupHandle viewBg, u32 viewOffset,
                           Array<render::ResolvedDraw>& out)
        {
            if (b->vertexCount == 0 || b->vertices == nullptr)
            {
                return;
            }
            const u32 count = Min(b->vertexCount, kTrailMaxIndices);
            m_frameTrailVerts +=
                count; // sum across all trail systems; sizes the trail ring next frame
            const render::DynamicUniformRing::Range tr = m_trailRing.AllocateRange(count);
            if (!tr.ok)
            {
                return;
            }
            MemCopy(tr.ptr, b->vertices, static_cast<usize>(count) * sizeof(TrailVertex));
            rhi::RenderPipelineHandle pso = EnsureTrailPipeline(ctx.colorFormat, b->blend);
            rhi::BindGroupHandle texBg =
                EnsureTextureBindGroup(b->texture.IsValid() ? b->texture : m_whiteView);
            if (!pso.IsValid() || !texBg.IsValid())
            {
                // Logged, not silent: a missing pipeline (such as a DX12 input-layout refusal over
                // POSITION semantics) otherwise just draws nothing. Once per renderer, not per
                // frame.
                if (!m_loggedTrailPipelineMiss)
                {
                    m_loggedTrailPipelineMiss = true;
                    rhi::LogErrorf("ParticleRenderer: trail pipeline%s unavailable - trails will "
                                   "not draw (see the backend's shader/layout errors above)",
                                   !pso.IsValid() ? "" : "'s texture bind group");
                }
                return;
            }
            render::ResolvedDraw d{};
            d.pso = pso;
            d.viewSet = viewBg;
            d.viewDynamic = true;
            d.viewOffset = viewOffset;
            d.drawSet = texBg;
            d.vertexBuffer0 = m_trailRing.Buffer();
            d.vertexOffset0 = tr.byteOffset;
            d.indexBuffer = m_trailIndexBuffer;
            d.indexFormat = rhi::IndexFormat::UInt32;
            d.indexCount = count;
            d.instanceCount = 1;
            out.PushBack(d);
        }

        rhi::RenderPipelineHandle EnsureTrailPipeline(rhi::TextureFormat colorFormat,
                                                 ParticleBlendMode mode)
        {
            Pipelines& p = m_trail[static_cast<u32>(mode)];
            const u64 shaderVersion = m_shaders->Version(u8"particletrail"); // hot reload rebuilds
            if (p.pso.IsValid() && p.format == colorFormat && p.shaderVersion == shaderVersion)
            {
                return p.pso;
            }
            if (p.pso.IsValid())
            {
                m_device->DestroyRenderPipeline(p.pso);
                p.pso = {};
            }

            rhi::ShaderModuleHandle vs = m_shaders->GetVariant(
                u8"particletrail", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            rhi::ShaderModuleHandle ps = m_shaders->GetVariant(
                u8"particletrail", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            if (!vs.IsValid() || !ps.IsValid())
            {
                return {};
            }

            const rhi::VertexAttribute attrs[] = {
                {rhi::VertexFormat::Float32x3, 0, 0},  // position
                {rhi::VertexFormat::Float32x2, 12, 1}, // texcoord
                {rhi::VertexFormat::Float32x4, 20, 2}, // color
            };
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(TrailVertex);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 3};

            rhi::ColorTargetState target{};
            target.format = colorFormat;
            target.blend = BlendFor(mode);

            rhi::FragmentState frag{};
            frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
            frag.targets = Span<const rhi::ColorTargetState>{&target, 1};
            rhi::DepthStencilState ds{};
            ds.format = m_depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = false;
            ds.depthCompare = rhi::depth::NearerOrEqual();

            rhi::RenderPipelineDesc pd{};
            pd.layout = m_trailPipelineLayout;
            pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = BlendLabel(mode); // shared labels (particle.*), fine for debug naming
            rhi::RenderPipelineHandle pso{};
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return {};
            }
            p.pso = pso;
            p.format = colorFormat;
            p.shaderVersion = shaderVersion; // never recorded, every frame after a reload rebuilt it
            return pso;
        }

        void Shutdown()
        {
            for (auto& kv : m_texBindGroups)
            {
                if (kv.value.IsValid())
                {
                    m_device->DestroyBindGroup(kv.value);
                }
            }
            m_texBindGroups.Clear();
            if (m_viewBg.IsValid())
            {
                m_device->DestroyBindGroup(m_viewBg);
                m_viewBg = {};
            }
            for (Pipelines& p : m_billboard)
            {
                if (p.pso.IsValid())
                {
                    m_device->DestroyRenderPipeline(p.pso);
                    p.pso = {};
                }
            }
            for (Pipelines& p : m_trail)
            {
                if (p.pso.IsValid())
                {
                    m_device->DestroyRenderPipeline(p.pso);
                    p.pso = {};
                }
            }
            if (m_trailIndexBuffer.IsValid())
            {
                m_device->DestroyBuffer(m_trailIndexBuffer);
                m_trailIndexBuffer = {};
            }
            if (m_trailPipelineLayout.IsValid())
            {
                m_device->DestroyPipelineLayout(m_trailPipelineLayout);
                m_trailPipelineLayout = {};
            }
            if (m_indexBuffer.IsValid())
            {
                m_device->DestroyBuffer(m_indexBuffer);
                m_indexBuffer = {};
            }
            if (m_whiteView.IsValid())
            {
                m_device->DestroyTextureView(m_whiteView);
                m_whiteView = {};
            }
            if (m_whiteTex.IsValid())
            {
                m_device->DestroyTexture(m_whiteTex);
                m_whiteTex = {};
            }
            if (m_sampler.IsValid())
            {
                m_device->DestroySampler(m_sampler);
                m_sampler = {};
            }
            for (rhi::BindGroupHandle& group : m_depthGroups)
            {
                m_device->DestroyBindGroup(group);
            }
            m_depthGroups.Clear();
            if (m_pipelineLayout.IsValid())
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = {};
            }
            if (m_depthLayout.IsValid())
            {
                m_device->DestroyBindGroupLayout(m_depthLayout);
                m_depthLayout = {};
            }
            if (m_texLayout.IsValid())
            {
                m_device->DestroyBindGroupLayout(m_texLayout);
                m_texLayout = {};
            }
            if (m_viewLayout.IsValid())
            {
                m_device->DestroyBindGroupLayout(m_viewLayout);
                m_viewLayout = {};
            }
        }

        struct Pipelines
        {
            rhi::RenderPipelineHandle pso{};
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        render::DynamicUniformRing m_instanceRing;
        render::DynamicUniformRing m_trailRing; // trail ribbon vertices (per-frame)
        render::ViewUniformRing m_viewRing;
        rhi::BindGroupLayoutHandle m_viewLayout{};
        rhi::BindGroupLayoutHandle m_texLayout{};
        rhi::BindGroupLayoutHandle m_depthLayout{}; // set 2: scene depth (soft particles)
        Array<rhi::BindGroupHandle> m_depthGroups; // this frame's scene-depth bind groups
        rhi::PipelineLayoutHandle m_pipelineLayout{};
        rhi::SamplerHandle m_sampler{};
        rhi::BufferHandle m_indexBuffer{};
        rhi::TextureHandle m_whiteTex{}; // 1x1 white default (untextured particles)
        rhi::TextureViewHandle m_whiteView{};
        bool m_loggedTrailPipelineMiss = false;     // once-per-renderer diagnostics
        bool m_loggedBillboardPipelineMiss = false;
        rhi::BindGroupHandle m_viewBg{};
        u32 m_viewBgGen = 0;
        // Keyed by view handle, as SpriteRenderer's (a new view never takes an old one's handle).
        // A destroyed view's group goes when the next group is made (DropGroupsOfDestroyedViews).
        HashMap<rhi::TextureViewHandle, rhi::BindGroupHandle> m_texBindGroups;
        Pipelines
            m_billboard[4]; // one per ParticleBlendMode (Alpha/Additive/Premultiplied/Multiply)
        rhi::BufferHandle m_trailIndexBuffer{}; // identity indices [0,1,2,...] for trail draws
        rhi::PipelineLayoutHandle m_trailPipelineLayout{}; // view + texture (no depth set)
        Pipelines m_trail[4]; // trail ribbon PSOs, one per blend mode
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Undefined;
        u32 m_maxInstancesSeen = 0;  // sizes the instance ring (see PrepareFrame)
        u32 m_frameInstances = 0;    // running sum of instances this frame
        u32 m_frameTrailVerts = 0;   // running sum of trail verts this frame
        u32 m_maxTrailVertsSeen = 0; // sizes the trail vertex ring
    };
}
