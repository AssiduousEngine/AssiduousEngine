// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// The Vulkan device's objects by handle. Encoders, bind groups, transfer batches and the swap
/// chain hold a pointer to the device's tables and look a handle up as they record; a stale
/// handle answers null. A table holds pointers, so the objects that themselves look handles up
/// (bind groups, pipelines) are declared here and defined in their own partitions.

module;
#include "Core/Prelude.h"

export module foundation.rhi.vulkan:tables;

import foundation.core;
import foundation.rhi;
import :buffer;
import :texture;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::vk
{
    class VkShaderModuleImpl;
    class VkBindGroupLayoutImpl;
    class VkBindGroupImpl;
    class VkPipelineLayoutImpl;
    class VkPipelineCacheImpl;
    class VkRenderPipelineImpl;
    class VkComputePipelineImpl;
    class VkMeshPipelineImpl;
    class VkQuerySetImpl;
    class VkAccelStructImpl;
    class VkRayTracingPipelineImpl;

    using VkBufferTable = ResourceTable<BufferTag, VkBufferImpl>;
    using VkTextureTable = ResourceTable<TextureTag, VkTextureImpl>;
    using VkTextureViewTable = ResourceTable<TextureViewTag, VkTextureViewImpl>;
    using VkSamplerTable = ResourceTable<SamplerTag, VkSamplerImpl>;
    using VkShaderModuleTable = ResourceTable<ShaderModuleTag, VkShaderModuleImpl>;
    using VkBindGroupLayoutTable = ResourceTable<BindGroupLayoutTag, VkBindGroupLayoutImpl>;
    using VkBindGroupTable = ResourceTable<BindGroupTag, VkBindGroupImpl>;
    using VkPipelineLayoutTable = ResourceTable<PipelineLayoutTag, VkPipelineLayoutImpl>;
    using VkPipelineCacheTable = ResourceTable<PipelineCacheTag, VkPipelineCacheImpl>;
    using VkRenderPipelineTable = ResourceTable<RenderPipelineTag, VkRenderPipelineImpl>;
    using VkComputePipelineTable = ResourceTable<ComputePipelineTag, VkComputePipelineImpl>;
    using VkMeshPipelineTable = ResourceTable<MeshPipelineTag, VkMeshPipelineImpl>;
    using VkQuerySetTable = ResourceTable<QuerySetTag, VkQuerySetImpl>;
    using VkAccelStructTable = ResourceTable<AccelStructTag, VkAccelStructImpl>;
    using VkRayTracingPipelineTable = ResourceTable<RayTracingPipelineTag, VkRayTracingPipelineImpl>;

    /// How each table's objects are freed (the device's functions, given the device as context).
    struct VkTableFrees
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
    struct VkTables
    {
        VkTables(IAllocator& allocator, SubmissionTracker& tracker, void* context, const VkTableFrees& frees) noexcept
            : buffers(allocator, tracker, frees.buffer, context, "Buffer"),
              textures(allocator, tracker, frees.texture, context, "Texture"),
              textureViews(allocator, tracker, frees.textureView, context, "TextureView"),
              samplers(allocator, tracker, frees.sampler, context, "Sampler"),
              shaderModules(allocator, tracker, frees.shaderModule, context, "ShaderModule"),
              bindGroupLayouts(allocator, tracker, frees.bindGroupLayout, context, "BindGroupLayout"),
              bindGroups(allocator, tracker, frees.bindGroup, context, "BindGroup"),
              pipelineLayouts(allocator, tracker, frees.pipelineLayout, context, "PipelineLayout"),
              pipelineCaches(allocator, tracker, frees.pipelineCache, context, "PipelineCache"),
              renderPipelines(allocator, tracker, frees.renderPipeline, context, "RenderPipeline"),
              computePipelines(allocator, tracker, frees.computePipeline, context, "ComputePipeline"),
              meshPipelines(allocator, tracker, frees.meshPipeline, context, "MeshPipeline"),
              querySets(allocator, tracker, frees.querySet, context, "QuerySet"),
              accelStructs(allocator, tracker, frees.accelStruct, context, "AccelStruct"),
              rayTracingPipelines(allocator, tracker, frees.rayTracingPipeline, context, "RayTracingPipeline")
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

        VkBufferTable buffers;
        VkTextureTable textures;
        VkTextureViewTable textureViews;
        VkSamplerTable samplers;
        VkShaderModuleTable shaderModules;
        VkBindGroupLayoutTable bindGroupLayouts;
        VkBindGroupTable bindGroups;
        VkPipelineLayoutTable pipelineLayouts;
        VkPipelineCacheTable pipelineCaches;
        VkRenderPipelineTable renderPipelines;
        VkComputePipelineTable computePipelines;
        VkMeshPipelineTable meshPipelines;
        VkQuerySetTable querySets;
        VkAccelStructTable accelStructs;
        VkRayTracingPipelineTable rayTracingPipelines;
    };

    [[nodiscard]] inline VkBufferImpl* LookUp(const VkTables* tables, BufferHandle handle) noexcept
    {
        return tables != nullptr ? tables->buffers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkTextureImpl* LookUp(const VkTables* tables, TextureHandle handle) noexcept
    {
        return tables != nullptr ? tables->textures.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkTextureViewImpl* LookUp(const VkTables* tables, TextureViewHandle handle) noexcept
    {
        return tables != nullptr ? tables->textureViews.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkSamplerImpl* LookUp(const VkTables* tables, SamplerHandle handle) noexcept
    {
        return tables != nullptr ? tables->samplers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkShaderModuleImpl* LookUp(const VkTables* tables, ShaderModuleHandle handle) noexcept
    {
        return tables != nullptr ? tables->shaderModules.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkBindGroupLayoutImpl* LookUp(const VkTables* tables, BindGroupLayoutHandle handle) noexcept
    {
        return tables != nullptr ? tables->bindGroupLayouts.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkBindGroupImpl* LookUp(const VkTables* tables, BindGroupHandle handle) noexcept
    {
        return tables != nullptr ? tables->bindGroups.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkPipelineLayoutImpl* LookUp(const VkTables* tables, PipelineLayoutHandle handle) noexcept
    {
        return tables != nullptr ? tables->pipelineLayouts.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkPipelineCacheImpl* LookUp(const VkTables* tables, PipelineCacheHandle handle) noexcept
    {
        return tables != nullptr ? tables->pipelineCaches.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkRenderPipelineImpl* LookUp(const VkTables* tables, RenderPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->renderPipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkComputePipelineImpl* LookUp(const VkTables* tables, ComputePipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->computePipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkMeshPipelineImpl* LookUp(const VkTables* tables, MeshPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->meshPipelines.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkQuerySetImpl* LookUp(const VkTables* tables, QuerySetHandle handle) noexcept
    {
        return tables != nullptr ? tables->querySets.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkAccelStructImpl* LookUp(const VkTables* tables, AccelStructHandle handle) noexcept
    {
        return tables != nullptr ? tables->accelStructs.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkRayTracingPipelineImpl* LookUp(const VkTables* tables, RayTracingPipelineHandle handle) noexcept
    {
        return tables != nullptr ? tables->rayTracingPipelines.Get(handle) : nullptr;
    }
}
