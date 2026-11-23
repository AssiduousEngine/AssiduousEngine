// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Abstract Backend, Adapter, and Device interfaces.
///
/// Backend is the entry point: it enumerates GPU adapters and creates
/// presentation surfaces. Adapter represents a physical GPU. Device is
/// the central factory for all GPU resources.
///
/// Mesh shader and ray tracing creation methods are on Device directly
/// (virtual no-ops returning ErrorCode::NotSupported when not supported).
/// Callers check device->features.meshShaders or .rayTracing before use.

module;

export module foundation.rhi:device;

import foundation.core;
import :forward;
import :enums;
import :texture_format;
import :types;
import :descriptors;
import :counters;
import :ext_descriptors;
import :resources;
import :commands;
import :queue;
import :swapchain;

using namespace foundation::core;

export namespace foundation::rhi
{
    // ---- Backend ----

    /// The windowing system that produced a native window's handles. Passed to CreateSurface so the backend
    /// creates the matching platform surface (feeding an X11 Display* to the Wayland WSI segfaults).
    /// The neutral RHI counterpart of shell::WindowSystem (the RHI must not depend on the shell); the
    /// graphics and sample layers map one to the other. With Unknown the backend falls back to its
    /// best guess.
    enum class SurfacePlatform
    {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
        Android
    };

    /// RHI backend entry point (Vulkan, DX12, etc.).
    class Backend
    {
    public:
        virtual ~Backend() = default;

        bool isInitialized = false;

        /// Returns all available GPU adapters in preference order, most
        /// preferred first (discrete, then integrated, then unknown, then CPU). Backends
        /// guarantee this ordering through SortAdaptersByPreference(), so callers
        /// that just want "the best available GPU" can take element [0].
        [[nodiscard]] virtual Span<Adapter* const> EnumerateAdapters() = 0;

        /// Creates a presentation surface from a native window handle. `platform` tells the backend which WSI
        /// produced the handles so it selects the matching surface type instead of guessing (Unknown guesses).
        /// On Win32: windowHandle = HWND, displayHandle = nullptr.
        /// On X11: windowHandle = XID (as void*), displayHandle = Display*.
        /// On Wayland: windowHandle = wl_surface*, displayHandle = wl_display*.
        virtual Status CreateSurface(void* windowHandle, void* displayHandle, Surface*& out,
                                     SurfacePlatform platform = SurfacePlatform::Unknown) = 0;

        Status CreateSurface(void* windowHandle, Surface*& out)
        {
            return CreateSurface(windowHandle, nullptr, out, SurfacePlatform::Unknown);
        }

        /// Destroy the backend and all objects it owns.
        virtual void Destroy() = 0;
    };

    // ---- Adapter ----

    /// Represents a physical GPU. Query capabilities and create a logical device.
    class Adapter
    {
    public:
        virtual ~Adapter() = default;

        /// Populate adapter info (name, vendor, features, limits).
        virtual void GetInfo(AdapterInfo& out) = 0;

        /// Convenience: returns a copy of the adapter info.
        [[nodiscard]] AdapterInfo Info()
        {
            AdapterInfo i;
            GetInfo(i);
            return i;
        }

        /// Create a logical device from this adapter.
        virtual Status CreateDevice(const DeviceDesc& desc, Device*& out) = 0;
    };

    /// Selection preference for an adapter type; lower is more preferred.
    /// Defines the single source of truth for "best GPU first" ordering.
    [[nodiscard]] inline int AdapterPreferenceRank(AdapterType type)
    {
        switch (type)
        {
        case AdapterType::DiscreteGpu:
            return 0;
        case AdapterType::IntegratedGpu:
            return 1;
        case AdapterType::Unknown:
            return 2;
        case AdapterType::Cpu:
            return 3;
        }
        return 4;
    }

    /// Reorder adapters so the most preferred GPU is first (see
    /// AdapterPreferenceRank). Backends call this after enumeration so that
    /// EnumerateAdapters()[0] is the recommended default. The sort is stable,
    /// preserving the driver's native order among adapters of equal type.
    inline void SortAdaptersByPreference(Array<Adapter*>& adapters)
    {
        // Stable insertion sort by preference rank (adapter counts are tiny).
        for (usize i = 1; i < adapters.Size(); ++i)
        {
            Adapter* key = adapters[i];
            const int keyRank = AdapterPreferenceRank(key->Info().type);
            usize j = i;
            while (j > 0 && AdapterPreferenceRank(adapters[j - 1]->Info().type) > keyRank)
            {
                adapters[j] = adapters[j - 1];
                --j;
            }
            adapters[j] = key;
        }
    }

