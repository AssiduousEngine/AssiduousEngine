// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Forward declarations for all RHI types.

export module foundation.rhi:forward;

import foundation.core;

export namespace foundation::rhi
{
    // The handles that name a device's resources (Specs/rhi-handles.md): a slot and a generation,
    // stale once the resource is destroyed, and failing on another device.
    struct BufferTag;
    using BufferHandle = foundation::core::Handle<BufferTag>;
    struct TextureTag;
    using TextureHandle = foundation::core::Handle<TextureTag>;
    struct TextureViewTag;
    using TextureViewHandle = foundation::core::Handle<TextureViewTag>;
    struct SamplerTag;
    using SamplerHandle = foundation::core::Handle<SamplerTag>;
    struct ShaderModuleTag;
    using ShaderModuleHandle = foundation::core::Handle<ShaderModuleTag>;
    struct BindGroupLayoutTag;
    using BindGroupLayoutHandle = foundation::core::Handle<BindGroupLayoutTag>;
    struct BindGroupTag;
    using BindGroupHandle = foundation::core::Handle<BindGroupTag>;
    struct PipelineLayoutTag;
    using PipelineLayoutHandle = foundation::core::Handle<PipelineLayoutTag>;
    struct PipelineCacheTag;
    using PipelineCacheHandle = foundation::core::Handle<PipelineCacheTag>;
    struct RenderPipelineTag;
    using RenderPipelineHandle = foundation::core::Handle<RenderPipelineTag>;
    struct ComputePipelineTag;
    using ComputePipelineHandle = foundation::core::Handle<ComputePipelineTag>;
    struct MeshPipelineTag;
    using MeshPipelineHandle = foundation::core::Handle<MeshPipelineTag>;
    struct QuerySetTag;
    using QuerySetHandle = foundation::core::Handle<QuerySetTag>;
    struct AccelStructTag;
    using AccelStructHandle = foundation::core::Handle<AccelStructTag>;
    struct RayTracingPipelineTag;
    using RayTracingPipelineHandle = foundation::core::Handle<RayTracingPipelineTag>;

    class Backend;
    class Adapter;
    class Device;
    class Queue;
    class Surface;
    class SwapChain;
    class Buffer;
    class Texture;
    class TextureView;
    class Sampler;
    class ShaderModule;
    class Fence;
    class QuerySet;
    class BindGroupLayout;
    class BindGroup;
    class PipelineLayout;
    class PipelineCache;
    class RenderPipeline;
    class ComputePipeline;
    class MeshPipeline;
    class AccelStruct;
    class RayTracingPipeline;
    class CommandBuffer;
    class CommandPool;
    class CommandEncoder;
    class RenderCommandEncoder;
    class RenderPassEncoder;
    class RenderBundleEncoder;
    class RenderBundle;
    class ComputePassEncoder;
    class TransferBatch;

    // Extension base classes (for multiple inheritance).
    class MeshShaderPassExt;
    class RayTracingEncoderExt;

} // namespace foundation::rhi
