// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:device - Device over WGPUDevice.
///
/// Handles device and queue lifecycle (creation, loss latch, WaitIdle, fences,
/// destruction). A resource, pipeline or command factory that WebGPU does not support
/// returns NotSupported: callers get failures, never silent fakes.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:device;

import foundation.core;
import foundation.rhi;
import :api;
import :blit_helper;
import :buffer;
import :texture;
import :texture_view;
import :sampler;
import :shader_module;
import :bind_group_layout;
import :bind_group;
import :pipeline_layout;
import :pipeline_cache;
import :render_pipeline;
import :compute_pipeline;
import :query_set;
import :command_pool;
import :surface;
import :swapchain;
import :fence;
import :queue;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuDevice final : public Device
    {
    public:
        WebGpuDevice(const WebGpuApi& api, WGPUInstance instance, WGPUAdapter adapter,
                     WGPUDevice device, IAllocator& allocator)
            : m_api(&api), m_instance(instance), m_adapter(adapter), m_device(device),
              m_allocator(allocator)
        {
            type = DeviceType::WebGPU;
            const WGPUQueue queue = m_api->wgpuDeviceGetQueue(m_device);
            // One WebGPU queue, three RHI-typed views of it (see :queue).
            m_graphicsQueue.Initialize(api, instance, device, queue, allocator,
                                       QueueType::Graphics, m_bufferRegistry);
            m_computeQueue.Initialize(api, instance, device, queue, allocator,
                                      QueueType::Compute, m_bufferRegistry);
            m_transferQueue.Initialize(api, instance, device, queue, allocator,
                                       QueueType::Transfer, m_bufferRegistry);
            m_graphicsQueue.counters = &m_counters;
            m_graphicsQueue.tables = &m_tables;
            m_computeQueue.tables = &m_tables;
            m_transferQueue.tables = &m_tables;
            m_computeQueue.counters = &m_counters;
            m_transferQueue.counters = &m_counters;
            m_blitHelper.Initialize(api, device);
            // Debug: force the full browser shader path on desktop wgpu-native (see
            // PreferredShaderFormat), both WGSL text and push-constant emulation. The WGSL cook
            // always emulates push constants (browsers reject var<push_constant>), so the layout
            // must emulate too or the emulated @group(N) @binding(0) block mismatches a
            // native-immediate layout.
            m_forceWgsl = GetEnvironmentVariable(u8"ENV_WEBGPU_WGSL").HasValue();
            if (m_forceWgsl)
            {
                m_forceUniformPushConstants = true;
            }
        }

        [[nodiscard]] WebGpuBlitHelper& BlitHelper() { return m_blitHelper; }

        /// The device-lost callback (registered at creation by the adapter) lands here.
        void MarkLost() { m_lost = true; }

        /// Take ownership of the adapter's heap-allocated lost-callback route (the userdata the
        /// wgpu device-lost callback dereferences on a real loss). Freed in Destroy after the
        /// wgpu device is released and pending callbacks are flushed; see the ordering note
        /// there. A trivially-destructible blob: freed with the raw allocator, no destructor.
        void AdoptLostRoute(void* route) noexcept { m_lostRoute = route; }

        /// Set by the adapter: whether the device carries the Immediates feature
        /// (push-constant support; pipeline layouts with ranges fail without it).
        void SetImmediatesSupported(bool supported) { m_immediatesSupported = supported; }

        /// Force the uniform-buffer push-constant fallback even where immediates exist.
        /// Web always emulates (no immediates in Dawn); this lets desktop tests exercise the
        /// same path against a real GPU. See WebGpuPipelineLayout and the pass encoders.
        void SetForceUniformPushConstants(bool force) { m_forceUniformPushConstants = force; }

        /// Whether push constants are emulated as a bound uniform buffer on this device
        /// (no immediates, or the fallback forced) rather than issued via SetImmediates.
        [[nodiscard]] bool EmulatesPushConstants() const
        {
            return m_forceUniformPushConstants || !m_immediatesSupported;
        }

        [[nodiscard]] WGPUDevice Handle() const { return m_device; }
        [[nodiscard]] WGPUInstance Instance() const { return m_instance; }
        [[nodiscard]] const WebGpuApi& Api() const { return *m_api; }

        // SPIR-V ingestion is a native wgpu-native feature (the ShaderSourceSPIRV instance
        // feature); a browser never exposes it, so there the cook must feed WGSL text instead.
        // Debug override: ENV_WEBGPU_WGSL forces the WGSL path on desktop wgpu-native, so the
        // desktop --webgpu run exercises the browser's shaders (WGSL, naga frontend) instead of
        // SPIR-V ingestion: a fast repro for web-render bugs. Requires a WGSL shaders.dpak.
        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            if (m_forceWgsl)
            {
                return ShaderFormat::WGSL;
            }
            return m_api->spirvIngestion ? ShaderFormat::SpirV : ShaderFormat::WGSL;
        }

        // False: WebGPU raster orientation matches Vulkan's for both shader paths, as the probes in
        // Render.Backend.Tests show. wgpu's runtime SPIR-V frontend applies no clip-space
        // adjustment, and the WGSL cook passes naga --keep-coordinate-space so the cooked WGSL
        // carries the same convention. No renderer pass needs a Y compensation on this backend.
        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override { return false; }

        DeviceCounters Counters() const noexcept override { return m_counters.Snapshot(); }
        // WebGPU does not report its memory budget.
        bool MemoryBudget(u64&, u64&) const override { return false; }

        // No pipeline statistics on WebGPU: a statistics result takes no room and reads as zeros.
        [[nodiscard]] u32 QueryResultSize(QueryType queryType) const noexcept override
        {
            return queryType == QueryType::PipelineStatistics ? 0u : 8u;
        }
        [[nodiscard]] PipelineStatistics DecodePipelineStatistics(const void*) const noexcept override
        {
            return PipelineStatistics{};
        }

        // ---- Queries ----
        Queue* GetQueue(QueueType queueType, u32 index) override
        {
            if (index != 0)
            {
                return nullptr;
            }
            switch (queueType)
            {
            case QueueType::Graphics:
                return &m_graphicsQueue;
            case QueueType::Compute:
                return &m_computeQueue;
            case QueueType::Transfer:
                return &m_transferQueue;
            }
            return nullptr;
        }

        u32 GetQueueCount(QueueType) override { return 1; }

        u32 MaxColorDepthSampleCount() const noexcept override
        {
            // WebGPU guarantees 4x for the color + depth formats the renderer uses;
            // 8x is not a WebGPU capability. So the engine ceiling (4) is always available on web.
            return 4;
        }

        bool SupportsSampleCount(u32 count) const noexcept override
        {
            // WebGPU guarantees only {1, 4} for the renderer's color and depth formats. 2x is not
            // guaranteed (it needs TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES, which we do not
            // enable), so creating a 2-sample texture or pipeline aborts the device; callers must
            // snap 2x to 1x here.
            return count == 1 || count == 4;
        }

        FormatSupport GetFormatSupport(TextureFormat format) override
        {
            // WebGPU's per-format capabilities are spec tables, not driver queries, so this
            // encodes the classes the renderer asks about.
            switch (format)
            {
            case TextureFormat::Undefined:
                return FormatSupport::Unsupported;

            // Depth/stencil family.
            case TextureFormat::Depth16Unorm:
            case TextureFormat::Depth24Plus:
            case TextureFormat::Depth24PlusStencil8:
            case TextureFormat::Depth32Float:
            case TextureFormat::Depth32FloatStencil8:
            case TextureFormat::Stencil8:
                return FormatSupport::Texture | FormatSupport::DepthStencil;

            // BC block formats: sampled-only, feature-gated.
            case TextureFormat::BC1RGBAUnorm:
            case TextureFormat::BC1RGBAUnormSrgb:
            case TextureFormat::BC2RGBAUnorm:
            case TextureFormat::BC2RGBAUnormSrgb:
            case TextureFormat::BC3RGBAUnorm:
            case TextureFormat::BC3RGBAUnormSrgb:
            case TextureFormat::BC4RUnorm:
            case TextureFormat::BC4RSnorm:
            case TextureFormat::BC5RGUnorm:
            case TextureFormat::BC5RGSnorm:
            case TextureFormat::BC6HRGBUfloat:
            case TextureFormat::BC6HRGBFloat:
            case TextureFormat::BC7RGBAUnorm:
            case TextureFormat::BC7RGBAUnormSrgb:
                return features.textureCompressionBC
                           ? FormatSupport::Texture | FormatSupport::LinearFilter
                           : FormatSupport::Unsupported;

            // ASTC block formats (mobile browsers): sampled-only, feature-gated.
            case TextureFormat::ASTC4x4Unorm:
            case TextureFormat::ASTC4x4UnormSrgb:
            case TextureFormat::ASTC5x5Unorm:
            case TextureFormat::ASTC5x5UnormSrgb:
            case TextureFormat::ASTC6x6Unorm:
            case TextureFormat::ASTC6x6UnormSrgb:
            case TextureFormat::ASTC8x8Unorm:
            case TextureFormat::ASTC8x8UnormSrgb:
                return features.textureCompressionASTC
                           ? FormatSupport::Texture | FormatSupport::LinearFilter
                           : FormatSupport::Unsupported;

            // 16-bit norm formats have no core WebGPU equivalent at all.
            case TextureFormat::RGBA16Unorm:
            case TextureFormat::RGBA16Snorm:
                return FormatSupport::Unsupported;

            // 32-bit float family: renderable and storage, not filterable (core).
            case TextureFormat::R32Float:
            case TextureFormat::RG32Float:
            case TextureFormat::RGBA32Float:
                return FormatSupport::Texture | FormatSupport::ColorAttachment |
                       FormatSupport::StorageTexture;

            // Integer formats: renderable, never blendable or filterable.
            case TextureFormat::R8Uint:
            case TextureFormat::R8Sint:
            case TextureFormat::R16Uint:
            case TextureFormat::R16Sint:
            case TextureFormat::R32Uint:
            case TextureFormat::R32Sint:
            case TextureFormat::RG8Uint:
            case TextureFormat::RG8Sint:
            case TextureFormat::RG16Uint:
            case TextureFormat::RG16Sint:
            case TextureFormat::RG32Uint:
            case TextureFormat::RG32Sint:
            case TextureFormat::RGBA8Uint:
            case TextureFormat::RGBA8Sint:
            case TextureFormat::RGBA16Uint:
            case TextureFormat::RGBA16Sint:
            case TextureFormat::RGBA32Uint:
            case TextureFormat::RGBA32Sint:
            case TextureFormat::RGB10A2Uint:
                return FormatSupport::Texture | FormatSupport::ColorAttachment;

            // Snorm, shared-exponent and packed-float formats: sampled and filterable.
            // RG11B10 is also renderable via the RG11B10UfloatRenderable feature on
            // some runtimes; it is conservatively sampled-only here.
            case TextureFormat::R8Snorm:
            case TextureFormat::RG8Snorm:
            case TextureFormat::RGBA8Snorm:
            case TextureFormat::RGB9E5Float:
            case TextureFormat::RG11B10Float:
                return FormatSupport::Texture | FormatSupport::LinearFilter;

            // Everything else in the RHI list is the classic filterable +
            // renderable + blendable color family.
            default:
                return FormatSupport::Texture | FormatSupport::ColorAttachment |
                       FormatSupport::BlendableColor | FormatSupport::LinearFilter;
            }
        }

        // ---- Resource creation ----
        Status CreateBuffer(const BufferDesc& bufferDesc, BufferHandle& out) override
        {
            out = BufferHandle::Invalid();
            Buffer* made = nullptr;
            const Status status = CreateResource<WebGpuBuffer>(
                made, [&](WebGpuBuffer& b)
                { return b.Initialize(*m_api, m_instance, m_device,
                                      m_graphicsQueue.Handle(), bufferDesc); });
            if (!status.IsOk())
                return status;
            auto* buffer = static_cast<WebGpuBuffer*>(made);
            out = m_tables.buffers.Add(buffer, bufferDesc.label);
            if (!out)
            {
                buffer->Release();
                m_allocator.Delete(buffer);
                return ErrorCode::OutOfMemory;
            }
            m_bufferRegistry.Add(buffer);
            m_counters.BufferMade(bufferDesc.size);
            return ErrorCode::Ok;
        }
        /// The backend's own buffer a handle names, or null: for tests that read its state (its
        /// shadow's uploads).
        [[nodiscard]] WebGpuBuffer* BufferOf(BufferHandle buffer) const noexcept { return m_tables.buffers.Get(buffer); }
        const BufferDesc* Describe(BufferHandle buffer) const override
        {
            const WebGpuBuffer* b = m_tables.buffers.Get(buffer);
            return b != nullptr ? &b->desc : nullptr;
        }
        void* Map(BufferHandle buffer) override
        {
            WebGpuBuffer* b = m_tables.buffers.Get(buffer);
            return b != nullptr ? b->Map() : nullptr;
        }
        void Unmap(BufferHandle buffer) override
        {
            if (WebGpuBuffer* b = m_tables.buffers.Get(buffer))
                b->Unmap();
        }
        void FlushRange(BufferHandle buffer, u64 offset, u64 size) override
        {
            if (WebGpuBuffer* b = m_tables.buffers.Get(buffer))
                b->FlushRange(offset, size);
        }
        Status CreateTexture(const TextureDesc& textureDesc, TextureHandle& out) override
        {
            const Status status = CreateInTable<WebGpuTexture>(
                m_tables.textures, out, textureDesc.label, [&](WebGpuTexture& t) { return t.Initialize(*m_api, m_device, textureDesc); });
            if (status.IsOk())
            {
                // WebGPU does not say what it allocated: the description's size.
                m_counters.TextureMade(EstimateTextureBytes(textureDesc), CountsAsRenderTarget(textureDesc));
            }
            return status;
        }
        Status CreateTextureView(TextureHandle texture, const TextureViewDesc& viewDesc,
                                 TextureViewHandle& out) override
        {
            const WebGpuTexture* owner = m_tables.textures.Get(texture);
            if (owner == nullptr)
            {
                out = TextureViewHandle::Invalid();
                return ErrorCode::InvalidArgument;
            }
            return CreateInTable<WebGpuTextureView>(
                m_tables.textureViews, out, viewDesc.label, [&](WebGpuTextureView& v) { return v.Initialize(*m_api, owner, texture, viewDesc); });
        }
        Status CreateSampler(const SamplerDesc& samplerDesc, SamplerHandle& out) override
        {
            return CreateInTable<WebGpuSampler>(
                m_tables.samplers, out, samplerDesc.label, [&](WebGpuSampler& smp) { return smp.Initialize(*m_api, m_device, samplerDesc); });
        }
        const TextureDesc* Describe(TextureHandle texture) const override
        {
            const WebGpuTexture* t = m_tables.textures.Get(texture);
            return t != nullptr ? &t->desc : nullptr;
        }
        const TextureViewDesc* Describe(TextureViewHandle view) const override
        {
            const WebGpuTextureView* v = m_tables.textureViews.Get(view);
            return v != nullptr ? &v->desc : nullptr;
        }
        StringView Label(BufferHandle handle) const override { return m_tables.buffers.Label(handle); }
        StringView Label(TextureHandle handle) const override { return m_tables.textures.Label(handle); }
        StringView Label(TextureViewHandle handle) const override { return m_tables.textureViews.Label(handle); }
        StringView Label(SamplerHandle handle) const override { return m_tables.samplers.Label(handle); }
        StringView Label(ShaderModuleHandle handle) const override { return m_tables.shaderModules.Label(handle); }
        StringView Label(BindGroupLayoutHandle handle) const override { return m_tables.bindGroupLayouts.Label(handle); }
        StringView Label(BindGroupHandle handle) const override { return m_tables.bindGroups.Label(handle); }
        StringView Label(PipelineLayoutHandle handle) const override { return m_tables.pipelineLayouts.Label(handle); }
        StringView Label(PipelineCacheHandle handle) const override { return m_tables.pipelineCaches.Label(handle); }
        StringView Label(RenderPipelineHandle handle) const override { return m_tables.renderPipelines.Label(handle); }
        StringView Label(ComputePipelineHandle handle) const override { return m_tables.computePipelines.Label(handle); }
        StringView Label(MeshPipelineHandle handle) const override { return m_tables.meshPipelines.Label(handle); }
        StringView Label(QuerySetHandle handle) const override { return m_tables.querySets.Label(handle); }
        StringView Label(AccelStructHandle handle) const override { return m_tables.accelStructs.Label(handle); }
        StringView Label(RayTracingPipelineHandle handle) const override { return m_tables.rayTracingPipelines.Label(handle); }
        bool IsLive(TextureViewHandle view) const override { return m_tables.textureViews.Find(view) != nullptr; }
        TextureHandle TextureOf(TextureViewHandle view) const override
        {
            const WebGpuTextureView* v = m_tables.textureViews.Get(view);
            return v != nullptr ? v->texture : TextureHandle::Invalid();
        }
        const SamplerDesc* Describe(SamplerHandle sampler) const override
        {
            const WebGpuSampler* smp = m_tables.samplers.Get(sampler);
            return smp != nullptr ? &smp->desc : nullptr;
        }
        Status CreateShaderModule(const ShaderModuleDesc& moduleDesc, ShaderModuleHandle& out) override
        {
            return CreateInTable<WebGpuShaderModule>(
                m_tables.shaderModules, out, moduleDesc.label,
                [&](WebGpuShaderModule& m)
                {
                    m.entryPoint = String(moduleDesc.entryPoint);
                    return m.Initialize(*m_api, m_device, moduleDesc);
                });
        }
        Status CreateBindGroupLayout(const BindGroupLayoutDesc& layoutDesc,
                                     BindGroupLayoutHandle& out) override
        {
            return CreateInTable<WebGpuBindGroupLayout>(
                m_tables.bindGroupLayouts, out, layoutDesc.label, [&](WebGpuBindGroupLayout& l)
                { return l.Initialize(*m_api, m_device, layoutDesc); });
        }
        Status CreateBindGroup(const BindGroupDesc& groupDesc, BindGroupHandle& out) override
        {
            return CreateInTable<WebGpuBindGroup>(
                m_tables.bindGroups, out, groupDesc.label,
                [&](WebGpuBindGroup& g)
                {
                    g.tables = &m_tables;
                    g.layout = groupDesc.layout;
                    return g.Initialize(*m_api, m_device, groupDesc);
                });
        }
        Status CreatePipelineLayout(const PipelineLayoutDesc& layoutDesc,
                                    PipelineLayoutHandle& out) override
        {
            return CreateInTable<WebGpuPipelineLayout>(
                m_tables.pipelineLayouts, out, layoutDesc.label, [&](WebGpuPipelineLayout& l)
                { return l.Initialize(*m_api, m_device, &m_tables, layoutDesc, EmulatesPushConstants()); });
        }
        Status CreatePipelineCache(const PipelineCacheDesc& cacheDesc, PipelineCacheHandle& out) override
        {
            // No cache object in WebGPU; an empty stand-in serves callers.
            return CreateInTable<WebGpuPipelineCache>(m_tables.pipelineCaches, out, cacheDesc.label,
                                                      [](WebGpuPipelineCache&) { return Status(ErrorCode::Ok); });
        }
        StringView EntryPointOf(ShaderModuleHandle module) const override
        {
            const WebGpuShaderModule* m = m_tables.shaderModules.Get(module);
            return m != nullptr ? m->entryPoint.AsView() : StringView{};
        }
        Span<const BindGroupLayoutEntry> LayoutEntries(BindGroupLayoutHandle layout) const override
        {
            const WebGpuBindGroupLayout* l = m_tables.bindGroupLayouts.Get(layout);
            return l != nullptr ? l->Entries() : Span<const BindGroupLayoutEntry>{};
        }
        BindGroupLayoutHandle LayoutOf(BindGroupHandle group) const override
        {
            const WebGpuBindGroup* g = m_tables.bindGroups.Get(group);
            return g != nullptr ? g->layout : BindGroupLayoutHandle::Invalid();
        }
        void UpdateBindless(BindGroupHandle group, Span<const BindlessUpdateEntry> entries) override
        {
            if (WebGpuBindGroup* g = m_tables.bindGroups.Get(group))
                g->UpdateBindless(entries);
        }
        u32 PipelineCacheDataSize(PipelineCacheHandle) override { return 0; }
        Status PipelineCacheData(PipelineCacheHandle cache, Span<u8>) override
        {
            return m_tables.pipelineCaches.Get(cache) != nullptr ? Status(ErrorCode::Ok)
                                                                 : Status(ErrorCode::InvalidArgument);
        }
        const QuerySetDesc* Describe(QuerySetHandle querySet) const override
        {
            const WebGpuQuerySet* q = m_tables.querySets.Get(querySet);
            return q != nullptr ? &q->desc : nullptr;
        }
        u64 DeviceAddress(AccelStructHandle) const override { return 0; } // no ray tracing
        Status CreateRenderPipeline(const RenderPipelineDesc& pipelineDesc,
                                    RenderPipelineHandle& out) override
        {
            return CreateInTable<WebGpuRenderPipeline>(
                m_tables.renderPipelines, out, pipelineDesc.label, [&](WebGpuRenderPipeline& p)
                { return p.Initialize(*m_api, m_device, &m_tables, pipelineDesc); });
        }
        Status CreateComputePipeline(const ComputePipelineDesc& pipelineDesc,
                                     ComputePipelineHandle& out) override
        {
            return CreateInTable<WebGpuComputePipeline>(
                m_tables.computePipelines, out, pipelineDesc.label, [&](WebGpuComputePipeline& p)
                { return p.Initialize(*m_api, m_device, &m_tables, pipelineDesc); });
        }
        Status CreateCommandPool(QueueType, CommandPool*& out) override
        {
            // All queue types funnel into the one WebGPU queue; pools are bookkeeping.
            auto* pool = m_allocator.New<WebGpuCommandPool>();
            pool->Initialize(*m_api, m_device, m_allocator, m_blitHelper);
            pool->counters = &m_counters;
            pool->tables = &m_tables;
            out = pool;
            return ErrorCode::Ok;
        }
        Status CreateFence(u64 initialValue, Fence*& out) override
        {
            out = m_allocator.New<WebGpuFence>(*m_api, m_instance, m_device, initialValue);
            return ErrorCode::Ok;
        }
        Status CreateQuerySet(const QuerySetDesc& setDesc, QuerySetHandle& out) override
        {
            return CreateInTable<WebGpuQuerySet>(
                m_tables.querySets, out, setDesc.label, [&](WebGpuQuerySet& q) { return q.Initialize(*m_api, m_device, setDesc); });
        }
        Status CreateSwapChain(Surface* surface, const SwapChainDesc& swapDesc,
                               SwapChain*& out) override
        {
            if (surface == nullptr)
            {
                out = nullptr;
                return ErrorCode::InvalidArgument;
            }
            return CreateResource<WebGpuSwapChain>(
                out, [&](WebGpuSwapChain& sc)
                { return sc.Initialize(*m_api, m_adapter, m_device, &m_graphicsQueue,
                                       static_cast<WebGpuSurface*>(surface), swapDesc, m_tables); });
        }

        // ---- Resource destruction ----
        // Every path tolerates the nullptr a failed Create* handed out.
        // WebGPU frees by releasing a reference: what an encoder recorded and what a submission
        // uses hold their own, so a destroyed buffer goes at once, nothing recorded or submitted
        // losing it (Specs/rhi-handles.md, Decision 4).
        void DestroyBuffer(BufferHandle& x) override
        {
            if (m_tables.buffers.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyTexture(TextureHandle& x) override
        {
            if (m_tables.textures.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyTextureView(TextureViewHandle& x) override
        {
            if (m_tables.textureViews.Retire(x))
                m_tracker.Collect({});
        }
        void DestroySampler(SamplerHandle& x) override
        {
            if (m_tables.samplers.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyShaderModule(ShaderModuleHandle& x) override
        {
            if (m_tables.shaderModules.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyBindGroupLayout(BindGroupLayoutHandle& x) override
        {
            if (m_tables.bindGroupLayouts.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyBindGroup(BindGroupHandle& x) override
        {
            if (m_tables.bindGroups.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyPipelineLayout(PipelineLayoutHandle& x) override
        {
            if (m_tables.pipelineLayouts.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyPipelineCache(PipelineCacheHandle& x) override
        {
            if (m_tables.pipelineCaches.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyRenderPipeline(RenderPipelineHandle& x) override
        {
            if (m_tables.renderPipelines.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyComputePipeline(ComputePipelineHandle& x) override
        {
            if (m_tables.computePipelines.Retire(x))
                m_tracker.Collect({});
        }
        void DestroyCommandPool(CommandPool*& x) override
        {
            if (x != nullptr)
            {
                auto* pool = static_cast<WebGpuCommandPool*>(x);
                m_allocator.Delete(pool);
                x = nullptr;
            }
        }
        void DestroyFence(Fence*& x) override { DeleteIfAny(x); }
        void DestroyQuerySet(QuerySetHandle& x) override
        {
            if (m_tables.querySets.Retire(x))
                m_tracker.Collect({});
        }
        void DestroySwapChain(SwapChain*& x) override
        {
            if (x != nullptr)
            {
                auto* swapChain = static_cast<WebGpuSwapChain*>(x);
                swapChain->Cleanup();
                m_allocator.Delete(swapChain);
                x = nullptr;
            }
        }
        void DestroySurface(Surface*& x) override { ReleaseAndDelete<WebGpuSurface>(x); }

        // ---- Lifecycle ----
        bool IsLost() override { return m_lost; }

        // No mesh shaders and no ray tracing in WebGPU.
        Status CreateMeshPipeline(const MeshPipelineDesc&, MeshPipelineHandle& out) override
        {
            out = {};
            return ErrorCode::NotSupported;
        }
        void DestroyMeshPipeline(MeshPipelineHandle& pipeline) override { pipeline = {}; }
        Status CreateAccelStruct(const AccelStructDesc&, AccelStructHandle& out) override
        {
            out = {};
            return ErrorCode::NotSupported;
        }
        void DestroyAccelStruct(AccelStructHandle& accelStruct) override { accelStruct = {}; }
        Status CreateRayTracingPipeline(const RayTracingPipelineDesc&, RayTracingPipelineHandle& out) override
        {
            out = {};
            return ErrorCode::NotSupported;
        }
        void DestroyRayTracingPipeline(RayTracingPipelineHandle& pipeline) override { pipeline = {}; }
        Status GetShaderGroupHandles(RayTracingPipelineHandle, u32, u32, Span<u8>) override
        {
            return ErrorCode::NotSupported;
        }

        void WaitIdle() override
        {
            // wgpu-native's DevicePoll(wait) drains the queue; on web (no poll
            // extension) the queue wrapper's callback pump does the same job.
            if (m_api->wgpuDevicePoll != nullptr)
            {
                (void)m_api->wgpuDevicePoll(m_device, 1u, nullptr);
            }
            else
            {
                m_graphicsQueue.WaitIdle();
            }
        }

        void Destroy() override
        {
            // Freed while the WebGPU device still lives (the members' destructors run after).
            m_tables.FreeAll();
            m_tracker.FreeAll();
            m_blitHelper.Release();
            m_api->wgpuQueueRelease(m_graphicsQueue.Handle());
            m_api->wgpuDeviceRelease(m_device);
            // Free the lost-callback route only after the device is released and its pending
            // callbacks are delivered: an orderly release fires the lost callback with reason
            // Destroyed (which never dereferences the route), and a real loss queued before this
            // point is flushed by the pump below. After that no callback can target this device, so
            // the route cannot be read again.
            if (m_lostRoute != nullptr)
            {
                m_api->wgpuInstanceProcessEvents(m_instance);
                m_allocator.Free(m_lostRoute);
                m_lostRoute = nullptr;
            }
            IAllocator& allocator = m_allocator;
            this->~WebGpuDevice();
            allocator.Free(this);
        }

    private:
        /// Allocate, run the init closure, roll back on failure; every Create* goes through it.
        template <typename TResource, typename TBase, typename TInit>
        Status CreateResource(TBase*& out, TInit&& initialize)
        {
            out = nullptr;
            auto* resource = m_allocator.New<TResource>();
            const Status status = initialize(*resource);
            if (!status.IsOk())
            {
                m_allocator.Delete(resource);
                return status;
            }
            out = resource;
            return ErrorCode::Ok;
        }

        /// As CreateResource, into `table`: `out` is the new object's handle, invalid on failure.
        template <typename TResource, typename TTable, typename THandle, typename TInit>
        Status CreateInTable(TTable& table, THandle& out, StringView label, TInit&& initialize)
        {
            out = THandle::Invalid();
            auto* resource = m_allocator.New<TResource>();
            const Status status = initialize(*resource);
            if (!status.IsOk())
            {
                FreeObject<TResource>(this, resource);
                return status;
            }
            out = table.Add(resource, label);
            if (!out)
            {
                FreeObject<TResource>(this, resource);
                return ErrorCode::OutOfMemory;
            }
            return ErrorCode::Ok;
        }

        template <typename TResource, typename TBase> void ReleaseAndDelete(TBase*& object)
        {
            if (object != nullptr)
            {
                auto* resource = static_cast<TResource*>(object);
                resource->Release();
                m_allocator.Delete(resource);
                object = nullptr;
            }
        }

        template <typename T> void DeleteIfAny(T*& object)
        {
            if (object != nullptr)
            {
                m_allocator.Delete(object);
                object = nullptr;
            }
        }

        const WebGpuApi* m_api;
        WGPUInstance m_instance;
        WGPUAdapter m_adapter;
        WGPUDevice m_device;
        IAllocator& m_allocator;
        WebGpuBlitHelper m_blitHelper;
        WebGpuBufferRegistry m_bufferRegistry;
        DeviceCounterBlock m_counters; // what its encoders recorded and its resources hold
        // Its objects by handle, and the tracker the tables retire through: no recordings are
        // registered, so a destroyed object is freed at once (see DestroyBuffer).
        SubmissionTracker m_tracker{m_allocator};
        WebGpuTables m_tables{m_allocator, m_tracker, this, Frees()};
        [[nodiscard]] static constexpr WebGpuTableFrees Frees() noexcept
        {
            WebGpuTableFrees f;
            f.buffer = &FreeBuffer;
            f.texture = &FreeTexture;
            f.textureView = &FreeObject<WebGpuTextureView>;
            f.sampler = &FreeObject<WebGpuSampler>;
            f.shaderModule = &FreeObject<WebGpuShaderModule>;
            f.bindGroupLayout = &FreeObject<WebGpuBindGroupLayout>;
            f.bindGroup = &FreeObject<WebGpuBindGroup>;
            f.pipelineLayout = &FreeObject<WebGpuPipelineLayout>;
            f.pipelineCache = &FreeObject<WebGpuPipelineCache>;
            f.renderPipeline = &FreeObject<WebGpuRenderPipeline>;
            f.computePipeline = &FreeObject<WebGpuComputePipeline>;
            f.querySet = &FreeObject<WebGpuQuerySet>;
            return f; // no mesh, ray tracing or acceleration structures on WebGPU
        }
        static void FreeTexture(void* context, void* object)
        {
            auto* device = static_cast<WebGpuDevice*>(context);
            auto* texture = static_cast<WebGpuTexture*>(object);
            device->m_counters.TextureFreed(EstimateTextureBytes(texture->desc), CountsAsRenderTarget(texture->desc));
            texture->Release();
            device->m_allocator.Delete(texture);
        }
        // Its WebGPU object, where it has one, then itself.
        template <typename T>
        static void FreeObject(void* context, void* object)
        {
            auto* device = static_cast<WebGpuDevice*>(context);
            T* typed = static_cast<T*>(object);
            if constexpr (requires { typed->Release(); })
                typed->Release();
            device->m_allocator.Delete(typed);
        }
        static void FreeBuffer(void* context, void* object)
        {
            auto* device = static_cast<WebGpuDevice*>(context);
            auto* buffer = static_cast<WebGpuBuffer*>(object);
            device->m_bufferRegistry.Remove(buffer);
            device->m_counters.BufferFreed(buffer->desc.size);
            buffer->Release();
            device->m_allocator.Delete(buffer);
        }
        WebGpuQueue m_graphicsQueue;
        WebGpuQueue m_computeQueue;
        WebGpuQueue m_transferQueue;
        void* m_lostRoute = nullptr; // adapter's lost-callback userdata; freed in Destroy
        bool m_lost = false;
        bool m_immediatesSupported = false;
        bool m_forceUniformPushConstants = false;
        bool m_forceWgsl = false; // ENV_WEBGPU_WGSL: use the browser's WGSL path on desktop
    };
}