    // ---- Device ----

    /// Central factory for GPU resources, pipelines, and command infrastructure.
    class Device
    {
    public:
        virtual ~Device() = default;

        DeviceType type = DeviceType::Null;
        DeviceFeatures features{};

        // The shader format this device's CreateShaderModule expects: SPIR-V (Vulkan, Null, native
        // wgpu-native WebGPU), DXIL (DX12), or WGSL text (browser WebGPU). The shader cook/system pick
        // the cooked-blob format from this.
        [[nodiscard]] virtual ShaderFormat PreferredShaderFormat() const noexcept = 0;

        // True when this backend does not flip clip-space Y in its viewport. Vulkan and DX12 use a
        // negative-height viewport (which also inverts front-face winding); WebGPU's setViewport
        // cannot take a negative height, so it gets no such flip. Screen-space reconstruction passes
        // (sky ray, shadows, SSAO/SSR) that unproject NDC via invViewProj must negate the NDC Y (or,
        // equivalently, flip the uploaded invViewProj's Y row) when this is true.
        [[nodiscard]] virtual bool NeedsClipSpaceYFlip() const noexcept = 0;

        // ---- Queries ----
        [[nodiscard]] virtual Queue* GetQueue(QueueType type, u32 index = 0) = 0;
        [[nodiscard]] virtual u32 GetQueueCount(QueueType type) = 0;
        /// Query hardware format support for a given texture format.
        [[nodiscard]] virtual FormatSupport GetFormatSupport(TextureFormat format) = 0;
        /// The maximum MSAA sample count supported for both color and depth framebuffer attachments
        /// (scene-pass MSAA). The engine caps it at 4 (8x is not supported). 1 is no MSAA: views
        /// clamp to single-sample.
        [[nodiscard]] virtual u32 MaxColorDepthSampleCount() const noexcept = 0;
        /// Whether this sample count is usable for both color and depth render targets. The set of
        /// valid counts is not simply [1 .. MaxColorDepthSampleCount()]: WebGPU, for instance,
        /// guarantees only {1, 4} and never 2, so a caller must snap an unsupported count rather than
        /// assume every count up to the ceiling works (an unsupported count aborts texture or pipeline
        /// creation). A device without MSAA accepts only 1.
        [[nodiscard]] virtual bool SupportsSampleCount(u32 count) const noexcept = 0;

        /// What this device has recorded since it was made and the memory it holds now (the
        /// profiler's counters; DeviceCounters).
        [[nodiscard]] virtual DeviceCounters Counters() const noexcept = 0;
        /// The driver's own GPU memory figures, where it gives them (Features::memoryBudget):
        /// what the device-local heaps may hold and what this process uses of them. False, both
        /// left alone, when it does not.
        [[nodiscard]] virtual bool MemoryBudget(u64& budgetBytes, u64& usedBytes) const = 0;

        /// The bytes one query of `type` takes in the buffer ResolveQuerySet writes: 8 for a
        /// timestamp or occlusion count, a backend's own record for pipeline statistics (0 when
        /// the device has none, Features::pipelineStatisticsQueries).
        [[nodiscard]] virtual u32 QueryResultSize(QueryType queryType) const noexcept = 0;
        /// One resolved pipeline-statistics query (QueryResultSize(PipelineStatistics) bytes at
        /// `result`) in the RHI's terms; all zero without the feature.
        [[nodiscard]] virtual PipelineStatistics DecodePipelineStatistics(const void* result) const noexcept = 0;

