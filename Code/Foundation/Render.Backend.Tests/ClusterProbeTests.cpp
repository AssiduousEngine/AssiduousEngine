// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The clustered light lists proven at the pixel: a ground seen from above, lit only by a grid of
// small point lights, through the full RenderFrame chain with the ClusterSystem on real Vulkan and
// WebGPU devices. Each light's patch is lit and the ground between them is dark, so every cluster
// found its own lights and no others. The grid has more lights than the shared index list's
// average (it is sized by that past 16), so the compact list is what is read.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
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
    constexpr f32 kSpacing = 2.0f; // between lights
    constexpr f32 kRange = 0.9f;   // short of the gap between them
    constexpr f32 kHeight = 0.4f;  // over the ground

    ViewCamera TopDown()
    {
        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 12.0f, 0.001f}, Float3{0, 0, 0}, Float3{0, 0, -1});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        camera.position = Float3{0, 12.0f, 0.001f};
        camera.farZ = 100.0f;
        return camera;
    }

    // Screen x of a world point on the middle row (x only: y's convention is the backend's).
    u32 PixelX(const ViewCamera& camera, Float3 world)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * (camera.view * camera.projection);
        return static_cast<u32>(Clamp((clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(kSize), 0.0f,
                                      static_cast<f32>(kSize - 1)));
    }

    // The lights of the scene: a grid of small ones, or (crowded) a dim sun and 40 large point lights
    // each reaching the whole view, more than the shared list's average per cluster.
    void AddLights(ExtractedScene& scene, bool crowded)
    {
        if (crowded)
        {
            GpuLight sun;
            sun.type = 0.0f;
            sun.directionWS = Normalized(Float3{0.2f, -1.0f, 0.1f});
            sun.color = Float3{1, 1, 1};
            sun.intensity = 1.0f;
            sun.shadowIndex = -1.0f;
            scene.AddLight(sun);
            for (i32 i = 0; i < 40; ++i)
            {
                GpuLight light;
                light.type = 1.0f;
                light.positionWS = Float3{static_cast<f32>(i % 8) - 3.5f, 30.0f, static_cast<f32>(i / 8) - 2.0f};
                light.range = 60.0f; // far above, reaching all of the ground in view, faintly
                light.color = Float3{1, 1, 1};
                light.intensity = 0.01f;
                light.shadowIndex = -1.0f;
                scene.AddLight(light);
            }
            return;
        }
        // A 9 by 5 grid: 45 lights, past the list's average per cluster.
        for (i32 gz = -2; gz <= 2; ++gz)
        {
            for (i32 gx = -4; gx <= 4; ++gx)
            {
                GpuLight light;
                light.type = 1.0f; // point
                light.positionWS = Float3{static_cast<f32>(gx) * kSpacing, kHeight, static_cast<f32>(gz) * kSpacing};
                light.range = kRange;
                light.color = Float3{1, 1, 1};
                light.intensity = 6.0f;
                light.shadowIndex = -1.0f;
                scene.AddLight(light);
            }
        }
    }

    testsupport::CapturedImage RenderLights(rhi::Device& device, const ViewCamera& camera, bool crowded = false)
    {
        testsupport::CapturedImage image;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return image;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem,
                                      /*framesInFlight*/ 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            ClusterSystem clusters(device, shaderSystem, /*framesInFlight*/ 2);
            REQUIRE(clusters.Initialize().IsOk());

            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2, &clusters,
                              /*tonemap*/ nullptr, /*shadows*/ nullptr, /*ibl*/ nullptr, /*sky*/ nullptr,
                              /*bloom*/ nullptr, /*taa*/ nullptr, /*ao*/ nullptr, /*fxaa*/ nullptr);

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{0.0f, 0.0f, 0.0f}); // only the point lights light anything
            RefPtr<geometry::StaticMesh> plane = geometry::Primitives::Plane(DefaultAllocator(), 30.0f, 30.0f);
            RefPtr<materials::Material> grey = materials::CreatePBR(u8"cluster.ground", Float4{0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.9f);
            MeshRenderData* ground = scene.Add<MeshRenderData>();
            ground->world = Float4x4::Identity();
            ground->worldCenter = Float3{0, 0, 0};
            ground->worldRadius = 30.0f;
            ground->mesh = plane.Get();
            ground->material = grey.Get();
            ground->category = RenderCategories::Opaque;
            AddLights(scene, crowded);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"cluster.target";
            rhi::TextureHandle target{};
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureViewHandle targetView{};
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());
            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.taaEnabled = false;
            settings.post.bloomEnabled = false;
            for (u32 i = 0; i < 2; ++i)
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
            image = testsupport::Readback(device, target, kSize, kSize);
            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return image;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        const ViewCamera camera = TopDown();
        const testsupport::CapturedImage image = RenderLights(device, camera);
        REQUIRE(image.valid);
        const u32 row = kSize / 2; // world z = 0: the middle row of lights
        for (i32 gx = -2; gx <= 2; ++gx) // in view: the ground shows about 6.9 either side
        {
            const f32 x = static_cast<f32>(gx) * kSpacing;
            const u32 lit = image.Luma(PixelX(camera, Float3{x, 0, 0}), row);
            const u32 between = image.Luma(PixelX(camera, Float3{x + kSpacing * 0.5f, 0, 0}), row);
            INFO(doctest::String(name) << " light " << gx << ": under it " << lit << ", between " << between);
            CHECK(lit > 120);    // its own light found its cluster
            CHECK(between < 30); // and no light past its range did
        }

        // Crowded: every cluster has the sun and all 40 point lights, more than its share. A packed
        // list cut at its region's end left the clusters past it with no lights at all, the sun
        // included: whole tiles of the ground went black.
        const testsupport::CapturedImage crowded = RenderLights(device, camera, /*crowded*/ true);
        REQUIRE(crowded.valid);
        u32 darkest = 765;
        for (u32 y = 8; y < kSize - 8; y += 4)
            for (u32 x = 8; x < kSize - 8; x += 4)
                darkest = Min(darkest, crowded.Luma(x, y));
        INFO(doctest::String(name) << " crowded: darkest " << darkest);
        CHECK(darkest > 60); // the sun reaches every pixel of the ground
    }
}

TEST_CASE("clusters: a grid of small point lights each lights its own patch - Vulkan + WebGPU")
{
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
    if (rhi::Device* device = vulkan != nullptr ? testsupport::MakeTestDevice(vulkan) : nullptr)
    {
        ProbeBackend(*device, "vulkan");
        device->Destroy();
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan probes skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu probes skipped");
    }
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
    if (webgpu != nullptr)
    {
        webgpu->Destroy();
    }
}
