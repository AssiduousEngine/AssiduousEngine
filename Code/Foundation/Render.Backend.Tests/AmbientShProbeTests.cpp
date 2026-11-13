// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The sky's diffuse light (SH9 irradiance) on a real device. The sky's radiance is z^2 along every
// direction, a function within the second SH band, so its cosine-convolved irradiance is exact:
// pi/2 on a surface facing +z, pi/4 on one facing +x. A white sphere lit by that sky alone (no
// lights, no specular IBL) must then read twice as bright where it faces +z as where it faces +x.
// The L20 term three times too strong made it ten times.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.testsupport;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u32 kSize = 128;
    constexpr u32 kFrames = 3; // the BRDF table and the sky's products bake on the first

    void Project(const ViewCamera& camera, Float3 p, u32& px, u32& py)
    {
        const Float4x4 vp = camera.view * camera.projection;
        f32 clip[4] = {};
        for (u32 j = 0; j < 4; ++j)
        {
            clip[j] = p.x * vp(0, j) + p.y * vp(1, j) + p.z * vp(2, j) + vp(3, j);
        }
        px = static_cast<u32>((clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<f32>(kSize));
        py = static_cast<u32>((0.5f - clip[1] / clip[3] * 0.5f) * static_cast<f32>(kSize));
    }

    f64 BoxLuma(const testsupport::CapturedImage& image, u32 cx, u32 cy)
    {
        f64 sum = 0.0;
        for (u32 y = cy - 1; y <= cy + 1; ++y)
        {
            for (u32 x = cx - 1; x <= cx + 1; ++x)
            {
                sum += image.Luma(x, y);
            }
        }
        return sum / 9.0;
    }

    // An equirect image of radiance z^2, in the mapping ibl_equirect.ps.hlsl samples with:
    // u = atan2(z, x) / 2pi + 0.5, v = 0.5 - asin(y) / pi.
    Array<f32> SkyOfZSquared(u32 w, u32 h)
    {
        Array<f32> rgba;
        rgba.Resize(static_cast<usize>(w) * h * 4);
        constexpr f32 kPi = 3.14159265359f;
        for (u32 y = 0; y < h; ++y)
        {
            const f32 theta = (0.5f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h)) * kPi;
            for (u32 x = 0; x < w; ++x)
            {
                const f32 phi = ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(w) - 0.5f) * 2.0f * kPi;
                const f32 z = Cos(theta) * Sin(phi);
                f32* p = &rgba[(static_cast<usize>(y) * w + x) * 4];
                p[0] = p[1] = p[2] = z * z;
                p[3] = 1.0f;
            }
        }
        return rgba;
    }
}

TEST_CASE("ambient SH: a sky of z^2 lights a surface facing +z twice as much as one facing +x - Vulkan")
{
    rhi::Backend* backend = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backend);
    rhi::Device* device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the ambient SH probe skipped");
        if (backend != nullptr)
        {
            backend->Destroy();
        }
        return;
    }

    ViewCamera camera; // from +x +z, so the sphere's +x and +z faces both show
    camera.view = Float4x4::LookAtRH(Float3{5, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(0.6f, 1.0f, 0.1f, 50.0f);
    camera.farZ = 50.0f;
    f64 facingZ = 0.0, facingX = 0.0;

    shaders::ShaderSystemHost host{DefaultAllocator()};
    REQUIRE(host.Initialize(*device, testsupport::DataFileSystem()));
    {
        shaders::ShaderSystem& shaderSystem = *host.System();
        materials::PipelineStateCache psoCache(shaderSystem, *device);
        materials::MaterialSystem materialSystem;
        REQUIRE(materialSystem.Initialize(*device).IsOk());
        MeshRenderer meshRenderer(DefaultAllocator(), *device, shaderSystem, psoCache, materialSystem, 2);
        REQUIRE(meshRenderer.Initialize().IsOk());
        RendererRegistry registry;
        registry.Register(&meshRenderer);
        IBLSystem ibl(*device, shaderSystem);
        REQUIRE(ibl.Initialize().IsOk());
        const Array<f32> sky = SkyOfZSquared(256, 128);
        ibl.SetEquirect(256, 128, Span<const f32>{sky.Data(), sky.Size()});
        RenderFrame frame(DefaultAllocator(), *device, registry, 2, nullptr, nullptr, nullptr, &ibl);

        RefPtr<geometry::StaticMesh> sphere = geometry::Primitives::Sphere(DefaultAllocator(), 1.0f, 64);
        RefPtr<materials::Material> white =
            materials::CreatePBR(u8"probe.white", Float4{1, 1, 1, 1}, 0.0f, 1.0f);

        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = kSize;
        td.height = kSize;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        td.label = u8"ambientshprobe.target";
        rhi::Texture* target = nullptr;
        REQUIRE(device->CreateTexture(td, target).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8Unorm;
        rhi::TextureView* targetView = nullptr;
        REQUIRE(device->CreateTextureView(target, vd, targetView).IsOk());
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device->CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        rhi::Fence* fence = nullptr;
        REQUIRE(device->CreateFence(0, fence).IsOk());
        rhi::Queue* queue = device->GetQueue(rhi::QueueType::Graphics);

        ExtractedScene scene{DefaultAllocator()};
        SkySnapshot skySettings;
        skySettings.mode = SkyMode::HDREquirect; // the programmatic image above
        skySettings.iblSpecularIntensity = 0.0f;  // the diffuse alone
        scene.SetSky(skySettings);
        MeshRenderData* data = scene.Add<MeshRenderData>();
        data->worldRadius = 1.0f;
        data->mesh = sphere.Get();
        data->material = white.Get();
        data->category = RenderCategories::Opaque;

        ViewSettings settings;
        settings.clear = rhi::ClearColor::Black();
        settings.targetTexture = target;
        settings.targetFinalState = rhi::ResourceState::CopySrc;
        settings.post.taaEnabled = false;
        settings.post.bloomEnabled = false;
        for (u32 i = 0; i < kFrames; ++i)
        {
            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            settings.targetCurrentState = (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
            frame.Begin(*encoder, i % 2);
            frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
            frame.End();
            rhi::CommandBuffer* commandBuffer = encoder->Finish();
            REQUIRE(commandBuffer != nullptr);
            rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
            queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
            REQUIRE(fence->Wait(i + 1, ~0ull));
            pool->DestroyEncoder(encoder);
        }
        const testsupport::CapturedImage image = testsupport::Readback(*device, target, kSize, kSize);
        REQUIRE(image.valid);
        // Just inside the sphere where its normal is +z and +x (both 45 degrees from the camera).
        u32 x = 0, y = 0;
        Project(camera, Float3{0, 0, 0.98f}, x, y);
        facingZ = BoxLuma(image, x, y);
        Project(camera, Float3{0.98f, 0, 0}, x, y);
        facingX = BoxLuma(image, x, y);

        device->WaitIdle();
        device->DestroyFence(fence);
        device->DestroyCommandPool(pool);
        device->DestroyTextureView(targetView);
        device->DestroyTexture(target);
    }
    host.Shutdown();

    MESSAGE("facing +z ", facingZ, ", facing +x ", facingX);
    CHECK(facingX > 30.0); // lit at all
    CHECK(facingZ / facingX == doctest::Approx(2.0).epsilon(0.1));
    device->Destroy();
    backend->Destroy();
}