        // ---- Resource creation ----
        virtual Status CreateBuffer(const BufferDesc& desc, BufferHandle& out) = 0;
        virtual Status CreateTexture(const TextureDesc& desc, TextureHandle& out) = 0;
        virtual Status CreateTextureView(TextureHandle texture, const TextureViewDesc& desc,
                                         TextureViewHandle& out) = 0;
        virtual Status CreateSampler(const SamplerDesc& desc, SamplerHandle& out) = 0;
        virtual Status CreateShaderModule(const ShaderModuleDesc& desc, ShaderModuleHandle& out) = 0;
        virtual Status CreateBindGroupLayout(const BindGroupLayoutDesc& desc,
                                             BindGroupLayoutHandle& out) = 0;
        virtual Status CreateBindGroup(const BindGroupDesc& desc, BindGroupHandle& out) = 0;
        virtual Status CreatePipelineLayout(const PipelineLayoutDesc& desc,
                                            PipelineLayoutHandle& out) = 0;
        virtual Status CreatePipelineCache(const PipelineCacheDesc& desc, PipelineCacheHandle& out) = 0;
        virtual Status CreateRenderPipeline(const RenderPipelineDesc& desc,
                                            RenderPipelineHandle& out) = 0;
        virtual Status CreateComputePipeline(const ComputePipelineDesc& desc,
                                             ComputePipelineHandle& out) = 0;
        virtual Status CreateCommandPool(QueueType queueType, CommandPool*& out) = 0;
        virtual Status CreateFence(u64 initialValue, Fence*& out) = 0;
        virtual Status CreateQuerySet(const QuerySetDesc& desc, QuerySetHandle& out) = 0;
        virtual Status CreateSwapChain(Surface* surface, const SwapChainDesc& desc,
                                       SwapChain*& out) = 0;

        // ---- Buffers ----
        /// What the buffer was created with; null for a stale handle.
        [[nodiscard]] virtual const BufferDesc* Describe(BufferHandle buffer) const = 0;
        /// Map the buffer for CPU access. Null if it is not mappable or the handle is stale.
        [[nodiscard]] virtual void* Map(BufferHandle buffer) = 0;
        virtual void Unmap(BufferHandle buffer) = 0;
        /// Make [offset, offset+size) of a mapped CpuToGpu buffer visible to the GPU. A real
        /// coherent mapping needs nothing; a backend that emulates mapping with a CPU shadow
        /// (WebGPU) uploads exactly that range. A caller that uses it flushes every range it wrote
        /// during the mapping; Unmap then only closes the mapping.
        virtual void FlushRange(BufferHandle buffer, u64 offset, u64 size) = 0;

        // ---- Textures, views and samplers ----
        /// What the texture was created with; null for a stale handle.
        [[nodiscard]] virtual const TextureDesc* Describe(TextureHandle texture) const = 0;
        /// What the view was created with; null for a stale handle.
        [[nodiscard]] virtual const TextureViewDesc* Describe(TextureViewHandle view) const = 0;
        /// Whether `view` names a live view. Never reported as a stale use: a cache keyed on views
        /// asks it to drop the entries of views their owners have destroyed.
        [[nodiscard]] virtual bool IsLive(TextureViewHandle view) const = 0;
        /// The texture a view is of; invalid for a stale handle.
        [[nodiscard]] virtual TextureHandle TextureOf(TextureViewHandle view) const = 0;
        /// What the sampler was created with; null for a stale handle.
        [[nodiscard]] virtual const SamplerDesc* Describe(SamplerHandle sampler) const = 0;

        // ---- Shaders, bindings, pipelines and queries ----
        /// The entry point the module was compiled for (ShaderModuleDesc::entryPoint); empty for a
        /// module whose stages name their entries, or a stale handle.
        [[nodiscard]] virtual StringView EntryPointOf(ShaderModuleHandle module) const = 0;
        /// The entries the layout was created with; empty for a stale handle.
        [[nodiscard]] virtual Span<const BindGroupLayoutEntry> LayoutEntries(BindGroupLayoutHandle layout) const = 0;
        /// The layout a bind group was created against; invalid for a stale handle.
        [[nodiscard]] virtual BindGroupLayoutHandle LayoutOf(BindGroupHandle group) const = 0;
        /// Update individual entries of a bindless bind group. Does nothing for a stale handle.
        virtual void UpdateBindless(BindGroupHandle group, Span<const BindlessUpdateEntry> entries) = 0;
        /// The bytes the cache's data takes; 0 for a stale handle.
        [[nodiscard]] virtual u32 PipelineCacheDataSize(PipelineCacheHandle cache) = 0;
        virtual Status PipelineCacheData(PipelineCacheHandle cache, Span<u8> outData) = 0;
        /// What the query set was created with; null for a stale handle.
        [[nodiscard]] virtual const QuerySetDesc* Describe(QuerySetHandle querySet) const = 0;
        /// The acceleration structure's GPU address; 0 for a stale handle.
        [[nodiscard]] virtual u64 DeviceAddress(AccelStructHandle accelStruct) const = 0;

