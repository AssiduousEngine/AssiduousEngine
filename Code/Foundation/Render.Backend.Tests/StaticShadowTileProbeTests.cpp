// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The static shadow atlas layer on a real device: two Static spot lights each cast a cube's shadow
// into their cached tiles; then a caster moves inside the first light's reach only, so only that
// light's tile redraws, frame after frame. The second light's shadow must stay: its tile is cached,
// not redrawn, and the pass that redraws the first must not clear it. Checked with the tile clear
// and with the whole-layer fallback a renderer without the clear's shader takes.
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
    constexpr u32 kStillFrames = 4;  // every static tile drawn into both frame slots, then clean
    constexpr u32 kMovingFrames = 4; // the first light's tile redraws each frame

    // Each light stands 1.5 to the left of its cube and 2 up, aiming right and down, so the cube's
    // shadow falls on the plane just right of the cube: x in [ox + 0.5, ox + 2.5].
    constexpr f32 kLightA = -6.0f;
    constexpr f32 kLightB = 4.0f;

    // A world point's pixel in the top-down view.
    void Project(const ViewCamera& camera, Float3 p, u32& px, u32& py)
    {
        const Float4x4 vp = camera.view * camera.projection;
        f32 clip[4] = {};
        for (u32 j = 0; j < 4; ++j)
        {
            clip[j] = p.x * vp(0, j) + p.y * vp(1, j) + p.z * vp(2, j) + vp(3, j);
        }
        const f32 nx = clip[0] / clip[3];
        const f32 ny = clip[1] / clip[3];
        px = static_cast<u32>((nx * 0.5f + 0.5f) * static_cast<f32>(kSize));
        py = static_cast<u32>((0.5f - ny * 0.5f) * static_cast<f32>(kSize));
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

    struct Readings
    {
        f64 shadowA = 0.0, litA = 0.0;
        f64 shadowB = 0.0, litB = 0.0;
    };

    Readings Render(rhi::Device& device, bool tileClear)
    {
        Readings out;
        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 20, 0.01f}, Float3{0, 0, 0}, Float3{0, 0, -1});
        camera.projection = Float4x4::PerspectiveFovRH(1.0f, 1.0f, 0.1f, 100.0f);
        camera.farZ = 100.0f;

        shaders::ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, testsupport::DataFileSystem()));
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            ShadowSystem shadows(device, 2u);
            REQUIRE(shadows.Initialize().IsOk());
            shadows.SetShaderSystem(tileClear ? &shaderSystem : nullptr);
            CHECK((shadows.TileClearPipeline() != nullptr) == tileClear); // the mode under test
            RenderFrame frame(DefaultAllocator(), device, registry, 2, nullptr, nullptr, &shadows);

            RefPtr<geometry::StaticMesh> planeMesh = geometry::Primitives::Plane(DefaultAllocator(), 24.0f, 8.0f);
            RefPtr<geometry::StaticMesh> cubeMesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
            RefPtr<geometry::StaticMesh> moverMesh = geometry::Primitives::Cube(DefaultAllocator(), 0.5f);
            RefPtr<materials::Material> grey =
                materials::CreatePBR(u8"probe.grey", Float4{0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.9f);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"statictileprobe.target";
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());
            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);

            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.taaEnabled = false;
            settings.post.bloomEnabled = false;

            for (u32 i = 0; i < kStillFrames + kMovingFrames; ++i)
            {
                ExtractedScene scene{DefaultAllocator()};
                scene.SetAmbient(Float3{0.02f, 0.02f, 0.02f});
                u64 nextId = 1;
                const auto add = [&](geometry::StaticMesh* mesh, Float3 at, f32 radius)
                {
                    MeshRenderData* data = scene.Add<MeshRenderData>();
                    data->world = Float4x4::Translation(at);
                    data->worldCenter = at;
                    data->worldRadius = radius;
                    data->mesh = mesh;
                    data->material = grey.Get();
                    data->category = RenderCategories::Opaque;
                    data->entityId = nextId++;
                };
                add(planeMesh.Get(), Float3{0, 0, 0}, 13.0f);
                const f32 cubes[2] = {kLightA, kLightB};
                for (f32 ox : cubes)
                {
                    add(cubeMesh.Get(), Float3{ox, 0.5f, 0}, 0.9f);
                }
                if (i >= kStillFrames)
                {
                    // Inside the first light's reach (8 from it), far outside the second's, and a
                    // little further along each frame, so the first light's tile redraws every frame.
                    add(moverMesh.Get(), Float3{kLightA - 4.5f, 0.25f, -3.0f + 0.2f * static_cast<f32>(i)}, 0.45f);
                }
                for (u32 l = 0; l < 2; ++l)
                {
                    const Float3 position{cubes[l] - 1.5f, 2.0f, 0.0f};
                    const Float3 direction = Normalized(Float3{3.0f, -2.0f, 0.0f});
                    GpuLight spot;
                    spot.type = 2.0f;
                    spot.positionWS = position;
                    spot.directionWS = direction;
                    spot.range = 8.0f;
                    spot.intensity = 30.0f;
                    spot.innerCos = Cos(0.9f);
                    spot.outerCos = Cos(1.1f);
                    spot.shadowIndex = static_cast<f32>(l); // one tile each, in caster order
                    scene.AddLight(spot);
                    LocalShadowCaster caster;
                    caster.type = 2;
                    caster.positionWS = position;
                    caster.directionWS = direction;
                    caster.range = 8.0f;
                    caster.outerAngle = 1.1f;
                    caster.isStatic = true;
                    scene.AddLocalShadowCaster(caster);
                }

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
            const testsupport::CapturedImage image = testsupport::Readback(device, target, kSize, kSize);
            REQUIRE(image.valid);
            u32 x = 0, y = 0;
            Project(camera, Float3{kLightA + 1.5f, 0, 0}, x, y);
            out.shadowA = BoxLuma(image, x, y);
            Project(camera, Float3{kLightA + 1.5f, 0, 2.0f}, x, y);
            out.litA = BoxLuma(image, x, y);
            Project(camera, Float3{kLightB + 1.5f, 0, 0}, x, y);
            out.shadowB = BoxLuma(image, x, y);
            Project(camera, Float3{kLightB + 1.5f, 0, 2.0f}, x, y);
            out.litB = BoxLuma(image, x, y);

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return out;
    }
}

TEST_CASE("static shadow atlas: a tile redrawn beside a cached one leaves the cached shadow - Vulkan")
{
    rhi::Backend* backend = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backend);
    rhi::Device* device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the static shadow tile probe skipped");
        if (backend != nullptr)
        {
            backend->Destroy();
        }
        return;
    }
    const bool modes[2] = {true, false};
    for (bool tileClear : modes)
    {
        const Readings r = Render(*device, tileClear);
        const char* mode = tileClear ? "tile clear" : "whole-layer fallback";
        MESSAGE(doctest::String(mode), ": A shadow ", r.shadowA, " lit ", r.litA,
                "; B shadow ", r.shadowB, " lit ", r.litB);
        CHECK(r.litA > 60.0);
        CHECK(r.litB > 60.0);
        CHECK(r.shadowA < r.litA * 0.5); // the redrawn light keeps its shadow
        CHECK(r.shadowB < r.litB * 0.5); // and the cached one keeps its own
    }
    device->Destroy();
    backend->Destroy();
}
