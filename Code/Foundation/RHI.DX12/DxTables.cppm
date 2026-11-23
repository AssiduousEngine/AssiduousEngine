// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// The D3D12 device's objects by handle. Encoders, bind groups, transfer batches and the swap
/// chain hold a pointer to the device's tables and look a handle up as they record; a stale
/// handle answers null. A table holds pointers, so the objects that themselves look handles up
/// (bind groups, pipelines) are declared here and defined in their own partitions.

module;
#include "Core/Prelude.h"

export module foundation.rhi.dx12:tables;

import foundation.core;
import foundation.rhi;
import :buffer;
import :texture;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{
    class DxShaderModuleImpl;
    class DxBindGroupLayoutImpl;
    class DxBindGroupImpl;
    class DxPipelineLayoutImpl;
    class DxPipelineCacheImpl;
    class DxRenderPipelineImpl;
    class DxComputePipelineImpl;
    class DxMeshPipelineImpl;
    class DxQuerySetImpl;
    class DxAccelStructImpl;
    class DxRayTracingPipelineImpl;

    using DxBufferTable = ResourceTable<BufferTag, DxBufferImpl>;
    using DxTextureTable = ResourceTable<TextureTag, DxTextureImpl>;
    using DxTextureViewTable = ResourceTable<TextureViewTag, DxTextureViewImpl>;
    using DxSamplerTable = ResourceTable<SamplerTag, DxSamplerImpl>;
    using DxShaderModuleTable = ResourceTable<ShaderModuleTag, DxShaderModuleImpl>;
    using DxBindGroupLayoutTable = ResourceTable<BindGroupLayoutTag, DxBindGroupLayoutImpl>;
    using DxBindGroupTable = ResourceTable<BindGroupTag, DxBindGroupImpl>;
    using DxPipelineLayoutTable = ResourceTable<PipelineLayoutTag, DxPipelineLayoutImpl>;
    using DxPipelineCacheTable = ResourceTable<PipelineCacheTag, DxPipelineCacheImpl>;
    using DxRenderPipelineTable = ResourceTable<RenderPipelineTag, DxRenderPipelineImpl>;
    using DxComputePipelineTable = ResourceTable<ComputePipelineTag, DxComputePipelineImpl>;
    using DxMeshPipelineTable = ResourceTable<MeshPipelineTag, DxMeshPipelineImpl>;
    using DxQuerySetTable = ResourceTable<QuerySetTag, DxQuerySetImpl>;
    using DxAccelStructTable = ResourceTable<AccelStructTag, DxAccelStructImpl>;
    using DxRayTracingPipelineTable = ResourceTable<RayTracingPipelineTag, DxRayTracingPipelineImpl>;

    /// How each table's objects are freed (the device's functions, given the device as context).
    struct DxTableFrees
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
    struct DxTables
    {
        DxTables(IAllocator& allocator, SubmissionTracker& tracker, void* context, const DxTableFrees& frees) noexcept
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

        DxBufferTable buffers;
        DxTextureTable textures;
        DxTextureViewTable textureViews;
        DxSamplerTable samplers;
        DxShaderModuleTable shaderModules;
        DxBindGroupLayoutTable bindGroupLayouts;
        DxBindGroupTable bindGroups;
        DxPipelineLayoutTable pipelineLayouts;
        DxPipelineCacheTable pipelineCaches;
        DxRenderPipelineTable renderPipelines;
        DxComputePipelineTable computePipelines;
        DxMeshPipelineTable meshPipelines;
        DxQuerySetTable querySets;
        DxAccelStructTable accelStructs;
        DxRayTracingPipelineTable rayTracingPipelines;
    };

    [[nodiscard]] inline DxBufferImpl* LookUp(const DxTables* tables, BufferHandle handle) noexcept
    {
        return tables != nullptr ? tables->buffers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxTextureImpl* LookUp(const DxTables* tables, TextureHandle handle) noexcept
    {
        return tables != nullptr ? tables->textures.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxTextureViewImpl* LookUp(const DxTables* tables, TextureViewHandle handle) noexcept
    {
        return tables != nullptr ? tables->textureViews.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxSamplerImpl* LookUp(const DxTables* tables, SamplerHandle handle) noexcept
    {
        return tables != nullptr ? tables->samplers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxShaderModuleImpl* LookUp(const DxTables* tables, ShaderModuleHandle handle) noexcept
    {
        return tables != nullptr ? tables->shaderModules.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxBindGroupLayoutImpl* LookUp(const DxTables* tables, BindGroupLayoutHandle handle) noexcept
    {
        return tables != nullptr ? tables->bindGroupLayouts.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxBindGroupImpl* LookUp(const DxTables* tables, BindGroupHandle handle) noexcept
    {
        return tables != nullptr ? tables->bindGroups.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxPipelineLayoutImpl* LookUp(const DxTables* tables, PipelineLayoutHandle handle) noexcept
    {
        return tables != nullptr ? tables->pipelineLayouts.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxPipelineCacheImpl* LookUp(const DxTables* tables, PipelineCacheHandle handle) noexcept
    {
        return tables != nullptr ? tables->pipelineCaches.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxRenderPipelineImpl* LookUp(const DxTables* tables, RenderPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->renderPipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxComputePipelineImpl* LookUp(const DxTables* tables, ComputePipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->computePipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxMeshPipelineImpl* LookUp(const DxTables* tables, MeshPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->meshPipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxQuerySetImpl* LookUp(const DxTables* tables, QuerySetHandle handle) noexcept
    {
        return tables != nullptr ? tables->querySets.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxAccelStructImpl* LookUp(const DxTables* tables, AccelStructHandle handle) noexcept
    {
        return tables != nullptr ? tables->accelStructs.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxRayTracingPipelineImpl* LookUp(const DxTables* tables, RayTracingPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->rayTracingPipelines.Get(handle) : nullptr;
    }
}
