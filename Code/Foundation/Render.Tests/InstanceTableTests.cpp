// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The instance table: each mesh's record written once per frame at most, and only when it changed;
// slots found by scene and entity, freed when the entity is no longer drawn; the copy into the
// table recorded between two transitions.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u64 kSceneA = 11;
    constexpr u64 kSceneB = 12;

    [[nodiscard]] constexpr u64 Entity(u32 index, u32 generation) noexcept
    {
        return (static_cast<u64>(generation) << 32) | index;
    }

    [[nodiscard]] Float4x4 At(f32 x) { return Float4x4::Translation(Float3{x, 0.0f, 0.0f}); }

    // A Null device, the table, and one encoder each frame records into.
    struct Bed
    {
        rhi::null::NullDevice device{DefaultAllocator()};
        InstanceTable table{DefaultAllocator(), device, 2};
        rhi::CommandPool* pool = nullptr;
        rhi::CommandEncoder* encoder = nullptr;
        u32 frame = 0;

        Bed()
        {
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        }
        ~Bed() { device.DestroyCommandPool(pool); }

        void Begin()
        {
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            table.BeginFrame(frame % 2);
        }
        // Ends the frame's writes and records the upload.
        const InstanceTableFrameStats& End()
        {
            table.EndScenes();
            table.Upload(*encoder);
            pool->DestroyEncoder(encoder);
            ++frame;
            return table.FrameStats();
        }
    };

    [[nodiscard]] bool Same(const Float4x4& a, const Float4x4& b)
    {
        return MemCompare(&a, &b, sizeof(Float4x4)) == 0;
    }
}

TEST_CASE("instance table: a record at rest uploads nothing and records no transition")
{
    Bed bed;
    bed.Begin();
    const u32 slot = bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1});
    const InstanceTableFrameStats first = bed.End();
    CHECK(first.written == 1u);
    CHECK(first.copies == 1u);
    CHECK(first.transitions == 2u);
    CHECK(Same(bed.table.Record(slot).prevWorld, At(1.0f))); // a new slot: no motion on its first frame

    for (int i = 0; i < 3; ++i)
    {
        bed.Begin();
        CHECK(bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1}) == slot);
        const InstanceTableFrameStats rest = bed.End();
        CHECK(rest.written == 0u);
        CHECK(rest.copies == 0u);
        CHECK(rest.transitions == 0u);
    }
}

TEST_CASE("instance table: a moved record uploads on the frame it moves and once more as its previous world catches up")
{
    Bed bed;
    bed.Begin();
    const u32 slot = bed.table.Write(kSceneA, Entity(3, 1), At(1.0f), Color{1, 1, 1, 1});
    (void)bed.End();

    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(3, 1), At(2.0f), Color{1, 1, 1, 1});
    CHECK(bed.End().written == 1u);
    CHECK(Same(bed.table.Record(slot).world, At(2.0f)));
    CHECK(Same(bed.table.Record(slot).prevWorld, At(1.0f)));

    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(3, 1), At(2.0f), Color{1, 1, 1, 1});
    CHECK(bed.End().written == 1u); // its previous world catches up
    CHECK(Same(bed.table.Record(slot).prevWorld, At(2.0f)));

    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(3, 1), At(2.0f), Color{1, 1, 1, 1});
    CHECK(bed.End().written == 0u); // at rest
}

TEST_CASE("instance table: the slot of an entity no longer drawn is freed and reused")
{
    Bed bed;
    bed.Begin();
    const u32 gone = bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1});
    (void)bed.table.Write(kSceneA, Entity(1, 1), At(2.0f), Color{1, 1, 1, 1});
    (void)bed.End();
    CHECK(bed.table.LiveSlots() == 2u);

    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(1, 1), At(2.0f), Color{1, 1, 1, 1}); // entity 0 not drawn
    (void)bed.End();
    CHECK(bed.table.LiveSlots() == 1u);

    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(1, 1), At(2.0f), Color{1, 1, 1, 1});
    CHECK(bed.table.Write(kSceneA, Entity(5, 1), At(3.0f), Color{1, 1, 1, 1}) == gone);
    (void)bed.End();
}

