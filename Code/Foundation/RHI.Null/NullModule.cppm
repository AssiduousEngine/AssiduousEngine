// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Null RHI backend: stub implementations for all interfaces.
/// Useful for headless testing, CI, or when no GPU is available.

module;
#include "Core/Prelude.h" // <new> reachability for placement-new in core templates (GCC)

export module foundation.rhi.null;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::null
{
    // ---- Stub resource classes ----

    class NullBuffer : public Buffer
    {
    public:
        void* Map() override { return m_mapped; }
        void Unmap() override {}
        void allocate(u64 size)
        {
            m_data.Resize(static_cast<usize>(size));
            m_mapped = m_data.Data();
        }

    private:
        Array<u8> m_data;
        void* m_mapped = nullptr;
    };

    class NullTexture : public Texture
    {
    };
    class NullTextureView : public TextureView
    {
    };
    class NullSampler : public Sampler
    {
    };
    class NullShaderModule : public ShaderModule
    {
    };
    class NullSurface : public Surface
    {
    };
    class NullCommandBuffer : public CommandBuffer
    {
    public:
        SubmissionTracker::Recording recording = 0; // from its encoder's Finish, ended at submit
    };

    class NullFence : public Fence
    {
    public:
        u64 CompletedValue() override { return m_value; }
        bool Wait(u64 value, u64) override
        {
            m_value = value;
            return true;
        }
        void signal(u64 v) { m_value = v; }

    private:
        u64 m_value = 0;
    };

    class NullQuerySet : public QuerySet
    {
    };

    class NullBindGroupLayout : public BindGroupLayout
    {
    public:
        explicit NullBindGroupLayout(IAllocator& allocator) : entries(allocator) {}
        Span<const BindGroupLayoutEntry> Entries() const override { return entries.AsSpan(); }
        Array<BindGroupLayoutEntry> entries; // as created (their labels not kept)
    };

    class NullBindGroup : public BindGroup
    {
    public:
        BindGroupLayout* Layout() override { return nullptr; }
        void UpdateBindless(Span<const BindlessUpdateEntry>) override {}
    };

    class NullPipelineLayout : public PipelineLayout
    {
    };

    class NullPipelineCache : public PipelineCache
    {
    public:
        u32 GetDataSize() override { return 0; }
        Status GetData(Span<u8>) override { return ErrorCode::Ok; }
    };

    class NullRenderPipeline : public RenderPipeline
    {
    };
    class NullComputePipeline : public ComputePipeline
    {
    };
    class NullMeshPipeline : public MeshPipeline
    {
    };

    class NullAccelStruct : public AccelStruct
    {
    public:
        AccelStructType Type() const override { return AccelStructType::BottomLevel; }
        u64 DeviceAddress() const override { return 0; }
    };

    class NullRayTracingPipeline : public RayTracingPipeline
    {
    };

    // ---- Stub encoders ----

    // The Null device does nothing, but counts what it is asked to do (Device::Counters), so the
    // profiler's counters can be tested without a GPU.
    class NullRenderBundle : public RenderBundle
    {
    public:
        RecordedCounts counts; // what it recorded, counted each time it is executed
    };

    class NullRenderPassEncoder : public RenderPassEncoder, public MeshShaderPassExt
    {
    public:
        DeviceCounterBlock* counters = nullptr;
        MeshShaderPassExt* AsMeshShaderExt() noexcept override { return this; }
        void SetPipeline(RenderPipelineHandle) override { counters->PipelineBinds(); }
        void SetBindGroup(u32, BindGroupHandle, Span<const u32>) override { counters->BindGroupBinds(); }
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void SetVertexBuffer(u32, BufferHandle, u64) override {}
        void SetIndexBuffer(BufferHandle, IndexFormat, u64) override {}
        void SetViewport(f32, f32, f32, f32, f32, f32) override {}
        void SetScissor(i32, i32, u32, u32) override {}
        void SetBlendConstant(f32, f32, f32, f32) override {}
        void SetStencilReference(u32) override {}
        void Draw(u32, u32, u32, u32) override { counters->Draws(); }
        void DrawIndexed(u32, u32, u32, i32, u32) override { counters->Draws(); }
        void DrawIndirect(BufferHandle, u64, u32 drawCount, u32) override { counters->Draws(drawCount); }
        void DrawIndexedIndirect(BufferHandle, u64, u32 drawCount, u32) override { counters->Draws(drawCount); }
        void ExecuteBundles(Span<RenderBundle* const> bundles) override
        {
            for (RenderBundle* bundle : bundles)
            {
                static_cast<NullRenderBundle*>(bundle)->counts.AddTo(*counters);
            }
        }
        void WriteTimestamp(QuerySetHandle, u32) override {}
        void BeginOcclusionQuery(QuerySetHandle, u32) override {}
        void EndOcclusionQuery(QuerySetHandle, u32) override {}
        void End() override {}
        void SetMeshPipeline(MeshPipelineHandle) override { counters->PipelineBinds(); }
        void DrawMeshTasks(u32, u32, u32) override { counters->Draws(); }
        void DrawMeshTasksIndirect(BufferHandle, u64, u32 drawCount, u32) override { counters->Draws(drawCount); }
        void DrawMeshTasksIndirectCount(BufferHandle, u64, BufferHandle, u64, u32 maxDrawCount, u32) override
        {
            counters->Draws(maxDrawCount); // the count is on the GPU: at most this many
        }
    };

