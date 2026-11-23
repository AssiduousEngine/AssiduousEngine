// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Validation wrapper for Device. Tracks all live resources for leak
/// detection, validates create/destroy parameters.

module;
#include "Core/Prelude.h"

export module foundation.rhi.validation:validated_device;

import foundation.core;
import foundation.rhi;
import :validated_fence;
import :validated_swap_chain;
import :validated_command_pool;
import :validated_queue;

using namespace foundation::core;

export namespace foundation::rhi::validation
{

    class ValidatedDevice : public Device
    {
    public:
        explicit ValidatedDevice(Device* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
            type = inner->type;
            features = inner->features;
            shaderGroupHandleSize = inner->shaderGroupHandleSize;
            shaderGroupHandleAlignment = inner->shaderGroupHandleAlignment;
            shaderGroupBaseAlignment = inner->shaderGroupBaseAlignment;
        }

        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            return m_inner->PreferredShaderFormat();
        }

        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override
        {
            return m_inner->NeedsClipSpaceYFlip();
        }

        // ---- Queues ----
        Queue* GetQueue(QueueType t, u32 index) override
        {
            // Wrap on first access.
            Queue* raw = m_inner->GetQueue(t, index);
            if (!raw)
                return nullptr;
            for (auto& w : m_queueWrappers)
                if (w.raw == raw)
                    return w.validated;
            auto* vq = m_allocator.New<ValidatedQueue>(raw, m_allocator);
            m_queueWrappers.PushBack({raw, vq});
            return vq;
        }
        u32 GetQueueCount(QueueType t) override { return m_inner->GetQueueCount(t); }
        u32 MaxColorDepthSampleCount() const noexcept override
        {
            return m_inner->MaxColorDepthSampleCount();
        }
        bool SupportsSampleCount(u32 count) const noexcept override
        {
            return m_inner->SupportsSampleCount(count);
        }
        FormatSupport GetFormatSupport(TextureFormat f) override
        {
            return m_inner->GetFormatSupport(f);
        }
        DeviceCounters Counters() const noexcept override { return m_inner->Counters(); }
        bool MemoryBudget(u64& budgetBytes, u64& usedBytes) const override
        {
            return m_inner->MemoryBudget(budgetBytes, usedBytes);
        }
        u32 QueryResultSize(QueryType queryType) const noexcept override
        {
            return m_inner->QueryResultSize(queryType);
        }
        PipelineStatistics DecodePipelineStatistics(const void* result) const noexcept override
        {
            return m_inner->DecodePipelineStatistics(result);
        }

        // ---- Create methods (with validation and tracking) ----