TEST_CASE("instance table: an entity that takes a freed index gets a slot of its own, without the old one's motion")
{
    Bed bed;
    bed.Begin();
    const u32 old = bed.table.Write(kSceneA, Entity(4, 1), At(1.0f), Color{1, 1, 1, 1});
    (void)bed.End();

    // The same frame destroyed the entity and made another at its index (a new generation).
    bed.Begin();
    const u32 fresh = bed.table.Write(kSceneA, Entity(4, 2), At(9.0f), Color{1, 1, 1, 1});
    (void)bed.End();
    CHECK(Same(bed.table.Record(fresh).prevWorld, At(9.0f))); // not the old entity's matrix
    CHECK(bed.table.LiveSlots() == 1u);
    (void)old;
}

TEST_CASE("instance table: the items one entity is split into share its slot and its first write")
{
    Bed bed;
    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(2, 1), At(1.0f), Color{1, 1, 1, 1});
    (void)bed.End();

    bed.Begin();
    const u32 frame = bed.table.Write(kSceneA, Entity(2, 1), At(2.0f), Color{1, 1, 1, 1});
    const u32 glass = bed.table.Write(kSceneA, Entity(2, 1), At(2.0f), Color{1, 1, 1, 1});
    CHECK(glass == frame);
    (void)bed.End();
    CHECK(Same(bed.table.Record(frame).prevWorld, At(1.0f))); // the second write kept the true previous
}

TEST_CASE("instance table: two scenes with the same entity index stay apart")
{
    Bed bed;
    bed.Begin();
    const u32 a = bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1});
    const u32 b = bed.table.Write(kSceneB, Entity(0, 1), At(2.0f), Color{1, 1, 1, 1});
    (void)bed.End();
    CHECK(a != b);
    CHECK(Same(bed.table.Record(a).world, At(1.0f)));
    CHECK(Same(bed.table.Record(b).world, At(2.0f)));
}

TEST_CASE("instance table: a scene not drawn this frame keeps its slots; one gone long enough drops them")
{
    Bed bed;
    bed.Begin();
    (void)bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1});
    const u32 hidden = bed.table.Write(kSceneB, Entity(0, 1), At(2.0f), Color{1, 1, 1, 1});
    (void)bed.End();

    // Scene B is a hidden editor page: frames go by without it.
    for (int i = 0; i < 3; ++i)
    {
        bed.Begin();
        (void)bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1});
        (void)bed.End();
    }
    CHECK(bed.table.LiveSlots() == 2u);
    bed.Begin();
    CHECK(bed.table.Write(kSceneB, Entity(0, 1), At(2.0f), Color{1, 1, 1, 1}) == hidden);
    CHECK(bed.End().written == 0u); // back as it was left

    for (u32 i = 0; i <= InstanceTable::kSceneDropFrames + 1; ++i)
    {
        bed.Begin();
        (void)bed.table.Write(kSceneA, Entity(0, 1), At(1.0f), Color{1, 1, 1, 1});
        (void)bed.End();
    }
    CHECK(bed.table.LiveSlots() == 1u);
}

TEST_CASE("instance table: growth makes the table again and fills it whole, every record kept")
{
    Bed bed;
    const u32 count = InstanceTable::kInitialSlots + 10u;
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(static_cast<f32>(i)), Color{1, 1, 1, 1});
    }
    const InstanceTableFrameStats grown = bed.End();
    CHECK(grown.grew);
    CHECK(bed.table.Capacity() >= count);
    CHECK(grown.bytes == static_cast<u64>(count) * sizeof(GpuInstance));
    const u32 generation = bed.table.Generation();

    bed.Begin();
    u32 last = 0;
    for (u32 i = 0; i < count; ++i)
    {
        last = bed.table.Write(kSceneA, Entity(i, 1), At(static_cast<f32>(i)), Color{1, 1, 1, 1});
    }
    const InstanceTableFrameStats steady = bed.End();
    CHECK_FALSE(steady.grew);
    CHECK(steady.written == 0u);
    CHECK(bed.table.Generation() == generation);
    CHECK(Same(bed.table.Record(last).world, At(static_cast<f32>(count - 1))));
}

