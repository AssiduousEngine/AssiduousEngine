// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Render - foundation.render:mesh_renderer implementation unit.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h"

module foundation.render;

import foundation.core;
import foundation.lod;
import foundation.rhi;
import foundation.geometry;
import foundation.shaders;
import foundation.shaders.system;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.profiler;
import :data;
import :views;
import :pipeline;
import :cluster_system;
import :resources;
import :gpu_mesh;

using namespace foundation::core;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    Status MeshRenderer::Initialize()
    {
        // set 0: per-view UBO (ViewProj, camera, light range), dynamic offset, Vertex|Fragment;
        // and the light list as a read-only StructuredBuffer (Fragment), bound whole.
        rhi::BindGroupLayoutEntry viewEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
            0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
        viewEntry.hasDynamicOffset = true;
        rhi::BindGroupLayoutEntry lightEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ sizeof(GpuLight)); // StructuredBuffer<GpuLight>
        // Directional shadow map (t1) and comparison sampler (s0) live in set 0 (the bind-group budget
        // is 4 sets, not 4 bindings, so shadows fold into the view set rather than needing a 5th set).
        rhi::BindGroupLayoutEntry shadowTexEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            1, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray);
        shadowTexEntry.textureSampleType = rhi::TextureSampleType::Depth; // SampleCmp source
        // Local-light (spot and point) shadow atlas (t2, Texture2D) and per-light shadow entries (t3, SRV).
        rhi::BindGroupLayoutEntry atlasTexEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            2, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray);
        atlasTexEntry.textureSampleType = rhi::TextureSampleType::Depth; // SampleCmp source
        rhi::BindGroupLayoutEntry localShadowEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            3, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ sizeof(GpuLocalShadow)); // StructuredBuffer<GpuLocalShadow>
        rhi::BindGroupLayoutEntry shadowSampEntry{};
        shadowSampEntry.binding = 0;
        shadowSampEntry.visibility = rhi::ShaderStage::Fragment;
        shadowSampEntry.type = rhi::BindingType::ComparisonSampler;
        // t4: the GPU skinning bone-matrix pool (Vertex-visible SRV). Bound on every set-0 BG; the
        // forward VS only reads it under the SKINNED permutation.
        rhi::BindGroupLayoutEntry boneEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            4, rhi::ShaderStage::Vertex, /*readOnly*/ true,
            /*stride*/ sizeof(Float4x4)); // StructuredBuffer<BoneMatrix> (4x float4)
        // IBL folds into set 0 too: SH9 diffuse coefficients (t5, SRV), prefiltered specular cube
        // (t6), BRDF LUT (t7) and a linear-clamp env sampler (s1, distinct from the comparison sampler s0).
        rhi::BindGroupLayoutEntry iblShEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            5, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 16); // StructuredBuffer<float4> IblSH
        rhi::BindGroupLayoutEntry prefilterEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            6, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCube);
        rhi::BindGroupLayoutEntry brdfEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            7, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2D);
        rhi::BindGroupLayoutEntry envSampEntry =
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment);
        // Reflection probes: a cube array of local probe radiance (t8) and a probe metadata buffer (t9, SRV).
        rhi::BindGroupLayoutEntry probeEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            8, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCubeArray);
        rhi::BindGroupLayoutEntry probeBufEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            9, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ sizeof(GpuProbe)); // StructuredBuffer<GpuProbe> (ReflectionProbeSystem)
        rhi::BindGroupLayoutEntry set0[] = {
            viewEntry,       lightEntry, shadowTexEntry, atlasTexEntry,  localShadowEntry,
            shadowSampEntry, boneEntry,  iblShEntry,     prefilterEntry, brdfEntry,
            envSampEntry,    probeEntry, probeBufEntry};
        rhi::BindGroupLayoutDesc s0d{};
        s0d.entries = Span<const rhi::BindGroupLayoutEntry>{set0, 13};
        if (!m_device->CreateBindGroupLayout(s0d, m_viewLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // set 1 (instanced): per-instance StructuredBuffer (read-only storage) and the draw's
        // InstanceSet block (its fade window, instance count and set pick id).
        {
            const rhi::BindGroupLayoutEntry instEntries[] = {
                rhi::BindGroupLayoutEntry::StorageBuffer(0, rhi::ShaderStage::Vertex, /*readOnly*/ true,
                                                         /*stride*/ sizeof(GpuInstance)), // StructuredBuffer<InstanceData>
                rhi::BindGroupLayoutEntry::UniformBuffer(1, rhi::ShaderStage::Vertex),    // cbuffer InstanceSet
            };
            rhi::BindGroupLayoutDesc d{};
            d.entries = Span<const rhi::BindGroupLayoutEntry>{instEntries, 2};
            if (!m_device->CreateBindGroupLayout(d, m_instanceLayout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
        }
        // The InstanceSet every per-frame instanced batch binds: no fade, each instance its own id.
        {
            rhi::BufferDesc bd{};
            bd.size = sizeof(GpuInstanceSet);
            bd.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"mesh.instance-set.unfaded";
            if (!m_device->CreateBuffer(bd, m_noFadeSetBuf).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            if (auto* p = static_cast<GpuInstanceSet*>(m_device->Map(m_noFadeSetBuf)))
            {
                *p = GpuInstanceSet{};
                m_device->Unmap(m_noFadeSetBuf);
            }
        }

        // set 2 (material) is not a fixed renderer layout: it is derived per material from the
        // material's own property list (materials::MaterialSystem::GetOrCreateLayout), and the forward
        // pipeline layout is assembled per set-2 layout (GetOrCreatePipelineLayout). A custom shader
        // with a different property set therefore gets its own set-2 layout and PSO without renderer
        // changes. The set 0, 1 and 3 layouts below are the stable frame contract every material
        // plugs into.

        // set 3: clustered light lists: per-cluster (offset, count) SRV (t0) and flat index SRV (t1).
        rhi::BindGroupLayoutEntry clOffEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 8); // StructuredBuffer<uint2> ClusterOffsets
        rhi::BindGroupLayoutEntry clIdxEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            1, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 4); // StructuredBuffer<uint> ClusterLightIndices
        rhi::BindGroupLayoutEntry set3[] = {clOffEntry, clIdxEntry};
        rhi::BindGroupLayoutDesc s3d{};
        s3d.entries = Span<const rhi::BindGroupLayoutEntry>{set3, 2};
        if (!m_device->CreateBindGroupLayout(s3d, m_clusterLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // Forward pipeline layouts (view, object or instance, material set 2, cluster) are built
        // on demand per material set-2 layout and cached in GetOrCreatePipelineLayout.

        // Shadow depth-only path: a vertex-only shader and a 2-set pipeline layout
        // (set 0 the light view UBO, set 1 the same object and instance layouts as forward, so the
        // object and instance bind groups are reused). No material or cluster sets. The view is the
        // pixel stage's too: a masked surface's depth and pick pixel stages run its Surface()
        // at the view's camera position and time (ShadowCamera and SceneTime, PickCamera and SceneTime).
        rhi::BindGroupLayoutEntry shadowViewEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
            0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
        shadowViewEntry.hasDynamicOffset = true;
        // set 0 also carries the skinning bone-matrix pool (t4) so skinned casters deform their shadow.
        rhi::BindGroupLayoutEntry shadowBoneEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            4, rhi::ShaderStage::Vertex, /*readOnly*/ true,
            /*stride*/ sizeof(Float4x4)); // StructuredBuffer<BoneMatrix> (4x float4)
        rhi::BindGroupLayoutEntry shadowSet0[] = {shadowViewEntry, shadowBoneEntry};
        rhi::BindGroupLayoutDesc svd{};
        svd.entries = Span<const rhi::BindGroupLayoutEntry>{shadowSet0, 2};
        if (!m_device->CreateBindGroupLayout(svd, m_shadowViewLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!MakePipelineLayout(m_shadowViewLayout, m_instanceLayout,
                                m_shadowPipelineLayoutInstanced))
        {
            return Status{ErrorCode::Unknown};
        }

        if (!CreateDefaultMaterial().IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateShadowResources().IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return CreateDummyClusters();
    }

    void MeshRenderer::SetShadowMap(rhi::TextureViewHandle view, u64 generation)
    {
        m_activeShadowView = (view.IsValid()) ? view : m_dummyShadowView;
        m_activeShadowGen = (view.IsValid()) ? generation : 0; // dummy never changes
    }

    void MeshRenderer::SetShadowAtlas(rhi::TextureViewHandle view, u64 generation, u32 passCount)
    {
        m_activeAtlasView = (view.IsValid()) ? view : m_dummyAtlasView;
        m_activeAtlasGen = (view.IsValid()) ? generation : 0;
        m_localShadowPassCount =
            (view.IsValid()) ? passCount : 0; // each tile re-emits the casters
    }

    void MeshRenderer::SetProbes(rhi::TextureViewHandle cubeArray, rhi::BufferHandle probeBuffer, u32 count)
    {
        const bool has = (cubeArray.IsValid() && probeBuffer.IsValid() && count > 0);
        m_activeProbeCube = has ? cubeArray : m_dummyProbeCubeView;
        m_activeProbeBuffer = has ? probeBuffer : m_dummyProbeBuffer;
        m_activeProbeCount =
            has ? count : 0; // becomes GpuView.probeCount (the forward's loop bound)
    }

    void MeshRenderer::UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex)
    {
        (void)frameIndex;
        m_localShadowBase = 0;
        u32 n = static_cast<u32>(shadows.Size());
        if (n == 0 || !m_ready)
        {
            return;
        }
        if (n > kMaxLocalShadows)
        {
            n = kMaxLocalShadows;
        }
        const DynamicUniformRing::Range r = m_localShadowRing.AllocateRange(n);
        if (r.ok)
        {
            MemCopy(r.ptr, shadows.Data(), static_cast<usize>(n) * sizeof(GpuLocalShadow));
            m_localShadowBase = r.slotIndex;
        }
    }

    Span<const RenderCategory> MeshRenderer::SupportedCategories() const
    {
        static constexpr RenderCategory kCats[] = {
            RenderCategories::Opaque, RenderCategories::Masked, RenderCategories::Transparent};
        return Span<const RenderCategory>{kCats, 3};
    }

    namespace
    {
        // A draw the offsets ring could not take is dropped; say so (rate-limited) rather than
        // leave a mesh silently missing from a pass.
        void WarnInstanceRingFull(const char* pass, u32 wanted, u32 used)
        {
            static u32 s_warned = 0;
            constexpr u32 kWarnLimit = 8;
            if (s_warned >= kWarnLimit)
            {
                return;
            }
            ++s_warned;
            rhi::LogWarningf("[MeshRenderer] the draw offsets ring is full: a %s batch of %u instances "
                             "was dropped (%u slots already used this frame).%s",
                             pass, wanted, used,
                             s_warned == kWarnLimit ? " Further such warnings suppressed." : "");
        }
    }

    void MeshRenderer::PrepareFrame(u32 maxDraws, u32 frameIndex)
    {
        m_ready = false;
        ++m_frameSerial;
        PruneStaleMaterialInstances();
        PruneOverrideInstances();
        m_meshes.Tick();            // and mesh streams of meshes gone or rebuilt
        if (maxDraws == 0)
        {
            return;
        }
        // Per-frame ring capacity covers every pass that re-emits draws: the depth prepass and the
        // forward, each CSM cascade, each local-atlas tile, probe faces and picks. Under-counting
        // overflows the offsets ring, Allocate() fails and draws are dropped.
        // Shadow passes are sized by their casters, not the view's draws: off-camera casters would
        // otherwise starve the camera's own draws, and meshes would vanish with their shadows drawn.
        const u32 drawCap = InstanceSlotsPerFrame(
            maxDraws, m_shadowCasterDraws, ShadowCascades::kCount + m_localShadowPassCount,
            m_captureFacePasses + m_pickPasses);
        // Pick passes take a shadow-view slot each, as shadow passes do.
        const u32 shadowViewCap = kMaxShadowPasses + m_pickPasses;
        if (!m_shadowViewRing.Reserve(shadowViewCap) ||
            !m_offsetsRing.Reserve(drawCap) || !m_lightRing.Reserve(Max(kMaxLights, m_sceneLights)) ||
            !m_localShadowRing.Reserve(kMaxLocalShadows) || !m_boneRing.Reserve(m_boneSlotsWanted))
        {
            return;
        }
        if (!EnsureBoneDevice())
        {
            return;
        } // device-local mirror of the bone staging ring (VS reads VRAM)
        if (!EnsureShadowViewBindGroup())
        {
            return;
        }
        m_frameIndex =
            frameIndex; // frames-in-flight slot for the per-set skinned MultiMesh offsets buffer
        // The view ring holds the views the last frame drew, and a probe's faces when one captures
        // this frame: a face missing its slot would bake an empty probe.
        if (!m_viewRing.BeginFrame(frameIndex, m_captureFacePasses))
        {
            return;
        }
        m_shadowViewRing.BeginFrame(frameIndex);
        m_offsetsRing.BeginFrame(frameIndex);
        m_lightRing.BeginFrame(frameIndex);
        m_lightUploads.Clear();
        m_localShadowRing.BeginFrame(frameIndex);
        m_boneRing.BeginFrame(frameIndex);
        m_instShareCache
            .Clear(); // per-frame: the camera prepass fills it, the forward reuses it (ring offsets are frame-scoped)
        m_ready = true;
    }

    // Grow the bone pool to hold `needed` matrices per frame: the next power of two, capped.
    // Safe mid-frame only here, before this frame's block is allocated: the ring has not mapped
    // yet (lazy), the old buffers are destroyed (the device keeps them for frames in flight), and the
    // mirror's generation bump makes every set-0 bind group rebuild at record time.
    bool MeshRenderer::GrowBonePool(u32 needed)
    {
        if (needed > kMaxBoneMatrices)
        {
            return false;
        }
        u32 wanted = m_boneSlotsWanted < kInitialBoneMatrices ? kInitialBoneMatrices
                                                              : m_boneSlotsWanted;
        while (wanted < needed)
        {
            wanted = (wanted >= (kMaxBoneMatrices >> 1)) ? kMaxBoneMatrices : wanted * 2u;
        }
        m_boneSlotsWanted = wanted;
        if (!m_boneRing.Reserve(wanted))
        {
            return false;
        }
        m_boneRing.BeginFrame(m_frameIndex); // the region base moved with the capacity
        return EnsureBoneDevice();
    }

    bool MeshRenderer::EnsureBoneDevice()
    {
        const u64 want = m_boneRing.ByteCapacity();
        if (m_boneDevice.IsValid() && m_boneDeviceBytes == want)
        {
            return true;
        }
        // Idle only when replacing a live buffer (the first allocation has nothing to protect;
        // a mid-frame wait on web pumps the event loop and drops the frame's submit).
        if (m_boneDevice.IsValid())
        {
            m_device->DestroyBuffer(m_boneDevice);
        }
        rhi::BufferDesc bd{};
        bd.size = want;
        bd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
        bd.memory = rhi::MemoryLocation::GpuOnly;
        bd.label = u8"mesh.bones.device";
        if (!m_device->CreateBuffer(bd, m_boneDevice).IsOk())
        {
            m_boneDevice = {};
            m_boneDeviceBytes = 0;
            return false;
        }
        m_boneDeviceBytes = want;
        ++m_boneDeviceGen; // invalidate set-0 bind groups that bind the device buffer
        return true;
    }

    void MeshRenderer::UploadInstances(Span<const ExtractedScene* const> scenes, rhi::CommandEncoder& encoder)
    {
        PROFILE_SCOPE("mesh.instances");
        m_instanceTable.BeginFrame(m_frameIndex);
        {
            PROFILE_SCOPE("mesh.instances.claim");
            for (const ExtractedScene* scene : scenes)
            {
                if (scene == nullptr)
                {
                    continue;
                }
                for (RenderData* data : scene->Items())
                {
                    if (data == nullptr || data->rendererId != RendererId())
                    {
                        continue;
                    }
                    auto* md = static_cast<MeshRenderData*>(data);
                    if (md->multiMesh)
                    {
                        continue; // a set keeps its own instances
                    }
                    const InstanceTable::Claimed claimed =
                        m_instanceTable.Claim(md->scene, md->entityId, md->world, md->color);
                    md->instanceSlot = claimed.slot;
                    if (claimed.refresh)
                    {
                        m_refreshItems.PushBack(md);
                    }
                }
            }
        }
        // The records of the meshes the table already held, compared and written across the job
        // system (each slot is one item's), then marked in item order so the copy makes few runs.
        const u32 count = static_cast<u32>(m_refreshItems.Size());
        m_refreshed.Resize(count);
        const auto refresh = [this](u32 i)
        {
            const MeshRenderData* md = m_refreshItems[i];
            m_refreshed[i] = m_instanceTable.Refresh(md->instanceSlot, md->world, md->color)
                                 ? md->instanceSlot
                                 : InstanceTable::kNoSlot;
        };
        {
            PROFILE_SCOPE("mesh.instances.refresh");
            if (count >= kParallelRefreshItems && HasGlobalJobSystem())
            {
                GlobalJobs().ParallelFor(count, refresh, kParallelRefreshItems / 4u);
            }
            else
            {
                for (u32 i = 0; i < count; ++i)
                {
                    refresh(i);
                }
            }
        }
        {
            PROFILE_SCOPE("mesh.instances.mark");
            // The written slots from a flat array: through the items it was a cache miss each.
            for (u32 slot : m_refreshed)
            {
                if (slot != InstanceTable::kNoSlot)
                {
                    m_instanceTable.MarkChanged(slot);
                }
            }
        }
        m_refreshItems.Clear();
        {
            PROFILE_SCOPE("mesh.instances.end");
            m_instanceTable.EndScenes();
        }
        PROFILE_SCOPE("mesh.instances.upload");
        m_instanceTable.Upload(encoder);
        (void)EnsureInstanceBindGroup();
    }

    bool MeshRenderer::EnsureInstanceBindGroup()
    {
        const rhi::BufferHandle table = m_instanceTable.Buffer();
        if (!table.IsValid())
        {
            return false;
        }
        if (m_instanceBG.IsValid() && m_instanceBGGen == m_instanceTable.Generation())
        {
            return true;
        }
        ReleaseBindGroup(m_instanceBG);
        m_instanceBG = {};
        const rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(table, 0, static_cast<u64>(m_instanceTable.Capacity()) * sizeof(GpuInstance)),
            rhi::BindGroupEntry::BufferEntry(m_noFadeSetBuf, 0, m_device->Describe(m_noFadeSetBuf)->size),
        };
        rhi::BindGroupDesc desc{};
        desc.layout = m_instanceLayout;
        desc.entries = Span<const rhi::BindGroupEntry>{entries, 2u};
        if (!m_device->CreateBindGroup(desc, m_instanceBG).IsOk())
        {
            m_instanceBG = {};
            return false;
        }
        m_instanceBGGen = m_instanceTable.Generation();
        return true;
    }

    bool MeshRenderer::RunHasSlots(Span<const DrawItem> items, usize first, u32 count)
    {
        for (u32 k = 0; k < count; ++k)
        {
            const auto* md = static_cast<const MeshRenderData*>(items[first + k].data);
            if (md->instanceSlot == MeshRenderData::kNoInstanceSlot)
            {
                if (!m_noSlotLogged)
                {
                    m_noSlotLogged = true;
                    LOG_ERROR(u8"Render", u8"a mesh resolved with no instance table slot (no upload this frame); not drawn");
                }
                return false;
            }
        }
        return m_instanceBG.IsValid();
    }

    void MeshRenderer::UploadSkinning(Span<const ExtractedScene* const> scenes, rhi::CommandEncoder& encoder)
    {
        // Once for the frame: every scene's skeletons share this lookup and this one block.
        m_boneStart.Clear();
        m_skinnedScratch.Clear();
        if (!m_ready)
        {
            return;
        }
        UploadInstances(scenes, encoder); // every plain mesh's record, before any pass reads one
        UploadMultiMeshes(scenes); // ensure and upload the persistent instanced-mesh buffers (once per frame)
        // One-time: bring the out-of-graph dummy depth textures into the layout their descriptors
        // expect, so they're never sampled while UNDEFINED on caster-less frames (VUID-09600).
        if (!m_dummyDepthInit)
        {
            if (m_dummyShadowTex.IsValid())
            {
                encoder.TransitionTexture(m_dummyShadowTex, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::DepthStencilRead);
            }
            if (m_dummyAtlasTex.IsValid())
            {
                encoder.TransitionTexture(m_dummyAtlasTex, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::DepthStencilRead);
            }
            // IBL color fallbacks (cube, BRDF LUT), also out of graph, sampled when no env is active.
            if (m_dummyCube.IsValid())
            {
                encoder.TransitionTexture(m_dummyCube, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::ShaderRead);
            }
            if (m_dummyBrdf.IsValid())
            {
                encoder.TransitionTexture(m_dummyBrdf, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::ShaderRead);
            }
            if (m_dummyProbeCube.IsValid())
            {
                encoder.TransitionTexture(m_dummyProbeCube, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::ShaderRead);
            }
            m_dummyDepthInit = true;
        }
        // Pass 1: collect distinct skinned instances (by boneMatrices pointer) across every scene of
        // the frame. The total is the sum of 2*boneCount (current slab and previous slab). The map
        // reserves the key so duplicates are skipped. All of them at once: the pool grows (if it must)
        // before this frame's one block is allocated, the only time it may.
        u32 total = 0;
        for (const ExtractedScene* scene : scenes)
        for (RenderData* data : scene->Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            } // skip non-mesh items (sprites, for example)
            const auto* md = static_cast<const MeshRenderData*>(data);
            if (md->boneMatrices == nullptr || md->boneCount == 0)
            {
                continue;
            }
            if (md->mesh == nullptr || !md->mesh->IsSkinned())
            {
                continue;
            }
            if (m_boneStart.Contains(md->boneMatrices))
            {
                continue;
            }
            m_boneStart.InsertOrAssign(md->boneMatrices,
                                       BoneSlot{}); // reserve; bases filled in pass 2
            m_skinnedScratch.PushBack(
                SkinnedRef{md->boneMatrices, md->prevBoneMatrices, md->boneCount});
            total += md->boneCount * 2u;
        }
        // Skinned-MultiMesh pose pools: M palettes (poseCount * boneCount matrices) per crowd, uploaded once
        // (no previous slab). Each set's per-instance bone base is filled
        // into its draw offsets buffer in FillSkinnedMultiMeshOffsets once the pool base is known.
        for (const ExtractedScene* scene : scenes)
        for (RenderData* data : scene->Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* mdb = static_cast<const MeshRenderData*>(data);
            if (!mdb->multiMesh)
            {
                continue;
            }
            const auto* mm = static_cast<const MultiMeshRenderData*>(mdb);
            if (mm->posePool == nullptr || mm->poseCount == 0 || mm->boneCount == 0)
            {
                continue;
            }
            if (mm->mesh == nullptr || !mm->mesh->IsSkinned())
            {
                continue;
            }
            if (m_boneStart.Contains(mm->posePool))
            {
                continue;
            }
            m_boneStart.InsertOrAssign(mm->posePool, BoneSlot{});
            const u32 poolCount = mm->poseCount * mm->boneCount;
            // current and previous slab (per-bone motion vectors); prev == null falls back to cur (zero motion).
            m_skinnedScratch.PushBack(
                SkinnedRef{mm->posePool, mm->prevPosePool, poolCount, /*hasPrev*/ true});
            total += poolCount * 2u;
        }
        if (total == 0)
        {
            m_boneStart.Clear();
            return;
        }
        if (total > m_boneRing.SlotsPerFrame() && !GrowBonePool(total))
        {
            m_boneStart.Clear();
            return; // over the cap (or allocation failed): this frame's casters render unskinned
        }
        const DynamicUniformRing::Range block = m_boneRing.AllocateRange(total);
        if (!block.ok)
        {
            m_boneStart.Clear();
            return;
        }

        // Pass 2: write each distinct instance into the block (current, then previous) and record its bases.
        u32 cursor = 0; // matrix-units offset within the block
        for (const SkinnedRef& r : m_skinnedScratch)
        {
            const u32 n = r.count;
            const u32 base = block.slotIndex + cursor;
            Float4x4* dst = static_cast<Float4x4*>(block.ptr) + cursor;
            MemCopy(dst, r.cur, static_cast<usize>(n) * sizeof(Float4x4));
            if (r.hasPrev)
            { // per-entity caster: also write the prev slab (motion vectors)
                MemCopy(dst + n, (r.prev != nullptr) ? r.prev : r.cur,
                        static_cast<usize>(n) * sizeof(Float4x4));
                if (BoneSlot* slot = m_boneStart.Find(r.cur))
                {
                    *slot = BoneSlot{base, base + n};
                }
                cursor += n * 2u;
            }
            else
            { // MultiMesh pose pool: just the M palettes, no prev slab
                if (BoneSlot* slot = m_boneStart.Find(r.cur))
                {
                    *slot = BoneSlot{base, base};
                }
                cursor += n;
            }
        }

        // Mirror the populated range to VRAM, then make it visible to vertex-shader reads.
        encoder.CopyBufferToBuffer(m_boneRing.Buffer(), block.byteOffset, m_boneDevice,
                                   block.byteOffset, static_cast<u64>(total) * sizeof(Float4x4));
        encoder.TransitionBuffer(m_boneDevice, rhi::ResourceState::CopyDst,
                                 rhi::ResourceState::ShaderRead);
        for (const ExtractedScene* scene : scenes)
            FillSkinnedMultiMeshOffsets(*scene); // per-instance bone base into each skinned set's draw offsets
    }

    void MeshRenderer::ReleaseMultiMeshSet(MultiMeshSet& set)
    {
        for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
        {
            if (set.instanceBG[r].IsValid())
            {
                m_device->DestroyBindGroup(set.instanceBG[r]);
                set.instanceBG[r] = {};
            }
        }
        set.activeInstanceBG = {};
        m_device->DestroyBuffer(set.instanceBuf);
        m_device->DestroyBuffer(set.offsetsBuf);
        m_device->DestroyBuffer(set.pickOffsetsBuf);
        m_device->DestroyBuffer(set.setBuf);
        set.capacity = 0;
        set.offsetsCapacity = 0;
    }

    void MeshRenderer::EvictStaleMultiMeshSets()
    {
        Array<u64> stale(*m_allocator);
        for (auto& kv : m_multiMeshSets)
        {
            if (m_multiMeshFrame - kv.value.lastFrame > kMultiMeshEvictFrames)
            {
                stale.PushBack(kv.key);
            }
        }
        for (const u64 key : stale)
        {
            if (MultiMeshSet* set = m_multiMeshSets.Find(key))
            {
                ReleaseMultiMeshSet(*set);
            }
            m_multiMeshSets.Remove(key);
        }
    }

    void MeshRenderer::UploadMultiMeshes(Span<const ExtractedScene* const> scenes)
    {
        // One frame, however many scenes it draws: a set ages by frames, not by scenes.
        ++m_multiMeshFrame;
        EvictStaleMultiMeshSets(); // before the early return: a frame with no sets still ages them
        u32 maxCount = 0;
        for (const ExtractedScene* scene : scenes)
        for (RenderData* data : scene->Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* md = static_cast<const MeshRenderData*>(data);
            if (!md->multiMesh)
            {
                continue;
            }
            const auto* mm = static_cast<const MultiMeshRenderData*>(md);
            if (mm->instanceCount > maxCount)
            {
                maxCount = mm->instanceCount;
            }
        }
        if (maxCount == 0)
        {
            return;
        }
        if (!EnsureRamp(maxCount))
        {
            return;
        }
        for (const ExtractedScene* scene : scenes)
        for (RenderData* data : scene->Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* md = static_cast<const MeshRenderData*>(data);
            if (!md->multiMesh)
            {
                continue;
            }
            EnsureMultiMeshSet(*static_cast<const MultiMeshRenderData*>(md));
        }
    }

    bool MeshRenderer::EnsureRamp(u32 count)
    {
        if (m_rampBuffer.IsValid() && m_pickRampBuffer.IsValid() && count <= m_rampCapacity)
        {
            return true;
        }
        // An in-flight frame may still reference the old ramps: retire when wired, otherwise drain.
        if (m_rampBuffer.IsValid())
        {
            m_device->DestroyBuffer(m_rampBuffer);
        }
        if (m_pickRampBuffer.IsValid())
        {
            m_device->DestroyBuffer(m_pickRampBuffer);
        }
        rhi::BufferDesc bd{};
        bd.size = static_cast<u64>(count) * sizeof(GpuDrawOffsets);
        bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        bd.label = u8"mesh.multimesh.ramp";
        if (!m_device->CreateBuffer(bd, m_rampBuffer).IsOk())
        {
            m_rampBuffer = {};
            m_rampCapacity = 0;
            return false;
        }
        if (auto* p = static_cast<GpuDrawOffsets*>(m_device->Map(m_rampBuffer)))
        {
            for (u32 i = 0; i < count; ++i)
            {
                p[i] = GpuDrawOffsets{i, 0, 0, 0.0f};
            }
            m_device->Unmap(m_rampBuffer);
        }
        rhi::BufferDesc pd = bd;
        pd.size = static_cast<u64>(count) * sizeof(GpuPickDrawOffsets);
        pd.label = u8"mesh.multimesh.pick-ramp";
        if (!m_device->CreateBuffer(pd, m_pickRampBuffer).IsOk())
        {
            m_pickRampBuffer = {};
            m_rampCapacity = 0;
            return false;
        }
        if (auto* p = static_cast<GpuPickDrawOffsets*>(m_device->Map(m_pickRampBuffer)))
        {
            for (u32 i = 0; i < count; ++i)
            {
                p[i] = GpuPickDrawOffsets{i, 0, 0, 0};
            }
            m_device->Unmap(m_pickRampBuffer);
        }
        m_rampCapacity = count;
        return true;
    }

    void MeshRenderer::EnsureMultiMeshSet(const MultiMeshRenderData& mm)
    {
        MultiMeshSet* set = m_multiMeshSets.Find(mm.key);
        if (set == nullptr)
        {
            set = &m_multiMeshSets.InsertOrAssign(mm.key, MultiMeshSet{});
        }
        set->lastFrame = m_multiMeshFrame;

        const u32 fif = Min(m_framesInFlight, kMultiMeshMaxFiF);
        const u32 region = m_frameIndex % fif; // this frame's InstanceData region
        // The whole list when the item carries one (a faded set draws a prefix of it).
        const u32 uploadCount = Max(mm.instanceCount, mm.uploadCount);

        if (!set->instanceBuf.IsValid() || uploadCount > set->capacity)
        {
            for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
            {
                if (set->instanceBG[r].IsValid())
                {
                    m_device->DestroyBindGroup(set->instanceBG[r]);
                    set->instanceBG[r] = {};
                }
            }
            if (set->instanceBuf.IsValid())
            {
                m_device->DestroyBuffer(set->instanceBuf);
                set->instanceBuf = {};
            }
            rhi::BufferDesc bd{};
            // The region capacity is rounded (MultiMeshRegionCapacity) so region r's byte offset
            // meets every backend's storage-buffer offset alignment.
            const u32 capacity = MultiMeshRegionCapacity(uploadCount);
            const u64 regionBytes = static_cast<u64>(capacity) * sizeof(GpuInstance);
            bd.size = static_cast<u64>(fif) * regionBytes; // one region per frame-in-flight
            bd.usage = rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"mesh.multimesh.instances";
            if (!m_device->CreateBuffer(bd, set->instanceBuf).IsOk())
            {
                set->instanceBuf = {};
                set->capacity = 0;
                return;
            }
            // The set's InstanceSet block, one slot per region (kept across a capacity change).
            if (!set->setBuf.IsValid())
            {
                rhi::BufferDesc sd{};
                sd.size = static_cast<u64>(kMultiMeshMaxFiF) * kViewSlot;
                sd.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst;
                sd.memory = rhi::MemoryLocation::CpuToGpu;
                sd.label = u8"mesh.multimesh.instance-set";
                if (!m_device->CreateBuffer(sd, set->setBuf).IsOk())
                {
                    set->setBuf = {};
                    return;
                }
            }
            // A set-1 bind group per region (each over its own slice of the buffer, so the shader's
            // Instances[i] indexes within the bound region; the shared static ramp and skinned offsets stay 0-based),
            // with that region's InstanceSet.
            bool ok = true;
            for (u32 r = 0; r < fif; ++r)
            {
                const rhi::BindGroupEntry be[] = {
                    rhi::BindGroupEntry::BufferEntry(set->instanceBuf, static_cast<u64>(r) * regionBytes, regionBytes),
                    rhi::BindGroupEntry::BufferEntry(set->setBuf, static_cast<u64>(r) * kViewSlot,
                                                     sizeof(GpuInstanceSet)),
                };
                rhi::BindGroupDesc bgd{};
                bgd.layout = m_instanceLayout;
                bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
                if (!m_device->CreateBindGroup(bgd, set->instanceBG[r]).IsOk())
                {
                    set->instanceBG[r] = {};
                    ok = false;
                    break;
                }
            }
            if (!ok)
            {
                for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
                {
                    if (set->instanceBG[r].IsValid())
                    {
                        m_device->DestroyBindGroup(set->instanceBG[r]);
                        set->instanceBG[r] = {};
                    }
                }
                m_device->DestroyBuffer(set->instanceBuf);
                set->instanceBuf = {};
                set->capacity = 0;
                return;
            }
            set->capacity = capacity;
            for (u32 r = 0; r < kMultiMeshMaxFiF; ++r) // fresh bytes: every region is behind
            {
                set->regionVersion[r] = 0;
                set->regionCount[r] = 0;
            }
        }
        set->count = mm.instanceCount;
        set->activeInstanceBG = set->instanceBG[region];

        // This frame's InstanceSet (its region, so the GPU's read of the last frame's is untouched):
        // the fade window, the count each instance's rank divides by (the whole list, of which a
        // faded set draws a prefix), and the one entity the set picks as.
        if (set->setBuf.IsValid())
        {
            if (auto* base = static_cast<byte*>(m_device->Map(set->setBuf)))
            {
                GpuInstanceSet block{};
                block.fadeStart = mm.fadeStart;
                block.fadeEnd = mm.fadeEnd;
                block.instanceCount = Max(uploadCount, 1u);
                block.pickIndex = EntityTag::Index(mm.entityId) + 1u;
                block.pickGeneration = EntityTag::Generation(mm.entityId);
                MemCopy(base + static_cast<usize>(region) * kViewSlot, &block, sizeof(block));
                m_device->Unmap(set->setBuf);
            }
        }

        // Each frame writes only its own region while the GPU reads the previous one (hazard-free),
        // and only when that region is behind the item: a new version (so a static set writes
        // once per region, a set that changes every frame writes every frame) or a draw count above what the
        // region was last written with (the tail would otherwise be stale bytes, per region).
        const bool behind = set->regionVersion[region] != mm.version ||
                            set->regionCount[region] < uploadCount;
        if (behind && set->instanceBuf.IsValid() && mm.transforms != nullptr)
        {
            if (auto* dst = static_cast<GpuInstance*>(m_device->Map(set->instanceBuf)))
            {
                GpuInstance* r = dst + static_cast<usize>(region) * set->capacity;
                for (u32 i = 0; i < uploadCount; ++i)
                {
                    const Color tint = (mm.tints != nullptr) ? mm.tints[i] : mm.color; // per-instance or shared
                    r[i] = GpuInstance{mm.transforms[i], mm.transforms[i], tint};
                }
                m_device->Unmap(set->instanceBuf);
                set->regionVersion[region] = mm.version;
                set->regionCount[region] = uploadCount;
            }
        }

        // Skinned crowds need a per-set draw offsets buffer (dynamic per-instance bone bases). Allocate or
        // grow it here; it is filled in FillSkinnedMultiMeshOffsets once the pose pool's bone base is known.
        set->skinned = (mm.posePool != nullptr && mm.poseCount > 0 && mm.boneCount > 0);
        if (set->skinned && (!set->offsetsBuf.IsValid() || mm.instanceCount > set->offsetsCapacity))
        {
            if (set->offsetsBuf.IsValid())
            {
                m_device->DestroyBuffer(set->offsetsBuf);
            }
            if (set->pickOffsetsBuf.IsValid())
            {
                m_device->DestroyBuffer(set->pickOffsetsBuf);
            }
            rhi::BufferDesc od{};
            // framesInFlight regions: the offsets are rewritten every frame (dynamic bone bases), so each
            // frame writes its own region while the GPU reads the previous frame's, never the same bytes.
            od.size = static_cast<u64>(m_framesInFlight) * static_cast<u64>(mm.instanceCount) *
                      sizeof(GpuDrawOffsets);
            od.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
            od.memory = rhi::MemoryLocation::CpuToGpu;
            od.label = u8"mesh.multimesh.offsets";
            if (!m_device->CreateBuffer(od, set->offsetsBuf).IsOk())
            {
                set->offsetsBuf = {};
                set->offsetsCapacity = 0;
                return;
            }
            rhi::BufferDesc pd = od;
            pd.size = static_cast<u64>(m_framesInFlight) * static_cast<u64>(mm.instanceCount) *
                      sizeof(GpuPickDrawOffsets);
            pd.label = u8"mesh.multimesh.pick-offsets";
            if (!m_device->CreateBuffer(pd, set->pickOffsetsBuf).IsOk())
            {
                m_device->DestroyBuffer(set->offsetsBuf);
                set->offsetsBuf = {};
                set->pickOffsetsBuf = {};
                set->offsetsCapacity = 0;
                return;
            }
            set->offsetsCapacity = mm.instanceCount;
        }
    }

    bool MeshRenderer::ReadMultiMeshInstanceTint(u64 key, u32 region, u32 index, Color& out)
    {
        MultiMeshSet* set = m_multiMeshSets.Find(key);
        if (set == nullptr || !set->instanceBuf.IsValid() || region >= kMultiMeshMaxFiF ||
            index >= set->capacity)
        {
            return false;
        }
        auto* data = static_cast<const GpuInstance*>(m_device->Map(set->instanceBuf));
        if (data == nullptr)
        {
            return false;
        }
        out = data[static_cast<usize>(region) * set->capacity + index].tint;
        m_device->Unmap(set->instanceBuf);
        return true;
    }

    bool MeshRenderer::ReadMultiMeshInstanceSet(u64 key, u32 region, GpuInstanceSet& out)
    {
        MultiMeshSet* set = m_multiMeshSets.Find(key);
        if (set == nullptr || !set->setBuf.IsValid() || region >= kMultiMeshMaxFiF)
        {
            return false;
        }
        const auto* base = static_cast<const byte*>(m_device->Map(set->setBuf));
        if (base == nullptr)
        {
            return false;
        }
        MemCopy(&out, base + static_cast<usize>(region) * kViewSlot, sizeof(out));
        m_device->Unmap(set->setBuf);
        return true;
    }

    bool MeshRenderer::ReadMultiMeshInstance(u64 key, u32 region, u32 index, Float4x4& out)
    {
        MultiMeshSet* set = m_multiMeshSets.Find(key);
        if (set == nullptr || !set->instanceBuf.IsValid() || region >= kMultiMeshMaxFiF ||
            index >= set->capacity)
        {
            return false;
        }
        auto* data = static_cast<const GpuInstance*>(m_device->Map(set->instanceBuf));
        if (data == nullptr)
        {
            return false;
        }
        out = data[static_cast<usize>(region) * set->capacity + index].world;
        m_device->Unmap(set->instanceBuf);
        return true;
    }

    void MeshRenderer::FillSkinnedMultiMeshOffsets(const ExtractedScene& scene)
    {
        const u32 region = m_frameIndex % m_framesInFlight; // this frame's frame-in-flight slot
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* mdb = static_cast<const MeshRenderData*>(data);
            if (!mdb->multiMesh)
            {
                continue;
            }
            const auto* mm = static_cast<const MultiMeshRenderData*>(mdb);
            if (mm->posePool == nullptr || mm->poseCount == 0 || mm->boneCount == 0)
            {
                continue;
            }
            MultiMeshSet* set = m_multiMeshSets.Find(mm->key);
            if (set == nullptr || !set->offsetsBuf.IsValid() || set->offsetsCapacity == 0)
            {
                continue;
            }
            const BoneSlot* pool = m_boneStart.Find(mm->posePool);
            if (pool == nullptr)
            {
                continue;
            }
            const u32 base =
                region *
                set->offsetsCapacity; // write only this frame's region (the GPU reads the previous one)
            auto* od = static_cast<GpuDrawOffsets*>(m_device->Map(set->offsetsBuf));
            if (od == nullptr)
            {
                continue;
            }
            auto* pickOd = set->pickOffsetsBuf.IsValid()
                               ? static_cast<GpuPickDrawOffsets*>(m_device->Map(set->pickOffsetsBuf))
                               : nullptr;
            const u32 M = mm->poseCount;
            for (u32 i = 0; i < mm->instanceCount; ++i)
            {
                // Pick this instance's pose out of the M shared palettes by the caller's policy (pure,
                // unit-tested in SelectPose). There are M palette evaluations regardless; the slot is
                // stable per i across frames, so motion blur (the previous pose) is unaffected.
                const u32 pose = SelectPose(mm->poseAssignment, i, M, mm->poseIndices,
                                            mm->instanceClips != nullptr ? mm->instanceClips[i] : 0u,
                                            mm->posesPerClip);
                const u32 bucket = pose * mm->boneCount;     // matrix-unit offset within a palette
                const u32 curBase = pool->base + bucket;     // current pose
                const u32 prvBase = pool->prevBase + bucket; // last frame's pose (motion vectors)
                od[base + i] = GpuDrawOffsets{i, curBase, prvBase, 0.0f};
                if (pickOd != nullptr)
                {
                    pickOd[base + i] = GpuPickDrawOffsets{i, curBase, 0, 0}; // ids 0: the set's own
                }
            }
            m_device->Unmap(set->offsetsBuf);
            if (pickOd != nullptr)
            {
                m_device->Unmap(set->pickOffsetsBuf);
            }
            set->offsetsByteOffset = base * static_cast<u32>(sizeof(GpuDrawOffsets));
        }
    }

    void MeshRenderer::Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                               Array<ResolvedDraw>& out)
    {
        if (!m_ready || items.IsEmpty())
        {
            return;
        }
        // This view's set-0 group (its scene's IBL products; per-view slots, see EnsureViewBindGroup).
        if (!EnsureViewBindGroup(ctx))
        {
            return;
        }

        // Upload this view's lights into the light ring (bound whole at set 0; the shader reads
        // Lights[lightOffset + i]). Clamp to the per-frame capacity.
        // A scene's lights go up once a frame: Resolve runs per renderer run in each pass of each
        // view, and an upload per call ran the ring out, after which a view read slot 0 onward, the
        // lights of whichever scene uploaded first.
        u32 lightCount = Min(static_cast<u32>(ctx.lights.Size()), kMaxLights);
        u32 lightOffset = 0;
        if (lightCount > 0)
        {
            if (const LightUpload* up = m_lightUploads.Find(ctx.lights.Data()); up != nullptr)
            {
                lightOffset = up->offset;
                lightCount = up->count;
            }
            else
            {
                const DynamicUniformRing::Range lr = m_lightRing.AllocateRange(lightCount);
                if (lr.ok)
                {
                    MemCopy(lr.ptr, ctx.lights.Data(), static_cast<usize>(lightCount) * sizeof(GpuLight));
                    lightOffset = lr.slotIndex;
                }
                else
                {
                    // SetSceneLights sized the ring for every scene; a renderer driven without it
                    // runs out here. The view draws unlit rather than with another scene's lights.
                    if (!m_lightOverflowLogged)
                    {
                        LOG_WARNING(u8"Render", u8"mesh renderer: light ring full ({} slots per frame), {} lights dropped, view drawn unlit",
                                    m_lightRing.SlotsPerFrame(), lightCount);
                        m_lightOverflowLogged = true;
                    }
                    lightCount = 0;
                }
                m_lightUploads.InsertOrAssign(ctx.lights.Data(), LightUpload{lightOffset, lightCount});
            }
        }

        // Per-view UBO (shared by every draw in this call): ViewProj, camera and light range. The
        // two pipeline layouts share set 0 (m_viewLayout), so the resolved binding is the same.
        // The clustered light lists for this view (set 3). An empty binding gets the dummy and
        // ClusterGridX=0, which makes the shader fall back to looping all lights.
        rhi::BindGroupHandle clusterBG = m_dummyClusterBG;
        if (ctx.cluster.Valid() && lightCount > 0) // no lights uploaded: the binned indices name none
        {
            // Per-(view, frame) slot: two views in one frame have different cluster buffers and
            // must not share a bind group, or it would be rebuilt and free a set still in flight.
            clusterBG = EnsureClusterBindGroup(ctx.viewIndex, ctx.frameIndex, ctx.cluster.offsets,
                                               ctx.cluster.lightIndices, ctx.cluster.version);
        }

        GpuView vd{};
        vd.viewProj = ctx.viewProj;
        vd.prevViewProj = ctx.prevViewProj; // motion vectors (camera)
        vd.jitter = ctx.jitter;
        vd.prevJitter = ctx.prevJitter;
        vd.view = ctx.viewMatrix;
        vd.cameraPos = ctx.cameraPos;
        vd.ambient = ctx.ambient;
        const bool iblActive = ctx.ibl.valid && ctx.ibl.shBuffer.IsValid() &&
                               ctx.ibl.prefilterView.IsValid() && ctx.ibl.brdfView.IsValid();
        vd.iblMaxLod = iblActive ? ctx.ibl.maxLod : -1.0f; // < 0: the forward uses flat ambient
        // Probes are disabled during probe capture (ctx.probesEnabled=false), giving count 0, so metallics
        // reflect the sky (global IBL) rather than the probe being built, which would bake them black.
        // This view's scene's base into the Probes SRV (scenes' records are concatenated per frame) and its
        // count (the forward's loop bound); box, slice and the rest per probe live in the buffer.
        vd.probeBase = ctx.probeBase;
        vd.probeCount = ctx.probesEnabled ? ctx.probeCount : 0u;

        vd.lightCount = static_cast<f32>(lightCount);
        vd.lightOffset = lightOffset;
        // CSM cascade data for this view (per-view fit, carried in ctx).
        if (ctx.cascades.valid)
        {
            for (u32 c = 0; c < ShadowCascades::kCount; ++c)
            {
                vd.cascadeViewProj[c] = ctx.cascades.viewProj[c];
            }
            vd.cascadeSplitFar = Float4{ctx.cascades.splitFar[0], ctx.cascades.splitFar[1],
                                        ctx.cascades.splitFar[2], ctx.cascades.splitFar[3]};
            vd.cascadeTexelSize =
                Float4{ctx.cascades.texelWorldSize[0], ctx.cascades.texelWorldSize[1],
                       ctx.cascades.texelWorldSize[2], ctx.cascades.texelWorldSize[3]};
            vd.shadowCascadeCount = static_cast<f32>(ShadowCascades::kCount);
            vd.cascadeLayerBase =
                static_cast<f32>(ctx.cascadeLayerBase); // this view's first array layer
            // The caster light's own biases (its LightComponent, through DirectionalShadow): the
            // normal offset in texels (scaled by the cascade's world texel size in the shader), and
            // the receiver's compare bias. Defaults: ShadowBiasDefaults.
            vd.shadowNormalBias = ctx.cascades.normalBias;
            vd.shadowDepthBias = ctx.cascades.depthBias;
            vd.shadowFarFade = ctx.cascades.farFade; // the scene's fade width at the reach
        }
        // The shadow-map sample uv.y sign. The shadow map rasterizes the same as the main color
        // target, so on Y-flip targets (WebGPU, positive viewport) uv.y = +ndc.y*0.5+0.5; on
        // Vulkan (negative-height viewport) it stays -ndc.y*0.5+0.5. Matches SkyPass's SceneYSign.
        // Set outside the cascades.valid block: SampleLocalShadow (spot and point tiles) uses this
        // sign too, and local shadows exist without a valid directional CSM; the struct default (0)
        // would collapse every local lookup onto one atlas row.
        vd.shadowUvYSign = m_device->NeedsClipSpaceYFlip() ? 1.0f : -1.0f;
        vd.debugMode = static_cast<u32>(ctx.debugSemantic);
        vd.iblDiffuse = ctx.iblDiffuseIntensity;
        vd.iblSpecular = ctx.iblSpecularIntensity;
        vd.sceneTime = ctx.timeSeconds;
        vd.prevSceneTime = ctx.prevTimeSeconds; // the previous position sways at last frame's time
        // Ring base plus this view's scene's entry base (scenes' entries are concatenated per frame;
        // lights carry scene-relative shadowIndex values).
        vd.localShadowBase = m_localShadowBase + ctx.localShadowEntryBase;
        if (ctx.cluster.Valid())
        {
            vd.clusterGridX = ctx.cluster.gridX;
            vd.clusterGridY = ctx.cluster.gridY;
            vd.clusterSliceCount = ctx.cluster.sliceCount;
            vd.clusterTileSize = ctx.cluster.tileSize;
            vd.clusterViewportX = ctx.cluster.viewportX;
            vd.clusterViewportY = ctx.cluster.viewportY;
            vd.clusterNear = ctx.cluster.nearZ;
            vd.clusterFar = ctx.cluster.farZ;
            vd.clusterLogScale = ctx.cluster.logScale;
            vd.clusterLogBias = ctx.cluster.logBias;
        }
        m_passView = vd; // a faded instanced set copies this with its window (ResolveMultiMesh)
        // Every run of a pass, and every pass of a view, writes the same block: one slot for all.
        u32 viewOffset = 0;
        if (!m_viewRing.SlotFor(&vd, sizeof(vd), viewOffset))
        {
            return;
        }

        const bool allowInstancing = (items[0].data->category != RenderCategories::Transparent);

        usize i = 0;
        while (i < items.Size())
        {
            const auto* head = static_cast<const MeshRenderData*>(items[i].data);
            // An instanced-mesh set (MultiMesh): one item drawn N times from its persistent buffer, no
            // per-frame fill. Handled standalone (never merged into a neighbouring run).
            if (head->multiMesh)
            {
                const GpuMesh* mmMesh = m_meshes.GetOrUpload(head->mesh);
                if (mmMesh != nullptr)
                {
                    ResolveMultiMesh(ctx, viewOffset, clusterBG,
                                     *static_cast<const MultiMeshRenderData*>(head), *mmMesh, out);
                }
                ++i;
                continue;
            }
            // Skinned and faded meshes batch like any other: identical (mesh, material) instances
            // collapse into one instanced draw, and a lone one into an instanced draw of one, each
            // instance with its own bones and fade.
            const bool headFaded = head->fade > 0.0f;
            // A run draws skinned or not by its head: an item that is the other breaks it.
            const bool headSkinned = head->boneMatrices != nullptr && head->boneCount > 0;
            // Extend the run while mesh and material match (a batchable group) and the run is faded
            // or solid as a whole: the two draw with different pipelines (DITHER). A mesh with
            // material overrides draws alone, with instances of its own.
            usize j = i + 1;
            if (allowInstancing && head->overrideCount == 0)
            {
                while (j < items.Size())
                {
                    const auto* nd = static_cast<const MeshRenderData*>(items[j].data);
                    if (nd->multiMesh || nd->mesh != head->mesh || nd->material != head->material ||
                        !nd->SameSubmeshMaterials(*head) || (nd->fade > 0.0f) != headFaded || nd->overrideCount > 0 ||
                        (nd->boneMatrices != nullptr && nd->boneCount > 0) != headSkinned ||
                        nd->categorySlots != head->categorySlots) // a split mesh's item draws some slots only
                    {
                        break;
                    }
                    ++j;
                }
            }
            const u32 runLen = static_cast<u32>(j - i);

            const GpuMesh* mesh = m_meshes.GetOrUpload(head->mesh);
            if (mesh != nullptr)
            {
                // A run of one draws as an instanced draw of one: one path for every mesh.
                ResolveInstanced(ctx, viewOffset, clusterBG, items, i, runLen, *head, *mesh, out);
            }
            i = j;
        }
    }

    void MeshRenderer::ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                        Array<ResolvedDraw>& out)
    {
        ResolveDepthLike(ctx, items, out, /*pick*/ false);
    }

    void MeshRenderer::ResolvePickIds(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                      Array<ResolvedDraw>& out)
    {
        ResolveDepthLike(ctx, items, out, /*pick*/ true);
    }

    void MeshRenderer::ResolveDepthLike(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                        Array<ResolvedDraw>& out, bool pick)
    {
        if (!m_ready || items.IsEmpty())
        {
            return;
        }

        const DynamicUniformRing::Range sv = m_shadowViewRing.Allocate();
        if (!sv.ok)
        {
            return;
        }
        if (pick)
        {
            // Pass-level view: the cropped VP, no group id (draws carry their own).
            GpuPickView pv{};
            pv.viewProj = ctx.viewProj;
            pv.sceneTime = ctx.timeSeconds;
            pv.camera = ctx.cameraPos;
            *static_cast<GpuPickView*>(sv.ptr) = pv;
        }
        else
        {
            GpuShadowView svd{};
            svd.lightViewProj = ctx.viewProj;
            svd.sceneTime = ctx.timeSeconds;
            svd.camera = ctx.cameraPos;
            // A faded set's shadow fades from the camera the pass couples to; a pass with none
            // (no view) draws every instance.
            svd.fadeFromCamera = ctx.view != nullptr ? 1u : 0u;
            *static_cast<GpuShadowView*>(sv.ptr) = svd;
        }
        const u32 shadowViewOffset = sv.byteOffset;

        usize i = 0;
        while (i < items.Size())
        {
            // Ours only: dispatchers group runs by rendererId, but a foreign item (a terrain
            // item in a mixed Opaque run) downcast to MeshRenderData is silent UB. Skip it,
            // as the terrain resolver does.
            if (items[i].data->rendererId != RendererId())
            {
                ++i;
                continue;
            }
            const auto* head = static_cast<const MeshRenderData*>(items[i].data);
            // MultiMesh caster: one set, drawn N times from its persistent buffer and the shared ramp,
            // shared across the depth prepass and every shadow cascade (no per-cascade fill).
            if (head->multiMesh)
            {
                const GpuMesh* mmMesh = m_meshes.GetOrUpload(head->mesh);
                if (mmMesh != nullptr)
                {
                    ResolveMultiMeshDepth(ctx, shadowViewOffset,
                                          *static_cast<const MultiMeshRenderData*>(head), *mmMesh,
                                          out, pick);
                }
                ++i;
                continue;
            }
            // Skinned casters batch like static ones, a lone one as a run of one. A run draws skinned
            // or not by its head, so an item that is the other breaks it.
            const bool headSkinned = head->boneMatrices != nullptr && head->boneCount > 0;
            usize j = i + 1;
            while (j < items.Size())
            {
                const auto* nd = static_cast<const MeshRenderData*>(items[j].data);
                if (nd->multiMesh || nd->mesh != head->mesh || nd->material != head->material || !nd->SameSubmeshMaterials(*head) ||
                    (nd->boneMatrices != nullptr && nd->boneCount > 0) != headSkinned ||
                    nd->categorySlots != head->categorySlots) // a split mesh's item draws some slots only
                {
                    break;
                }
                ++j;
            }
            const u32 runLen = static_cast<u32>(j - i);
            const GpuMesh* mesh = m_meshes.GetOrUpload(head->mesh);
            if (mesh != nullptr)
            {
                ResolveDepthInstanced(ctx, shadowViewOffset, items, i, runLen, *mesh, out, pick);
            }
            i = j;
        }
    }

    // What this frame wrote for the GPU, per kind, as profiler counters (a measurement reads them):
    // the rings are mapped memory the device's own counters never see.
    void MeshRenderer::PublishFrameBytes()
    {
        foundation::profiler::Profiler& profiler = foundation::profiler::Profiler::Get();
        if (!profiler.Armed())
        {
            return;
        }
        if (!m_frameBytesCounters)
        {
            using foundation::profiler::CounterUnit;
            m_offsetsBytesCounter = profiler.RegisterCounter(u8"mesh.offsets bytes", CounterUnit::Bytes);
            m_instanceBytesCounter = profiler.RegisterCounter(u8"mesh.instances bytes", CounterUnit::Bytes);
            m_viewBytesCounter = profiler.RegisterCounter(u8"mesh.views bytes", CounterUnit::Bytes);
            m_lightBytesCounter = profiler.RegisterCounter(u8"mesh.lights bytes", CounterUnit::Bytes);
            m_boneBytesCounter = profiler.RegisterCounter(u8"mesh.bones bytes", CounterUnit::Bytes);
            m_frameBytesCounters = true;
        }
        const auto bytes = [](const DynamicUniformRing& ring)
        { return static_cast<f64>(ring.FrameAllocatedSlots()) * static_cast<f64>(ring.SlotSize()); };
        profiler.SetCounter(m_offsetsBytesCounter, bytes(m_offsetsRing));
        profiler.SetCounter(m_instanceBytesCounter, static_cast<f64>(m_instanceTable.FrameStats().bytes));
        profiler.SetCounter(m_viewBytesCounter, static_cast<f64>(m_viewRing.FrameSlots()) * static_cast<f64>(kViewDataSlot) +
                                                    bytes(m_shadowViewRing));
        profiler.SetCounter(m_lightBytesCounter, bytes(m_lightRing) + bytes(m_localShadowRing));
        profiler.SetCounter(m_boneBytesCounter, bytes(m_boneRing));
    }

    void MeshRenderer::FinishFrame()
    {
        PublishFrameBytes();
        m_viewRing.EndFrame();
        m_shadowViewRing.EndFrame();
        m_boneRing.EndFrame();
        m_offsetsRing.EndFrame();
        m_ready = false;
    }

    void MeshRenderer::ResolveInstanced(const RenderRecordContext& ctx, u32 viewOffset,
                                        rhi::BindGroupHandle clusterBG, Span<const DrawItem> items,
                                        usize first, u32 count, const MeshRenderData& head,
                                        const GpuMesh& mesh, Array<ResolvedDraw>& out)
    {
        materials::Material* mat =
            (head.material != nullptr) ? head.material : m_defaultMaterial.Get();
        materials::PipelineConfig config = ConfigFor(head, ctx);
        // Skinned instanced draw: the shared skin stream (joints, weights) is per mesh; each instance's
        // bone base rides in GpuDrawOffsets::boneBase (current) and prevBoneBase, so one draw skins N characters.
        // A skeleton the bone pool could not take (it overflowed) draws in its bind pose, not with
        // another skeleton's bones.
        const bool skinned = head.boneMatrices != nullptr && head.boneCount > 0 && head.mesh != nullptr &&
                             head.mesh->IsSkinned() && mesh.skinBuffer.IsValid() &&
                             m_boneStart.Find(head.boneMatrices) != nullptr;
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }
        // The camera depth prepass already filled this group's InstanceData and draw offsets (identical objects,
        // same order) and cached the range; reuse it instead of allocating and filling again. Falls
        // back to a fresh fill on a miss (no prepass, count mismatch, or ring exhausted). A faded run is
        // never prepassed, and a solid group of the same mesh and material must not lend it its range.
        const bool faded = head.fade > 0.0f;
        u64 offsByteOffset;
        const InstShare* shared =
            faded ? nullptr : m_instShareCache.Find(InstShareKey(ctx.view, items[first].data));
        if (shared != nullptr && shared->count == count)
        {
            offsByteOffset = shared->offsByteOffset;
        }
        else
        {
            if (!RunHasSlots(items, first, count))
            {
                return;
            }
            const DynamicUniformRing::Range offs = m_offsetsRing.AllocateRange(count);
            if (!offs.ok)
            {
                WarnInstanceRingFull("forward", count, m_offsetsRing.FrameAllocatedSlots());
                return;
            }
            GpuDrawOffsets* od = static_cast<GpuDrawOffsets*>(offs.ptr);
            for (u32 k = 0; k < count; ++k)
            {
                const auto* md = static_cast<const MeshRenderData*>(items[first + k].data);
                u32 boneBase = 0, prevBase = 0;
                if (skinned)
                {
                    if (const BoneSlot* s = m_boneStart.Find(md->boneMatrices))
                    {
                        boneBase = s->base;
                        prevBase = s->prevBase;
                    }
                }
                od[k] = GpuDrawOffsets{md->instanceSlot, boneBase, prevBase, md->fade};
            }
            offsByteOffset = offs.byteOffset;
        }

        // Shared per-submesh draw state (view, instance and cluster sets, vertex and index buffers, skinning).
        ResolvedDraw base{};
        base.viewSet = m_viewBG;
        base.viewOffset = viewOffset;
        base.viewDynamic = true; // set 0: view
        base.drawSet = m_instanceBG;
        base.drawDynamic = false;    // set 1: instances (whole)
        base.clusterSet = clusterBG; // set 3: cluster lists
        base.vertexBuffer0 = mesh.vertexBuffer;
        base.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            base.vertexBuffer1 = mesh.skinBuffer;
            base.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream (6/7)
            base.vertexBuffer2 = m_offsetsRing.Buffer();
            base.vertexOffset2 = offsByteOffset; // slot 2: draw offsets (5)
        }
        else
        {
            base.vertexBuffer1 = m_offsetsRing.Buffer();
            base.vertexOffset1 = offsByteOffset; // slot 1: draw offsets (5)
        }
        base.indexBuffer = mesh.indexBuffer;
        base.indexFormat = mesh.indexFormat;
        base.instanceCount = count;

        // Emit one instanced draw for an index sub-range with `m`'s material (set 2 is its bind group):
        // the run's own instance of it when its mesh overrides properties in that `slot` (a mesh with
        // overrides draws as a run of one), else the material's shared instance.
        const auto emit = [&](materials::Material* m, u32 slot, u64 indexOffset, u32 indexCount)
        {
            materials::Material* use = (m != nullptr) ? m : mat;
            rhi::BindGroupLayoutHandle set2 = m_materials->GetOrCreateLayout(*use);
            rhi::PipelineLayoutHandle plLayout = GetOrCreatePipelineLayout(set2);
            if (!plLayout.IsValid())
            {
                return;
            }
            const materials::PipelineConfig own = SlotConfigFor(head, config, m, ctx, skinned);
            rhi::RenderPipelineHandle pso = m_psoCache->GetPipeline(own, plLayout, ctx.colorFormat);
            if (!pso.IsValid())
            {
                return;
            }
            ResolvedDraw d = base;
            d.pso = pso;
            d.materialSet = m_materials->PrepareInstance(*InstanceFor(head, slot, use), set2);
            d.indexOffset = indexOffset;
            d.indexCount = indexCount;
            out.PushBack(d);
        };

        // Multi-material: one instanced draw per submesh (its own material and index range); otherwise
        // one draw. A LOD chain selects one level for the whole batch from the head's bounds
        // (per-batch selection, not per-instance LOD).
        const bool hasChain = head.mesh != nullptr && head.mesh->lodCount > 1;
        if (head.mesh != nullptr && !head.mesh->subMeshes.IsEmpty() &&
            (head.submeshMaterialCount > 0 || hasChain))
        {
            const Span<const geometry::SubMesh> subs =
                hasChain ? head.mesh->SubMeshesForLod(SelectLod(ctx, head))
                         : Span<const geometry::SubMesh>(head.mesh->subMeshes.Data(),
                                                         head.mesh->subMeshes.Size());
            const u64 stride = (mesh.indexFormat == rhi::IndexFormat::UInt16) ? 2u : 4u;
            for (const geometry::SubMesh& sub : subs)
            {
                const bool inRange = sub.materialIndex >= 0 &&
                                     static_cast<u32>(sub.materialIndex) < head.submeshMaterialCount;
                materials::Material* m = inRange ? head.submeshMaterials[sub.materialIndex].Get() : nullptr;
                u32 slot = inRange ? static_cast<u32>(sub.materialIndex) : 0u;
                if (m == nullptr)
                {
                    m = head.material;
                    slot = 0;
                } // OOB/unresolved slot -> slot 0
                if (head.categorySlots && MeshSlotCategory(m, head.fade) != head.category)
                    continue; // another of this mesh's items draws it, in its own pass
                emit(m, slot, mesh.indexOffset + static_cast<u64>(sub.startIndex) * stride,
                     static_cast<u32>(sub.indexCount));
            }
        }
        else
        {
            emit(mat, 0, mesh.indexOffset, mesh.indexCount);
        }
    }

    void MeshRenderer::ResolveDepthInstanced(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                             Span<const DrawItem> items, usize first, u32 count,
                                             const GpuMesh& mesh, Array<ResolvedDraw>& out,
                                             bool pick)
    {
        const auto& head = *static_cast<const MeshRenderData*>(items[first].data);
        // A skeleton the bone pool could not take (it overflowed) draws in its bind pose, not with
        // another skeleton's bones.
        const bool skinned = head.boneMatrices != nullptr && head.boneCount > 0 && head.mesh != nullptr &&
                             head.mesh->IsSkinned() && mesh.skinBuffer.IsValid() &&
                             m_boneStart.Find(head.boneMatrices) != nullptr;
        if (!RunHasSlots(items, first, count))
        {
            return;
        }
        const DynamicUniformRing::Range offs = m_offsetsRing.AllocateRange(count);
        if (!offs.ok)
        {
            WarnInstanceRingFull("depth/shadow", count, m_offsetsRing.FrameAllocatedSlots());
            return;
        }

        // One ring entry per instance in either pass's layout (both 16 bytes): the pick fill is
        // pick-private, never shared with the forward, which reads the previous bone base from z.
        GpuDrawOffsets* od = static_cast<GpuDrawOffsets*>(offs.ptr);
        GpuPickDrawOffsets* pickOd = static_cast<GpuPickDrawOffsets*>(offs.ptr);
        // When this prepass feeds the forward (camera depth prepass, fillInstanceCache), the forward
        // reuses its draw offsets: each names the item's record in the instance table.
        const bool feedsForward = ctx.fillInstanceCache;
        for (u32 k = 0; k < count; ++k)
        {
            const auto* md = static_cast<const MeshRenderData*>(items[first + k].data);
            u32 boneBase = 0, prevBase = 0;
            if (skinned)
            {
                if (const BoneSlot* s = m_boneStart.Find(md->boneMatrices))
                {
                    boneBase = s->base;
                    prevBase = s->prevBase;
                }
            }
            if (pick)
            {
                pickOd[k] = GpuPickDrawOffsets{md->instanceSlot, boneBase,
                                               EntityTag::Generation(md->entityId),
                                               EntityTag::Index(md->entityId) + 1u};
            }
            else
            {
                od[k] = GpuDrawOffsets{md->instanceSlot, boneBase, prevBase, 0.0f};
            }
        }
        if (feedsForward)
        { // record this group's draw offsets range for the forward to reuse
            m_instShareCache.InsertOrAssign(InstShareKey(ctx.view, items[first].data),
                                            InstShare{offs.byteOffset, count});
        }

        ResolvedDraw d{};
        d.viewSet = m_shadowViewBG;
        d.viewOffset = shadowViewOffset;
        d.viewDynamic = true; // set 0: light view
        d.drawSet = m_instanceBG;
        d.drawDynamic = false; // set 1: instances (whole)
        d.vertexBuffer0 = mesh.vertexBuffer;
        d.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            d.vertexBuffer1 = mesh.skinBuffer;
            d.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream
            d.vertexBuffer2 = m_offsetsRing.Buffer();
            d.vertexOffset2 = offs.byteOffset; // slot 2: draw (or pick) offsets
        }
        else
        {
            d.vertexBuffer1 = m_offsetsRing.Buffer();
            d.vertexOffset1 = offs.byteOffset; // slot 1: draw (or pick) offsets
        }
        d.indexBuffer = mesh.indexBuffer;
        d.indexOffset = mesh.indexOffset;
        d.indexFormat = mesh.indexFormat;
        d.indexCount = mesh.indexCount;
        d.instanceCount = count;
        // Each submesh with its own slot's material; a LOD chain's one level for the whole batch
        // (the head's bounds).
        const bool chain = head.mesh != nullptr && head.mesh->lodCount > 1;
        DepthLikeRanges(ctx, head.material, head.submeshMaterials, head.submeshMaterialCount,
                        DepthLikeSubMeshes(ctx, head, chain), chain, pick, m_depthRanges, &head);
        EmitDepthLike(ctx, d, mesh, Span<const DepthLikeRange>(m_depthRanges.Data(), m_depthRanges.Size()), pick, skinned, out);
    }

    void MeshRenderer::ResolveMultiMesh(const RenderRecordContext& ctx, u32 viewOffset,
                                        rhi::BindGroupHandle clusterBG, const MultiMeshRenderData& mm,
                                        const GpuMesh& mesh, Array<ResolvedDraw>& out)
    {
        const MultiMeshSet* set = m_multiMeshSets.Find(mm.key);
        if (set == nullptr || !set->activeInstanceBG.IsValid() || set->count == 0)
        {
            return;
        }
        // Skinned crowd: per-instance bone base in the set's own draw offsets buffer (and the skin stream);
        // static: the shared [i,0,0,0] ramp. A skinned set falls back to static if its skin data is missing.
        const bool skinned =
            set->skinned && set->offsetsBuf.IsValid() && mesh.skinBuffer.IsValid();
        if (!skinned && !m_rampBuffer.IsValid())
        {
            return;
        }
        materials::Material* mat = (mm.material != nullptr) ? mm.material : m_defaultMaterial.Get();
        materials::PipelineConfig config = ConfigFor(mm, ctx);
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }

        ResolvedDraw base{};
        base.viewSet = m_viewBG;
        base.viewOffset = viewOffset;
        base.viewDynamic = true; // set 0: view
        base.drawSet = set->activeInstanceBG;
        base.drawDynamic = false;    // set 1: this set's persistent instances
        base.clusterSet = clusterBG; // set 3: cluster lists
        base.vertexBuffer0 = mesh.vertexBuffer;
        base.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            base.vertexBuffer1 = mesh.skinBuffer;
            base.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream
            base.vertexBuffer2 = set->offsetsBuf;
            base.vertexOffset2 =
                set->offsetsByteOffset; // slot 2: per-set draw offsets (this frame's region)
        }
        else
        {
            base.vertexBuffer1 = m_rampBuffer;
            base.vertexOffset1 = 0; // slot 1: shared draw offsets ramp
        }
        base.indexBuffer = mesh.indexBuffer;
        base.indexFormat = mesh.indexFormat;
        base.instanceCount = set->count;

        const auto emit = [&](materials::Material* m, u64 indexOffset, u32 indexCount)
        {
            materials::Material* use = (m != nullptr) ? m : mat;
            rhi::BindGroupLayoutHandle set2 = m_materials->GetOrCreateLayout(*use);
            rhi::PipelineLayoutHandle plLayout = GetOrCreatePipelineLayout(set2);
            if (!plLayout.IsValid())
            {
                return;
            }
            const materials::PipelineConfig own = SlotConfigFor(mm, config, m, ctx, skinned);
            rhi::RenderPipelineHandle pso = m_psoCache->GetPipeline(own, plLayout, ctx.colorFormat);
            if (!pso.IsValid())
            {
                return;
            }
            ResolvedDraw d = base;
            d.pso = pso;
            d.materialSet = m_materials->PrepareInstance(*InstanceFor(use), set2);
            d.indexOffset = indexOffset;
            d.indexCount = indexCount;
            out.PushBack(d);
        };

        // A LOD chain selects one level for the whole set (from its merged bounds).
        const bool hasChain = mm.mesh != nullptr && mm.mesh->lodCount > 1;
        if (mm.mesh != nullptr && !mm.mesh->subMeshes.IsEmpty() &&
            (mm.submeshMaterialCount > 0 || hasChain))
        {
            const Span<const geometry::SubMesh> subs =
                hasChain ? mm.mesh->SubMeshesForLod(SelectLod(ctx, mm))
                         : Span<const geometry::SubMesh>(mm.mesh->subMeshes.Data(),
                                                         mm.mesh->subMeshes.Size());
            const u64 stride = (mesh.indexFormat == rhi::IndexFormat::UInt16) ? 2u : 4u;
            for (const geometry::SubMesh& sub : subs)
            {
                materials::Material* m =
                    (sub.materialIndex >= 0 &&
                     static_cast<u32>(sub.materialIndex) < mm.submeshMaterialCount)
                        ? mm.submeshMaterials[sub.materialIndex].Get()
                        : nullptr;
                if (m == nullptr)
                {
                    m = mm.material;
                } // OOB/unresolved slot -> slot 0
                emit(m, mesh.indexOffset + static_cast<u64>(sub.startIndex) * stride,
                     static_cast<u32>(sub.indexCount));
            }
        }
        else
        {
            emit(mat, mesh.indexOffset, mesh.indexCount);
        }
    }

    void MeshRenderer::ResolveMultiMeshDepth(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                             const MultiMeshRenderData& mm, const GpuMesh& mesh,
                                             Array<ResolvedDraw>& out, bool pick)
    {
        const MultiMeshSet* set = m_multiMeshSets.Find(mm.key);
        if (set == nullptr || !set->activeInstanceBG.IsValid() || set->count == 0)
        {
            return;
        }
        const bool skinned =
            set->skinned && set->offsetsBuf.IsValid() && mesh.skinBuffer.IsValid() &&
            (!pick || set->pickOffsetsBuf.IsValid());
        if (!skinned && !(pick ? m_pickRampBuffer : m_rampBuffer).IsValid())
        {
            return;
        }
        ResolvedDraw d{};
        d.viewSet = m_shadowViewBG;
        d.viewOffset = shadowViewOffset;
        d.viewDynamic = true; // set 0: light view
        d.drawSet = set->activeInstanceBG;
        d.drawDynamic = false; // set 1: this set's instances
        d.vertexBuffer0 = mesh.vertexBuffer;
        d.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            d.vertexBuffer1 = mesh.skinBuffer;
            d.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream
            // Slot 2: the per-set draw offsets (this frame's region), in the pick layout for a pick.
            d.vertexBuffer2 = pick ? set->pickOffsetsBuf : set->offsetsBuf;
            d.vertexOffset2 = set->offsetsByteOffset; // the same stride and regions in both layouts
        }
        else
        {
            d.vertexBuffer1 = pick ? m_pickRampBuffer : m_rampBuffer; // slot 1: the shared ramp, by layout
            d.vertexOffset1 = 0;
        }
        d.indexBuffer = mesh.indexBuffer;
        d.indexOffset = mesh.indexOffset;
        d.indexFormat = mesh.indexFormat;
        d.indexCount = mesh.indexCount;
        d.instanceCount = set->count;
        // Each submesh with its own slot's material; a LOD chain's one level for the whole set (its
        // merged bounds).
        const bool chain = mm.mesh != nullptr && mm.mesh->lodCount > 1;
        DepthLikeRanges(ctx, mm.material, mm.submeshMaterials, mm.submeshMaterialCount,
                        DepthLikeSubMeshes(ctx, mm, chain), chain, pick, m_depthRanges, &mm);
        EmitDepthLike(ctx, d, mesh, Span<const DepthLikeRange>(m_depthRanges.Data(), m_depthRanges.Size()), pick, skinned, out);
    }

    Span<const geometry::SubMesh> MeshRenderer::DepthLikeSubMeshes(const RenderRecordContext& ctx,
                                                                   const MeshRenderData& md, bool chain)
    {
        if (md.mesh == nullptr)
            return {};
        if (chain)
            return md.mesh->SubMeshesForLod(SelectLod(ctx, md));
        if (md.submeshMaterialCount == 0)
            return {}; // one material covers the mesh
        return Span<const geometry::SubMesh>(md.mesh->subMeshes.Data(), md.mesh->subMeshes.Size());
    }

    void MeshRenderer::DepthLikeRanges(const RenderRecordContext& ctx, materials::Material* slot0,
                                       const RefPtr<materials::Material>* slots, u32 slotCount,
                                       Span<const geometry::SubMesh> subMeshes, bool chain, bool pick, Array<DepthLikeRange>& out, const MeshRenderData* only)
    {
        const bool filter = only != nullptr && only->categorySlots;
        out.Clear();
        if (subMeshes.IsEmpty())
        {
            out.PushBack(DepthLikeRange{slot0, 0, 0, true});
            return;
        }
        for (const geometry::SubMesh& sub : subMeshes)
        {
            materials::Material* m = (slots != nullptr && sub.materialIndex >= 0 &&
                                      static_cast<u32>(sub.materialIndex) < slotCount)
                                         ? slots[sub.materialIndex].Get()
                                         : nullptr;
            if (m == nullptr)
                m = slot0; // an unresolved slot draws as slot 0, as the forward does
            if (filter && MeshSlotCategory(m, only->fade) != only->category)
                continue; // another of the mesh's items draws it (in its own pass, or none)
            out.PushBack(DepthLikeRange{m, static_cast<u32>(sub.startIndex), static_cast<u32>(sub.indexCount), false});
        }
        if (chain || filter || out.IsEmpty())
            return; // a LOD level is its ranges (the buffer holds every level), as is a filtered mesh's
        // One draw of the whole mesh when every range draws alike: one material, or the plain
        // program with no material set for all (opaque slots), as most meshes' shadows are.
        const DepthLikeChoice first = DepthLikeFor(ctx, out[0].material, pick);
        bool alike = true;
        for (usize i = 1; i < out.Size() && alike; ++i)
        {
            if (out[i].material == out[0].material)
                continue;
            const DepthLikeChoice other = DepthLikeFor(ctx, out[i].material, pick);
            alike = !first.materialSet && !other.materialSet && other.config == first.config;
        }
        if (alike)
        {
            materials::Material* m = out[0].material;
            out.Clear();
            out.PushBack(DepthLikeRange{m, 0, 0, true});
        }
    }

    namespace
    {
        // A float of an instance's uniform block, by property name, or `fallback` where the material
        // has no such property (a custom shader that declares none).
        [[nodiscard]] f32 UniformFloat(const materials::MaterialInstance& instance, StringView name,
                                       usize component, f32 fallback) noexcept
        {
            const materials::Material* material = instance.GetMaterial();
            const materials::MaterialPropertyDef* def = material != nullptr ? material->FindProperty(name) : nullptr;
            const Span<const u8> values = instance.UniformData();
            const usize at = def != nullptr ? static_cast<usize>(def->offset) + component * sizeof(f32) : 0;
            if (def == nullptr || !def->IsUniform() || (component + 1) * sizeof(f32) > def->size ||
                at + sizeof(f32) > values.Size())
            {
                return fallback;
            }
            f32 value = fallback;
            MemCopy(&value, values.Data() + at, sizeof(f32));
            return value;
        }
    }

    // The threshold a built-in depth-like program tests the albedo's alpha against: the forward
    // cuts out where albedo alpha x base colour alpha falls below AlphaCutoff, so the albedo alone
    // is tested against AlphaCutoff over the base alpha (a clear base colour cuts out everything).
    f32 MeshRenderer::CutoutThreshold(const materials::MaterialInstance& instance) noexcept
    {
        const f32 cutoff = Clamp(UniformFloat(instance, u8"AlphaCutoff", 0, 0.5f), 0.0f, 1.0f);
        const f32 baseAlpha = Clamp(UniformFloat(instance, u8"BaseColor", 3, 1.0f), 0.0f, 1.0f);
        return baseAlpha > 0.0f ? Min(cutoff / baseAlpha, 2.0f) : 2.0f;
    }

    bool MeshRenderer::DepthLikeDraw(const RenderRecordContext& ctx, materials::Material* material,
                                     bool pick, bool skinned, bool depthStream, ResolvedDraw& d,
                                     bool& usesDepthStream)
    {
        const DepthLikeChoice choice = DepthLikeFor(ctx, material, pick);
        materials::PipelineConfig config = choice.config;
        usesDepthStream = false;
        d.alphaThreshold = -1.0f;
        const auto skin = [skinned, depthStream, pick, &usesDepthStream](materials::PipelineConfig& c)
        { usesDepthStream = ChooseDepthLikeStream(c, skinned, depthStream, pick); };
        skin(config);
        rhi::PipelineLayoutHandle const plain = m_shadowPipelineLayoutInstanced;
        rhi::PipelineLayoutHandle layout = plain;
        rhi::BindGroupHandle matSet{};
        if (choice.materialSet && material != nullptr)
        {
            rhi::BindGroupLayoutHandle set2 = m_materials->GetOrCreateLayout(*material);
            layout = GetOrCreateShadowMaskedLayout(set2);
            materials::MaterialInstance* const instance = InstanceFor(material);
            matSet = m_materials->PrepareInstance(*instance, set2);
            // A built-in program cuts out at the material's own threshold, as its forward draw
            // does; a material's own program reads its cutoff itself.
            if (shaders::HasFlag(config.shaderFlags, shaders::ShaderFlags::AlphaTest) &&
                (config.shaderName == StringView(u8"shadow_depth") || config.shaderName == StringView(u8"pick_ids")))
            {
                d.alphaThreshold = CutoutThreshold(*instance);
            }
            if (!layout.IsValid())
            {
                // No layout over the material's set: the built-in program, without it.
                layout = plain;
                matSet = {};
                d.alphaThreshold = -1.0f;
                config = pick ? PickConfigFor(ctx, false) : ShadowConfigFor(ctx, false);
                skin(config);
            }
        }
        rhi::RenderPipelineHandle pso = m_psoCache->GetPipeline(
            config, layout, pick ? ctx.colorFormat : rhi::TextureFormat::Undefined);
        if (!pso.IsValid())
            return false;
        d.pso = pso;
        d.materialSet = matSet; // set 2: the material (a cut-out, wind, or its own program)
        return true;
    }

    void MeshRenderer::EmitDepthLike(const RenderRecordContext& ctx, const ResolvedDraw& base,
                                     const GpuMesh& mesh, Span<const DepthLikeRange> ranges,
                                     bool pick, bool skinned, Array<ResolvedDraw>& out)
    {
        const u64 stride = (mesh.indexFormat == rhi::IndexFormat::UInt16) ? 2u : 4u;
        const bool depthStream = mesh.depthBuffer.IsValid();
        for (const DepthLikeRange& range : ranges)
        {
            ResolvedDraw d = base;
            bool usesDepthStream = false;
            if (!DepthLikeDraw(ctx, range.material, pick, skinned, depthStream, d, usesDepthStream))
                continue;
            if (usesDepthStream)
            {
                d.vertexBuffer0 = mesh.depthBuffer;
                d.vertexOffset0 = mesh.depthOffset;
            }
            if (!range.whole)
            {
                d.indexOffset = mesh.indexOffset + static_cast<u64>(range.startIndex) * stride;
                d.indexCount = range.indexCount;
            }
            out.PushBack(d);
        }
    }

    bool MeshRenderer::ChooseDepthLikeStream(materials::PipelineConfig& config, bool skinned, bool depthStream,
                                             bool pick) noexcept
    {
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
            return false;
        }
        // The built-in shadow and prepass program reads only position (and uv for a cut-out): given
        // the mesh's slim stream, its vertex fetch is 20 bytes, not the full 52. A shadow pass of a
        // forest is bound by the triangles it pushes, so what each vertex fetches counts.
        if (!depthStream || pick || config.shaderName != StringView(u8"shadow_depth"))
        {
            return false;
        }
        config.vertexLayout = materials::VertexLayoutType::DepthStream;
        config.shaderFlags |= shaders::ShaderFlags::DepthStream;
        return true;
    }

    MeshRenderer::DepthLikeChoice MeshRenderer::DepthLikeFor(const RenderRecordContext& ctx,
                                                             const materials::Material* material,
                                                             bool pick)
    {
        const bool masked =
            material != nullptr && material->pipeline.blendMode == materials::BlendMode::Masked;
        const shaders::ShaderFlags effects = MaterialEffectFlags(material);
        DepthLikeChoice choice;
        choice.config = pick ? PickConfigFor(ctx, masked, effects)
                             : ShadowConfigFor(ctx, masked, effects);
        // The material's shader has its own program for this pass (it moves its vertices, or cuts
        // out with its own alpha): drawn with it, over the material's set 2 it was generated for.
        const StringView own =
            material == nullptr ? StringView{}
                                : (pick ? material->pickShaderName.AsView()
                                        : material->depthShaderName.AsView());
        if (!own.IsEmpty())
        {
            choice.config.shaderName = own;
        }
        if (material != nullptr)
        {
            // The material's faces, in every depth-like pass: a two-sided card casts its shadow and
            // fills the prepass from both sides, and picks from both, as the forward draws it.
            choice.config.cullMode = material->pipeline.cullMode;
        }
        // An active effect's block is in the material's set (b1), so its draw binds the set.
        choice.materialSet = masked || effects != shaders::ShaderFlags::None || !own.IsEmpty();
        return choice;
    }

    materials::PipelineConfig MeshRenderer::ShadowConfigFor(const RenderRecordContext& ctx,
                                                            bool masked,
                                                            shaders::ShaderFlags effects)
    {
        materials::PipelineConfig c{};
        c.shaderName = u8"shadow_depth";
        c.shaderFlags |= effects; // the caster moves as the mesh does (a swaying card's shadow sways)
        c.vertexLayout = materials::VertexLayoutType::Mesh;
        c.instanced = true;
        c.shaderFlags |= shaders::ShaderFlags::Instanced;
        // Masked casters run the alpha-test fragment (it samples cutout alpha and discards) so their shadows
        // have holes; opaque casters stay vertex-only (depthOnly, no fragment).
        c.depthOnly = !masked;
        c.colorTargetCount = 0;
        if (masked)
        {
            c.shaderFlags |= shaders::ShaderFlags::AlphaTest;
        }
        c.depthFormat = ctx.depthFormat;
        // Scene-pass MSAA: only the camera depth prepass is multisampled (its depth must
        // match the MSAA forward for early-Z). Shadow-map passes (ctx.depthPrepass == false) stay
        // single-sample; shadows are not multisampled.
        c.sampleCount = ctx.depthPrepass ? ctx.sampleCount : static_cast<u8>(1);
        c.depthMode = materials::DepthMode::ReadWrite;
        c.depthCompare = rhi::depth::Nearer();
        // Render front faces into the shadow map (cull back), the conventional default: flat and
        // architectural casters get tight contacts. Curved casters (spheres) keep a small grazing-contact
        // gap inherent to shadow maps; a contact or screen-space shadow pass would close it, where a
        // cull-mode or bias change only moves the gap onto flat casters, which are far more common.
        // A material's own cull mode replaces it (DepthLikeFor): a two-sided one casts from both.
        c.cullMode = materials::CullModeConfig::Back;
        // Hardware depth bias: a constant offset and a slope-scaled term, applied in shadow-map depth
        // space (so it adds little visible spatial gap, unlike the normal offset). Pairs with the
        // receiver-side (1 - NdotL) normal-offset bias in lighting.hlsli for acne control. The camera
        // depth prepass must not bias: its depth has to equal the forward pass's exactly so the
        // NearerOrEqual early-Z accepts the redrawn opaque fragments (a bias would reject them). The
        // bias pushes casters away from the light; its sign follows the depth convention.
        if (!ctx.depthPrepass)
        {
            c.depthBias = static_cast<i16>(rhi::depth::BiasAwayFromViewer(50));
            c.depthBiasSlopeScale = rhi::depth::SlopeBiasAwayFromViewer(1.5f);
            // The cascade fit bounds the receivers the camera sees; a tall caster toward the light
            // reaches past its near plane and was clipped away, its shadow with it. Clamped, it
            // lands on the near plane and still occludes everything behind it. Only a directional
            // cascade's pass clamps: a spot's or point's would clamp its own lamp's housing.
            c.depthClamp = ctx.clampShadowCasters;
        }
        return c;
    }

    materials::PipelineConfig MeshRenderer::PickConfigFor(const RenderRecordContext& ctx,
                                                          bool masked,
                                                          shaders::ShaderFlags effects)
    {
        materials::PipelineConfig c{};
        c.shaderName = u8"pick_ids";
        c.shaderFlags |= effects; // the pick follows the mesh as it moves
        c.vertexLayout = materials::VertexLayoutType::Mesh;
        c.instanced = true;
        c.shaderFlags |= shaders::ShaderFlags::Instanced;
        if (masked)
        {
            c.shaderFlags |= shaders::ShaderFlags::AlphaTest; // cutouts are not pickable holes
        }
        c.depthOnly = false; // the id-writing fragment
        c.colorTargetCount = 1;
        c.colorFormats[0] = ctx.colorFormat; // RG32Uint (kPickIdFormat); overridden per build too
        c.blendMode = materials::BlendMode::Opaque; // integer target: no blend
        c.colorWriteMask = rhi::ColorWriteMask::All;
        c.depthFormat = ctx.depthFormat;
        c.sampleCount = 1;
        c.depthMode = materials::DepthMode::ReadWrite;
        c.depthCompare = rhi::depth::Nearer(); // nearest surface owns the texel
        c.cullMode = materials::CullModeConfig::Back; // callers override from the material
        return c;
    }

    materials::PipelineConfig MeshRenderer::ConfigFor(const MeshRenderData& md,
                                                      const RenderRecordContext& ctx)
    {
        return ConfigFor(md, md.material, ctx);
    }

    materials::PipelineConfig MeshRenderer::SlotConfigFor(const MeshRenderData& md,
                                                          const materials::PipelineConfig& base,
                                                          const materials::Material* slot,
                                                          const RenderRecordContext& ctx, bool skinned)
    {
        if (slot == nullptr || slot == md.material)
        {
            return base;
        }
        materials::PipelineConfig config = ConfigFor(md, slot, ctx);
        // A slot of another blend class (a transparent slot on an opaque mesh) belongs to another
        // pass, whose targets this one does not have: it keeps the mesh's pipeline. An item of a
        // mesh split by category draws its own pass's slots alone, each with its own pipeline.
        if (!md.categorySlots && config.colorTargetCount != base.colorTargetCount)
        {
            return base;
        }
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }
        return config;
    }

    materials::PipelineConfig MeshRenderer::ConfigFor(const MeshRenderData& md, const materials::Material* material,
                                                      const RenderRecordContext& ctx)
    {
        materials::PipelineConfig config =
            (material != nullptr) ? material->pipeline : materials::PipelineConfig::ForOpaqueMesh(u8"forward");
        config.depthFormat = ctx.depthFormat;
        // Scene-pass MSAA: the opaque pass records at the view's count (set in ctx) and the
        // transparent pass at 1, so the forward mesh PSO matches its pass's MSAA attachments. Shadow
        // PSOs use ShadowConfigFor and are unaffected; shadows stay single-sample.
        config.sampleCount = ctx.sampleCount;
        config.instanced = true;
        config.shaderFlags |= shaders::ShaderFlags::Instanced;
        config.shaderFlags |= MaterialEffectFlags(material); // its active effects (the Wind group sways it)
        if (md.fade > 0.0f)
        {
            config.shaderFlags |= shaders::ShaderFlags::Dither; // the screen-door fade (a cutaway)
        }
        // Opaque and masked render the MRT G-buffer pass: target 0 is the shaded color (format overridden
        // per view at build); targets 1 and 2 the view-space normal and motion vector, 3 roughness and
        // metallic, 4 diffuse albedo (the GBUFFER permutation writes them). Transparent renders a separate
        // color-only pass, so it stays single-target.
        const bool gbuffer = (config.blendMode == materials::BlendMode::Opaque ||
                              config.blendMode == materials::BlendMode::Masked);
        if (gbuffer)
        {
            config.colorTargetCount = 5;
            config.colorFormats[1] = kGNormalFormat;
            config.colorFormats[2] = kGVelocityFormat;
            config.colorFormats[3] = kGMaterialFormat; // SSR: roughness and metallic
            config.colorFormats[4] = kGAlbedoFormat;   // SSGI: diffuse albedo
            config.shaderFlags |= shaders::ShaderFlags::GBuffer;
            // Equal-depth fragments from the depth prepass must pass (early-Z shades each opaque pixel once).
            config.depthCompare = rhi::depth::NearerOrEqual();
            // Masked: enable the alpha-test discard permutation (opaque-like, but cuts sub-cutoff pixels).
            if (config.blendMode == materials::BlendMode::Masked)
            {
                config.shaderFlags |= shaders::ShaderFlags::AlphaTest;
            }
        }
        else
        {
            config.colorTargetCount = 1; // transparent: color-only
            config.depthMode = TransparentPassDepth(config.depthMode);
        }
        return config;
    }

    bool MeshRenderer::MakeLayout(const rhi::BindGroupLayoutEntry& entry,
                                  rhi::BindGroupLayoutHandle& out)
    {
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{&entry, 1};
        return m_device->CreateBindGroupLayout(ld, out).IsOk();
    }

    bool MeshRenderer::MakePipelineLayout(rhi::BindGroupLayoutHandle set0, rhi::BindGroupLayoutHandle set1,
                                          rhi::BindGroupLayoutHandle set2, rhi::BindGroupLayoutHandle set3,
                                          rhi::PipelineLayoutHandle& out)
    {
        rhi::BindGroupLayoutHandle layouts[] = {set0, set1, set2, set3};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 4};
        return m_device->CreatePipelineLayout(pld, out).IsOk();
    }

    bool MeshRenderer::MakePipelineLayout(rhi::BindGroupLayoutHandle set0, rhi::BindGroupLayoutHandle set1,
                                          rhi::PipelineLayoutHandle& out)
    {
        rhi::BindGroupLayoutHandle layouts[] = {set0, set1};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 2};
        return m_device->CreatePipelineLayout(pld, out).IsOk();
    }

    bool MeshRenderer::MakePipelineLayout3(rhi::BindGroupLayoutHandle set0, rhi::BindGroupLayoutHandle set1,
                                           rhi::BindGroupLayoutHandle set2, rhi::PipelineLayoutHandle& out)
    {
        rhi::BindGroupLayoutHandle layouts[] = {set0, set1, set2};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 3};
        return m_device->CreatePipelineLayout(pld, out).IsOk();
    }

    rhi::PipelineLayoutHandle MeshRenderer::GetOrCreateShadowMaskedLayout(rhi::BindGroupLayoutHandle set2)
    {
        const u64 key = set2.Key();
        if (rhi::PipelineLayoutHandle* cached = m_shadowMaskedLayouts.Find(key))
        {
            return *cached;
        }
        rhi::BindGroupLayoutHandle set1 = m_instanceLayout;
        // The built-in programs' cut-out threshold (GpuCutoutPush) is the fragment's push block; a
        // material's own program ignores it.
        rhi::BindGroupLayoutHandle layouts[] = {m_shadowViewLayout, set1, set2};
        rhi::PushConstantRange cutout{};
        cutout.stages = rhi::ShaderStage::Fragment;
        cutout.offset = 0;
        cutout.size = sizeof(GpuCutoutPush);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayoutHandle const>{layouts, 3};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&cutout, 1};
        rhi::PipelineLayoutHandle layout{};
        if (!m_device->CreatePipelineLayout(pld, layout).IsOk())
        {
            return {};
        }
        m_shadowMaskedLayouts.InsertOrAssign(key, layout);
        return layout;
    }

    Status MeshRenderer::CreateDefaultMaterial()
    {
        m_defaultMaterial = materials::CreatePBR(u8"__default_pbr");
        return (m_defaultMaterial.Get() != nullptr) ? Status{} : Status{ErrorCode::Unknown};
    }

    materials::MaterialInstance* MeshRenderer::InstanceFor(materials::Material* material)
    {
        // Keyed by the material's UID, never its pointer: a hot-reloaded material can
        // reallocate at the freed address and would silently reuse the stale instance
        // (whose bind group references destroyed texture views).
        if (materials::MaterialInstance** found = m_instances.Find(material->uid))
        {
            return *found;
        }
        UniquePtr<materials::MaterialInstance> created = NewInstance(material);
        materials::MaterialInstance* inst = created.Get();
        m_instanceStorage.PushBack(Move(created));       // owns the instance
        m_instances.InsertOrAssign(material->uid, inst); // lookup (storage owns the instance)
        return inst;
    }

    UniquePtr<materials::MaterialInstance> MeshRenderer::NewInstance(materials::Material* material)
    {
        return MakeUnique<materials::MaterialInstance>(*m_allocator, material);
    }

    materials::MaterialInstance* MeshRenderer::InstanceFor(const MeshRenderData& md, u32 slot,
                                                           materials::Material* material)
    {
        bool overridden = false;
        for (u32 i = 0; i < md.overrideCount && !overridden; ++i)
        {
            overridden = md.overrides[i].slot == slot;
        }
        if (!overridden)
        {
            return InstanceFor(material);
        }
        const u64 identity[3] = {md.scene, md.entityId, slot};
        const u64 key = HashBytes(identity, sizeof(identity));
        OverrideInstance* entry = m_overrideInstances.Find(key);
        if (entry != nullptr && (entry->scene != md.scene || entry->entityId != md.entityId || entry->slot != slot ||
                                 entry->materialUid != material->uid))
        {
            // Another mesh hashed here, or the slot's material changed: start over with the new one.
            ReleaseInstance(*entry->instance);
            m_overrideInstances.Remove(key);
            entry = nullptr;
        }
        if (entry == nullptr)
        {
            OverrideInstance created;
            created.scene = md.scene;
            created.entityId = md.entityId;
            created.slot = slot;
            created.materialUid = material->uid;
            created.instance = NewInstance(material);
            entry = &m_overrideInstances.InsertOrAssign(key, Move(created));
        }
        entry->lastUsed = m_frameSerial;
        if (!entry->applied || entry->version != md.overrideVersion)
        {
            // Back to the material's own values, then this slot's overrides over them. A name
            // `Effect.Property` (`Wind.Strength`) reaches the material's effect group: the instance's
            // setters route it there. This instance is the forward pass's alone: the shadow and pick
            // passes draw with the shared material's (RenderData.cppm, MeshRenderData::overrides), and
            // the variant follows the shared material's gate (MaterialEffectFlags), so an override
            // changes how an active effect moves the visible mesh, not whether it is on.
            materials::MaterialInstance& inst = *entry->instance;
            for (const materials::MaterialPropertyDef& d : material->Properties())
            {
                if (d.IsUniform())
                {
                    inst.ResetProperty(d.name);
                }
            }
            inst.ResetEffects();
            for (u32 i = 0; i < md.overrideCount; ++i)
            {
                const MaterialPropertyOverride& o = md.overrides[i];
                if (o.slot != slot)
                {
                    continue;
                }
                if (o.size == sizeof(f32))
                {
                    inst.SetFloat(o.name, o.value.x);
                }
                else
                {
                    inst.SetFloat4(o.name, o.value);
                }
            }
            entry->version = md.overrideVersion;
            entry->applied = true;
        }
        return entry->instance.Get();
    }

    void MeshRenderer::ReleaseInstance(materials::MaterialInstance& instance)
    {
        ReleaseBindGroup(m_materials->DetachBindGroup(&instance));
        ReleaseBuffer(m_materials->DetachUniformBuffer(&instance));
        ReleaseEffectBuffers(instance);
    }

    void MeshRenderer::ReleaseEffectBuffers(materials::MaterialInstance& instance)
    {
        Array<rhi::BufferHandle> buffers(*m_allocator);
        m_materials->DetachEffectBuffers(&instance, buffers);
        for (rhi::BufferHandle buffer : buffers)
        {
            ReleaseBuffer(buffer);
        }
    }

    void MeshRenderer::PruneOverrideInstances()
    {
        // Unused for longer than the frames in flight: no draw wants it any more.
        Array<u64> stale(*m_allocator);
        for (auto& entry : m_overrideInstances)
        {
            if (entry.value.lastUsed + m_framesInFlight + 1 < m_frameSerial)
            {
                stale.PushBack(entry.key);
            }
        }
        for (u64 key : stale)
        {
            if (OverrideInstance* entry = m_overrideInstances.Find(key))
            {
                ReleaseInstance(*entry->instance);
            }
            m_overrideInstances.Remove(key);
        }
    }

    shaders::ShaderFlags MeshRenderer::MaterialEffectFlags(const materials::Material* material) noexcept
    {
        shaders::ShaderFlags flags = shaders::ShaderFlags::None;
        if (material == nullptr)
        {
            return flags;
        }
        for (const materials::MaterialEffectGroup& group : material->EffectGroups())
        {
            if (group.IsActive())
            {
                flags |= group.flag;
            }
        }
        return flags;
    }

    rhi::BindGroupHandle MeshRenderer::MaterialBindGroup(materials::Material* material)
    {
        rhi::BindGroupLayoutHandle layout = m_materials->GetOrCreateLayout(*material);
        return m_materials->PrepareInstance(*InstanceFor(material), layout);
    }

    rhi::PipelineLayoutHandle MeshRenderer::GetOrCreatePipelineLayout(rhi::BindGroupLayoutHandle set2)
    {
        const u64 key = set2.Key();
        if (rhi::PipelineLayoutHandle* cached = m_pipelineLayouts.Find(key))
        {
            return *cached;
        }
        rhi::BindGroupLayoutHandle set1 = m_instanceLayout;
        rhi::PipelineLayoutHandle layout{};
        if (!MakePipelineLayout(m_viewLayout, set1, set2, m_clusterLayout, layout))
        {
            return {};
        }
        m_pipelineLayouts.InsertOrAssign(key, layout);
        return layout;
    }

    void MeshRenderer::ReleaseBindGroup(rhi::BindGroupHandle bg)
    {
        m_device->DestroyBindGroup(bg); // kept by the device while a frame in flight uses it
    }

    void MeshRenderer::PruneStaleMaterialInstances()
    {
        usize w = 0;
        for (usize i = 0; i < m_instanceStorage.Size(); ++i)
        {
            materials::MaterialInstance* inst = m_instanceStorage[i].Get();
            materials::Material* material = inst->GetMaterial();
            if (material != nullptr && material->RefCount() <= 1)
            {
                ReleaseBindGroup(m_materials->DetachBindGroup(inst));
                ReleaseBuffer(m_materials->DetachUniformBuffer(inst));
                ReleaseEffectBuffers(*inst);
                m_instances.Remove(material->uid);
                continue; // dropping the UniquePtr slot destroys the instance (bind group already detached)
            }
            if (w != i)
            {
                m_instanceStorage[w] = Move(m_instanceStorage[i]);
            }
            ++w;
        }
        m_instanceStorage.Resize(w);
    }

    void MeshRenderer::ReleaseBuffer(rhi::BufferHandle buffer)
    {
        m_device->DestroyBuffer(buffer); // kept by the device while a frame in flight uses it
    }

    bool MeshRenderer::EnsureViewBindGroup(const RenderRecordContext& ctx)
    {
        if (!m_activeShadowView.IsValid())
        {
            m_activeShadowView = m_dummyShadowView;
        }
        if (!m_activeAtlasView.IsValid())
        {
            m_activeAtlasView = m_dummyAtlasView;
        }
        if (!m_activeProbeCube.IsValid())
        {
            m_activeProbeCube = m_dummyProbeCubeView;
        }
        if (!m_activeProbeBuffer.IsValid())
        {
            m_activeProbeBuffer = m_dummyProbeBuffer;
        }
        // This view's scene's IBL products (dummies when IBL is off or unavailable).
        const bool iblActive = ctx.ibl.valid && ctx.ibl.shBuffer.IsValid() &&
                               ctx.ibl.prefilterView.IsValid() && ctx.ibl.brdfView.IsValid();
        rhi::BufferHandle sh = iblActive ? ctx.ibl.shBuffer : m_dummyShBuffer;
        rhi::TextureViewHandle prefilter = iblActive ? ctx.ibl.prefilterView : m_dummyCubeView;
        rhi::TextureViewHandle brdf = iblActive ? ctx.ibl.brdfView : m_dummyBrdfView;
        const u64 iblGen = iblActive ? ctx.ibl.generation : 0;

        if (m_viewBGs.Size() <= ctx.viewIndex)
        {
            m_viewBGs.Resize(ctx.viewIndex + 1u);
        }
        ViewBGSlot& slot = m_viewBGs[ctx.viewIndex];
        if (slot.bg.IsValid() && slot.viewGen == m_viewRing.Generation() &&
            slot.lightGen == m_lightRing.Generation() && slot.shadow == m_activeShadowView &&
            slot.shadowGen == m_activeShadowGen && slot.atlas == m_activeAtlasView &&
            slot.atlasGen == m_activeAtlasGen && slot.localGen == m_localShadowRing.Generation() &&
            slot.boneGen == m_boneDeviceGen && slot.iblGen == iblGen &&
            slot.prefilter == prefilter && slot.probeCube == m_activeProbeCube &&
            slot.probeBuf == m_activeProbeBuffer)
        {
            m_viewBG = slot.bg;
            return true;
        }
        ReleaseBindGroup(slot.bg);
        slot.bg = {};
        m_viewBG = {};
        rhi::BufferHandle viewBuf = m_viewRing.Buffer();
        rhi::BufferHandle lightBuf = m_lightRing.Buffer();
        rhi::BufferHandle localBuf = m_localShadowRing.Buffer();
        rhi::BufferHandle boneBuf = m_boneDevice; // VS reads the device mirror, not the staging ring
        if (!viewBuf.IsValid() || !lightBuf.IsValid() || !localBuf.IsValid() ||
            !boneBuf.IsValid() || !m_activeShadowView.IsValid() || !m_activeAtlasView.IsValid() ||
            !m_shadowSampler.IsValid() || !sh.IsValid() || !prefilter.IsValid() ||
            !brdf.IsValid() || !m_envSampler.IsValid())
        {
            return false;
        }
        // Order must match the set-0 layout: view UBO, lights, cascade map (t1), local atlas (t2),
        // local-shadow entries (t3), comparison sampler (s0), bone-matrix pool (t4), IBL SH9 (t5),
        // prefilter cube (t6), BRDF LUT (t7), env sampler (s1). Buffers bound whole.
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(viewBuf, 0, sizeof(GpuView)),
            rhi::BindGroupEntry::BufferEntry(lightBuf, 0, m_lightRing.ByteCapacity()),
            rhi::BindGroupEntry::TextureEntry(m_activeShadowView),
            rhi::BindGroupEntry::TextureEntry(m_activeAtlasView),
            rhi::BindGroupEntry::BufferEntry(localBuf, 0, m_localShadowRing.ByteCapacity()),
            rhi::BindGroupEntry::SamplerEntry(m_shadowSampler),
            rhi::BindGroupEntry::BufferEntry(boneBuf, 0, m_boneDeviceBytes),
            rhi::BindGroupEntry::BufferEntry(sh, 0, kShBytes),
            rhi::BindGroupEntry::TextureEntry(prefilter),
            rhi::BindGroupEntry::TextureEntry(brdf),
            rhi::BindGroupEntry::SamplerEntry(m_envSampler),
            rhi::BindGroupEntry::TextureEntry(m_activeProbeCube),
            rhi::BindGroupEntry::BufferEntry(m_activeProbeBuffer, 0, kProbeBufferBytes),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_viewLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 13};
        if (!m_device->CreateBindGroup(bgd, slot.bg).IsOk())
        {
            slot.bg = {};
            return false;
        }
        slot.viewGen = m_viewRing.Generation();
        slot.lightGen = m_lightRing.Generation();
        slot.shadow = m_activeShadowView;
        slot.shadowGen = m_activeShadowGen;
        slot.atlas = m_activeAtlasView;
        slot.atlasGen = m_activeAtlasGen;
        slot.localGen = m_localShadowRing.Generation();
        slot.boneGen = m_boneDeviceGen;
        slot.iblGen = iblGen;
        slot.prefilter = prefilter;
        slot.probeCube = m_activeProbeCube;
        slot.probeBuf = m_activeProbeBuffer;
        m_viewBG = slot.bg;
        return true;
    }

    Status MeshRenderer::CreateShadowResources()
    {
        rhi::SamplerDesc sd{};
        sd.minFilter = rhi::FilterMode::Linear;
        sd.magFilter = rhi::FilterMode::Linear;
        sd.mipmapFilter = rhi::MipmapFilterMode::Nearest;
        sd.addressU = rhi::AddressMode::ClampToEdge;
        sd.addressV = rhi::AddressMode::ClampToEdge;
        sd.addressW = rhi::AddressMode::ClampToEdge;
        sd.compare = rhi::depth::NearerOrEqual(); // lit when the receiver is at or nearer than the occluder
        sd.label = u8"mesh.shadowSampler";
        if (!m_device->CreateSampler(sd, m_shadowSampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::Depth32Float;
        td.width = 1;
        td.height = 1;
        td.arrayLayerCount = 1;
        td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        td.label = u8"mesh.dummyShadow";
        if (!m_device->CreateTexture(td, m_dummyShadowTex).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // A Texture2DArray view (1 layer) so it matches the shader's Texture2DArray shadow binding.
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::Depth32Float;
        vd.aspect = rhi::TextureAspect::DepthOnly;
        vd.dimension = rhi::TextureViewDimension::Texture2DArray;
        vd.arrayLayerCount = 1;
        if (!m_device->CreateTextureView(m_dummyShadowTex, vd, m_dummyShadowView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        m_activeShadowView = m_dummyShadowView;

        // A 1x1 Texture2D dummy for the local-shadow atlas (t2) and a 1-element dummy data buffer (t3),
        // bound when no local shadow caster exists this frame (the descriptor set stays complete).
        rhi::TextureDesc atd{};
        atd.format = rhi::TextureFormat::Depth32Float;
        atd.width = 1;
        atd.height = 1;
        atd.arrayLayerCount = 2;
        atd.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        atd.label = u8"mesh.dummyAtlas";
        if (!m_device->CreateTexture(atd, m_dummyAtlasTex).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc avd{};
        avd.format = rhi::TextureFormat::Depth32Float;
        avd.aspect = rhi::TextureAspect::DepthOnly;
        avd.dimension = rhi::TextureViewDimension::Texture2DArray;
        avd.arrayLayerCount = 2;
        if (!m_device->CreateTextureView(m_dummyAtlasTex, avd, m_dummyAtlasView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        m_activeAtlasView = m_dummyAtlasView;

        // IBL fallbacks (bound when no environment is active): a zero-filled SH9 buffer (no diffuse
        // ambient), a 1x1x6 black cube (prefilter), a 1x1 black BRDF LUT, and a linear-clamp env
        // sampler. The color dummies transition from UNDEFINED to ShaderRead once (with the depth dummies).
        rhi::SamplerDesc es{};
        es.minFilter = rhi::FilterMode::Linear;
        es.magFilter = rhi::FilterMode::Linear;
        es.mipmapFilter = rhi::MipmapFilterMode::Linear;
        es.addressU = rhi::AddressMode::ClampToEdge;
        es.addressV = rhi::AddressMode::ClampToEdge;
        es.addressW = rhi::AddressMode::ClampToEdge;
        es.label = u8"mesh.envSampler";
        if (!m_device->CreateSampler(es, m_envSampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BufferDesc shd{};
        shd.size = kShBytes;
        shd.usage = rhi::BufferUsage::Storage;
        shd.memory = rhi::MemoryLocation::GpuOnly;
        shd.label = u8"mesh.dummySH";
        if (!m_device->CreateBuffer(shd, m_dummyShBuffer).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::TextureDesc cd{};
        cd.format = rhi::TextureFormat::RGBA16Float;
        cd.width = 1;
        cd.height = 1;
        cd.arrayLayerCount = 6;
        cd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        cd.label = u8"mesh.dummyCube";
        if (!m_device->CreateTexture(cd, m_dummyCube).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc cvd{};
        cvd.format = rhi::TextureFormat::RGBA16Float;
        cvd.dimension = rhi::TextureViewDimension::TextureCube;
        cvd.arrayLayerCount = 6;
        if (!m_device->CreateTextureView(m_dummyCube, cvd, m_dummyCubeView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::TextureDesc ld{};
        ld.format = rhi::TextureFormat::RG16Float;
        ld.width = 1;
        ld.height = 1;
        ld.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        ld.label = u8"mesh.dummyBRDF";
        if (!m_device->CreateTexture(ld, m_dummyBrdf).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc lvd{};
        lvd.format = rhi::TextureFormat::RG16Float;
        lvd.dimension = rhi::TextureViewDimension::Texture2D;
        if (!m_device->CreateTextureView(m_dummyBrdf, lvd, m_dummyBrdfView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // Dummy probe cube array (6 layers, 1 cube) bound when no probe is active; count 0 keeps the
        // forward on the global IBL reflection, so the content is irrelevant.
        rhi::TextureDesc pcd{};
        pcd.format = rhi::TextureFormat::RGBA16Float;
        pcd.width = 1;
        pcd.height = 1;
        pcd.arrayLayerCount = 6;
        pcd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        pcd.label = u8"mesh.dummyProbeCube";
        if (!m_device->CreateTexture(pcd, m_dummyProbeCube).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc pcv{};
        pcv.format = rhi::TextureFormat::RGBA16Float;
        pcv.dimension = rhi::TextureViewDimension::TextureCubeArray;
        pcv.arrayLayerCount = 6;
        if (!m_device->CreateTextureView(m_dummyProbeCube, pcv, m_dummyProbeCubeView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // Dummy probe-metadata buffer (t9) for the no-probe path: never read (count 0), just a valid SRV.
        rhi::BufferDesc pbd{};
        pbd.size = kProbeBufferBytes;
        pbd.usage = rhi::BufferUsage::Storage;
        pbd.memory = rhi::MemoryLocation::GpuOnly;
        pbd.label = u8"mesh.dummyProbeBuf";
        if (!m_device->CreateBuffer(pbd, m_dummyProbeBuffer).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        m_activeProbeCube = m_dummyProbeCubeView;
        m_activeProbeBuffer = m_dummyProbeBuffer;
        return Status{};
    }

    bool MeshRenderer::EnsureShadowViewBindGroup()
    {
        if (m_shadowViewBG.IsValid() && m_shadowViewBGGen == m_shadowViewRing.Generation() &&
            m_shadowViewBGBoneGen == m_boneDeviceGen)
        {
            return true;
        }
        ReleaseBindGroup(m_shadowViewBG);
        m_shadowViewBG = {};
        rhi::BufferHandle buf = m_shadowViewRing.Buffer();
        rhi::BufferHandle boneBuf = m_boneDevice; // shared device mirror (same as the forward set 0)
        if (!buf.IsValid() || !boneBuf.IsValid())
        {
            return false;
        }
        rhi::BindGroupEntry be[] = {
            // The range is the larger of the two layouts that read this slot: ShadowView (96) and
            // the pick pass's PickView (96). WebGPU validates the bound size against the shader's
            // declared cbuffer, so the range must cover both; the slot (256) has room.
            rhi::BindGroupEntry::BufferEntry(buf, 0, Max(sizeof(GpuPickView), sizeof(GpuShadowView))),
            rhi::BindGroupEntry::BufferEntry(boneBuf, 0,
                                             m_boneDeviceBytes), // t4: skinning pool
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_shadowViewLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, m_shadowViewBG).IsOk())
        {
            m_shadowViewBG = {};
            return false;
        }
        m_shadowViewBGGen = m_shadowViewRing.Generation();
        m_shadowViewBGBoneGen = m_boneDeviceGen;
        return true;
    }

    Status MeshRenderer::CreateDummyClusters()
    {
        rhi::BufferDesc obd{};
        obd.size = sizeof(u32) * 2;
        obd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
        obd.memory = rhi::MemoryLocation::GpuOnly;
        obd.label = u8"cluster.dummyOffsets";
        if (!m_device->CreateBuffer(obd, m_dummyClusterOffsets).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BufferDesc ibd{};
        ibd.size = sizeof(u32);
        ibd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
        ibd.memory = rhi::MemoryLocation::GpuOnly;
        ibd.label = u8"cluster.dummyIndices";
        if (!m_device->CreateBuffer(ibd, m_dummyClusterIndices).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // Zero-initialize dummy cluster buffers. DX12 default-heap buffers have undefined
        // contents; the forward shader reads ClusterOffsets[].y as a loop count, so garbage
        // data causes an infinite GPU loop (TDR). Vulkan happens to zero-initialize.
        {
            rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics);
            rhi::TransferBatch* batch = nullptr;
            if (q != nullptr && q->CreateTransferBatch(batch).IsOk() && batch != nullptr)
            {
                u32 zeros[2] = {0, 0};
                batch->WriteBuffer(m_dummyClusterOffsets, 0,
                                   Span<const u8>(reinterpret_cast<const u8*>(zeros), sizeof(zeros)));
                u32 zero = 0;
                batch->WriteBuffer(m_dummyClusterIndices, 0,
                                   Span<const u8>(reinterpret_cast<const u8*>(&zero), sizeof(zero)));
                batch->Submit();
                q->DestroyTransferBatch(batch);
            }
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(m_dummyClusterOffsets, 0, sizeof(u32) * 2),
            rhi::BindGroupEntry::BufferEntry(m_dummyClusterIndices, 0, sizeof(u32)),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_clusterLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
        if (!m_device->CreateBindGroup(bgd, m_dummyClusterBG).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    rhi::BindGroupHandle MeshRenderer::EnsureClusterBindGroup(u32 viewIndex, u32 frameIndex,
                                                         rhi::BufferHandle offsets, rhi::BufferHandle indices,
                                                         u32 version)
    {
        if (!offsets.IsValid() || !indices.IsValid())
        {
            return m_dummyClusterBG;
        }
        ClusterSlot& slot = m_clusterSlots.At(viewIndex, frameIndex);
        if (slot.group.IsValid() && slot.offsets == offsets && slot.version == version)
        {
            return slot.group;
        }
        if (slot.group.IsValid())
        {
            m_device->DestroyBindGroup(slot.group);
            slot.group = {};
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(offsets, 0, m_device->Describe(offsets)->size),
            rhi::BindGroupEntry::BufferEntry(indices, 0, m_device->Describe(indices)->size),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_clusterLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
        if (!m_device->CreateBindGroup(bgd, slot.group).IsOk())
        {
            slot.group = {};
            return m_dummyClusterBG;
        }
        slot.offsets = offsets;
        slot.version = version;
        return slot.group;
    }

    void MeshRenderer::Shutdown()
    {
        // Release material instances first (their dtors notify the still-live MaterialSystem, which
        // owns and destroys their data-driven bind groups). The default material's RefPtr then drops.
        m_overrideInstances.Clear();
        m_instances.Clear();
        m_instanceStorage.Clear();
        m_defaultMaterial.Reset();
        m_meshes.Clear();
        for (ViewBGSlot& slot : m_viewBGs)
        {
            if (slot.bg.IsValid())
            {
                m_device->DestroyBindGroup(slot.bg);
                slot.bg = {};
            }
        }
        m_viewBGs.Clear();
        m_viewBG = {}; // borrowed from a slot, already destroyed above
        if (m_shadowViewBG)
        {
            m_device->DestroyBindGroup(m_shadowViewBG);
            m_shadowViewBG = {};
        }
        if (m_boneDevice)
        {
            m_device->DestroyBuffer(m_boneDevice);
            m_boneDevice = {};
            m_boneDeviceBytes = 0;
        }
        if (m_dummyShadowView)
        {
            m_device->DestroyTextureView(m_dummyShadowView);
            m_dummyShadowView = {};
        }
        if (m_dummyShadowTex)
        {
            m_device->DestroyTexture(m_dummyShadowTex);
            m_dummyShadowTex = {};
        }
        if (m_dummyAtlasView)
        {
            m_device->DestroyTextureView(m_dummyAtlasView);
            m_dummyAtlasView = {};
        }
        if (m_dummyAtlasTex)
        {
            m_device->DestroyTexture(m_dummyAtlasTex);
            m_dummyAtlasTex = {};
        }
        if (m_shadowSampler)
        {
            m_device->DestroySampler(m_shadowSampler);
            m_shadowSampler = {};
        }
        if (m_dummyCubeView)
        {
            m_device->DestroyTextureView(m_dummyCubeView);
            m_dummyCubeView = {};
        }
        if (m_dummyCube)
        {
            m_device->DestroyTexture(m_dummyCube);
            m_dummyCube = {};
        }
        if (m_dummyBrdfView)
        {
            m_device->DestroyTextureView(m_dummyBrdfView);
            m_dummyBrdfView = {};
        }
        if (m_dummyBrdf)
        {
            m_device->DestroyTexture(m_dummyBrdf);
            m_dummyBrdf = {};
        }
        if (m_dummyProbeCubeView)
        {
            m_device->DestroyTextureView(m_dummyProbeCubeView);
            m_dummyProbeCubeView = {};
        }
        if (m_dummyProbeCube)
        {
            m_device->DestroyTexture(m_dummyProbeCube);
            m_dummyProbeCube = {};
        }
        if (m_dummyProbeBuffer)
        {
            m_device->DestroyBuffer(m_dummyProbeBuffer);
            m_dummyProbeBuffer = {};
        }
        if (m_dummyShBuffer)
        {
            m_device->DestroyBuffer(m_dummyShBuffer);
            m_dummyShBuffer = {};
        }
        if (m_envSampler)
        {
            m_device->DestroySampler(m_envSampler);
            m_envSampler = {};
        }
        if (m_noFadeSetBuf)
        {
            m_device->DestroyBuffer(m_noFadeSetBuf);
            m_noFadeSetBuf = {};
        }
        if (m_instanceBG)
        {
            m_device->DestroyBindGroup(m_instanceBG);
            m_instanceBG = {};
        }
        // MultiMesh persistent per-set buffers and bind groups, and the shared ramp.
        for (auto& kv : m_multiMeshSets)
        {
            for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
            {
                if (kv.value.instanceBG[r].IsValid())
                {
                    m_device->DestroyBindGroup(kv.value.instanceBG[r]);
                }
            }
            if (kv.value.instanceBuf.IsValid())
            {
                m_device->DestroyBuffer(kv.value.instanceBuf);
            }
            if (kv.value.offsetsBuf.IsValid())
            {
                m_device->DestroyBuffer(kv.value.offsetsBuf);
            }
            if (kv.value.pickOffsetsBuf.IsValid())
            {
                m_device->DestroyBuffer(kv.value.pickOffsetsBuf);
            }
            if (kv.value.setBuf.IsValid())
            {
                m_device->DestroyBuffer(kv.value.setBuf);
            }
        }
        m_multiMeshSets.Clear();
        if (m_rampBuffer)
        {
            m_device->DestroyBuffer(m_rampBuffer);
            m_rampBuffer = {};
            m_rampCapacity = 0;
        }
        if (m_pickRampBuffer)
        {
            m_device->DestroyBuffer(m_pickRampBuffer);
            m_pickRampBuffer = {};
        }
        for (ClusterSlot& slot : m_clusterSlots.All())
        {
            if (slot.group.IsValid())
            {
                m_device->DestroyBindGroup(slot.group);
            }
        }
        m_clusterSlots.Clear();
        if (m_dummyClusterBG)
        {
            m_device->DestroyBindGroup(m_dummyClusterBG);
            m_dummyClusterBG = {};
        }
        if (m_dummyClusterOffsets)
        {
            m_device->DestroyBuffer(m_dummyClusterOffsets);
            m_dummyClusterOffsets = {};
        }
        if (m_dummyClusterIndices)
        {
            m_device->DestroyBuffer(m_dummyClusterIndices);
            m_dummyClusterIndices = {};
        }
        for (auto& kv : m_pipelineLayouts)
        {
            if (kv.value.IsValid())
            {
                m_device->DestroyPipelineLayout(kv.value);
            }
        }
        m_pipelineLayouts.Clear();
        for (auto& kv : m_shadowMaskedLayouts)
        {
            if (kv.value.IsValid())
            {
                m_device->DestroyPipelineLayout(kv.value);
            }
        }
        m_shadowMaskedLayouts.Clear();
        if (m_shadowPipelineLayoutInstanced)
        {
            m_device->DestroyPipelineLayout(m_shadowPipelineLayoutInstanced);
            m_shadowPipelineLayoutInstanced = {};
        }
        if (m_viewLayout)
        {
            m_device->DestroyBindGroupLayout(m_viewLayout);
            m_viewLayout = {};
        }
        if (m_instanceLayout)
        {
            m_device->DestroyBindGroupLayout(m_instanceLayout);
            m_instanceLayout = {};
        }
        if (m_clusterLayout)
        {
            m_device->DestroyBindGroupLayout(m_clusterLayout);
            m_clusterLayout = {};
        }
        if (m_shadowViewLayout)
        {
            m_device->DestroyBindGroupLayout(m_shadowViewLayout);
            m_shadowViewLayout = {};
        }
        // rings free their buffers in their destructors (m_device still valid after this).
    }

    u64 MeshRenderer::InstShareKey(const void* view, const void* runHead) noexcept
    {
        u64 k = kFnv1a64OffsetBasis; // FNV-1a
        k = (k ^ static_cast<u64>(reinterpret_cast<usize>(view))) * kFnv1a64Prime;
        k = (k ^ static_cast<u64>(reinterpret_cast<usize>(runHead))) * kFnv1a64Prime;
        return k;
    }

    // The three mesh-facing selection functions are thin adapters over Foundation::Lod, the
    // RHI-free LOD-math library (LOD is a domain concept core math does not know about), so
    // foundation.terrain's chunk LOD and any headless consumer share the same formula.
    namespace
    {
        [[nodiscard]] Span<const f32> MeshThresholds(const geometry::StaticMesh& mesh)
        {
            const usize count =
                (static_cast<usize>(mesh.lodCount) < mesh.lodCoverage.Size())
                    ? static_cast<usize>(mesh.lodCount)
                    : mesh.lodCoverage.Size();
            return Span<const f32>(mesh.lodCoverage.Data(), count);
        }
    }

    f32 LodCoverageFor(const ViewCamera& camera, Float3 worldCenter, f32 worldRadius, f32 lodBias)
    {
        return foundation::lod::ProjectedSphereCoverage(camera.view, camera.projection,
                                                        worldCenter, worldRadius, lodBias);
    }

    u32 PickLodLevel(const geometry::StaticMesh& mesh, f32 coverage)
    {
        if (mesh.lodCount <= 1)
        {
            return 0;
        }
        return foundation::lod::SelectLevelByCoverage(MeshThresholds(mesh), coverage);
    }

    u32 ApplyLodHysteresis(const geometry::StaticMesh& mesh, f32 coverage, u32 rawSelection,
                           u32 last)
    {
        return foundation::lod::ApplyCoverageHysteresis(MeshThresholds(mesh), coverage,
                                                         rawSelection, last);
    }

    u32 MeshRenderer::SelectLod(const RenderRecordContext& ctx, const MeshRenderData& md)
    {
        const geometry::StaticMesh* mesh = md.mesh;
        if (mesh == nullptr || mesh->lodCount <= 1)
        {
            return 0;
        }
        const u32 maxLod = mesh->lodCount - 1;
        if (md.forceLod >= 0)
        {
            return (static_cast<u32>(md.forceLod) < maxLod) ? static_cast<u32>(md.forceLod)
                                                            : maxLod;
        }
        if (ctx.view == nullptr)
        {
            // Camera-independent pass (local-shadow tiles): the coarsest level, since shadows
            // never render finer than any view shows. Cascades pass their
            // owning view instead and share that view's selection memory.
            return maxLod;
        }
        const f32 coverage =
            LodCoverageFor(ctx.view->Camera(), md.worldCenter, md.worldRadius, md.lodBias);
        u32 sel = PickLodLevel(*mesh, coverage);
        // Keyed per (view pointer, item): entityId when there is one, else the mesh uid
        // (page-local previews). Kept across frames, as hysteresis needs.
        const u64 itemId = (md.entityId != 0) ? md.entityId : mesh->uid;
        u64 key = kFnv1a64OffsetBasis; // FNV-1a
        key = (key ^ static_cast<u64>(reinterpret_cast<usize>(ctx.view))) * kFnv1a64Prime;
        key = (key ^ itemId) * kFnv1a64Prime;
        if (const u32* last = m_lodLast.Find(key))
        {
            sel = ApplyLodHysteresis(*mesh, coverage, sel, *last);
        }
        if (m_lodLast.Size() > 65536)
        {
            m_lodLast.Clear(); // bounded memory; at worst one frame without hysteresis
        }
        m_lodLast.InsertOrAssign(key, sel);
        return sel;
    }
}