        // ---- Debugging ----
        /// The label a resource was created with; for a stale handle, the one it had (while the device
        /// still remembers it), else empty.
        [[nodiscard]] virtual StringView Label(BufferHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(TextureHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(TextureViewHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(SamplerHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(ShaderModuleHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(BindGroupLayoutHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(BindGroupHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(PipelineLayoutHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(PipelineCacheHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(RenderPipelineHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(ComputePipelineHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(MeshPipelineHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(QuerySetHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(AccelStructHandle handle) const = 0;
        [[nodiscard]] virtual StringView Label(RayTracingPipelineHandle handle) const = 0;

        // ---- Resource destruction ----
        /// The handle (and the caller's copy) is stale at once; the buffer is freed when the GPU
        /// can no longer use it (Specs/rhi-handles.md, Decision 4).
        virtual void DestroyBuffer(BufferHandle& buf) = 0;
        /// As DestroyBuffer. A view stays usable while its texture is retired; a texture destroyed
        /// before its views is freed once the GPU is done with it, and the views then name nothing.
        virtual void DestroyTexture(TextureHandle& tex) = 0;
        virtual void DestroyTextureView(TextureViewHandle& view) = 0;
        virtual void DestroySampler(SamplerHandle& sampler) = 0;
        virtual void DestroyShaderModule(ShaderModuleHandle& module) = 0;
        virtual void DestroyBindGroupLayout(BindGroupLayoutHandle& layout) = 0;
        virtual void DestroyBindGroup(BindGroupHandle& group) = 0;
        virtual void DestroyPipelineLayout(PipelineLayoutHandle& layout) = 0;
        virtual void DestroyPipelineCache(PipelineCacheHandle& cache) = 0;
        virtual void DestroyRenderPipeline(RenderPipelineHandle& pipeline) = 0;
        virtual void DestroyComputePipeline(ComputePipelineHandle& pipeline) = 0;
        virtual void DestroyCommandPool(CommandPool*& pool) = 0;
        virtual void DestroyFence(Fence*& fence) = 0;
        virtual void DestroyQuerySet(QuerySetHandle& querySet) = 0;
        virtual void DestroySwapChain(SwapChain*& swapChain) = 0;
        virtual void DestroySurface(Surface*& surface) = 0;

        // ---- Mesh shader extension (folded into Device) ----
        /// Create a mesh shader pipeline. Returns NotSupported if mesh shaders
        /// are not enabled on this device.
        virtual Status CreateMeshPipeline(const MeshPipelineDesc& desc, MeshPipelineHandle& out) = 0;
        virtual void DestroyMeshPipeline(MeshPipelineHandle& pipeline) = 0;

        // ---- Ray tracing extension (folded into Device) ----
        /// Shader binding table handle properties. Populated by the backend
        /// during device creation when ray tracing is enabled.
        u32 shaderGroupHandleSize = 0;
        u32 shaderGroupHandleAlignment = 0;
        u32 shaderGroupBaseAlignment = 0;

        /// Create an acceleration structure. Returns NotSupported if ray tracing
        /// is not enabled on this device.
        virtual Status CreateAccelStruct(const AccelStructDesc& desc, AccelStructHandle& out) = 0;
        virtual void DestroyAccelStruct(AccelStructHandle& accelStruct) = 0;

        virtual Status CreateRayTracingPipeline(const RayTracingPipelineDesc& desc,
                                                RayTracingPipelineHandle& out) = 0;
        virtual void DestroyRayTracingPipeline(RayTracingPipelineHandle& pipeline) = 0;

        /// Retrieve shader group handles for building shader binding tables.
        virtual Status GetShaderGroupHandles(RayTracingPipelineHandle pipeline, u32 firstGroup,
                                             u32 groupCount, Span<u8> outData) = 0;

        // ---- Lifecycle ----
        /// Whether the device has been lost (GPU hang/TDR, driver reset, removal).
        /// Sticky: once lost, the device cannot recover; stop submitting work and
        /// destroy and recreate it. DX12 queries the device directly; Vulkan latches
        /// VK_ERROR_DEVICE_LOST from queue submit, present and wait results.
        [[nodiscard]] virtual bool IsLost() = 0;
        virtual void WaitIdle() = 0;
        virtual void Destroy() = 0;
    };

} // namespace foundation::rhi
