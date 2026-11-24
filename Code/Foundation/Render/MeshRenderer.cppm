// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:mesh_renderer` partition.
///
/// `MeshRenderer` is the `Renderer` for the mesh categories (Opaque, Masked, Transparent). It
/// owns the built-in forward shader (two permutations), the GPU rings and the mesh cache,
/// and records draws for the mesh `DrawItem`s it is handed.
///
/// Every mesh draws instanced: a run of consecutive draws sharing one mesh and material is issued
/// as one instanced draw, and a lone draw as an instanced draw of one. Each reads the
/// view-projection from a shared per-view UBO (set 0) and a per-instance
/// `StructuredBuffer<GpuInstance>` (set 1) indexed by a uint4 draw offsets vertex stream
/// (location 5, instance-stepped). That is portable base-plus-offset addressing; SV_InstanceID
/// differs between DX12 and Vulkan. Transparent draws are never instanced, since back-to-front
/// order must win.

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof

export module foundation.render:mesh_renderer;

import foundation.core;
import foundation.rhi;
import foundation.geometry;
import foundation.shaders;
import foundation.shaders.system;
import foundation.materials;
import foundation.materials.pipelinecache;
import :data;
import :views;
import :pipeline;
import :cluster_system;
import :resources;
import :bind_group_cache; // ViewSlots
import :gpu_mesh;

