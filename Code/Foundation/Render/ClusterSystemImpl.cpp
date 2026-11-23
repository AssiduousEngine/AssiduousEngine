// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:cluster_system` partition.
///
/// Clustered light culling. Bins lights into a 3D froxel grid (screen tiles by
/// logarithmic depth slices) once per frame via a compute pass, so the forward shader evaluates
/// only the lights touching each fragment's cluster instead of all lights. This partition owns the
/// build compute pipeline and the per-frame cluster buffers, and declares the build pass into the
/// frame graph (ordered before the forward pass, which reads the buffers it writes).

module;
#include "Core/Prelude.h"

module foundation.render;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data;
import :views;
import :resources;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    Status ClusterSystem::Initialize()
    {
        rhi::ShaderModuleHandle cs = m_shaders->GetVariant(
            u8"cluster_build", shaders::ShaderStage::Compute, shaders::ShaderFlags::None);
        if (!cs.IsValid())
        {
            return Status{ErrorCode::Unknown};
        }

        // set 0: build params UBO (b0, dynamic), lights SRV (t0), per-cluster offsets (u0) and the
        // flat light-index list (u1). DXC shifts keep b0, t0, u0 and u1 from colliding in SPIR-V.
        rhi::BindGroupLayoutEntry paramsEntry =
            rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Compute);
        paramsEntry.hasDynamicOffset = true;
        rhi::BindGroupLayoutEntry lightsEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Compute, /*readOnly*/ true,
            /*stride*/ sizeof(GpuLight)); // StructuredBuffer<GpuLight>
        rhi::BindGroupLayoutEntry offsetsEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Compute, /*readOnly*/ false,
            /*stride*/ 8); // RWStructuredBuffer<uint2>
        rhi::BindGroupLayoutEntry indicesEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            1, rhi::ShaderStage::Compute, /*readOnly*/ false,
            /*stride*/ 4); // RWStructuredBuffer<uint>
        rhi::BindGroupLayoutEntry set0[] = {paramsEntry, lightsEntry, offsetsEntry, indicesEntry};
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{set0, 4};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutHandle layouts[] = {m_layout};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::ComputePipelineDesc cpd{};
        cpd.layout = m_pipelineLayout;
        cpd.compute = rhi::StageOf(cs, rhi::ShaderStage::Compute);
        cpd.label = u8"cluster.build";
        if (!m_device->CreateComputePipeline(cpd, m_pipeline).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void ClusterSystem::PrepareFrame(u32 frameIndex, u32 viewCount)
    {
        // Hot reload: rebuild the compute pipeline when the shader changed (the GPU is
        // idle on reload).
        const u64 shaderVersion = m_shaders->Version(u8"cluster_build");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            rhi::ShaderModuleHandle cs = m_shaders->GetVariant(
                u8"cluster_build", shaders::ShaderStage::Compute, shaders::ShaderFlags::None);
            if (cs.IsValid())
            {
                if (m_pipeline.IsValid())
                {
                    m_device->DestroyComputePipeline(m_pipeline);
                    m_pipeline = {};
                }
                rhi::ComputePipelineDesc cpd{};
                cpd.layout = m_pipelineLayout;
                cpd.compute = rhi::StageOf(cs, rhi::ShaderStage::Compute);
                cpd.label = u8"cluster.build";
                if (!m_device->CreateComputePipeline(cpd, m_pipeline).IsOk())
                {
                    m_pipeline = {};
                }
            }
            m_pipelineShaderVersion = shaderVersion;
        }
        const u32 views = viewCount == 0 ? 1u : viewCount;
        m_ready = m_paramsRing.Reserve(views) && m_lightRing.Reserve(kMaxLights * views);
        if (m_ready)
        {
            m_paramsRing.BeginFrame(frameIndex);
            m_lightRing.BeginFrame(frameIndex);
        }
    }

    ClusterBinding ClusterSystem::DeclareBuild(rendergraph::RenderGraph& graph,
                                               const RenderView& view, u32 frameIndex,
                                               u32 viewIndex)
    {
        ClusterBinding binding;
        if (!m_ready || !m_pipeline.IsValid() || view.Width() == 0 || view.Height() == 0)
        {
            return binding;
        }
        // The grid covers the viewport (not the full target) in viewport-local screen space; the forward
        // pass subtracts the viewport offset from SV_Position before tiling.
        const u32 gridX = (view.ViewportWidth() + kTileSize - 1) / kTileSize;
        const u32 gridY = (view.ViewportHeight() + kTileSize - 1) / kTileSize;
        const u32 totalClusters = gridX * gridY * kSliceCount;
        if (totalClusters == 0)
        {
            return binding;
        }

        // Own buffers per (view, frame-in-flight): two views in one frame must not share a slot.
        Slot& slot = m_slots.At(viewIndex, frameIndex);
        // The shared list: per cluster as many as the view's lights, to the average (plus the
        // directional lights, which reach every cluster) past that; a region of it per group.
        const Span<const GpuLight> sceneLights =
            (view.Scene() != nullptr) ? view.Scene()->Lights() : Span<const GpuLight>{};
        const u32 viewLights = Min(static_cast<u32>(sceneLights.Size()), kMaxLights);
        u32 directional = 0;
        for (usize i = 0; i < static_cast<usize>(viewLights); ++i)
        {
            directional += sceneLights[i].type < 0.5f ? 1u : 0u;
        }
        const u32 perCluster = Max(1u, Min(Min(viewLights, kAveragePerCluster + directional), kMaxPerCluster));
        const u32 groupCount = (totalClusters + kGroupClusters - 1) / kGroupClusters;
        const u32 indexCapacity = groupCount * kGroupClusters * perCluster;
        if (!EnsureBuffers(slot, totalClusters, indexCapacity))
        {
            return binding;
        }

        // The camera's own planes. The slices are logarithmic in view depth, so the near one stays
        // in front of the eye (an orthographic projection's near plane may sit at or behind it).
        const f32 nearZ = Max(view.Camera().NearZ(), 0.01f);
        const f32 farZ = Max(view.Camera().FarZ(), nearZ + 1.0f);
        // slice = SliceCount * log(depth/near) / log(far/near), the inverse of GetSliceDepth in the
        // kernel. Without the SliceCount factor every depth collapses to slice 0.
        const f32 logScale = static_cast<f32>(kSliceCount) / Log(farZ / nearZ);
        const f32 logBias = -Log(nearZ) * logScale;

        // Upload this view's lights into the cluster light buffer. It is a separate copy because the
        // build runs before the forward pass uploads its lights; both use the same order, so the stored
        // indices are valid for both.
        const Span<const GpuLight> lights =
            (view.Scene() != nullptr) ? view.Scene()->Lights() : Span<const GpuLight>{};
        u32 lightCount = static_cast<u32>(lights.Size());
        if (lightCount > kMaxLights)
        {
            lightCount = kMaxLights;
        }
        u32 lightOffset = 0;
        if (lightCount > 0)
        {
            const DynamicUniformRing::Range lr = m_lightRing.AllocateRange(lightCount);
            if (!lr.ok)
            {
                return binding;
            }
            MemCopy(lr.ptr, lights.Data(), static_cast<usize>(lightCount) * sizeof(GpuLight));
            lightOffset = lr.slotIndex;
        }

        // Upload this view's build params (a dynamic-offset slot in the params ring).
        const DynamicUniformRing::Range pr = m_paramsRing.Allocate();
        if (!pr.ok)
        {
            return binding;
        }
        GpuClusterBuildParams* bp = static_cast<GpuClusterBuildParams*>(pr.ptr);
        *bp = GpuClusterBuildParams{};
        bp->gridX = gridX;
        bp->gridY = gridY;
        bp->sliceCount = kSliceCount;
        bp->tileSize = kTileSize;
        bp->nearZ = nearZ;
        bp->farZ = farZ;
        bp->logScale = logScale;
        bp->logBias = logBias;
        bp->lightCount = lightCount;
        bp->lightOffset = lightOffset;
        bp->viewMatrix = view.Camera().view;
        bp->invProjection = Inverse(view.Camera().projection);
        bp->orthographic = projection::IsOrthographic(view.Camera().projection) ? 1u : 0u;
        bp->slotsPerCluster = perCluster;

        rhi::BufferHandle offsets = slot.offsets;
        rhi::BufferHandle indices = slot.indices;
        if (!EnsureBindGroup(slot))
        {
            return binding;
        }

        const u32 paramsOffset = pr.byteOffset;
        const u32 groups = (totalClusters + 63u) / 64u;
        rhi::BindGroupHandle bg = slot.bindGroup;
        rhi::ComputePipelineHandle pipeline = m_pipeline;

        const rendergraph::RGHandle offsetsH = graph.ImportBuffer(u8"cluster.offsets", offsets);
        const rendergraph::RGHandle indicesH = graph.ImportBuffer(u8"cluster.indices", indices);
        graph.AddComputePass(
            u8"cluster.build",
            [=](rendergraph::PassBuilder& b)
            {
                b.WriteStorage(offsetsH);
                b.WriteStorage(indicesH);
                b.HasSideEffects(); // keep the pass alive even when no reader of the buffers is declared
                b.SetComputeExecute(
                    [=](rhi::ComputePassEncoder& cp)
                    {
                        cp.SetPipeline(pipeline);
                        cp.SetBindGroup(0, bg, Span<const u32>{&paramsOffset, 1});
                        cp.Dispatch(groups, 1, 1);
                    });
            });

        binding.offsets = offsets;
        binding.lightIndices = indices;
        binding.version = slot.bufferVersion;
        binding.offsetsHandle = offsetsH;
        binding.indicesHandle = indicesH;
        binding.gridX = gridX;
        binding.gridY = gridY;
        binding.sliceCount = kSliceCount;
        binding.tileSize = kTileSize;
        binding.viewportX = view.ViewportX();
        binding.viewportY = view.ViewportY();
        binding.nearZ = nearZ;
        binding.farZ = farZ;
        binding.logScale = logScale;
        binding.logBias = logBias;
        return binding;
    }

    bool ClusterSystem::EnsureBuffers(Slot& slot, u32 totalClusters, u32 indexCapacity)
    {
        const u64 offsetsBytes =
            static_cast<u64>(totalClusters) * sizeof(u32) * 2; // uint2 per cluster
        const u64 indicesBytes = static_cast<u64>(indexCapacity) * sizeof(u32); // the shared list
        if (slot.offsets.IsValid() && offsetsBytes <= slot.offsetsBytes && indicesBytes <= slot.indicesBytes)
        {
            return true;
        }

        // Grow this slot. In-flight frames may still read the old buffers and the bind
        // group over them, so retire them through the queue when one is wired (on web a
        // mid-frame WaitIdle pumps the event loop, expires the canvas texture and drops
        // the whole frame's submit); otherwise drain (standalone and test use).
        const bool replacing = slot.offsets.IsValid() || slot.indices.IsValid();
        if (replacing && m_retire == nullptr)
        {
            m_device->WaitIdle();
        }
        if (slot.offsets.IsValid())
        {
            if (m_retire != nullptr)
            {
                m_retire->Retire(slot.offsets);
            }
            else
            {
                m_device->DestroyBuffer(slot.offsets);
            }
            slot.offsets = {};
        }
        if (slot.indices.IsValid())
        {
            if (m_retire != nullptr)
            {
                m_retire->Retire(slot.indices);
            }
            else
            {
                m_device->DestroyBuffer(slot.indices);
            }
            slot.indices = {};
        }
        rhi::BufferDesc obd{};
        obd.size = offsetsBytes;
        obd.usage = rhi::BufferUsage::Storage;
        obd.memory = rhi::MemoryLocation::GpuOnly;
        obd.label = u8"cluster.offsets";
        if (!m_device->CreateBuffer(obd, slot.offsets).IsOk())
        {
            slot.offsets = {};
            return false;
        }
        rhi::BufferDesc ibd{};
        ibd.size = indicesBytes;
        ibd.usage = rhi::BufferUsage::Storage;
        ibd.memory = rhi::MemoryLocation::GpuOnly;
        ibd.label = u8"cluster.indices";
        if (!m_device->CreateBuffer(ibd, slot.indices).IsOk())
        {
            slot.indices = {};
            return false;
        }

        slot.offsetsBytes = offsetsBytes;
        slot.indicesBytes = indicesBytes;
        ++slot.bufferVersion; // signal consumers to rebuild cached bind groups (the address may be reused)
        // The slot's bind group referenced the old buffers; retire or drop it with them.
        if (slot.bindGroup.IsValid())
        {
            if (m_retire != nullptr)
            {
                m_retire->Retire(slot.bindGroup);
            }
            else
            {
                m_device->DestroyBindGroup(slot.bindGroup);
            }
            slot.bindGroup = {};
        }
        slot.bgOffsets = {};
        slot.bgIndices = {};
        return true;
    }

    bool ClusterSystem::EnsureBindGroup(Slot& slot)
    {
        rhi::BufferHandle offsets = slot.offsets;
        rhi::BufferHandle indices = slot.indices;
        rhi::BufferHandle lights = m_lightRing.Buffer();
        const bool stable = slot.bindGroup.IsValid() &&
                            slot.bgParamsGen == m_paramsRing.Generation() &&
                            slot.bgLightGen == m_lightRing.Generation() &&
                            slot.bgOffsets == offsets && slot.bgIndices == indices;
        if (stable)
        {
            return true;
        }
        if (slot.bindGroup.IsValid())
        {
            m_device->DestroyBindGroup(slot.bindGroup);
            slot.bindGroup = {};
        }
        rhi::BufferHandle params = m_paramsRing.Buffer();
        if (!params.IsValid() || !lights.IsValid() || !offsets.IsValid() || !indices.IsValid())
        {
            return false;
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(params, 0, sizeof(GpuClusterBuildParams)),
            rhi::BindGroupEntry::BufferEntry(lights, 0, m_lightRing.ByteCapacity()),
            rhi::BindGroupEntry::BufferEntry(offsets, 0, slot.offsetsBytes),
            rhi::BindGroupEntry::BufferEntry(indices, 0, slot.indicesBytes),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 4};
        if (!m_device->CreateBindGroup(bgd, slot.bindGroup).IsOk())
        {
            slot.bindGroup = {};
            return false;
        }
        slot.bgParamsGen = m_paramsRing.Generation();
        slot.bgLightGen = m_lightRing.Generation();
        slot.bgOffsets = offsets;
        slot.bgIndices = indices;
        return true;
    }

    void ClusterSystem::Shutdown()
    {
        for (Slot& slot : m_slots.All())
        {
            if (slot.bindGroup.IsValid())
            {
                m_device->DestroyBindGroup(slot.bindGroup);
            }
            if (slot.offsets.IsValid())
            {
                m_device->DestroyBuffer(slot.offsets);
            }
            if (slot.indices.IsValid())
            {
                m_device->DestroyBuffer(slot.indices);
            }
        }
        m_slots.Clear();
        if (m_pipeline.IsValid())
        {
            m_device->DestroyComputePipeline(m_pipeline);
            m_pipeline = {};
        }
        if (m_pipelineLayout.IsValid())
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = {};
        }
        if (m_layout.IsValid())
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = {};
        }
    }
}
