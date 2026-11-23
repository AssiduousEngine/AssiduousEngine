// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:sky` partition.
///
/// Draws the environment as the visible background: a fullscreen triangle at the far plane, depth-
/// tested (LessEqual, no write) against the forward depth so it only fills pixels no geometry covered,
/// reconstructing a world-space view ray per pixel (inverse view-proj) and sampling the env cubemap.
/// Runs after the forward pass, into the same (HDR) color target, before tonemap, so the sky is in
/// the linear working space and gets tonemapped with the scene.

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof

export module foundation.render:sky;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :bind_group_cache; // ViewSlots
import :data;        // kGVelocityFormat (sky writes camera-motion velocity for TAA)

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // The sky's uniform (cbuffer Sky, sky_common.hlsli); Integration.ShaderBlocks checks it against
    // the compiled shader, member by member.
    struct GpuSky
    {
        Float4x4 invViewProj;  // inverse of the unjittered view-proj (a stable sky ray under TAA)
        Float4x4 prevViewProj; // last frame's view-proj (motion vectors)
        Float3 cameraPos;            // camera world position
        f32 backgroundIntensity = 1; // the sky background's display multiplier (not IBL)
        Float3 sunDirection;         // the light's direction
        f32 sunAngularSizeDegrees = 0;
        Float3 sunColor;
        f32 sunIntensity = 0;
        Float2 jitter;          // this frame's NDC jitter
        Float2 prevJitter;      // last frame's NDC jitter (unjitters the reprojection)
        f32 sceneYSign = 1;     // the scene NDC's Y sign: -1 on Y-flip targets, +1 on Vulkan
        f32 _pad0 = 0, _pad1 = 0, _pad2 = 0;
    };
    static_assert(sizeof(GpuSky) == 208, "cbuffer Sky layout drift");
    static_assert(offsetof(GpuSky, cameraPos) == 128);
    static_assert(offsetof(GpuSky, backgroundIntensity) == 140);
    static_assert(offsetof(GpuSky, sunDirection) == 144);
    static_assert(offsetof(GpuSky, sunAngularSizeDegrees) == 156);
    static_assert(offsetof(GpuSky, sunColor) == 160);
    static_assert(offsetof(GpuSky, sunIntensity) == 172);
    static_assert(offsetof(GpuSky, jitter) == 176);
    static_assert(offsetof(GpuSky, prevJitter) == 184);
    static_assert(offsetof(GpuSky, sceneYSign) == 192);

    class SkyPass
    {
    public:
        SkyPass(rhi::Device& device, shaders::ShaderSystem& shaders, u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders), m_fif(framesInFlight < 1 ? 1 : framesInFlight)
        {
            m_slots.SetFramesInFlight(m_fif);
        }
        ~SkyPass() { Shutdown(); }
        SkyPass(const SkyPass&) = delete;
        SkyPass& operator=(const SkyPass&) = delete;

        Status Initialize();

        // Declare the sky pass: load `color`, depth-test (read-only) against `depth`, read `envH`, draw a
        // fullscreen triangle sampling the env cube along the per-pixel world ray. `invViewProj` is the
        // inverse of this view's view*proj; `camPos` and `intensity` scale the result.
        void DeclareSky(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                        rendergraph::RGHandle velocity, rendergraph::RGHandle depth,
                        rendergraph::RGHandle envH, rhi::TextureViewHandle envView,
                        rhi::TextureFormat colorFormat, rhi::TextureFormat depthFormat,
                        const Float4x4& invViewProj, const Float4x4& prevViewProj, Float2 jitter,
                        Float2 prevJitter, const Float3& camPos, f32 backgroundIntensity,
                        const Float3& sunDir, f32 sunSize, const Float3& sunColor, f32 sunIntensity,
                        i32 vpX, i32 vpY, u32 vpW, u32 vpH, u32 frameIndex, u32 viewIndex,
                        u64 envUid, u32 samples = 1,
                        rendergraph::RGSubresourceRange colorSub = {});

    private:
        rhi::RenderPipelineHandle EnsurePipeline(rhi::TextureFormat colorFmt,
                                            rhi::TextureFormat depthFmt, u32 samples);

        // Env identity is (view handle, context uid): the uid changes when an IBL context is evicted
        // and made again.
        rhi::BindGroupHandle EnsureBindGroup(u32 viewIndex, u32 frameIndex, rhi::TextureViewHandle envView, u64 envUid,
                                        const GpuSky& u);

        void DestroyPipelines();
        void Shutdown();

        struct Slot
        {
            rhi::BufferHandle ubo{};
            rhi::BindGroupHandle bindGroup{};
            rhi::TextureViewHandle env{};
            u64 envUid = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_fif = 2;
        rhi::BindGroupLayoutHandle m_layout{};
        rhi::PipelineLayoutHandle m_pipelineLayout{};
        // One pipeline per target layout. Views of one frame can draw the sky into different
        // layouts (an MSAA viewport beside a Game tab without), and a pass declared earlier in the
        // frame holds its pipeline until the graph runs: replacing one per view would destroy a
        // pipeline a declared pass still draws with.
        struct Pipeline
        {
            rhi::TextureFormat color = rhi::TextureFormat::Undefined;
            rhi::TextureFormat depth = rhi::TextureFormat::Undefined;
            u32 samples = 1;
            rhi::RenderPipelineHandle pipeline{};
        };
        Array<Pipeline> m_pipelines;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version they were built at (hot reload)
        rhi::SamplerHandle m_sampler{};
        ViewSlots<Slot> m_slots; // per (view, frame in flight), however many views a frame has
    };

} // namespace foundation::render