    class NullRenderBundleEncoder : public RenderBundleEncoder
    {
    public:
        // Counts what it records into its bundle (counted when the bundle runs), not the device.
        void SetPipeline(RenderPipelineHandle) override { ++bundle.counts.pipelineBinds; }
        void SetBindGroup(u32, BindGroupHandle, Span<const u32>) override { ++bundle.counts.bindGroupBinds; }
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void SetVertexBuffer(u32, BufferHandle, u64) override {}
        void SetIndexBuffer(BufferHandle, IndexFormat, u64) override {}
        void Draw(u32, u32, u32, u32) override { ++bundle.counts.draws; }
        void DrawIndexed(u32, u32, u32, i32, u32) override { ++bundle.counts.draws; }
        void DrawIndirect(BufferHandle, u64, u32 drawCount, u32) override { bundle.counts.draws += drawCount; }
        void DrawIndexedIndirect(BufferHandle, u64, u32 drawCount, u32) override { bundle.counts.draws += drawCount; }
        RenderBundle* Finish() override { return &bundle; }
        NullRenderBundle bundle;
    };

    class NullComputePassEncoder : public ComputePassEncoder
    {
    public:
        DeviceCounterBlock* counters = nullptr;
        void SetPipeline(ComputePipelineHandle) override { counters->PipelineBinds(); }
        void SetBindGroup(u32, BindGroupHandle, Span<const u32>) override { counters->BindGroupBinds(); }
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void Dispatch(u32, u32, u32) override { counters->Dispatches(); }
        void DispatchIndirect(BufferHandle, u64) override { counters->Dispatches(); }
        void ComputeBarrier() override {}
        void WriteTimestamp(QuerySetHandle, u32) override {}
        void End() override {}
    };

    class NullCommandEncoder : public CommandEncoder, public RayTracingEncoderExt
    {
    public:
        RayTracingEncoderExt* AsRayTracingExt() noexcept override { return this; }
        NullRenderPassEncoder rpe;
        NullComputePassEncoder cpe;
        NullRenderBundleEncoder rbe;
        NullCommandBuffer cb;

        DeviceCounterBlock* counters = nullptr;
        SubmissionTracker::Recording recording = 0; // open from CreateEncoder to Finish

        // Made by a NullCommandPool, it counts into its device; made on its own (tests), into a
        // block of its own.
        NullCommandEncoder() noexcept { Wire(m_standalone); }
        explicit NullCommandEncoder(DeviceCounterBlock& block) noexcept { Wire(block); }
        void Wire(DeviceCounterBlock& block) noexcept
        {
            counters = &block;
            rpe.counters = &block;
            cpe.counters = &block;
        }
        DeviceCounterBlock m_standalone;

        RenderPassEncoder* BeginRenderPass(const RenderPassDesc&) override
        {
            counters->RenderPass();
            return &rpe;
        }
        ComputePassEncoder* BeginComputePass(StringView) override
        {
            counters->ComputePass();
            return &cpe;
        }
        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc&) override
        {
            rbe.bundle.counts = RecordedCounts{};
            return &rbe;
        }
        void Barrier(const BarrierGroup&) override {}
        void CopyBufferToBuffer(BufferHandle, u64, BufferHandle, u64, u64) override {}
        void CopyBufferToTexture(BufferHandle, TextureHandle, const BufferTextureCopyRegion&) override {}
        void CopyTextureToBuffer(TextureHandle, BufferHandle, const BufferTextureCopyRegion&) override {}
        void CopyTextureToTexture(TextureHandle, TextureHandle, const TextureCopyRegion&) override {}
        void Blit(TextureHandle, TextureHandle) override {}
        void GenerateMipmaps(TextureHandle) override {}
        void ResolveTexture(TextureHandle, TextureHandle) override {}
        void ResetQuerySet(QuerySetHandle, u32, u32) override {}
        void WriteTimestamp(QuerySetHandle, u32) override {}
        void BeginPipelineStatisticsQuery(QuerySetHandle, u32) override {}
        void EndPipelineStatisticsQuery(QuerySetHandle, u32) override {}
        void ResolveQuerySet(QuerySetHandle, u32, u32, BufferHandle, u64) override {}
        void BeginDebugLabel(StringView, f32, f32, f32, f32) override {}
        void EndDebugLabel() override {}
        void InsertDebugLabel(StringView, f32, f32, f32, f32) override {}
        CommandBuffer* Finish() override
        {
            cb.recording = recording;
            recording = 0;
            return &cb;
        }

