// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:cluster_system` partition.
///
/// Clustered light culling. Bins lights into a 3D froxel grid (screen tiles by
/// logarithmic depth slices) once per frame via a compute pass, so the forward shader evaluates
/// only the lights touching each fragment's cluster instead of all lights. This partition owns the
/// build compute pipeline and the per-frame cluster buffers, and declares the build pass into the
/// frame graph (ordered before the forward pass, which reads the buffers it writes). The kernel,
/// cluster_build.cs.hlsl, builds each froxel's view-space AABB and assigns it every directional
/// light and each point or spot light whose range sphere meets the box.

module;
#include "Core/Prelude.h"

export module foundation.render:cluster_system;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data;
import :views;
import :resources;
import :bind_group_cache; // ViewSlots

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // The cluster build's parameters (cbuffer ClusterBuildParams, cluster_build.cs.hlsl): 16-byte
    // rows of scalars, then two row-major matrices. Integration.ShaderBlocks checks it against the
    // compiled shader, member by member.
    struct GpuClusterBuildParams
    {
        u32 gridX = 0, gridY = 0, sliceCount = 0, tileSize = 0;
        f32 nearZ = 0.0f, farZ = 0.0f, logScale = 0.0f, logBias = 0.0f;
        u32 lightCount = 0, lightOffset = 0;
        u32 orthographic = 0; // the kernel unprojects tile corners without the depth scale
        f32 pad0 = 0.0f;
        Float4x4 viewMatrix;
        Float4x4 invProjection;
    };
    static_assert(sizeof(GpuClusterBuildParams) == 176, "cbuffer ClusterBuildParams layout drift");

    // What a built cluster grid exposes to the forward pass: the buffers, and the grid parameters the
    // fragment shader needs to map a pixel and depth to its cluster. Filled by DeclareBuild.
    struct ClusterBinding
    {
        rhi::Buffer* offsets = nullptr;      // uint2 per cluster: (indexStart, count)
        rhi::Buffer* lightIndices = nullptr; // flat uint light-index list
        // Bumped every time this slot's buffers are reallocated (resize). Consumers must invalidate any
        // cached bind group on a version change: a freed rhi::Buffer* address can be reused by the new
        // allocation, so pointer equality is not a reliable "unchanged" test.
        u32 version = 0;
        rendergraph::RGHandle offsetsHandle =
            {}; // graph handles so the forward pass can ReadBuffer them
        rendergraph::RGHandle indicesHandle =
            {}; // (orders the build write before the shading read)
        u32 gridX = 0, gridY = 0, sliceCount = 0, tileSize = 0;
        i32 viewportX = 0,
            viewportY = 0; // grid is viewport-local; forward subtracts this from SV_Position
        f32 nearZ = 0.0f, farZ = 0.0f, logScale = 0.0f, logBias = 0.0f;
        [[nodiscard]] bool Valid() const noexcept
        {
            return offsets != nullptr && lightIndices != nullptr;
        }
    };

    class ClusterSystem
    {
    public:
        ClusterSystem(rhi::Device& device, shaders::ShaderSystem& shaders,
                      u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight),
              m_paramsRing(device, m_framesInFlight, kParamsSlot,
                           rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
                           u8"cluster.params"),
              m_lightRing(device, m_framesInFlight, sizeof(GpuLight),
                          rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst, u8"cluster.lights")
        {
            m_slots.SetFramesInFlight(m_framesInFlight);
        }

        ~ClusterSystem() { Shutdown(); }
        ClusterSystem(const ClusterSystem&) = delete;
        ClusterSystem& operator=(const ClusterSystem&) = delete;

        // Build the compute pipeline and its layout. Returns non-Ok if the shader or pipeline fails.
        Status Initialize();

        // Size the params and light rings for `viewCount` views this frame and select their
        // region. Call once per frame before any view.
        void PrepareFrame(u32 frameIndex, u32 viewCount);

        // Declare this view's cluster build compute pass into the graph (ordered before the forward
        // pass that reads the cluster buffers). Returns the binding the forward pass consumes, or an
        // empty binding if unavailable. The cluster grid is derived from the view's resolution and camera.
        ClusterBinding DeclareBuild(rendergraph::RenderGraph& graph, const RenderView& view,
                                    u32 frameIndex, u32 viewIndex);

        /// Wire the frames-in-flight retire queue (web-safe buffer grows). Null means drain.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept
        {
            m_retire = retire;
            m_paramsRing.SetRetireQueue(retire);
            m_lightRing.SetRetireQueue(retire);
        }

    private:
        GpuRetireQueue* m_retire = nullptr; // borrowed; null means WaitIdle on grow
        static constexpr u32 kTileSize = 16;
        static constexpr u32 kSliceCount = 24;
        static constexpr u32 kMaxPerCluster =
            64; // per-cluster light cap (matches MAX_PER_CLUSTER in the kernel)
        static constexpr u32 kMaxLights = 256; // per-view light budget (matches the forward's)
        static constexpr u64 kParamsSlot =
            256; // dynamic UBO offset alignment (>= sizeof(GpuClusterBuildParams))

        // One (view, frame-in-flight) slot's cluster buffers and the bind group over them. Two views
        // in one frame must not share buffers, or the second view's build overwrites the first's.
        struct Slot
        {
            rhi::Buffer* offsets = nullptr; // cluster (offset, count)
            rhi::Buffer* indices = nullptr; // flat light-index list
            u64 offsetsBytes = 0;           // size of the slot's buffers (views can differ)
            u64 indicesBytes = 0;
            u32 bufferVersion = 0; // incremented on realloc; consumers invalidate cached bind groups
            rhi::BindGroup* bindGroup = nullptr;
            u32 bgParamsGen = 0;
            u32 bgLightGen = 0;
            rhi::Buffer* bgOffsets = nullptr;
        };

        // (Re)create one slot's cluster buffers when its cluster count grows. Lazy: only the slots
        // actually used by rendered views are allocated.
        bool EnsureBuffers(Slot& slot, u32 totalClusters);

        // (Re)build the bind group for a (view, frame) slot. There is one bind group per slot, each over
        // its own buffers, so a slot's group is stable across frames and is never freed while the previous
        // frame's command buffer that referenced it is still in flight.
        bool EnsureBindGroup(Slot& slot);

        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;

        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::ComputePipeline* m_pipeline = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)

        DynamicUniformRing m_paramsRing;
        DynamicUniformRing
            m_lightRing; // ClusterSystem's own light copy (built before the forward pass uploads its own)
        // Per (view, frame in flight), grown to the most views one frame draws; a slot's group is
        // never freed while an in-flight frame still references it.
        ViewSlots<Slot> m_slots;
        bool m_ready = false;
    };

} // namespace foundation::render
