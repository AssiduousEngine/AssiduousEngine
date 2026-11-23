// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// The WebGPU device's objects by handle. Encoders, bind groups, transfer batches and the swap
/// chain hold a pointer to the device's tables and look a handle up as they record; a stale
/// handle answers null. A table holds pointers, so the objects that themselves look handles up
/// (bind groups, pipelines) are declared here and defined in their own partitions.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:tables;

import foundation.core;
import foundation.rhi;
import :buffer;
import :texture;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuShaderModule;
    class WebGpuBindGroupLayout;
    class WebGpuBindGroup;
    class WebGpuPipelineLayout;
    class WebGpuPipelineCache;
    class WebGpuRenderPipeline;
    class WebGpuComputePipeline;
    class WebGpuMeshPipeline;
    class WebGpuQuerySet;
    class WebGpuAccelStruct;
    class WebGpuRayTracingPipeline;

    using WebGpuBufferTable = ResourceTable<BufferTag, WebGpuBuffer>;
    using WebGpuTextureTable = ResourceTable<TextureTag, WebGpuTexture>;
    using WebGpuTextureViewTable = ResourceTable<TextureViewTag, WebGpuTextureView>;
    using WebGpuSamplerTable = ResourceTable<SamplerTag, WebGpuSampler>;
    using WebGpuShaderModuleTable = ResourceTable<ShaderModuleTag, WebGpuShaderModule>;
    using WebGpuBindGroupLayoutTable = ResourceTable<BindGroupLayoutTag, WebGpuBindGroupLayout>;
    using WebGpuBindGroupTable = ResourceTable<BindGroupTag, WebGpuBindGroup>;
    using WebGpuPipelineLayoutTable = ResourceTable<PipelineLayoutTag, WebGpuPipelineLayout>;
    using WebGpuPipelineCacheTable = ResourceTable<PipelineCacheTag, WebGpuPipelineCache>;
    using WebGpuRenderPipelineTable = ResourceTable<RenderPipelineTag, WebGpuRenderPipeline>;
    using WebGpuComputePipelineTable = ResourceTable<ComputePipelineTag, WebGpuComputePipeline>;
    using WebGpuMeshPipelineTable = ResourceTable<MeshPipelineTag, WebGpuMeshPipeline>;
    using WebGpuQuerySetTable = ResourceTable<QuerySetTag, WebGpuQuerySet>;
    using WebGpuAccelStructTable = ResourceTable<AccelStructTag, WebGpuAccelStruct>;
    using WebGpuRayTracingPipelineTable = ResourceTable<RayTracingPipelineTag, WebGpuRayTracingPipeline>;

    /// How each table's objects are freed (the device's functions, given the device as context).
    struct WebGpuTableFrees
    {
        SubmissionTracker::FreeFn buffer = nullptr;
        SubmissionTracker::FreeFn texture = nullptr;
        SubmissionTracker::FreeFn textureView = nullptr;
        SubmissionTracker::FreeFn sampler = nullptr;
        SubmissionTracker::FreeFn shaderModule = nullptr;
        SubmissionTracker::FreeFn bindGroupLayout = nullptr;
        SubmissionTracker::FreeFn bindGroup = nullptr;
        SubmissionTracker::FreeFn pipelineLayout = nullptr;
        SubmissionTracker::FreeFn pipelineCache = nullptr;
        SubmissionTracker::FreeFn renderPipeline = nullptr;
        SubmissionTracker::FreeFn computePipeline = nullptr;
        SubmissionTracker::FreeFn meshPipeline = nullptr;
        SubmissionTracker::FreeFn querySet = nullptr;
        SubmissionTracker::FreeFn accelStruct = nullptr;
        SubmissionTracker::FreeFn rayTracingPipeline = nullptr;
    };

    /// One table per handle type, each freeing its objects through the device (`context`).
    struct WebGpuTables
    {
        WebGpuTables(IAllocator& allocator, SubmissionTracker& tracker, void* context, const WebGpuTableFrees& frees) noexcept
            : buffers(allocator, tracker, frees.buffer, context),
              textures(allocator, tracker, frees.texture, context),
              textureViews(allocator, tracker, frees.textureView, context),
              samplers(allocator, tracker, frees.sampler, context),
              shaderModules(allocator, tracker, frees.shaderModule, context),
              bindGroupLayouts(allocator, tracker, frees.bindGroupLayout, context),
              bindGroups(allocator, tracker, frees.bindGroup, context),
              pipelineLayouts(allocator, tracker, frees.pipelineLayout, context),
              pipelineCaches(allocator, tracker, frees.pipelineCache, context),
              renderPipelines(allocator, tracker, frees.renderPipeline, context),
              computePipelines(allocator, tracker, frees.computePipeline, context),
              meshPipelines(allocator, tracker, frees.meshPipeline, context),
              querySets(allocator, tracker, frees.querySet, context),
              accelStructs(allocator, tracker, frees.accelStruct, context),
              rayTracingPipelines(allocator, tracker, frees.rayTracingPipeline, context)
        {
        }

        /// Frees what is still in them, each object before those it names.
        void FreeAll()
        {
            bindGroups.FreeAll();
            rayTracingPipelines.FreeAll();
            meshPipelines.FreeAll();
            computePipelines.FreeAll();
            renderPipelines.FreeAll();
            pipelineCaches.FreeAll();
            pipelineLayouts.FreeAll();
            bindGroupLayouts.FreeAll();
            shaderModules.FreeAll();
            querySets.FreeAll();
            accelStructs.FreeAll();
            samplers.FreeAll();
            textureViews.FreeAll();
            textures.FreeAll();
            buffers.FreeAll();
        }

        WebGpuBufferTable buffers;
        WebGpuTextureTable textures;
        WebGpuTextureViewTable textureViews;
        WebGpuSamplerTable samplers;
        WebGpuShaderModuleTable shaderModules;
        WebGpuBindGroupLayoutTable bindGroupLayouts;
        WebGpuBindGroupTable bindGroups;
        WebGpuPipelineLayoutTable pipelineLayouts;
        WebGpuPipelineCacheTable pipelineCaches;
        WebGpuRenderPipelineTable renderPipelines;
        WebGpuComputePipelineTable computePipelines;
        WebGpuMeshPipelineTable meshPipelines;
        WebGpuQuerySetTable querySets;
        WebGpuAccelStructTable accelStructs;
        WebGpuRayTracingPipelineTable rayTracingPipelines;
    };

    [[nodiscard]] inline WebGpuBuffer* LookUp(const WebGpuTables* tables, BufferHandle handle) noexcept
    {
        return tables != nullptr ? tables->buffers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuTexture* LookUp(const WebGpuTables* tables, TextureHandle handle) noexcept
    {
        return tables != nullptr ? tables->textures.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuTextureView* LookUp(const WebGpuTables* tables, TextureViewHandle handle) noexcept
    {
        return tables != nullptr ? tables->textureViews.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuSampler* LookUp(const WebGpuTables* tables, SamplerHandle handle) noexcept
    {
        return tables != nullptr ? tables->samplers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuShaderModule* LookUp(const WebGpuTables* tables, ShaderModuleHandle handle) noexcept
    {
        return tables != nullptr ? tables->shaderModules.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuBindGroupLayout* LookUp(const WebGpuTables* tables, BindGroupLayoutHandle handle) noexcept
    {
        return tables != nullptr ? tables->bindGroupLayouts.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuBindGroup* LookUp(const WebGpuTables* tables, BindGroupHandle handle) noexcept
    {
        return tables != nullptr ? tables->bindGroups.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuPipelineLayout* LookUp(const WebGpuTables* tables, PipelineLayoutHandle handle) noexcept
    {
        return tables != nullptr ? tables->pipelineLayouts.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuPipelineCache* LookUp(const WebGpuTables* tables, PipelineCacheHandle handle) noexcept
    {
        return tables != nullptr ? tables->pipelineCaches.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuRenderPipeline* LookUp(const WebGpuTables* tables, RenderPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->renderPipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuComputePipeline* LookUp(const WebGpuTables* tables, ComputePipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->computePipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuMeshPipeline* LookUp(const WebGpuTables* tables, MeshPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->meshPipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuQuerySet* LookUp(const WebGpuTables* tables, QuerySetHandle handle) noexcept
    {
        return tables != nullptr ? tables->querySets.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuAccelStruct* LookUp(const WebGpuTables* tables, AccelStructHandle handle) noexcept
    {
        return tables != nullptr ? tables->accelStructs.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuRayTracingPipeline* LookUp(const WebGpuTables* tables, RayTracingPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->rayTracingPipelines.Get(handle) : nullptr;
    }

    /// The WebGPU object a handle names, or null (a stale handle).
    [[nodiscard]] inline WGPUBuffer WgpuOf(const WebGpuTables* tables, BufferHandle handle) noexcept
    {
        const WebGpuBuffer* buffer = LookUp(tables, handle);
        return buffer != nullptr ? buffer->Handle() : nullptr;
    }
    [[nodiscard]] inline WGPUTexture WgpuOf(const WebGpuTables* tables, TextureHandle handle) noexcept
    {
        const WebGpuTexture* texture = LookUp(tables, handle);
        return texture != nullptr ? texture->Handle() : nullptr;
    }
    [[nodiscard]] inline WGPUTextureView WgpuOf(const WebGpuTables* tables, TextureViewHandle handle) noexcept
    {
        const WebGpuTextureView* view = LookUp(tables, handle);
        return view != nullptr ? view->Handle() : nullptr;
    }
    [[nodiscard]] inline WGPUSampler WgpuOf(const WebGpuTables* tables, SamplerHandle handle) noexcept
    {
        const WebGpuSampler* sampler = LookUp(tables, handle);
        return sampler != nullptr ? sampler->Handle() : nullptr;
    }
}