        // RayTracingEncoderExt
        void BuildBottomLevelAccelStruct(AccelStructHandle, BufferHandle, u64,
                                         Span<const AccelStructGeometryTriangles>,
                                         Span<const AccelStructGeometryAABBs>) override
        {
        }
        void BuildTopLevelAccelStruct(AccelStructHandle, BufferHandle, u64, BufferHandle, u64, u32) override {}
        void SetRayTracingPipeline(RayTracingPipelineHandle) override { counters->PipelineBinds(); }
        void SetBindGroup(u32, BindGroupHandle, Span<const u32>) override { counters->BindGroupBinds(); }
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void TraceRays(BufferHandle, u64, u64, BufferHandle, u64, u64, BufferHandle, u64, u64, u32, u32,
                       u32) override
        {
            counters->Dispatches();
        }
    };

    // The pool's one encoder records at a time; what it began is a recording the device's tracker
    // counts until it is submitted, or discarded by DestroyEncoder, Reset or the pool's end.
    class NullCommandPool : public CommandPool
    {
    public:
        NullCommandPool(DeviceCounterBlock& counters, SubmissionTracker& tracker) noexcept
            : enc(counters), m_tracker(&tracker)
        {
        }
        ~NullCommandPool() override { Discard(); }
        NullCommandEncoder enc;
        Status CreateEncoder(CommandEncoder*& out) override
        {
            m_tracker->End(enc.recording); // one left open by an encoder never finished
            enc.recording = m_tracker->Begin();
            out = &enc;
            return ErrorCode::Ok;
        }
        void DestroyEncoder(CommandEncoder*& encoder) override
        {
            m_tracker->End(enc.recording);
            enc.recording = 0;
            encoder = nullptr;
        }
        void Reset() override { Discard(); }
        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc&) override
        {
            // A bundle may be executed by any later command buffer until the pool resets.
            if (m_bundle == 0)
            {
                m_bundle = m_tracker->Begin();
            }
            enc.rbe.bundle.counts = RecordedCounts{};
            return &enc.rbe;
        }

    private:
        void Discard()
        {
            m_tracker->End(enc.recording);
            m_tracker->End(enc.cb.recording);
            m_tracker->End(m_bundle);
            enc.recording = 0;
            enc.cb.recording = 0;
            m_bundle = 0;
        }

        SubmissionTracker* m_tracker;
        SubmissionTracker::Recording m_bundle = 0;
    };

    class NullTransferBatch : public TransferBatch
    {
    public:
        DeviceCounterBlock* counters = nullptr;
        void WriteBuffer(BufferHandle, u64, Span<const u8> data) override { counters->Transfer(data.Size()); }
        void WriteTexture(TextureHandle, Span<const u8> data, const TextureDataLayout&, Extent3D, u32,
                          u32) override
        {
            counters->Transfer(data.Size());
        }
        Status Submit() override { return ErrorCode::Ok; }
        Status SubmitAsync(Fence*, u64) override { return ErrorCode::Ok; }
        void Reset() override {}
        void Destroy() override {}
    };

    class NullSwapChain : public SwapChain
    {
    public:
        NullTexture tex;
        NullTextureView view;
        NullTextureView alternateView;
        // The device's handles to them, taken back out of its tables when the swap chain goes.
        TextureHandle texHandle{};
        TextureViewHandle viewHandle{};
        TextureViewHandle alternateHandle{};
        TextureFormat m_format = TextureFormat::BGRA8UnormSrgb;
        TextureFormat m_alternate = TextureFormat::Undefined;
        u32 m_width = 0;
        u32 m_height = 0;
        u32 m_count = 2;
        u32 m_imgIdx = 0;
        ContentRotation m_rotation = ContentRotation::None; // the device's displayTurn, when asked to pre-rotate
        PresentMode m_presentMode = PresentMode::Fifo; // as it was asked for, for a test to read

        TextureFormat Format() const override { return m_format; }
        u32 Width() const override { return m_width; }
        u32 Height() const override { return m_height; }
        u32 BufferCount() const override { return m_count; }
        u32 CurrentImageIndex() const override { return m_imgIdx; }
        // Faults for tests, each taken once: what the next acquire, present or resize answers.
        Status nextAcquire = ErrorCode::Ok;
        Status nextPresent = ErrorCode::Ok;
        Status nextResize = ErrorCode::Ok;
        u32 resizes = 0; // Resize calls, failed ones included

        Status AcquireNextImage() override
        {
            const Status fault = nextAcquire;
            nextAcquire = ErrorCode::Ok;
            if (!fault.IsOk())
                return fault;
            m_imgIdx = (m_imgIdx + 1) % m_count;
            return ErrorCode::Ok;
        }
        TextureHandle CurrentTexture() override { return texHandle; }
        TextureViewHandle CurrentTextureView() override { return viewHandle; }
        TextureFormat AlternateViewFormat() const override { return m_alternate; }
        ContentRotation PreRotation() const override { return m_rotation; }
        TextureViewHandle CurrentAlternateTextureView() override { return alternateHandle; }
        Status Present(Queue*) override
        {
            const Status fault = nextPresent;
            nextPresent = ErrorCode::Ok;
            return fault;
        }
        Status Resize(u32 w, u32 h) override
        {
            ++resizes;
            const Status fault = nextResize;
            nextResize = ErrorCode::Ok;
            if (!fault.IsOk())
                return fault;
            const bool swaps = TurnSwapsSides(m_rotation);
            m_width = swaps ? h : w;
            m_height = swaps ? w : h;
            return ErrorCode::Ok;
        }
    };

    // A submission completes as it is made (there is no GPU): it ends its command buffers'
    // recordings at the queue's next value and frees what nothing can use any more.
    class NullQueue : public Queue
    {
    public:
        NullTransferBatch tb;
        SubmissionTracker* tracker = nullptr;
        u32 slot = 0;
        Function<void()> collect; // the device's: frees what is safe now

        void Submit(Span<CommandBuffer* const> cbs) override { Submitted(cbs); }
        void Submit(Span<CommandBuffer* const> cbs, Fence* f, u64 v) override
        {
            Submitted(cbs);
            if (auto* nf = static_cast<NullFence*>(f))
                nf->signal(v);
        }
        void Submit(Span<CommandBuffer* const> cbs, Span<Fence* const>, Span<const u64>, Fence* f,
                    u64 v) override
        {
            Submitted(cbs);
            if (auto* nf = static_cast<NullFence*>(f))
                nf->signal(v);
        }
        void SubmitFrame(Span<CommandBuffer* const> cbs, SwapChain&, Span<Fence* const>, Span<const u64>, Fence* f,
                         u64 v) override
        {
            Submitted(cbs);
            if (auto* nf = static_cast<NullFence*>(f))
                nf->signal(v);
        }
        void Submitted(Span<CommandBuffer* const> cbs)
        {
            if (tracker == nullptr)
            {
                return;
            }
            const u64 value = tracker->NextSubmission(slot);
            for (CommandBuffer* cb : cbs)
            {
                if (auto* null = static_cast<NullCommandBuffer*>(cb))
                {
                    tracker->End(null->recording, slot, value);
                    null->recording = 0;
                }
            }
            if (collect)
            {
                collect();
            }
        }
        using Queue::SubmitFrame;
        void WaitIdle() override {}
        Status CreateTransferBatch(TransferBatch*& out) override
        {
            out = &tb;
            return ErrorCode::Ok;
        }
        void DestroyTransferBatch(TransferBatch*&) override {}
        f32 TimestampPeriod() const override { return 1.0f; }
        bool CalibrateTimestamps(u64&, u64&) override { return false; } // no GPU, no clock to read
    };

    // ---- Null Device ----

    class NullDevice : public Device
    {
    public:
        NullQueue gfxQueue, compQueue, xferQueue;
        IAllocator& m_allocator;
        DeviceCounterBlock m_counters;
        // The turn its display presents at, as a phone's surface reports one held away from its
        // natural orientation (a test sets it): a swapchain asked to pre-rotate takes it, its
        // images the window's size turned; one not asked is drawn as the window stands.
        ContentRotation displayTurn = ContentRotation::None;
        // A present mode the device refuses a swapchain in, as a surface without it (or a driver
        // without tearing) would. A test sets it; off, every mode is taken.
        bool refusePresentMode = false;
        PresentMode refusedPresentMode = PresentMode::Immediate;

        SubmissionTracker m_tracker{m_allocator};
        ResourceTable<BufferTag, NullBuffer> m_buffers{m_allocator, m_tracker, &FreeBuffer, this};
        ResourceTable<TextureTag, NullTexture> m_textures{m_allocator, m_tracker, &FreeTexture, this};
        ResourceTable<TextureViewTag, NullTextureView> m_views{m_allocator, m_tracker, &FreeObject<NullTextureView>,
                                                               this};
        ResourceTable<SamplerTag, NullSampler> m_samplers{m_allocator, m_tracker, &FreeObject<NullSampler>, this};
        ResourceTable<ShaderModuleTag, NullShaderModule> m_shaderModules{m_allocator, m_tracker, &FreeObject<NullShaderModule>, this};
        ResourceTable<BindGroupLayoutTag, NullBindGroupLayout> m_bindGroupLayouts{m_allocator, m_tracker, &FreeObject<NullBindGroupLayout>, this};
        ResourceTable<BindGroupTag, NullBindGroup> m_bindGroups{m_allocator, m_tracker, &FreeObject<NullBindGroup>, this};
        ResourceTable<PipelineLayoutTag, NullPipelineLayout> m_pipelineLayouts{m_allocator, m_tracker, &FreeObject<NullPipelineLayout>, this};
        ResourceTable<PipelineCacheTag, NullPipelineCache> m_pipelineCaches{m_allocator, m_tracker, &FreeObject<NullPipelineCache>, this};
        ResourceTable<RenderPipelineTag, NullRenderPipeline> m_renderPipelines{m_allocator, m_tracker, &FreeObject<NullRenderPipeline>, this};
        ResourceTable<ComputePipelineTag, NullComputePipeline> m_computePipelines{m_allocator, m_tracker, &FreeObject<NullComputePipeline>, this};
        ResourceTable<QuerySetTag, NullQuerySet> m_querySets{m_allocator, m_tracker, &FreeObject<NullQuerySet>, this};

        static void FreeTexture(void* context, void* object)
        {
            auto* device = static_cast<NullDevice*>(context);
            auto* texture = static_cast<NullTexture*>(object);
            device->m_counters.TextureFreed(EstimateTextureBytes(texture->desc), CountsAsRenderTarget(texture->desc));
            device->m_allocator.Delete(texture);
        }
        template <typename T>
        static void FreeObject(void* context, void* object)
        {
            static_cast<NullDevice*>(context)->m_allocator.Delete(static_cast<T*>(object));
        }

        static void FreeBuffer(void* context, void* object)
        {
            auto* device = static_cast<NullDevice*>(context);
            auto* buffer = static_cast<NullBuffer*>(object);
            device->m_counters.BufferFreed(buffer->desc.size);
            device->m_allocator.Delete(buffer);
        }

        // Everything submitted is complete: free what no open recording can still use.
        void CollectRetired()
        {
            u64 completed[SubmissionTracker::kMaxQueues] = {};
            for (u32 q = 0; q < SubmissionTracker::kMaxQueues; ++q)
            {
                completed[q] = m_tracker.LastSubmission(q);
            }
            m_tracker.Collect(Span<const u64>(completed, SubmissionTracker::kMaxQueues));
        }

        explicit NullDevice(IAllocator& allocator) : m_allocator(allocator)
        {
            type = DeviceType::Null;
            NullQueue* const queues[] = {&gfxQueue, &compQueue, &xferQueue};
            for (NullQueue* queue : queues)
            {
                queue->tracker = &m_tracker;
                queue->slot = m_tracker.AddQueue();
                queue->collect = [this]() { CollectRetired(); };
            }
            gfxQueue.tb.counters = &m_counters;
            compQueue.tb.counters = &m_counters;
            xferQueue.tb.counters = &m_counters;
            gfxQueue.queueType = QueueType::Graphics;
            compQueue.queueType = QueueType::Compute;
            xferQueue.queueType = QueueType::Transfer;
        }

        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            return ShaderFormat::SpirV;
        }

        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override { return false; }

        // What it was asked to do, and the memory its resources would take (from their descs).
        [[nodiscard]] DeviceCounters Counters() const noexcept override { return m_counters.Snapshot(); }
        [[nodiscard]] bool MemoryBudget(u64&, u64&) const override { return false; } // no driver

        // No statistics: a statistics result takes no room and reads as zeros.
        [[nodiscard]] u32 QueryResultSize(QueryType queryType) const noexcept override
        {
            return queryType == QueryType::PipelineStatistics ? 0u : 8u;
        }
        [[nodiscard]] PipelineStatistics DecodePipelineStatistics(const void*) const noexcept override
        {
            return PipelineStatistics{};
        }

        Queue* GetQueue(QueueType t, u32) override
        {
            switch (t)
            {
            case QueueType::Graphics:
                return &gfxQueue;
            case QueueType::Compute:
                return &compQueue;
            case QueueType::Transfer:
                return &xferQueue;
            }
            return nullptr;
        }
        u32 GetQueueCount(QueueType) override { return 1; }
        FormatSupport GetFormatSupport(TextureFormat) override
        {
            return FormatSupport::Texture | FormatSupport::ColorAttachment |
                   FormatSupport::DepthStencil;
        }

        Status CreateBuffer(const BufferDesc& d, BufferHandle& out) override
        {
            auto* b = m_allocator.New<NullBuffer>();
            b->desc = d;
            b->allocate(d.size);
            out = m_buffers.Add(b);
            if (!out.IsValid())
            {
                m_allocator.Delete(b);
                return ErrorCode::OutOfMemory;
            }
            m_counters.BufferMade(d.size);
            return ErrorCode::Ok;
        }
        const BufferDesc* Describe(BufferHandle buffer) const override
        {
            const NullBuffer* b = m_buffers.Get(buffer);
            return b != nullptr ? &b->desc : nullptr;
        }
        void* Map(BufferHandle buffer) override
        {
            NullBuffer* b = m_buffers.Get(buffer);
            return b != nullptr ? b->Map() : nullptr;
        }
        void Unmap(BufferHandle buffer) override
        {
            if (NullBuffer* b = m_buffers.Get(buffer))
                b->Unmap();
        }
        void FlushRange(BufferHandle buffer, u64 offset, u64 size) override
        {
            if (NullBuffer* b = m_buffers.Get(buffer))
                b->FlushRange(offset, size);
        }
        Status CreateTexture(const TextureDesc& d, TextureHandle& out) override
        {
            auto* t = m_allocator.New<NullTexture>();
            t->desc = d;
            m_counters.TextureMade(EstimateTextureBytes(d), CountsAsRenderTarget(d));
            out = m_textures.Add(t);
            return ErrorCode::Ok;
        }
        Status CreateTextureView(TextureHandle tex, const TextureViewDesc& d, TextureViewHandle& out) override
        {
            out = TextureViewHandle::Invalid();
            if (m_textures.Get(tex) == nullptr)
                return ErrorCode::InvalidArgument;
            auto* v = m_allocator.New<NullTextureView>();
            v->desc = d;
            v->texture = tex;
            out = m_views.Add(v);
            return ErrorCode::Ok;
        }
        Status CreateSampler(const SamplerDesc& d, SamplerHandle& out) override
        {
            auto* s = m_allocator.New<NullSampler>();
            s->desc = d;
            out = m_samplers.Add(s);
            return ErrorCode::Ok;
        }
        const TextureDesc* Describe(TextureHandle texture) const override
        {
            const NullTexture* t = m_textures.Get(texture);
            return t != nullptr ? &t->desc : nullptr;
        }
        const TextureViewDesc* Describe(TextureViewHandle view) const override
        {
            const NullTextureView* v = m_views.Get(view);
            return v != nullptr ? &v->desc : nullptr;
        }
        bool IsLive(TextureViewHandle view) const override { return m_views.Get(view) != nullptr; }
        TextureHandle TextureOf(TextureViewHandle view) const override
        {
            const NullTextureView* v = m_views.Get(view);
            return v != nullptr ? v->texture : TextureHandle::Invalid();
        }
        const SamplerDesc* Describe(SamplerHandle sampler) const override
        {
            const NullSampler* s = m_samplers.Get(sampler);
            return s != nullptr ? &s->desc : nullptr;
        }
        Status CreateShaderModule(const ShaderModuleDesc& d, ShaderModuleHandle& out) override
        {
            auto* module = m_allocator.New<NullShaderModule>();
            module->entryPoint = String(d.entryPoint);
            out = m_shaderModules.Add(module);
            return ErrorCode::Ok;
        }
        Status CreateBindGroupLayout(const BindGroupLayoutDesc& d, BindGroupLayoutHandle& out) override
        {
            auto* layout = m_allocator.New<NullBindGroupLayout>(m_allocator);
            for (BindGroupLayoutEntry entry : d.entries)
            {
                entry.label = {};
                layout->entries.PushBack(entry);
            }
            out = m_bindGroupLayouts.Add(layout);
            return ErrorCode::Ok;
        }
        Status CreateBindGroup(const BindGroupDesc& d, BindGroupHandle& out) override
        {
            out = BindGroupHandle::Invalid();
            if (m_bindGroupLayouts.Get(d.layout) == nullptr)
                return ErrorCode::InvalidArgument;
            auto* group = m_allocator.New<NullBindGroup>();
            group->layout = d.layout;
            out = m_bindGroups.Add(group);
            return ErrorCode::Ok;
        }
        StringView EntryPointOf(ShaderModuleHandle module) const override
        {
            const NullShaderModule* m = m_shaderModules.Get(module);
            return m != nullptr ? m->entryPoint.AsView() : StringView{};
        }
        Span<const BindGroupLayoutEntry> LayoutEntries(BindGroupLayoutHandle layout) const override
        {
            const NullBindGroupLayout* l = m_bindGroupLayouts.Get(layout);
            return l != nullptr ? l->Entries() : Span<const BindGroupLayoutEntry>{};
        }
        BindGroupLayoutHandle LayoutOf(BindGroupHandle group) const override
        {
            const NullBindGroup* g = m_bindGroups.Get(group);
            return g != nullptr ? g->layout : BindGroupLayoutHandle::Invalid();
        }
        void UpdateBindless(BindGroupHandle, Span<const BindlessUpdateEntry>) override {}
        u32 PipelineCacheDataSize(PipelineCacheHandle) override { return 0; }
        Status PipelineCacheData(PipelineCacheHandle cache, Span<u8>) override
        {
            return m_pipelineCaches.Get(cache) != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::InvalidArgument);
        }
        const QuerySetDesc* Describe(QuerySetHandle querySet) const override
        {
            const NullQuerySet* q = m_querySets.Get(querySet);
            return q != nullptr ? &q->desc : nullptr;
        }
        u64 DeviceAddress(AccelStructHandle) const override { return 0; }
        Status CreatePipelineLayout(const PipelineLayoutDesc&, PipelineLayoutHandle& out) override
        {
            out = m_pipelineLayouts.Add(m_allocator.New<NullPipelineLayout>());
            return ErrorCode::Ok;
        }
        Status CreatePipelineCache(const PipelineCacheDesc&, PipelineCacheHandle& out) override
        {
            out = m_pipelineCaches.Add(m_allocator.New<NullPipelineCache>());
            return ErrorCode::Ok;
        }
        Status CreateRenderPipeline(const RenderPipelineDesc&, RenderPipelineHandle& out) override
        {
            out = m_renderPipelines.Add(m_allocator.New<NullRenderPipeline>());
            return ErrorCode::Ok;
        }
        Status CreateComputePipeline(const ComputePipelineDesc&, ComputePipelineHandle& out) override
        {
            out = m_computePipelines.Add(m_allocator.New<NullComputePipeline>());
            return ErrorCode::Ok;
        }
        Status CreateCommandPool(QueueType, CommandPool*& out) override
        {
            out = m_allocator.New<NullCommandPool>(m_counters, m_tracker);
            return ErrorCode::Ok;
        }
        Status CreateFence(u64, Fence*& out) override
        {
            out = m_allocator.New<NullFence>();
            return ErrorCode::Ok;
        }
        Status CreateQuerySet(const QuerySetDesc& d, QuerySetHandle& out) override
        {
            auto* q = m_allocator.New<NullQuerySet>();
            q->desc = d;
            q->desc.label = {};
            out = m_querySets.Add(q);
            return ErrorCode::Ok;
        }
        Status CreateSwapChain(Surface*, const SwapChainDesc& d, SwapChain*& out) override
        {
            if (refusePresentMode && d.presentMode == refusedPresentMode)
                return ErrorCode::NotSupported;
            auto* sc = m_allocator.New<NullSwapChain>();
            sc->m_format = d.format;
            sc->m_presentMode = d.presentMode;
            // Nothing to refuse: a twin request is honoured, as a capable device would.
            sc->m_alternate = d.alternateViewFormat == SrgbTwin(d.format) ? d.alternateViewFormat
                                                                          : TextureFormat::Undefined;
            sc->tex.desc.format = d.format;
            sc->tex.desc.alternateViewFormat = sc->m_alternate;
            sc->view.desc.format = d.format;
            sc->alternateView.desc.format = sc->m_alternate;
            sc->m_rotation = d.preRotate ? displayTurn : ContentRotation::None;
            const bool swaps = TurnSwapsSides(sc->m_rotation);
            sc->m_width = swaps ? d.height : d.width;
            sc->m_height = swaps ? d.width : d.height;
            sc->m_count = d.bufferCount;
            sc->texHandle = m_textures.Add(&sc->tex);
            sc->view.texture = sc->texHandle;
            sc->alternateView.texture = sc->texHandle;
            sc->viewHandle = m_views.Add(&sc->view);
            if (sc->m_alternate != TextureFormat::Undefined)
                sc->alternateHandle = m_views.Add(&sc->alternateView);
            out = sc;
            return ErrorCode::Ok;
        }

        void DestroyBuffer(BufferHandle& x) override
        {
            if (m_buffers.Retire(x))
                CollectRetired();
        }
        void DestroyTexture(TextureHandle& x) override
        {
            if (m_textures.Retire(x))
                CollectRetired();
        }
        void DestroyTextureView(TextureViewHandle& x) override
        {
            if (m_views.Retire(x))
                CollectRetired();
        }
        void DestroySampler(SamplerHandle& x) override
        {
            if (m_samplers.Retire(x))
                CollectRetired();
        }
        void DestroyShaderModule(ShaderModuleHandle& x) override
        {
            if (m_shaderModules.Retire(x))
                CollectRetired();
        }
        void DestroyBindGroupLayout(BindGroupLayoutHandle& x) override
        {
            if (m_bindGroupLayouts.Retire(x))
                CollectRetired();
        }
        void DestroyBindGroup(BindGroupHandle& x) override
        {
            if (m_bindGroups.Retire(x))
                CollectRetired();
        }
        void DestroyPipelineLayout(PipelineLayoutHandle& x) override
        {
            if (m_pipelineLayouts.Retire(x))
                CollectRetired();
        }
        void DestroyPipelineCache(PipelineCacheHandle& x) override
        {
            if (m_pipelineCaches.Retire(x))
                CollectRetired();
        }
        void DestroyRenderPipeline(RenderPipelineHandle& x) override
        {
            if (m_renderPipelines.Retire(x))
                CollectRetired();
        }
        void DestroyComputePipeline(ComputePipelineHandle& x) override
        {
            if (m_computePipelines.Retire(x))
                CollectRetired();
        }
        void DestroyCommandPool(CommandPool*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyFence(Fence*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyQuerySet(QuerySetHandle& x) override
        {
            if (m_querySets.Retire(x))
                CollectRetired();
        }
        void DestroySwapChain(SwapChain*& x) override
        {
            if (auto* sc = static_cast<NullSwapChain*>(x))
            {
                (void)m_textures.Remove(sc->texHandle);
                (void)m_views.Remove(sc->viewHandle);
                (void)m_views.Remove(sc->alternateHandle);
            }
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroySurface(Surface*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }

        bool IsLost() override { return false; } // a null device cannot be lost
        void WaitIdle() override {}
        // No MSAA: one sample only.
        [[nodiscard]] u32 MaxColorDepthSampleCount() const noexcept override { return 1; }
        [[nodiscard]] bool SupportsSampleCount(u32 count) const noexcept override { return count <= 1; }

        // No mesh shaders and no ray tracing here.
        Status CreateMeshPipeline(const MeshPipelineDesc&, MeshPipelineHandle& out) override
        {
            out = MeshPipelineHandle::Invalid();
            return ErrorCode::NotSupported;
        }
        void DestroyMeshPipeline(MeshPipelineHandle& pipeline) override { pipeline = MeshPipelineHandle::Invalid(); }
        Status CreateAccelStruct(const AccelStructDesc&, AccelStructHandle& out) override
        {
            out = AccelStructHandle::Invalid();
            return ErrorCode::NotSupported;
        }
        void DestroyAccelStruct(AccelStructHandle& accelStruct) override { accelStruct = AccelStructHandle::Invalid(); }
        Status CreateRayTracingPipeline(const RayTracingPipelineDesc&, RayTracingPipelineHandle& out) override
        {
            out = RayTracingPipelineHandle::Invalid();
            return ErrorCode::NotSupported;
        }
        void DestroyRayTracingPipeline(RayTracingPipelineHandle& pipeline) override
        {
            pipeline = RayTracingPipelineHandle::Invalid();
        }
        Status GetShaderGroupHandles(RayTracingPipelineHandle, u32, u32, Span<u8>) override
        {
            return ErrorCode::NotSupported;
        }
        void Destroy() override
        {
            IAllocator& alloc = m_allocator;
            this->~NullDevice();
            alloc.Free(this);
        }
    };

    // ---- Null Adapter ----

    class NullAdapter : public Adapter
    {
    public:
        IAllocator& m_allocator;

        explicit NullAdapter(IAllocator& allocator) : m_allocator(allocator) {}

        void GetInfo(AdapterInfo& out) override
        {
            out.name = u8"Null Device";
            out.vendorId = 0;
            out.deviceId = 0;
            out.type = AdapterType::Cpu;
        }
        Status CreateDevice(const DeviceDesc&, Device*& out) override
        {
            out = m_allocator.New<NullDevice>(m_allocator);
            return ErrorCode::Ok;
        }
    };

    // ---- Null Backend ----

    class NullBackend : public Backend
    {
    public:
        IAllocator& m_allocator;
        NullAdapter adapter;
        Adapter* adapterPtr = &adapter;

        explicit NullBackend(IAllocator& allocator) : m_allocator(allocator), adapter(allocator) {}

        Span<Adapter* const> EnumerateAdapters() override
        {
            return Span<Adapter* const>(&adapterPtr, 1);
        }

        Status CreateSurface(void*, void*, Surface*& out,
                             SurfacePlatform = SurfacePlatform::Unknown) override
        {
            out = m_allocator.New<NullSurface>();
            return ErrorCode::Ok;
        }

        void Destroy() override
        {
            IAllocator& alloc = m_allocator;
            this->~NullBackend();
            alloc.Free(this);
        }
    };

    /// Creates a null backend for headless, GPU-less testing.
    Status CreateNullBackend(Backend*& out, IAllocator& allocator = DefaultAllocator())
    {
        auto* b = allocator.New<NullBackend>(allocator);
        b->isInitialized = true;
        out = b;
        return ErrorCode::Ok;
    }

} // namespace foundation::rhi::null