        Status CreateBuffer(const BufferDesc& d, BufferHandle& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] CreateBuffer: device destroyed");
                out = BufferHandle::Invalid();
                return ErrorCode::Unknown;
            }
            if (d.size == 0) { LogError("[Validation] CreateBuffer: size is 0"); out = BufferHandle::Invalid(); return ErrorCode::InvalidArgument; }
            if (d.memory == MemoryLocation::CpuToGpu && (static_cast<u32>(d.usage) & static_cast<u32>(BufferUsage::Storage)))
            {
                LogError("[Validation] CreateBuffer: Storage usage is not compatible with CpuToGpu memory. "
                         "DX12 UPLOAD heaps cannot have ALLOW_UNORDERED_ACCESS. "
                         "Use StorageRead for read-only structured buffers, or GpuOnly memory with a staging copy pattern.");
                out = {}; return ErrorCode::InvalidArgument;
            }
            if (d.memory == MemoryLocation::GpuToCpu && (static_cast<u32>(d.usage) & static_cast<u32>(BufferUsage::Storage)))
            {
                LogError("[Validation] CreateBuffer: Storage usage is not compatible with GpuToCpu memory. "
                         "DX12 READBACK heaps cannot have ALLOW_UNORDERED_ACCESS.");
                out = {}; return ErrorCode::InvalidArgument;
            }
            Status r = m_inner->CreateBuffer(d, out);
            if (r == ErrorCode::Ok && out)
                m_buffers.Made(out, d.label);
            return r;
        }

        Status CreateTexture(const TextureDesc& d, TextureHandle& out) override
        {
            out = TextureHandle::Invalid();
            if (!checkAlive("CreateTexture")) return ErrorCode::Unknown;
            if (d.width == 0 || d.height == 0) { LogError("[Validation] CreateTexture: width or height is 0"); return ErrorCode::InvalidArgument; }
            // The one alternate view format a backend can honour is the format's sRGB or UNORM twin.
            if (d.alternateViewFormat != TextureFormat::Undefined && d.alternateViewFormat != SrgbTwin(d.format))
            {
                LogError("[Validation] CreateTexture: alternateViewFormat must be the format's sRGB or UNORM twin");
                return ErrorCode::InvalidArgument;
            }
            Status r = m_inner->CreateTexture(d, out);
            if (r == ErrorCode::Ok && out)
                m_textures.Made(out, d.label);
            return r;
        }

        Status CreateSampler(const SamplerDesc& d, SamplerHandle& out) override
        {
            out = SamplerHandle::Invalid();
            if (!checkAlive("CreateSampler")) return ErrorCode::Unknown;
            Status r = m_inner->CreateSampler(d, out);
            if (r == ErrorCode::Ok && out)
                m_samplers.Made(out, d.label);
            return r;
        }

        Status CreateShaderModule(const ShaderModuleDesc& d, ShaderModule*& out) override
        {
            if (!checkAlive("CreateShaderModule", out)) return ErrorCode::Unknown;
            if (d.code.Size() == 0) { LogError("[Validation] CreateShaderModule: code is empty"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateShaderModule(d, out);
            if (r == ErrorCode::Ok && out) m_liveShaderModules.PushBack(out);
            return r;
        }

        Status CreateBindGroupLayout(const BindGroupLayoutDesc& d, BindGroupLayout*& out) override
        {
            if (!checkAlive("CreateBindGroupLayout", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreateBindGroupLayout(d, out);
            if (r == ErrorCode::Ok && out) m_liveBindGroupLayouts.PushBack(out);
            return r;
        }

        Status CreateBindGroup(const BindGroupDesc& d, BindGroup*& out) override
        {
            if (!checkAlive("CreateBindGroup", out)) return ErrorCode::Unknown;
            if (d.layout == nullptr) { LogError("[Validation] CreateBindGroup: layout is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            // Count non-bindless layout entries; positional entries must match.
            {
                auto layoutEntries = d.layout->Entries();
                u32 regularCount = 0;
                for (usize i = 0; i < layoutEntries.Size(); ++i)
                {
                    const auto& le = layoutEntries[i];
                    if (le.type != BindingType::BindlessTextures && le.type != BindingType::BindlessSamplers &&
                        le.type != BindingType::BindlessStorageBuffers && le.type != BindingType::BindlessStorageTextures)
                        ++regularCount;
                }
                if (d.entries.Size() != regularCount)
                {
                    LogErrorf("[Validation] CreateBindGroup: entry count (%u) does not match non-bindless layout entry count (%u)",
                              static_cast<unsigned>(d.entries.Size()), static_cast<unsigned>(regularCount));
                    out = nullptr; return ErrorCode::InvalidArgument;
                }
                // Per-entry resource type validation.
                u32 entryIdx = 0;
                for (usize i = 0; i < layoutEntries.Size() && entryIdx < d.entries.Size(); ++i)
                {
                    const auto& le = layoutEntries[i];
                    if (le.type == BindingType::BindlessTextures || le.type == BindingType::BindlessSamplers ||
                        le.type == BindingType::BindlessStorageBuffers || le.type == BindingType::BindlessStorageTextures)
                        continue;
                    const auto& entry = d.entries[entryIdx];
                    switch (le.type)
                    {
                    case BindingType::UniformBuffer:
                    case BindingType::StorageBufferReadOnly:
                    case BindingType::StorageBufferReadWrite:
                        if (!entry.buffer.IsValid())
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] expects a buffer but Buffer is null", entryIdx);
                        break;
                    case BindingType::SampledTexture:
                    case BindingType::StorageTextureReadOnly:
                    case BindingType::StorageTextureReadWrite:
                        if (!entry.textureView)
                            {
                                const String label8 = String(d.label);
                                LogErrorf("[Validation] CreateBindGroup('%s'): entry [%u] "
                                          "expects a texture view but TextureView is null",
                                          reinterpret_cast<const char*>(label8.CStr()),
                                          entryIdx);
                            }
                        else if (!Live(m_views, entry.textureView, "CreateBindGroup"))
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] names a destroyed texture view", entryIdx);
                        break;
                    case BindingType::Sampler:
                    case BindingType::ComparisonSampler:
                        if (!entry.sampler)
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] expects a sampler but Sampler is null", entryIdx);
                        else if (!Live(m_samplers, entry.sampler, "CreateBindGroup"))
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] names a destroyed sampler", entryIdx);
                        break;
                    case BindingType::AccelerationStructure:
                        if (entry.accelStruct == nullptr)
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] expects an acceleration structure but AccelStruct is null", entryIdx);
                        break;
                    default: break;
                    }
                    ++entryIdx;
                }
            }
            Status r = m_inner->CreateBindGroup(d, out);
            if (r == ErrorCode::Ok && out) m_liveBindGroups.PushBack(out);
            return r;
        }

        Status CreatePipelineLayout(const PipelineLayoutDesc& d, PipelineLayout*& out) override
        {
            if (!checkAlive("CreatePipelineLayout", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreatePipelineLayout(d, out);
            if (r == ErrorCode::Ok && out) m_livePipelineLayouts.PushBack(out);
            return r;
        }

        Status CreatePipelineCache(const PipelineCacheDesc& d, PipelineCache*& out) override
        {
            if (!checkAlive("CreatePipelineCache", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreatePipelineCache(d, out);
            if (r == ErrorCode::Ok && out) m_livePipelineCaches.PushBack(out);
            return r;
        }

        Status CreateRenderPipeline(const RenderPipelineDesc& d, RenderPipeline*& out) override
        {
            if (!checkAlive("CreateRenderPipeline", out)) return ErrorCode::Unknown;
            if (d.layout == nullptr) { LogError("[Validation] CreateRenderPipeline: layout is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.vertex.shader.module == nullptr) { LogError("[Validation] CreateRenderPipeline: vertex shader module is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (EntryPointOf(d.vertex.shader).IsEmpty() || (d.fragment.HasValue() && d.fragment->shader.module != nullptr && EntryPointOf(d.fragment->shader).IsEmpty())) { LogError("[Validation] CreateRenderPipeline: a stage names no entry point and its module declares none"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateRenderPipeline(d, out);
            if (r == ErrorCode::Ok && out) m_liveRenderPipelines.PushBack(out);
            return r;
        }

        Status CreateComputePipeline(const ComputePipelineDesc& d, ComputePipeline*& out) override
        {
            if (!checkAlive("CreateComputePipeline", out)) return ErrorCode::Unknown;
            if (d.layout == nullptr) { LogError("[Validation] CreateComputePipeline: layout is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.compute.module == nullptr) { LogError("[Validation] CreateComputePipeline: compute shader module is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (EntryPointOf(d.compute).IsEmpty()) { LogError("[Validation] CreateComputePipeline: the stage names no entry point and its module declares none"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateComputePipeline(d, out);
            if (r == ErrorCode::Ok && out) m_liveComputePipelines.PushBack(out);
            return r;
        }

        Status CreateQuerySet(const QuerySetDesc& d, QuerySet*& out) override
        {
            if (!checkAlive("CreateQuerySet", out)) return ErrorCode::Unknown;
            if (d.count == 0) { LogError("[Validation] CreateQuerySet: count is 0"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateQuerySet(d, out);
            if (r == ErrorCode::Ok && out) m_liveQuerySets.PushBack(out);
            return r;
        }

        Status CreateTextureView(TextureHandle tex, const TextureViewDesc& d, TextureViewHandle& out) override
        {
            out = TextureViewHandle::Invalid();
            if (!checkAlive("CreateTextureView")) return ErrorCode::Unknown;
            if (!Live(m_textures, tex, "CreateTextureView"))
                return ErrorCode::InvalidArgument;
            // A view in the texture's sRGB or UNORM twin needs the texture to have declared it.
            const TextureDesc* texDesc = m_inner->Describe(tex);
            if (d.format != TextureFormat::Undefined && d.format != texDesc->format &&
                d.format == SrgbTwin(texDesc->format) && texDesc->alternateViewFormat != d.format)
            {
                LogError("[Validation] CreateTextureView: a view in the texture's sRGB or UNORM twin needs "
                         "TextureDesc::alternateViewFormat");
                return ErrorCode::InvalidArgument;
            }
            Status r = m_inner->CreateTextureView(tex, d, out);
            if (r == ErrorCode::Ok && out)
            {
                // A view is named after its texture: a leaked view then says whose it is.
                m_views.Made(out, Format(u8"{}/view", m_textures.LabelOf(tex)).AsView());
            }
            return r;
        }

        Status CreateCommandPool(QueueType qt, CommandPool*& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] createCommandPool: device destroyed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            CommandPool* innerPool = nullptr;
            Status r = m_inner->CreateCommandPool(qt, innerPool);
            if (r != ErrorCode::Ok || !innerPool)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedCommandPool>(innerPool, m_allocator);
            m_liveCommandPools.PushBack(out);
            return ErrorCode::Ok;
        }

        Status CreateFence(u64 initialValue, Fence*& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] createFence: device destroyed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Fence* innerFence = nullptr;
            Status r = m_inner->CreateFence(initialValue, innerFence);
            if (r != ErrorCode::Ok || !innerFence)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedFence>(innerFence);
            m_liveFences.PushBack(out);
            return ErrorCode::Ok;
        }

        Status CreateSwapChain(Surface* surface, const SwapChainDesc& d, SwapChain*& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] CreateSwapChain: device destroyed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            if (surface == nullptr) { LogError("[Validation] CreateSwapChain: surface is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.width == 0 || d.height == 0) { LogError("[Validation] CreateSwapChain: width or height is 0"); out = nullptr; return ErrorCode::InvalidArgument; }
            SwapChain* innerSc = nullptr;
            Status r = m_inner->CreateSwapChain(surface, d, innerSc);
            if (r != ErrorCode::Ok || !innerSc)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedSwapChain>(innerSc);
            m_liveSwapChains.PushBack(out);
            return ErrorCode::Ok;
        }

        // ---- Mesh and RT (forwarded, validated for destroyed state) ----
        Status CreateMeshPipeline(const MeshPipelineDesc& d, MeshPipeline*& out) override
        {
            if (m_destroyed)
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Status r = m_inner->CreateMeshPipeline(d, out);
            if (r == ErrorCode::Ok && out)
                m_liveMeshPipelines.PushBack(out);
            return r;
        }
        void DestroyMeshPipeline(MeshPipeline*& p) override
        {
            if (!p) return;
            if (!removeFromList(m_liveMeshPipelines, p))
                LogWarning("[Validation] DestroyMeshPipeline: resource was not tracked (double-destroy or wrong device?)");
            m_inner->DestroyMeshPipeline(p);
            p = nullptr;
        }

        Status CreateAccelStruct(const AccelStructDesc& d, AccelStruct*& out) override
        {
            if (m_destroyed)
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Status r = m_inner->CreateAccelStruct(d, out);
            if (r == ErrorCode::Ok && out)
                m_liveAccelStructs.PushBack(out);
            return r;
        }
        void DestroyAccelStruct(AccelStruct*& a) override
        {
            if (!a) return;
            if (!removeFromList(m_liveAccelStructs, a))
                LogWarning("[Validation] DestroyAccelStruct: resource was not tracked (double-destroy or wrong device?)");
            m_inner->DestroyAccelStruct(a);
            a = nullptr;
        }

        Status CreateRayTracingPipeline(const RayTracingPipelineDesc& d,
                                        RayTracingPipeline*& out) override
        {
            if (m_destroyed)
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Status r = m_inner->CreateRayTracingPipeline(d, out);
            if (r == ErrorCode::Ok && out)
                m_liveRtPipelines.PushBack(out);
            return r;
        }
        void DestroyRayTracingPipeline(RayTracingPipeline*& p) override
        {
            if (!p) return;
            if (!removeFromList(m_liveRtPipelines, p))
                LogWarning("[Validation] DestroyRayTracingPipeline: resource was not tracked (double-destroy or wrong device?)");
            m_inner->DestroyRayTracingPipeline(p);
            p = nullptr;
        }

        Status GetShaderGroupHandles(RayTracingPipeline* p, u32 first, u32 count,
                                     Span<u8> out) override
        {
            return m_inner->GetShaderGroupHandles(p, first, count, out);
        }

        // ---- Destroy methods (with tracking removal and a not-tracked warning) ----
#define V_DESTROY(Type, method, list)                                                              \
    void method(Type*& x) override                                                                 \
    {                                                                                              \
        if (!x) return;                                                                            \
        if (!removeFromList(list, x))                                                              \
            LogWarning("[Validation] " #method ": resource was not tracked (double-destroy or wrong device?)"); \
        m_inner->method(x);                                                                        \
        x = nullptr;                                                                               \
    }

        void DestroyBuffer(BufferHandle& x) override { Destroy(m_buffers, x, "DestroyBuffer", &Device::DestroyBuffer); }
        void DestroyTexture(TextureHandle& x) override { Destroy(m_textures, x, "DestroyTexture", &Device::DestroyTexture); }
        void DestroyTextureView(TextureViewHandle& x) override
        {
            Destroy(m_views, x, "DestroyTextureView", &Device::DestroyTextureView);
        }
        void DestroySampler(SamplerHandle& x) override { Destroy(m_samplers, x, "DestroySampler", &Device::DestroySampler); }
        const BufferDesc* Describe(BufferHandle b) const override
        {
            const BufferDesc* desc = m_inner->Describe(b);
            if (desc == nullptr && b)
                m_buffers.ReportStale("Describe", b);
            return desc;
        }
        const TextureDesc* Describe(TextureHandle t) const override
        {
            const TextureDesc* desc = m_inner->Describe(t);
            if (desc == nullptr && t)
                m_textures.ReportStale("Describe", t);
            return desc;
        }
        const TextureViewDesc* Describe(TextureViewHandle v) const override
        {
            const TextureViewDesc* desc = m_inner->Describe(v);
            if (desc == nullptr && v)
                m_views.ReportStale("Describe", v);
            return desc;
        }
        TextureHandle TextureOf(TextureViewHandle v) const override
        {
            const TextureHandle texture = m_inner->TextureOf(v);
            if (!texture && v)
                m_views.ReportStale("TextureOf", v);
            return texture;
        }
        const SamplerDesc* Describe(SamplerHandle s) const override
        {
            const SamplerDesc* desc = m_inner->Describe(s);
            if (desc == nullptr && s)
                m_samplers.ReportStale("Describe", s);
            return desc;
        }
        void* Map(BufferHandle b) override
        {
            if (!Live(m_buffers, b, "Map"))
                return nullptr;
            return m_inner->Map(b);
        }
        void Unmap(BufferHandle b) override
        {
            if (Live(m_buffers, b, "Unmap"))
                m_inner->Unmap(b);
        }
        void FlushRange(BufferHandle b, u64 offset, u64 size) override
        {
            if (!Live(m_buffers, b, "FlushRange"))
                return;
            const BufferDesc* desc = m_inner->Describe(b);
            if (offset + size > desc->size)
                LogErrorf("[Validation] FlushRange: [%llu, %llu) is past the buffer's %llu bytes",
                          static_cast<unsigned long long>(offset), static_cast<unsigned long long>(offset + size),
                          static_cast<unsigned long long>(desc->size));
            m_inner->FlushRange(b, offset, size);
        }
        V_DESTROY(ShaderModule, DestroyShaderModule, m_liveShaderModules)
        V_DESTROY(BindGroupLayout, DestroyBindGroupLayout, m_liveBindGroupLayouts)
        V_DESTROY(BindGroup, DestroyBindGroup, m_liveBindGroups)
        V_DESTROY(PipelineLayout, DestroyPipelineLayout, m_livePipelineLayouts)
        V_DESTROY(PipelineCache, DestroyPipelineCache, m_livePipelineCaches)
        V_DESTROY(RenderPipeline, DestroyRenderPipeline, m_liveRenderPipelines)
        V_DESTROY(ComputePipeline, DestroyComputePipeline, m_liveComputePipelines)
        V_DESTROY(QuerySet, DestroyQuerySet, m_liveQuerySets)
#undef V_DESTROY

        void DestroyCommandPool(CommandPool*& pool) override
        {
            if (!pool)
                return;
            removeFromList(m_liveCommandPools, pool);
            auto* vp = static_cast<ValidatedCommandPool*>(pool);
            if (vp)
            {
                CommandPool* innerPool = vp->inner();
                m_inner->DestroyCommandPool(innerPool);
                m_allocator.Delete(vp);
            }
            else
                m_inner->DestroyCommandPool(pool);
            pool = nullptr;
        }

        void DestroyFence(Fence*& fence) override
        {
            if (!fence)
                return;
            removeFromList(m_liveFences, fence);
            auto* vf = static_cast<ValidatedFence*>(fence);
            if (vf)
            {
                Fence* innerFence = vf->inner();
                m_inner->DestroyFence(innerFence);
                m_allocator.Delete(vf);
            }
            else
                m_inner->DestroyFence(fence);
            fence = nullptr;
        }

        void DestroySwapChain(SwapChain*& sc) override
        {
            if (!sc)
                return;
            removeFromList(m_liveSwapChains, sc);
            auto* vs = static_cast<ValidatedSwapChain*>(sc);
            if (vs)
            {
                SwapChain* innerSc = vs->inner();
                m_inner->DestroySwapChain(innerSc);
                m_allocator.Delete(vs);
            }
            else
                m_inner->DestroySwapChain(sc);
            sc = nullptr;
        }

        void DestroySurface(Surface*& s) override { m_inner->DestroySurface(s); }

        bool IsLost() override { return m_inner->IsLost(); }

        void WaitIdle() override { m_inner->WaitIdle(); }

        void Destroy() override
        {
            if (m_destroyed)
            {
                LogError("[Validation] Device::destroy: already destroyed");
                return;
            }
            m_destroyed = true;
            reportLeaks();
            for (auto& w : m_queueWrappers)
                m_allocator.Delete(w.validated);
            m_queueWrappers.Clear();
            m_inner->Destroy();
            IAllocator& alloc = m_allocator;
            this->~ValidatedDevice();
            alloc.Free(this);
        }

    private:
        template <typename T>
        bool checkAlive(const char* method, T*& out)
        {
            if (m_destroyed)
            {
                LogErrorf("[Validation] %s: device destroyed", method);
                out = nullptr;
                return false;
            }
            return true;
        }
        bool checkAlive(const char* method)
        {
            if (m_destroyed)
                LogErrorf("[Validation] %s: device destroyed", method);
            return !m_destroyed;
        }

        template <typename T>
        bool removeFromList(Array<T>& list, const T& item)
        {
            for (usize i = 0; i < list.Size(); ++i)
            {
                if (list[i] == item)
                {
                    list.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }

        // What a device's handles of one type name: the live ones with their creation labels, for
        // leak reports, and the labels of the last ones destroyed, so a stale use soon after is
        // reported by name.
        template <typename Tag>
        class HandleRecords
        {
        public:
            using HandleType = Handle<Tag>;
            explicit HandleRecords(const char* noun) noexcept : m_noun(noun) {}

            void Made(HandleType handle, StringView label)
            {
                m_live.PushBack(handle);
                m_labels.InsertOrAssign(KeyOf(handle), label.IsEmpty() ? String(u8"(unlabelled)") : String(label));
            }
            /// False when `handle` was not live here (destroyed already, or another device's).
            bool Destroyed(HandleType handle)
            {
                bool found = false;
                for (usize i = 0; i < m_live.Size(); ++i)
                {
                    if (m_live[i] == handle)
                    {
                        m_live.RemoveAt(i);
                        found = true;
                        break;
                    }
                }
                if (const String* label = m_labels.Find(KeyOf(handle)))
                {
                    DestroyedLabel& slot = m_destroyed[m_destroyedCursor++ % kDestroyedLabelCount];
                    slot.key = KeyOf(handle);
                    slot.label = *label;
                }
                (void)m_labels.Remove(KeyOf(handle));
                return found;
            }
            [[nodiscard]] StringView LabelOf(HandleType handle) const
            {
                if (const String* label = m_labels.Find(KeyOf(handle)))
                    return label->AsView();
                for (const DestroyedLabel& destroyed : m_destroyed)
                    if (destroyed.key == KeyOf(handle) && !destroyed.label.IsEmpty())
                        return destroyed.label.AsView();
                return StringView(u8"(destroyed long ago, or another device's)");
            }
            void ReportStale(const char* method, HandleType handle) const
            {
                LogErrorf("[Validation] %s: stale %s handle (slot %u, count %u), last created as '%s'", method, m_noun,
                          handle.index, handle.Count(), reinterpret_cast<const char*>(LabelOf(handle).Data()));
            }
            void ReportLeaks() const
            {
                if (m_live.IsEmpty())
                    return;
                LogWarningf("[Validation] Device destroyed with %zu live %s(s)", m_live.Size(), m_noun);
                for (HandleType handle : m_live)
                    LogWarningf("[Validation]   live %s: %s", m_noun, reinterpret_cast<const char*>(LabelOf(handle).Data()));
            }
            [[nodiscard]] const char* Noun() const noexcept { return m_noun; }

        private:
            [[nodiscard]] static u64 KeyOf(HandleType h) noexcept { return (u64{h.index} << 32) | h.generation; }
            static constexpr u32 kDestroyedLabelCount = 1024;
            struct DestroyedLabel
            {
                u64 key = 0;
                String label;
            };
            const char* m_noun;
            Array<HandleType> m_live;
            HashMap<u64, String> m_labels; // a live handle's creation label, by KeyOf
            DestroyedLabel m_destroyed[kDestroyedLabelCount];
            u32 m_destroyedCursor = 0;
        };

        // Whether `handle` names a live resource; reported when it does not.
        template <typename Tag>
        [[nodiscard]] bool Live(const HandleRecords<Tag>& records, Handle<Tag> handle, const char* method) const
        {
            if (!handle)
            {
                LogErrorf("[Validation] %s: %s handle is null", method, records.Noun());
                return false;
            }
            if (m_inner->Describe(handle) == nullptr)
            {
                records.ReportStale(method, handle);
                return false;
            }
            return true;
        }

        template <typename Tag>
        void Destroy(HandleRecords<Tag>& records, Handle<Tag>& handle, const char* method,
                     void (Device::*destroy)(Handle<Tag>&))
        {
            if (!handle)
                return;
            if (!records.Destroyed(handle))
                LogErrorf("[Validation] %s: stale or foreign %s handle (slot %u, count %u)", method, records.Noun(),
                          handle.index, handle.Count());
            (m_inner->*destroy)(handle);
            handle = Handle<Tag>::Invalid();
        }

        void reportLeaks()
        {
            auto report = [](const char* name, usize count)
            {
                if (count > 0)
                    LogWarningf("[Validation] Device destroyed with %zu live %s(s)", count, name);
            };
            // Every leaked handle by name: the label is what a reader can grep.
            m_buffers.ReportLeaks();
            m_textures.ReportLeaks();
            m_views.ReportLeaks();
            m_samplers.ReportLeaks();
            report("ShaderModule", m_liveShaderModules.Size());
            report("BindGroupLayout", m_liveBindGroupLayouts.Size());
            report("BindGroup", m_liveBindGroups.Size());
            report("PipelineLayout", m_livePipelineLayouts.Size());
            report("PipelineCache", m_livePipelineCaches.Size());
            report("RenderPipeline", m_liveRenderPipelines.Size());
            report("ComputePipeline", m_liveComputePipelines.Size());
            report("MeshPipeline", m_liveMeshPipelines.Size());
            report("AccelStruct", m_liveAccelStructs.Size());
            report("RayTracingPipeline", m_liveRtPipelines.Size());
            report("CommandPool", m_liveCommandPools.Size());
            report("Fence", m_liveFences.Size());
            report("SwapChain", m_liveSwapChains.Size());
            report("QuerySet", m_liveQuerySets.Size());
        }

        Device* m_inner;
        IAllocator& m_allocator;
        bool m_destroyed = false;

        struct QueueWrap
        {
            Queue* raw;
            ValidatedQueue* validated;
        };
        Array<QueueWrap> m_queueWrappers;

        HandleRecords<BufferTag> m_buffers{"Buffer"};
        HandleRecords<TextureTag> m_textures{"Texture"};
        HandleRecords<TextureViewTag> m_views{"TextureView"};
        HandleRecords<SamplerTag> m_samplers{"Sampler"};
        Array<ShaderModule*> m_liveShaderModules;
        Array<BindGroupLayout*> m_liveBindGroupLayouts;
        Array<BindGroup*> m_liveBindGroups;
        Array<PipelineLayout*> m_livePipelineLayouts;
        Array<PipelineCache*> m_livePipelineCaches;
        Array<RenderPipeline*> m_liveRenderPipelines;
        Array<ComputePipeline*> m_liveComputePipelines;
        Array<MeshPipeline*> m_liveMeshPipelines;
        Array<AccelStruct*> m_liveAccelStructs;
        Array<RayTracingPipeline*> m_liveRtPipelines;
        Array<CommandPool*> m_liveCommandPools;
        Array<Fence*> m_liveFences;
        Array<SwapChain*> m_liveSwapChains;
        Array<QuerySet*> m_liveQuerySets;
    };

} // namespace foundation::rhi::validation