TEST_CASE("instance table: changed slots far apart are copied as one span; neighbours as one run")
{
    Bed bed;
    const u32 count = 1000;
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(0.0f), Color{1, 1, 1, 1});
    }
    (void)bed.End();
    bed.Begin(); // the previous worlds catch up (nothing moved): no writes
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(0.0f), Color{1, 1, 1, 1});
    }
    REQUIRE(bed.End().written == 0u);

    // Ten neighbours move: one run, one copy.
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(i >= 100 && i < 110 ? 1.0f : 0.0f), Color{1, 1, 1, 1});
    }
    const InstanceTableFrameStats run = bed.End();
    CHECK(run.written == 10u);
    CHECK(run.copies == 1u);

    // Every other slot moves: more runs than kMaxCopyRuns, one span.
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(i % 2 == 0 ? 3.0f : 0.0f), Color{1, 1, 1, 1});
    }
    const InstanceTableFrameStats scattered = bed.End();
    CHECK(scattered.copies == 1u);
}

TEST_CASE("instance table: changes in slot order copy as one run, and a tint alone writes once")
{
    Bed bed;
    const u32 count = 300;
    for (int f = 0; f < 2; ++f) // made, then the previous worlds catch up
    {
        bed.Begin();
        for (u32 i = 0; i < count; ++i)
        {
            (void)bed.table.Write(kSceneA, Entity(i, 1), At(0.0f), Color{1, 1, 1, 1});
        }
        (void)bed.End();
    }
    // Everything moves: one run, one copy.
    bed.Begin();
    u32 last = 0;
    for (u32 i = 0; i < count; ++i)
    {
        last = bed.table.Write(kSceneA, Entity(i, 1), At(2.0f), Color{1, 1, 1, 1});
    }
    const InstanceTableFrameStats moved = bed.End();
    CHECK(moved.written == count);
    CHECK(moved.copies == 1u);
    CHECK(moved.bytes == static_cast<u64>(count) * sizeof(GpuInstance));
    CHECK(Same(bed.table.Record(last).prevWorld, At(0.0f)));

    // A tint alone changes: written, its previous world already equal to its world.
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(2.0f), Color{1, 1, 1, 1});
    }
    (void)bed.End(); // the catch-up
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(2.0f), i == 7 ? Color{1, 0, 0, 1} : Color{1, 1, 1, 1});
    }
    CHECK(bed.End().written == 1u);
    bed.Begin();
    for (u32 i = 0; i < count; ++i)
    {
        (void)bed.table.Write(kSceneA, Entity(i, 1), At(2.0f), i == 7 ? Color{1, 0, 0, 1} : Color{1, 1, 1, 1});
    }
    CHECK(bed.End().written == 0u); // nothing to catch up after a tint
}

TEST_CASE("instance table: an item with no entity has a slot for its frame alone")
{
    Bed bed;
    bed.Begin();
    const u32 slot = bed.table.Write(kSceneA, 0, At(5.0f), Color{1, 1, 1, 1});
    CHECK(Same(bed.table.Record(slot).prevWorld, At(5.0f)));
    (void)bed.End();
    CHECK(bed.table.LiveSlots() == 1u);

    bed.Begin(); // the next frame frees it before anything is written
    CHECK(bed.table.LiveSlots() == 0u);
    CHECK(bed.table.Write(kSceneA, 0, At(6.0f), Color{1, 1, 1, 1}) == slot);
    (void)bed.End();
}