using namespace foundation::core;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // ---- Pure LOD selection math (unit-testable, no GPU) ----
    // Coverage: the fraction of the viewport half-height the item's bounding-sphere radius
    // spans in this camera. Perspective divides by view depth; ortho is depth-free (both read
    // projection[1][1]; m[3][3] tells them apart). Each unit of lodBias halves effective coverage.
    [[nodiscard]] f32 LodCoverageFor(const ViewCamera& camera, Float3 worldCenter, f32 worldRadius,
                                     f32 lodBias);
    // Threshold walk: level l takes over while coverage < lodCoverage[l] (descending
    // thresholds; a malformed tail stops the walk). Chainless meshes always answer 0.
    [[nodiscard]] u32 PickLodLevel(const geometry::StaticMesh& mesh, f32 coverage);
    // Hysteresis: keep `last` while it is still pickable inside a +-5% coverage band around
    // the raw pick, so an item hovering on a threshold never flickers between levels.
    [[nodiscard]] u32 ApplyLodHysteresis(const geometry::StaticMesh& mesh, f32 coverage,
                                         u32 rawSelection, u32 last);

    /// MeshRenderer::kMultiMeshEvictFrames, at namespace scope for another library to read (MSVC does
    /// not export a class's static constexpr member from a shared library).
    inline constexpr u32 kMeshRendererMultiMeshEvictFrames = 120;

    // The renderers' GPU blocks as the shaders declare them (forward_vs.hlsli, view.hlsli,
    // shadow_depth_vs.hlsli, pick_ids_vs.hlsli); Integration.ShaderBlocks checks each against the
    // compiled shaders, member by member.
    struct GpuView
    {                                // 656 (cbuffer View, view.hlsli)
        Float4x4 viewProj;           // 64
        Float4x4 view;               // 64  (view-space depth: cluster and cascade select)
        Float4x4 cascadeViewProj[4]; // 256 (CSM: world to each cascade's light clip)
        Float3 cameraPos;
        f32 lightCount; // 16  (light count as float, mirrors HLSL)
        u32 lightOffset;
        i32 clusterViewportX, clusterViewportY;
        f32 iblMaxLod = -1.0f; // 16 (iblMaxLod < 0 means no IBL)
        u32 clusterGridX = 0, clusterGridY = 0, clusterSliceCount = 0,
            clusterTileSize = 0;                                                      // 16
        f32 clusterNear = 0, clusterFar = 0, clusterLogScale = 0, clusterLogBias = 0; // 16
        Float3 ambient = Float3{0, 0, 0};
        f32 shadowCascadeCount = 0.0f;                // 16
        Float4 cascadeSplitFar = Float4{0, 0, 0, 0};  // 16
        Float4 cascadeTexelSize = Float4{0, 0, 0, 0}; // 16
        f32 shadowNormalBias = 0, shadowDepthBias = 0, cascadeLayerBase = 0;
        u32 localShadowBase = 0; // 16
        Float4x4 prevViewProj =
            Float4x4::Identity(); // 64  (motion vectors: last frame's world to clip)
        Float2 jitter = Float2{0, 0};     // this frame's NDC jitter (TAA)
        Float2 prevJitter = Float2{0, 0}; // 16  (last frame's: unjitters the motion vectors)
        u32 probeBase = 0;  // this view's scene's first record in Probes[]
        u32 probeCount = 0; // its reflection probes (0 for none)
        u32 _probePad0 = 0, _probePad1 = 0;
        f32 shadowFarFade = 40.0f;   // the CSM's far fade width (world units)
        f32 shadowUvYSign = 0.0f;    // the shadow map's uv Y sign (backend-driven)
        f32 _shadowPad0 = 0.0f, _shadowPad1 = 0.0f;
        u32 debugMode = 0;           // the semantic debug view (0 is off)
        u32 _debugPad0 = 0, _debugPad1 = 0, _debugPad2 = 0;
        f32 iblDiffuse = 1.0f;  // the sky-lighting dimmer
        f32 iblSpecular = 1.0f; // the sky-reflection dimmer
        f32 sceneTime = 0.0f;   // the view's scene clock (s): the WIND sway and a surface's Time()
        f32 prevSceneTime = 0.0f; // last frame's (motion vectors)
    };
    static_assert(sizeof(GpuView) == 656, "cbuffer View layout drift");
    static_assert(offsetof(GpuView, jitter) == 576);
    static_assert(offsetof(GpuView, prevJitter) == 584);
    struct GpuInstance
    {
        Float4x4 world;
        Float4x4 prevWorld;
        Color tint;
    }; // 144 (StructuredBuffer element)
    // An instanced draw's set (cbuffer InstanceSet, instance_fade.hlsli; set 1 beside the instances
    // in the forward, shadow and pick passes): its distance fade's window (FadeEnd <= 0 for none), its
    // instance count (each instance's rank in the fade is (index + 0.5) / count), and the entity a
    // MultiMesh set picks as (index + 1, generation; 0 means each instance's own id).
    struct GpuInstanceSet
    {
        f32 fadeStart = 0.0f;
        f32 fadeEnd = 0.0f;
        u32 instanceCount = 1;
        u32 _pad0 = 0;
        u32 pickIndex = 0;
        u32 pickGeneration = 0;
        u32 _pad1 = 0;
        u32 _pad2 = 0;
    };
    static_assert(sizeof(GpuInstanceSet) == 32, "cbuffer InstanceSet layout drift");
    // The instance-stepped uint4 at location 5 an instanced draw binds beside its instances, one
    // entry per instance, in two layouts by pass. GpuDrawOffsets is the forward's and the shadow
    // depth's (drawOffsets in forward_vs.hlsli and shadow_depth_vs.hlsli): written by their instanced fills,
    // the shared MultiMesh ramp and a skinned set's per-instance offsets. The fade is a float whose
    // bits travel in the uint4 lane w; the shader reads it with asfloat. A depth fill writes 0 (a
    // faded run is never prepassed, so the forward reuses only solid ranges).
    struct GpuDrawOffsets
    {
        u32 instanceIndex = 0; // into the draw's Instances[]
        u32 boneBase = 0;      // the instance's first bone matrix (skinned); 0 otherwise
        u32 prevBoneBase = 0;  // last frame's bone base (skinned motion vectors)
        f32 fade = 0.0f;       // the screen-door fade, 0 is solid (forward pass, the DITHER variant)
    };
    static_assert(sizeof(GpuDrawOffsets) == 16, "drawOffsets vertex stream layout drift");
    static_assert(offsetof(GpuDrawOffsets, instanceIndex) == 0);
    static_assert(offsetof(GpuDrawOffsets, boneBase) == 4);
    static_assert(offsetof(GpuDrawOffsets, prevBoneBase) == 8);
    static_assert(offsetof(GpuDrawOffsets, fade) == 12);
    // The pick pass's (pickOffsets in pick_ids_vs.hlsli): the instance and bone base, then the
    // entity id. Its instanced fill writes them per draw; a MultiMesh set's pick draw binds the
    // pick ramp or the set's pick offsets, whose ids are 0 (the set picks as its InstanceSet's).
    struct GpuPickDrawOffsets
    {
        u32 instanceIndex = 0;  // into the draw's Instances[]
        u32 boneBase = 0;       // the instance's first bone matrix (skinned); 0 otherwise
        u32 pickGeneration = 0; // the entity's generation
        u32 pickIndex = 0;      // the entity's index + 1, 0 for nothing
    };
    static_assert(sizeof(GpuPickDrawOffsets) == 16, "pickOffsets vertex stream layout drift");
    static_assert(offsetof(GpuPickDrawOffsets, instanceIndex) == 0);
    static_assert(offsetof(GpuPickDrawOffsets, boneBase) == 4);
    static_assert(offsetof(GpuPickDrawOffsets, pickGeneration) == 8);
    static_assert(offsetof(GpuPickDrawOffsets, pickIndex) == 12);
    static_assert(sizeof(GpuPickDrawOffsets) == sizeof(GpuDrawOffsets),
                  "both passes' entries share one ring stride");
    struct GpuShadowView
    {
        Float4x4 lightViewProj;
        Float3 camera = Float3{0, 0, 0}; // the camera's position (the instance fade's distance)
        u32 fadeFromCamera = 0;          // 1: the pass couples to a camera, so instances fade
        f32 sceneTime = 0.0f;            // the scene clock (s): the WIND sway and a surface's Time()
        f32 _pad0 = 0.0f, _pad1 = 0.0f, _pad2 = 0.0f;
    }; // 96  (cbuffer ShadowView)
    // GPU-layout contract: these mirror HLSL cbuffer and StructuredBuffer elements and use the packed
    // Float4x4 (64B, tight). A stray SIMD Matrix4 (also 64B, but 16-byte aligned) would shift members
    // through padding; the guards pin the exact byte layout the shaders expect.
    static_assert(sizeof(GpuInstance) == 144,
                  "StructuredBuffer<GpuInstance> element layout drift");
    // The pick pass's set-0 view (cbuffer PickView in pick_ids.vs): the cropped VP, the WIND clock
    // and the camera the instance fade measures from. A MultiMesh set's own id is its InstanceSet's.
    struct GpuPickView
    {
        Float4x4 viewProj;
        f32 sceneTime = 0.0f; // the scene clock (s): the WIND sway and a surface's Time()
        f32 _pad0 = 0.0f, _pad1 = 0.0f, _pad2 = 0.0f;
        Float3 camera = Float3{0, 0, 0}; // the camera's position (the instance fade's distance)
        f32 _pad3 = 0.0f;
    }; // 96  (cbuffer PickView; shares the shadow-view ring's 256B slots)
    static_assert(sizeof(GpuShadowView) == 96, "cbuffer ShadowView layout drift");
    static_assert(sizeof(GpuPickView) == 96, "cbuffer PickView layout drift");

    class MeshRenderer final : public Renderer
    {
    public:
        // `allocator`: the owner's, for what the renderer keeps (its material instances and lists).
        MeshRenderer(IAllocator& allocator, rhi::Device& device, shaders::ShaderSystem& shaderSystem,
                     materials::PipelineStateCache& psoCache,
                     materials::MaterialSystem& materialSystem, u32 framesInFlight) noexcept
            : m_allocator(&allocator), m_device(&device), m_shaders(&shaderSystem), m_psoCache(&psoCache),
              m_materials(&materialSystem), m_meshes(device, framesInFlight),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight),
              m_viewRing(device, framesInFlight, kViewDataSlot, kMinViewSlots, u8"mesh.view"),
              m_shadowViewRing(device, framesInFlight, kViewSlot,
                               rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
                               u8"mesh.shadowView"),
              m_instanceRing(device, framesInFlight, sizeof(GpuInstance),
                             rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst,
                             u8"mesh.instances"),
              m_offsetsRing(device, framesInFlight, sizeof(GpuDrawOffsets),
                            rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst, u8"mesh.offsets"),
              m_lightRing(device, framesInFlight, sizeof(GpuLight),
                          rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst, u8"mesh.lights"),
              m_localShadowRing(device, framesInFlight, sizeof(GpuLocalShadow),
                                rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst,
                                u8"mesh.localShadows"),
              m_boneRing(device, framesInFlight, sizeof(Float4x4),
                         rhi::BufferUsage::CopySrc,
                         u8"mesh.bones.staging")
        {
            m_clusterSlots.SetFramesInFlight(m_framesInFlight);
        }

        ~MeshRenderer() override { Shutdown(); }

        MeshRenderer(const MeshRenderer&) = delete;
        MeshRenderer& operator=(const MeshRenderer&) = delete;

        // Registers the forward shader and creates the bind-group and pipeline layouts.
        Status Initialize();

        // The directional shadow map sampled in the forward shader this frame (the ShadowSystem's
        // texture when a caster exists, else null for the 1x1 dummy). Set by RenderFrame before PrepareFrame.
        void SetShadowMap(rhi::TextureViewHandle view, u64 generation) override;

        // The local-light (spot and point) shadow atlas sampled this frame (the ShadowSystem's atlas when
        // any local caster exists, else null for the 1x1 dummy). Set by RenderFrame before PrepareFrame.
        void SetShadowAtlas(rhi::TextureViewHandle view, u64 generation, u32 passCount) override;

        // Reflection-probe capture faces re-emit the draws (one forward pass each); count them into the ring.
        void SetCaptureFacePasses(u32 passes) override { m_captureFacePasses = passes; }
        void SetPickPasses(u32 passes) override { m_pickPasses = passes; }
        void SetSceneLights(u32 lights) override { m_sceneLights = lights; }
        void SetShadowCasterDraws(u32 draws) override { m_shadowCasterDraws = draws; }

        /// The per-frame slots the instance and offset rings need. Each camera pass (the
        /// depth prepass, the forward, a probe face, a pick) re-emits at most the views' draws;
        /// each shadow pass (a cascade, a local tile) at most its scene's casters, which the
        /// camera does not cull. Sized for the larger of the two in every pass, so neither can
        /// starve the other.
        [[nodiscard]] static u32 InstanceSlotsPerFrame(u32 maxDraws, u32 casterDraws,
                                                       u32 shadowPasses, u32 extraCameraPasses) noexcept
        {
            const u32 largest = (casterDraws > maxDraws) ? casterDraws : maxDraws;
            return largest * (2u + shadowPasses + extraCameraPasses);
        }

        // This frame's reflection probes: the captured cube-array view (set-0 t8) and the probe buffer
        // (each probe's box, slice and intensity) for the forward's local-reflection path. A null view
        // binds the dummy cube array with count 0, and the forward keeps the global IBL reflection.
        void SetProbes(rhi::TextureViewHandle cubeArray, rhi::BufferHandle probeBuffer, u32 count) override;

        // Upload this frame's local-shadow entries into the local-shadow ring (bound whole at set 0;
        // the shader reads LocalShadows[LocalShadowBase + shadowIndex]). Called once per frame (after
        // PrepareFrame, which begins the ring). The base is stamped into each view's GpuView in Resolve.
        void UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex) override;

        // ---- Renderer ----

        [[nodiscard]] Span<const RenderCategory> SupportedCategories() const override;

        // Size every ring for the whole frame's draws (once) and select this frame's region. The
        // per-object and instance rings are sized for 2x the camera draws so the shadow depth pass can
        // re-emit the same geometry into the same rings without starving the forward pass.
        void PrepareFrame(u32 maxDraws, u32 frameIndex) override;

        // Device-local mirror of the bone staging ring: vertex skinning reads bones across many passes
        // (forward and every cascade), so a CpuToGpu buffer would stream them over PCIe each read. One copy
        // per frame into VRAM (UploadSkinning) makes subsequent reads land at device bandwidth.
        bool EnsureBoneDevice();

        // Write every distinct skinned instance's matrices into the bone pool once this frame (the
        // current slab, then the previous slab) and copy the populated range to the device mirror.
        // Builds m_boneStart, mapping a boneMatrices pointer to its { current base, previous base } in
        // matrix units, which Resolve uses for the per-draw bone bases (GpuDrawOffsets::boneBase and
        // prevBoneBase). Called once per frame before any pass, with every scene the frame draws.
        void UploadSkinning(Span<const ExtractedScene* const> scenes, rhi::CommandEncoder& encoder) override;

        // Ensure every instanced-mesh set in the snapshot has an up-to-date persistent GPU buffer, and grow
        // the shared draw offsets ramp to the largest set. Called once per frame (from UploadSkinning). Static
        // sets fall through after their first upload, so they cost O(1) per frame.
        void UploadMultiMeshes(Span<const ExtractedScene* const> scenes);

        // A MultiMesh set not extracted for this many upload frames releases its GPU buffers and
        // bind groups (destroyed through the device, which keeps them for frames in flight) and leaves
        // the pool: per-chunk vegetation sets come and go with the camera, and a set seen once must
        // not pin its buffer for the rest of the run.
        static constexpr u32 kMultiMeshEvictFrames = kMeshRendererMultiMeshEvictFrames;

        // The variant flags a material's active effects turn on: an effect group is active while
        // its gate is above zero in the material's values, and an active effect adds its flag
        // (`WIND` for the Wind group). The renderer knows that rule, not what any effect does. A
        // material with no active effect adds no flags. Read from the shared material: a
        // per-entity override of a gate does not switch the variant (the pipeline is chosen per
        // material); it only changes the values an already active effect draws with.
        [[nodiscard]] static shaders::ShaderFlags MaterialEffectFlags(const materials::Material* material) noexcept;

        // The depth-like pipeline (depth prepass and shadows, or `pick`) for a draw of `material`:
        // the built-in program, or the material's own when its shader has one; `materialSet` when
        // the draw binds the material's set 2 (a masked cut-out, an active effect, whose block is in
        // the material's set, or the material's program).
        struct DepthLikeChoice
        {
            materials::PipelineConfig config;
            bool materialSet = false;
        };
        // The vertex stream a depth-like draw's pipeline reads, set on `config` (from DepthLikeFor): a
        // skinned mesh's full stream and its skinning; else, when the mesh has a depth stream
        // (`depthStream`), a shadow or depth prepass draw of the built-in program reads that slim
        // stream (VertexLayoutType::DepthStream, the DEPTH_STREAM variant), since it uses only
        // position and uv. A material's own program and the pick take the full vertex. True when
        // the draw reads the depth stream, which the caller then binds.
        [[nodiscard]] static bool ChooseDepthLikeStream(materials::PipelineConfig& config, bool skinned,
                                                        bool depthStream, bool pick) noexcept;
        // The threshold the built-in shadow and pick programs test a cut-out's albedo alpha against
        // (GpuCutoutPush): AlphaCutoff over the base colour's alpha, as the forward tests.
        [[nodiscard]] static f32 CutoutThreshold(const materials::MaterialInstance& instance) noexcept;
        [[nodiscard]] static DepthLikeChoice DepthLikeFor(const RenderRecordContext& ctx,
                                                          const materials::Material* material,
                                                          bool pick);

        // The forward pipeline of one submesh slot's material: a slot draws with its own program (a
        // custom shader on one slot, PBR on the rest), over the mesh's per-draw state (its fade).
        [[nodiscard]] static materials::PipelineConfig ConfigFor(const MeshRenderData& md, const materials::Material* material,
                                                          const RenderRecordContext& ctx);
        // A submesh's pipeline: its slot material's when that differs from the mesh's and draws in
        // the same pass (the same colour targets), or always for an item of a mesh split by
        // category (MeshRenderData::categorySlots: every slot it draws is of its pass); else the
        // mesh's `base` (an unsplit mesh's slot of another pass has no targets here), skinning
        // flags as given.
        [[nodiscard]] static materials::PipelineConfig SlotConfigFor(const MeshRenderData& md,
                                                              const materials::PipelineConfig& base,
                                                              const materials::Material* slot,
                                                              const RenderRecordContext& ctx, bool skinned);

        // One range of a mesh's depth-like draw: its material and its indices (`whole`: the mesh's
        // whole index range).
        struct DepthLikeRange
        {
            materials::Material* material = nullptr;
            u32 startIndex = 0;
            u32 indexCount = 0;
            bool whole = false;
        };
        // The ranges of a mesh's depth-like draw (depth prepass, shadows, or `pick`): each of
        // `subMeshes` with its slot's material (`slots[materialIndex]`, else `slot0`), as the forward
        // pass draws them, so a masked material, an active effect or a material's own depth program
        // in any slot casts and prepasses as itself; one whole range when there are no submeshes,
        // or, outside a LOD chain (`chain`: a level is its own ranges), when every range draws
        // alike (one material, or the plain program with no material set for all). For `only`, an
        // item of a mesh split by category (MeshRenderData::categorySlots), its category's ranges
        // alone.
        static void DepthLikeRanges(const RenderRecordContext& ctx, materials::Material* slot0,
                                    const RefPtr<materials::Material>* slots, u32 slotCount,
                                    Span<const geometry::SubMesh> subMeshes, bool chain, bool pick, Array<DepthLikeRange>& out, const MeshRenderData* only = nullptr);

        // The depth a blended material's pipeline uses in the transparent pass, which reads the
        // depth buffer but never writes it (its bundle is depth-read-only): a writing mode becomes
        // ReadOnly. MaterialBuilder::Transparent pairs the two, but a material authored as data can
        // set a blend mode and leave the depth at its ReadWrite default, and WebGPU refuses that
        // pipeline inside the pass (Vulkan does not check).
        [[nodiscard]] static materials::DepthMode TransparentPassDepth(materials::DepthMode mode) noexcept
        {
            return (mode == materials::DepthMode::ReadWrite || mode == materials::DepthMode::WriteOnly)
                       ? materials::DepthMode::ReadOnly
                       : mode;
        }

        // Instances a set's per-frame region holds for `count` live instances: rounded up to a
        // multiple of 16 so every region's byte offset (capacity x 144-byte InstanceData) is a
        // multiple of 2304 = 9 x 256, which meets the strictest storage-buffer offset alignment any
        // backend asks for (WebGPU validates 32 or 256, D3D12 256). Without the rounding an odd count
        // puts region 1 at an unaligned offset and WebGPU refuses the bind group.
        [[nodiscard]] static constexpr u32 MultiMeshRegionCapacity(u32 count) noexcept
        {
            return (count + 15u) & ~15u;
        }
        [[nodiscard]] usize MultiMeshSetCount() const noexcept { return m_multiMeshSets.Size(); }
        // Test read-back: the world matrix uploaded for instance `index` of region `region` of the
        // set keyed `key` (false when there is no such set, region or slot). Maps the buffer, so it
        // is for host-visible test devices.
        [[nodiscard]] bool ReadMultiMeshInstance(u64 key, u32 region, u32 index, Float4x4& out);
        // Test read-back of the tint written for instance `index`.
        [[nodiscard]] bool ReadMultiMeshInstanceTint(u64 key, u32 region, u32 index, Color& out);
        // Test read-back of a set's InstanceSet block for `region` (its fade window, count, pick id).
        [[nodiscard]] bool ReadMultiMeshInstanceSet(u64 key, u32 region, GpuInstanceSet& out);

        // Grow the shared draw offsets ramp (GpuDrawOffsets) to at least `count` slots: [{0,0,0,0},{1,0,0,0},...]. Filled once
        // per (re)allocation (the values are static per index and never rewritten). instanceIndex is each instance's index
        // into its set's own StructuredBuffer<InstanceData>; the bone bases and the fade stay 0, since skinned sets use
        // their own offsets (FillSkinnedMultiMeshOffsets).
        bool EnsureRamp(u32 count);

        // Ensure this set's persistent InstanceData buffer and set-1 bind group exist and hold the current
        // instances. (Re)allocates and rebuilds the bind group on first sight or a grow (draining the GPU
        // first, since a resize is rare); re-uploads InstanceData only when the component's version changed
        // (static sets upload once). InstanceData.prevWorld is the world matrix (static content has no motion).
        void EnsureMultiMeshSet(const MultiMeshRenderData& mm);

        // Fill each skinned MultiMesh set's per-instance GpuDrawOffsets: instanceIndex is the instance's index into its
        // InstanceData buffer; boneBase and prevBoneBase are its pose base in the shared bone pool, poolBase +
        // (Hash(i) % poseCount) * boneCount. Runs after the bone upload (pool bases known): O(N) 16-byte writes, once per
        // frame, shared across passes.
        void FillSkinnedMultiMeshOffsets(const ExtractedScene& scene);

        void Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                     Array<ResolvedDraw>& out) override;

        // Re-emit this view's draws as depth-only shadow casters (ctx.viewProj is the light view-proj,
        // ctx.depthFormat the shadow map format). Follows Resolve's run batching but produces depth-only
        // ResolvedDraws (set 0 the light view, set 1 object or instance; no material or cluster). Reuses
        // the object and instance rings and their bind groups (sized 2x in PrepareFrame).
        void ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                              Array<ResolvedDraw>& out) override;

        // Re-emit this view's draws as pick-id writers (ctx.viewProj is the cropped camera VP,
        // ctx.colorFormat the RG32Uint id target): the depth-only path with the `pick_ids`
        // shader, whose fragment writes each draw's (EntityTag index + 1, generation). Static,
        // skinned, instanced and MultiMesh draws all pick; masked materials keep their cutout.
        void ResolvePickIds(const RenderRecordContext& ctx, Span<const DrawItem> items,
                            Array<ResolvedDraw>& out) override;

        void FinishFrame() override;

    private:
        struct MultiMeshSet; // defined with the pool below
        // Release one set's GPU objects: retired when the queue is wired, destroyed in place
        // otherwise (Null-device tests, shutdown after the device idled).
        void ReleaseMultiMeshSet(MultiMeshSet& set);
        // Sweep sets whose lastFrame is older than kMultiMeshEvictFrames.
        void EvictStaleMultiMeshSets();
        static constexpr u64 kViewSlot = 256; // dynamic UBO offset alignment (object, shadow view)
        static constexpr u64 kViewDataSlot =
            1024;                              // view UBO slot (GpuView is 656 bytes)
        static constexpr u32 kMinViewSlots = 8; // the view ring's start; it grows to the frame's views
        static constexpr u32 kMaxLights = kMaxLightsPerView; // per-view light budget
        // shadow-view UBO slots per frame: one per shadow pass and category run. Cascades (up to
        // kMaxShadowViews*kCount) and local-shadow atlas tiles (up to kMaxLocalShadows), each times a few
        // categories. Sized with headroom, since a slot is tiny (256B).
        static constexpr u32 kMaxShadowPasses = 256;
        static constexpr u32 kMaxLocalShadows =
            kMaxLocalShadowEntries; // frame-global entry cap (shared with the pipeline)
        // GPU skinning bone-matrix pool: the staging ring and its VRAM mirror start at
        // kInitialBoneMatrices slots per frame and grow (power of two, up to the cap) the first
        // frame that needs more: the pool is empty in any scene without skinning, and reserving
        // the cap up front would cost 128 MB of staging and 128 MB of VRAM unconditionally. Growth
        // happens in UploadSkinning before anything has referenced this frame's ring or mirror
        // (the set-0 bind groups check the mirror's generation at record time), so the frame that
        // grows still skins everything. The cap is what the stress test needs at once.
        static constexpr u32 kInitialBoneMatrices = 1u << 12;
        static constexpr u32 kMaxBoneMatrices = 1u << 20;

        // Per-view LOD selection: projected-sphere coverage of the item's
        // world bounds against this ctx's camera (perspective divides by view depth; ortho is
        // depth-free; both read projection[1][1]), biased by md.lodBias (each unit halves
        // effective coverage), pinned by md.forceLod, and stabilized by a +-5% hysteresis
        // band remembered per (view pointer, item), so the depth prepass and forward pass
        // of one view always agree within a frame. Returns 0 for chainless meshes.
        [[nodiscard]] u32 SelectLod(const RenderRecordContext& ctx, const MeshRenderData& md);

        bool GrowBonePool(u32 needed); // impl unit; see kInitialBoneMatrices
    public:
        /// Bone-matrix slots the pool currently holds per frame (grows on demand; tests pin it).
        [[nodiscard]] u32 BonePoolSlotsPerFrame() const noexcept { return m_boneRing.SlotsPerFrame(); }
        /// Whether the skeleton whose matrices are at `boneMatrices` was uploaded this frame (its meshes
        /// draw skinned; otherwise in their bind pose).
        [[nodiscard]] bool IsSkinUploaded(const Float4x4* boneMatrices) const { return m_boneStart.Contains(boneMatrices); }
        [[nodiscard]] static constexpr u32 InitialBonePoolSlots() noexcept { return kInitialBoneMatrices; }
        /// Light slots the ring holds per frame, and those this frame's uploads took (tests pin them).
        [[nodiscard]] u32 LightSlotsPerFrame() const noexcept { return m_lightRing.SlotsPerFrame(); }
        [[nodiscard]] u32 LightSlotsUsed() const noexcept { return m_lightRing.FrameAllocatedSlots(); }
        [[nodiscard]] u32 ViewSlotsUsed() const noexcept { return m_viewRing.FrameSlots(); }
    private:

        void ResolveInstanced(const RenderRecordContext& ctx, u32 viewOffset,
                              rhi::BindGroupHandle clusterBG, Span<const DrawItem> items, usize first,
                              u32 count, const MeshRenderData& head, const GpuMesh& mesh,
                              Array<ResolvedDraw>& out);

        // The shared body of ResolveDepthOnly (pick = false) and ResolvePickIds (pick = true).
        void ResolveDepthLike(const RenderRecordContext& ctx, Span<const DrawItem> items,
                              Array<ResolvedDraw>& out, bool pick);

        void ResolveDepthInstanced(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                   Span<const DrawItem> items, usize first, u32 count,
                                   const GpuMesh& mesh, Array<ResolvedDraw>& out, bool pick);

        // The submeshes a depth-like draw of `md` resolves per slot: the view's LOD level of a chain,
        // every submesh when it has per-submesh materials, else none (its one material covers it).
        [[nodiscard]] Span<const geometry::SubMesh> DepthLikeSubMeshes(const RenderRecordContext& ctx,
                                                                       const MeshRenderData& md, bool chain);
        // The depth-like pipeline and material set (set 2) of a draw of `material` onto `d`; false
        // when no pipeline is had. With `depthStream` (the mesh has one), a shadow or depth prepass
        // draw of the built-in program reads it (VertexLayoutType::DepthStream), and `usesDepthStream`
        // says so: the caller binds the stream.
        bool DepthLikeDraw(const RenderRecordContext& ctx, materials::Material* material, bool pick, bool skinned, bool depthStream, ResolvedDraw& d, bool& usesDepthStream);
        // Pushes `base` once per range, each with its range's pipeline, material set and indices.
        void EmitDepthLike(const RenderRecordContext& ctx, const ResolvedDraw& base, const GpuMesh& mesh,
                           Span<const DepthLikeRange> ranges, bool pick, bool skinned,
                           Array<ResolvedDraw>& out);

        // Forward draw for a MultiMesh: bind this set's persistent InstanceData (set 1) and the shared draw offsets
        // ramp, one instanced draw per submesh material. There is no fill loop: the buffer already holds the
        // instances (uploaded in UploadMultiMeshes). The shader is the same instanced path (Instances[drawOffsets.x]).
        void ResolveMultiMesh(const RenderRecordContext& ctx, u32 viewOffset,
                              rhi::BindGroupHandle clusterBG, const MultiMeshRenderData& mm,
                              const GpuMesh& mesh, Array<ResolvedDraw>& out);

        // Depth-only draw for a MultiMesh caster (camera prepass and every shadow cascade): the same persistent
        // buffer and shared ramp, with the depth and shadow pipeline layout. Non-masked casters are one draw; masked
        // casters fold in the material set for the alpha test (like ResolveDepthInstanced).
        void ResolveMultiMeshDepth(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                   const MultiMeshRenderData& mm, const GpuMesh& mesh,
                                   Array<ResolvedDraw>& out, bool pick);

        // Depth-only PSO config for the shadow pass. Back-face culling and a small depth and slope bias push
        // shadow acne off lit surfaces; the shader adds a normal-offset bias on top.
        [[nodiscard]] static materials::PipelineConfig
        ShadowConfigFor(const RenderRecordContext& ctx, bool masked = false,
                        shaders::ShaderFlags effects = shaders::ShaderFlags::None);

        // Pick-id PSO config: the depth-only layouts with the `pick_ids` fragment writing the
        // RG32Uint id target (ctx.colorFormat), single-sample, no bias, masked materials alpha-tested.
        [[nodiscard]] static materials::PipelineConfig
        PickConfigFor(const RenderRecordContext& ctx, bool masked,
                      shaders::ShaderFlags effects = shaders::ShaderFlags::None);

        [[nodiscard]] static materials::PipelineConfig
        ConfigFor(const MeshRenderData& md, const RenderRecordContext& ctx);

        bool MakeLayout(const rhi::BindGroupLayoutEntry& entry, rhi::BindGroupLayoutHandle& out);

        bool MakePipelineLayout(rhi::BindGroupLayoutHandle set0, rhi::BindGroupLayoutHandle set1,
                                rhi::BindGroupLayoutHandle set2, rhi::BindGroupLayoutHandle set3,
                                rhi::PipelineLayoutHandle& out);

        // Two-set pipeline layout (the depth-only shadow pipelines: light view, then object or instance).
        bool MakePipelineLayout(rhi::BindGroupLayoutHandle set0, rhi::BindGroupLayoutHandle set1,
                                rhi::PipelineLayoutHandle& out);

        // Three-set pipeline layout (masked shadow: light view, object or instance, material); the material
        // set feeds the alpha-test fragment's albedo sample.
        bool MakePipelineLayout3(rhi::BindGroupLayoutHandle set0, rhi::BindGroupLayoutHandle set1,
                                 rhi::BindGroupLayoutHandle set2, rhi::PipelineLayoutHandle& out);

        // Masked-shadow pipeline layout for a material set-2 layout, cached by (set2): light view
        // (0), object or instance (1), material (2). Retired with the renderer.
        [[nodiscard]] rhi::PipelineLayoutHandle GetOrCreateShadowMaskedLayout(rhi::BindGroupLayoutHandle set2);

        // The default material: a standard PBR material (materials::CreatePBR) used whenever a draw
        // carries no material. Its set-2 layout and bind group flow through the same data-driven path as any
        // other material, so there is nothing special about the "no material" case.
        Status CreateDefaultMaterial();

        // The MaterialInstance the renderer owns for `material` (one per material, created lazily). The
        // instance carries per-draw overrides (textures, uniforms) and caches its bind group in the system.
        [[nodiscard]] materials::MaterialInstance* InstanceFor(materials::Material* material);
        [[nodiscard]] UniquePtr<materials::MaterialInstance> NewInstance(materials::Material* material);

        // The instance a mesh with material overrides draws `slot` with: one of its own per (entity,
        // slot), its overrides re-applied over the material's values when their version changes; the
        // shared instance when none of its overrides is for `slot`.
        [[nodiscard]] materials::MaterialInstance* InstanceFor(const MeshRenderData& md, u32 slot,
                                                               materials::Material* material);
        // Release an instance's bind group and uniform buffers (the device keeps them while in-flight frames bind them).
        void ReleaseInstance(materials::MaterialInstance& instance);
        // Release an instance's effect group buffers (materials::MaterialSystem::DetachEffectBuffers).
        void ReleaseEffectBuffers(materials::MaterialInstance& instance);
        // Drop the override instances no draw has used for a while (overrides cleared, entity gone).
        void PruneOverrideInstances();

        // The set-2 (material) bind group for `material`, built data-driven from its property list by the
        // material system (UBO, then textures and samplers in declared order, with white, flat-normal and
        // default-sampler fallbacks for unset slots). `material` is non-null (callers substitute the default).
        [[nodiscard]] rhi::BindGroupHandle MaterialBindGroup(materials::Material* material);

        // The forward pipeline layout for a given material set-2 layout, cached by (set2 layout).
        // set0: view; set1: instances; set2: material; set3: clusters.
        [[nodiscard]] rhi::PipelineLayoutHandle GetOrCreatePipelineLayout(rhi::BindGroupLayoutHandle set2);

        // Per-frame bind groups (view, shadow view, instance) are rebuilt when their inputs change;
        // the directional shadow view, for example, alternates per in-flight slot, so the view BG rebuilds every
        // frame. The old one may still be referenced by an in-flight command buffer: the device keeps it
        // until that buffer has run.
        void ReleaseBindGroup(rhi::BindGroupHandle bg);
        // Drop material instances whose material nobody else references (the resource layer
        // hot-reloaded it away): release their bind group (the device keeps it while in-flight
        // frames bind it) and destroy the instance. Without this, stale descriptor sets keep destroyed
        // texture views referenced forever.
        void PruneStaleMaterialInstances();

        void ReleaseBuffer(rhi::BufferHandle buffer);

        // (Re)create the instanced set-1 bind group when the instance ring (re)allocated: the whole
        // ring (read as Instances[slot]) and, as binding 1, the no-fade InstanceSet.
        bool EnsureInstanceBindGroup();

        // The set-0 bind group spans two rings: the per-view UBO (dynamic-offset window of one
        // GpuView) and the light list (the whole light buffer, read as Lights[lightOffset + i]). Slots
        // are per view: views of different scenes bind different IBL products in one frame (per-scene
        // sky), so each view index caches its own group, keyed by the ring generations, the shadow and
        // atlas generations, and the IBL context generation (unique across contexts). Sets m_viewBG
        // (the current view's group) for the Resolve* bodies.
        bool EnsureViewBindGroup(const RenderRecordContext& ctx);

        // The comparison sampler (hardware PCF) and a 1x1 dummy depth map bound into set 0 when no shadow
        // caster exists this frame (so the descriptor set is always complete).
        Status CreateShadowResources();

        // The set-0 bind group for the shadow depth pass: just the light-view UBO (dynamic-offset
        // window of one GpuShadowView). Rebuilt when the shadow-view ring (re)allocated.
        bool EnsureShadowViewBindGroup();

        // Tiny placeholder cluster buffers and a set-3 bind group over them, bound when clustering is
        // unavailable. The shader's ClusterGridX==0 path never reads them, but set 3 must be bound.
        Status CreateDummyClusters();

        // The set-3 bind group for a (view, frame in flight) slot, over the ClusterSystem's cluster
        // buffers for it. One per slot (the buffers alternate per frame) so a slot's group is stable.
        // Rebuild when the cluster buffers' version changes (the ClusterSystem bumps it on realloc).
        // Pointer identity is not a safe "unchanged" test: a freed rhi::BufferHandle address can be reused by
        // the new allocation, and a stale bind group would point at a destroyed VkBuffer.
        rhi::BindGroupHandle EnsureClusterBindGroup(u32 viewIndex, u32 frameIndex, rhi::BufferHandle offsets,
                                               rhi::BufferHandle indices, u32 version);

        void Shutdown();

        IAllocator* m_allocator; // first: the containers below are made with it
        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        materials::PipelineStateCache* m_psoCache;
        materials::MaterialSystem* m_materials;
        GpuMeshCache m_meshes;
        u32 m_framesInFlight = 2;
        u32 m_frameIndex = 0; // this frame's index (selects the offsets buffer's frame-in-flight slot)

        rhi::BindGroupLayoutHandle m_viewLayout{};
        rhi::BindGroupLayoutHandle m_instanceLayout{};
        rhi::BindGroupLayoutHandle m_clusterLayout{};
        rhi::BindGroupLayoutHandle m_shadowViewLayout{}; // set 0 for the depth-only shadow pipeline
        rhi::PipelineLayoutHandle m_shadowPipelineLayoutInstanced{};
        // Forward pipeline layouts, keyed by (material set-2 layout); built on demand so each
        // material's own set-2 layout drives the PSO (which supports custom shaders).
        HashMap<u64, rhi::PipelineLayoutHandle> m_pipelineLayouts{*m_allocator};
        HashMap<u64, rhi::PipelineLayoutHandle>
            m_shadowMaskedLayouts{*m_allocator}; // masked-shadow 3-set layouts (by set-2)

        // Material set-2 resources. The default material (standard PBR) backs draws with no material;
        // instances carry per-material overrides and flow through the material system's data-driven BG path.
        RefPtr<materials::Material> m_defaultMaterial;
        HashMap<u64, materials::MaterialInstance*> m_instances{*m_allocator};          // lookup by Material::uid
        Array<UniquePtr<materials::MaterialInstance>> m_instanceStorage{*m_allocator}; // ownership
        struct OverrideInstance
        {
            u64 scene = 0;
            u64 entityId = 0;
            u32 slot = 0;
            u64 materialUid = 0;
            u32 version = 0;
            bool applied = false;
            u64 lastUsed = 0; // m_frameSerial when a draw last used it
            UniquePtr<materials::MaterialInstance> instance;
        };
        HashMap<u64, OverrideInstance> m_overrideInstances{*m_allocator}; // by (scene, entity, slot)
        u64 m_frameSerial = 0;

        ViewUniformRing m_viewRing; // one slot per distinct view block a frame
        DynamicUniformRing m_shadowViewRing;
        GpuView m_passView{}; // the pass's view block, copied per faded instanced set with its window
        DynamicUniformRing m_instanceRing;
        DynamicUniformRing m_offsetsRing;
        DynamicUniformRing m_lightRing;
        DynamicUniformRing m_localShadowRing; // per-frame GpuLocalShadow entries (spot and point atlas)
        DynamicUniformRing m_boneRing; // bone-matrix staging ring (CpuToGpu; written once per frame)
        u32 m_boneSlotsWanted = kInitialBoneMatrices; // per-frame slots the pool is sized for
        rhi::BufferHandle m_boneDevice{}; // GpuOnly device mirror the VS reads (set-0 t4 SRV)
        u64 m_boneDeviceBytes = 0;
        u32 m_boneDeviceGen = 0; // bumps on (re)create, invalidating set-0 bind groups
        // Per-frame map: a skinned instance's boneMatrices pointer -> its bases (matrix units) in the pool.
        struct BoneSlot
        {
            u32 base = 0;
            u32 prevBase = 0;
        };
        // cur and prev are palette pointers, count the matrices in `cur`. hasPrev: per-entity casters write a
        // current and a previous slab (motion vectors); a skinned-MultiMesh pose pool writes only its
        // M*boneCount palettes (no previous).
        struct SkinnedRef
        {
            const Float4x4* cur;
            const Float4x4* prev;
            u32 count;
            bool hasPrev = true;
        };
        HashMap<const Float4x4*, BoneSlot> m_boneStart{*m_allocator};
        Array<SkinnedRef> m_skinnedScratch{*m_allocator};
        Array<DepthLikeRange> m_depthRanges{*m_allocator}; // a depth-like draw's ranges, reused

        // Per-entity previous-frame world matrix, for rigid-object motion vectors, kept per scene:
        // scenes drawn in one frame (two editor pages, a page and its play) reuse entity handles, and
        // one history for all of them gave an entity another scene's last position (a smear). Within a
        // scene a flat double buffer indexed by entity index (entityId's low 32 bits) rather than a
        // hash map: a per-instance lookup is the motion-vector hot path at scale, and a direct index
        // avoids hashing; the scene's history is found once per run of its draws (the last one is
        // kept). `prev` holds last frame's worlds (read by every view this frame); resolves write this
        // frame's into `cur`; the two swap at FinishFrame. An out-of-range or never-written entry
        // gives prev == cur (no motion), so newly visible objects don't smear on their first frame.
        struct MotionHistory
        {
            Array<Float4x4> prev;
            Array<Float4x4> cur;
            u64 lastUsed = 0; // m_frameSerial when a draw last read it; a scene gone for a while is dropped
        };
        HashMap<u64, MotionHistory> m_motion{*m_allocator}; // by scene serial
        u64 m_motionScene = ~0ull;
        MotionHistory* m_motionLast = nullptr; // m_motion's entry for m_motionScene (reset when it rehashes)

        // Record `cur` as this frame's world (idempotent across a frame's views, which pass the same value) and
        // return the entity's previous-frame world in its scene (or `cur` if unknown). Indexed by entity
        // index so multiple views resolving the same object read a stable prev (cur and prev are separate
        // buffers).
        [[nodiscard]] Float4x4 PrevWorldFor(u64 scene, u64 entityId, const Float4x4& cur);

        // Instance sharing from the camera depth prepass to the forward pass. The prepass fills the full
        // InstanceData (world, real prevWorld, tint) for each opaque run once and records the run's draw
        // offsets range here; the forward looks it up and reuses that instance data instead of filling it
        // again. Keyed by (view, the run's first item): runs of one mesh and material split by their slot
        // materials, skinning or fade are apart, and each finds its own. Cleared each frame. Instanced
        // path only.
        struct InstShare
        {
            u64 offsByteOffset = 0;
            u32 count = 0;
        };
        HashMap<u64, InstShare> m_instShareCache{*m_allocator};
        // This frame's light uploads, one per scene's light list (keyed by the list): every view
        // and pass of a scene reads the same range, so the ring holds each scene's lights once.
        struct LightUpload
        {
            u32 offset = 0;
            u32 count = 0;
        };
        HashMap<const GpuLight*, LightUpload> m_lightUploads{*m_allocator};
        u32 m_sceneLights = 0;           // SetSceneLights: what the light ring is sized for
        bool m_lightOverflowLogged = false; // the first overflow is said once
        // Keyed by the view pointer, not viewIndex: the main view's prepass and forward share one RenderView, but
        // probe-capture forwards reuse viewIndex 0 with a different draw list, and a distinct pointer keeps them apart.
        static u64 InstShareKey(const void* view, const void* runHead) noexcept;

        // LOD hysteresis memory: the last selected level per (view pointer, item), kept
        // across frames. Bounded by a size cap: when it overflows, the map clears wholesale
        // (at worst, one frame of selection without hysteresis).
        HashMap<u64, u32> m_lodLast{*m_allocator};

        // Per-view set-0 slots (views of different scenes bind different IBL products) and the
        // current view's group (set by EnsureViewBindGroup; read by the Resolve* bodies).
        struct ViewBGSlot
        {
            rhi::BindGroupHandle bg{};
            u32 viewGen = 0, lightGen = 0, localGen = 0, boneGen = 0;
            rhi::TextureViewHandle shadow{};
            u64 shadowGen = 0;
            rhi::TextureViewHandle atlas{};
            u64 atlasGen = 0;
            u64 iblGen = 0;
            rhi::TextureViewHandle prefilter{};
            rhi::TextureViewHandle probeCube{};
            rhi::BufferHandle probeBuf{};
        };
        Array<ViewBGSlot> m_viewBGs{*m_allocator};
        rhi::BindGroupHandle m_viewBG{}; // current view's (borrowed from its slot)
        rhi::BindGroupHandle m_shadowViewBG{};
        u32 m_shadowViewBGGen = 0;
        u32 m_shadowViewBGBoneGen = 0;
        rhi::BindGroupHandle m_instanceBG{};
        // The InstanceSet a per-frame instanced batch binds: no fade, each instance its own id.
        rhi::BufferHandle m_noFadeSetBuf{};
        // The set-0 bind group spans the view UBO, light buffer, shadow map and sampler; rebuild it when any
        // of those change (rings roll over, or the active shadow map view changes).
        // The set-0 shadow binding is cached by (view handle, ShadowSystem generation): the generation
        // also changes when the shadow maps are remade (atlas or resolution changes).
        // ClusterBinding::version and ShadowSystem::Generation() follow the same rule.
        // Shadow set-0 resources: the comparison sampler and a 1x1 dummy map; m_activeShadowView points at
        // the real ShadowSystem map (set each frame) or the dummy.
        rhi::SamplerHandle m_shadowSampler{};
        rhi::TextureHandle m_dummyShadowTex{};
        rhi::TextureViewHandle m_dummyShadowView{};
        rhi::TextureViewHandle m_activeShadowView{};
        // The dummy shadow and atlas depth textures are bound (and statically sampled by the mesh shader) on
        // frames with no real caster, but live outside the render graph, so the barrier solver never
        // transitions them out of UNDEFINED. Transition them once to DepthStencilRead the first frame we
        // hold the command encoder, so the layout the descriptor expects always matches (VUID-09600).
        bool m_dummyDepthInit = false;
        u64 m_activeShadowGen = 0;
        // Local-light (spot and point) shadow atlas (t2) and per-light entries (t3). The atlas view, its
        // generation and the local-shadow ring generation extend the set-0 bind-group cache key.
        rhi::TextureHandle m_dummyAtlasTex{};
        rhi::TextureViewHandle m_dummyAtlasView{};
        rhi::TextureViewHandle m_activeAtlasView{};
        u64 m_activeAtlasGen = 0;
        u32 m_localShadowBase = 0;      // this frame's base into m_localShadowRing
        u32 m_localShadowPassCount = 0; // # atlas depth passes (caster re-emits) this frame
        u32 m_captureFacePasses = 0;    // # probe-capture face passes (caster re-emits) this frame
        u32 m_pickPasses = 0;           // # pick passes (id re-emits) this frame
        u32 m_shadowCasterDraws = 0;    // the shadow passes' caster draws this frame (all views)
        u32 m_instanceBGGen = 0;

        // --- MultiMesh (instanced-mesh) persistent buffers ---
        // Each InstancedMeshComponent (a "set") owns a persistent InstanceData storage buffer and a set-1 bind
        // group over it, keyed by the component's stable key. It is uploaded only when the set's version
        // changes, so a static set uploads once and then costs nothing per frame. A single shared
        // draw offsets ramp [{0,..},{1,..},...] serves every set (.x indexes each set's own buffer), grown to
        // the largest set. Bound identically for the depth prepass, forward, all shadow cascades and the
        // pick pass (which reads only its instanceIndex and boneBase, the lanes GpuPickDrawOffsets shares).
        static constexpr u32 kMultiMeshMaxFiF =
            4; // upper bound on frames-in-flight for the per-region arrays
        struct MultiMeshSet
        {
            // The InstanceData is N-buffered (one region per frame-in-flight) so a runtime position change can
            // rewrite this frame's region while the GPU reads the previous frame's, hazard-free even for
            // crowds that move every frame. Each region has its own set-1 bind group; the draw binds this frame's.
            rhi::BufferHandle instanceBuf{}; // Storage, CpuToGpu: framesInFlight x capacity x InstanceData
            rhi::BindGroupHandle instanceBG[kMultiMeshMaxFiF] =
                {}; // one set-1 bind group per FiF region
            rhi::BindGroupHandle activeInstanceBG{};      // this frame's region bind group (set in EnsureMultiMeshSet)
            // Its InstanceSet block (fade window, instance count, pick id), one kViewSlot per
            // frame-in-flight region, rewritten each frame the set draws (Uniform, CpuToGpu).
            rhi::BufferHandle setBuf{};
            u32 capacity = 0; // instances per region
            u32 count = 0;    // live instance count this frame
            // Per region: the version its bytes hold and how many instances were written with it.
            // A region is rewritten when either falls behind the frame's item: a new version, or
            // a draw count that grew past what this region holds (a distance fade opening up
            // within the rounded capacity). Otherwise the tail draws whatever the region held
            // before, which differs per region and flickers.
            u32 regionVersion[kMultiMeshMaxFiF] = {};
            u32 regionCount[kMultiMeshMaxFiF] = {};
            u32 lastFrame = 0; // last frame this set was extracted (for eviction)
            // Skinned crowds: a per-set draw offsets buffer (dynamic, refilled each frame with per-instance bone
            // bases), used instead of the shared static ramp, whose .y is always 0. Allocated only for skinned sets.
            rhi::BufferHandle offsetsBuf{};             // Vertex, CpuToGpu: framesInFlight x capacity x GpuDrawOffsets
            // The same instances in the pick pass's layout ({i, bone base, 0, 0}: the ids 0, so the set
            // picks as its InstanceSet's entity), filled beside offsetsBuf with the same regions.
            rhi::BufferHandle pickOffsetsBuf{}; // Vertex, CpuToGpu: framesInFlight x capacity x GpuPickDrawOffsets
            u32 offsetsCapacity = 0; // instances per frame region
            u32 offsetsByteOffset =
                0; // this frame's region byte offset (rewritten every frame, so one region per frame in flight)
            bool skinned = false; // drew skinned this frame (posePool present)
        };
        HashMap<u64, MultiMeshSet> m_multiMeshSets{*m_allocator};
        rhi::BufferHandle m_rampBuffer{}; // shared draw offsets ramp (Vertex, CpuToGpu)
        // The ramp in the pick pass's layout, {i, 0, 0, 0}: a MultiMesh set's pick draws bind it (its ids
        // 0, so the set picks as its InstanceSet's entity), never the forward layout's ramp.
        rhi::BufferHandle m_pickRampBuffer{};
        u32 m_rampCapacity = 0;
        u32 m_multiMeshFrame = 0; // bumped each UploadMultiMeshes (eviction clock)

        // IBL: SH9 diffuse buffer (t5), prefiltered specular cube (t6), BRDF LUT (t7) and a
        // linear env sampler (s1), all set 0. Neutral 1x1 dummies are bound when no environment is active.
        static constexpr u64 kShBytes = sizeof(f32) * 4 * 9; // 9 RGB SH coefficients as float4
        rhi::SamplerHandle m_envSampler{};
        rhi::BufferHandle m_dummyShBuffer{};
        rhi::TextureHandle m_dummyCube{};
        rhi::TextureViewHandle m_dummyCubeView{};
        rhi::TextureHandle m_dummyBrdf{};
        rhi::TextureViewHandle m_dummyBrdfView{};

        // Reflection probes: prefiltered cube array (t8), probe-metadata SRV (t9) and probe count.
        static constexpr u64 kProbeBufferBytes =
            64u * 16u; // sizeof(GpuProbe)=64 * kMaxReflectionProbes=16
        rhi::TextureHandle m_dummyProbeCube{};
        rhi::TextureViewHandle m_dummyProbeCubeView{};
        rhi::BufferHandle m_dummyProbeBuffer{}; // 1 zeroed record for the no-probe path
        rhi::TextureViewHandle m_activeProbeCube{};
        rhi::BufferHandle m_activeProbeBuffer{};
        u32 m_activeProbeCount = 0;

        // set 3 (clustered light lists). A dummy bound when clustering is off; otherwise one bind group
        // per (view, frame-in-flight) slot over the ClusterSystem's per-view cluster buffers.
        rhi::BufferHandle m_dummyClusterOffsets{};
        rhi::BufferHandle m_dummyClusterIndices{};
        rhi::BindGroupHandle m_dummyClusterBG{};
        struct ClusterSlot
        {
            rhi::BindGroupHandle group{};
            rhi::BufferHandle offsets{};
            u32 version = 0;
        };
        ViewSlots<ClusterSlot> m_clusterSlots; // grown to the most views one frame draws

        bool m_ready = false;
    };

} // namespace foundation::render