TEST_CASE("instance table: the mesh renderer's frame gives each mesh its record, written only when it moves")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    {
        rhi::null::NullDevice device(DefaultAllocator());
        shaders::ShaderSystem shaderSystem(*compiler, device);
        materials::PipelineStateCache psoCache(shaderSystem, device);
        materials::MaterialSystem materialSystem;
        REQUIRE(materialSystem.Initialize(device).IsOk());
        MeshRenderer renderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
        REQUIRE(renderer.Initialize().IsOk());
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());

        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        RefPtr<materials::Material> material = materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
        // One frame of a scene of three meshes, the first at `x`; returns the first's slot.
        auto frame = [&](u32 index, f32 x, Array<u32>& slots)
        {
            ExtractedScene scene{DefaultAllocator()};
            for (u32 i = 0; i < 3; ++i)
            {
                MeshRenderData* md = scene.Add<MeshRenderData>();
                REQUIRE(md != nullptr);
                md->mesh = cube.Get();
                md->material = material.Get();
                md->rendererId = renderer.RendererId();
                md->category = RenderCategories::Opaque;
                md->scene = kSceneA;
                md->entityId = Entity(i, 1);
                md->world = At(i == 0 ? x : static_cast<f32>(i));
            }
            renderer.PrepareFrame(3, index % 2);
            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            const ExtractedScene* one = &scene;
            renderer.UploadSkinning(Span<const ExtractedScene* const>(&one, 1u), *encoder);
            slots.Clear();
            for (RenderData* data : scene.Items())
            {
                slots.PushBack(static_cast<MeshRenderData*>(data)->instanceSlot);
            }
            pool->DestroyEncoder(encoder);
            renderer.FinishFrame();
        };

        Array<u32> slots;
        frame(0, 0.0f, slots);
        REQUIRE(slots.Size() == 3u);
        for (u32 slot : slots)
        {
            CHECK(slot != MeshRenderData::kNoInstanceSlot);
        }
        CHECK(renderer.InstanceRecords().FrameStats().written == 3u);
        const u32 first = slots[0];

        frame(1, 0.0f, slots);
        CHECK(slots[0] == first);
        CHECK(renderer.InstanceRecords().FrameStats().written == 0u); // at rest: nothing written

        frame(2, 4.0f, slots); // the first moves
        CHECK(renderer.InstanceRecords().FrameStats().written == 1u);
        CHECK(Same(renderer.InstanceRecords().Record(first).world, At(4.0f)));
        CHECK(Same(renderer.InstanceRecords().Record(first).prevWorld, At(0.0f)));
        device.DestroyCommandPool(pool);
    }
    compiler->Destroy();
}

TEST_CASE("instance table: refreshed across the job system, every record is the one its mesh carries")
{
    // Enough meshes to refresh in parallel and to stage in pieces at once.
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    InitGlobalJobSystem(4);
    {
        rhi::null::NullDevice device(DefaultAllocator());
        shaders::ShaderSystem shaderSystem(*compiler, device);
        materials::PipelineStateCache psoCache(shaderSystem, device);
        materials::MaterialSystem materialSystem;
        REQUIRE(materialSystem.Initialize(device).IsOk());
        MeshRenderer renderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
        REQUIRE(renderer.Initialize().IsOk());
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        RefPtr<materials::Material> material = materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();

        constexpr u32 kMeshes = 10000;
        Array<u32> slots;
        auto frame = [&](u32 index, f32 offset)
        {
            ExtractedScene scene{DefaultAllocator()};
            for (u32 i = 0; i < kMeshes; ++i)
            {
                MeshRenderData* md = scene.Add<MeshRenderData>();
                REQUIRE(md != nullptr);
                md->mesh = cube.Get();
                md->material = material.Get();
                md->rendererId = renderer.RendererId();
                md->category = RenderCategories::Opaque;
                md->scene = kSceneA;
                md->entityId = Entity(i, 1);
                md->world = At(static_cast<f32>(i) + offset);
            }
            renderer.PrepareFrame(kMeshes, index % 2);
            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            const ExtractedScene* one = &scene;
            renderer.UploadSkinning(Span<const ExtractedScene* const>(&one, 1u), *encoder);
            slots.Clear();
            for (RenderData* data : scene.Items())
            {
                slots.PushBack(static_cast<MeshRenderData*>(data)->instanceSlot);
            }
            pool->DestroyEncoder(encoder);
            renderer.FinishFrame();
        };

        frame(0, 0.0f);
        frame(1, 0.0f); // the previous worlds catch up
        CHECK(renderer.InstanceRecords().FrameStats().written == 0u);
        frame(2, 0.5f); // every mesh moves
        const InstanceTableFrameStats moved = renderer.InstanceRecords().FrameStats();
        CHECK(moved.written == kMeshes);
        CHECK(moved.bytes == static_cast<u64>(kMeshes) * sizeof(GpuInstance));
        u32 wrong = 0;
        for (u32 i = 0; i < kMeshes; ++i)
        {
            const GpuInstance& record = renderer.InstanceRecords().Record(slots[i]);
            if (!Same(record.world, At(static_cast<f32>(i) + 0.5f)) || !Same(record.prevWorld, At(static_cast<f32>(i))))
            {
                ++wrong;
            }
        }
        CHECK(wrong == 0u);
        device.DestroyCommandPool(pool);
    }
    ShutdownGlobalJobSystem();
    compiler->Destroy();
}
