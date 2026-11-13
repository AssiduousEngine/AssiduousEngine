// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The mesh GPU cache uploads a StaticMesh's vertex/index streams to RHI buffers on first use and
// reuses them after. Exercised on the Null RHI.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.geometry;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace geometry = foundation::geometry;

TEST_CASE("mesh GPU cache: uploads on first use, reuses after, frees on clear")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuMeshCache cache(device);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);

    const GpuMesh* g = cache.GetOrUpload(cube.Get());
    REQUIRE(g != nullptr);
    CHECK(g->vertexBuffer != nullptr);
    CHECK(g->indexBuffer != nullptr);
    CHECK(g->indexCount == cube->IndexCount()); // 36
    CHECK(g->indexFormat == rhi::IndexFormat::UInt32);
    CHECK(cache.Size() == 1);

    // second request for the same mesh returns the same cached entry (no re-upload)
    const GpuMesh* again = cache.GetOrUpload(cube.Get());
    CHECK(again == g);
    CHECK(cache.Size() == 1);

    // a different mesh is a distinct entry
    RefPtr<geometry::StaticMesh> sphere = geometry::Primitives::Sphere(DefaultAllocator(), 1.0f, 8, 4);
    const GpuMesh* s = cache.GetOrUpload(sphere.Get());
    REQUIRE(s != nullptr);
    CHECK(s->indexCount == sphere->IndexCount());
    CHECK(cache.Size() == 2);

    cache.Clear();
    CHECK(cache.Size() == 0);
}

TEST_CASE("mesh GPU cache: null + empty meshes upload nothing")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuMeshCache cache(device);
    CHECK(cache.GetOrUpload(nullptr) == nullptr);
    geometry::StaticMesh empty;
    CHECK(cache.GetOrUpload(&empty) == nullptr); // no vertices/indices
    CHECK(cache.Size() == 0);
}

TEST_CASE("mesh GPU cache: a mesh gone gives its streams back after the frames in flight, and the next mesh reuses them")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuMeshCache cache(device, /*framesInFlight*/ 2);
    RefPtr<geometry::StaticMesh> keep = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    REQUIRE(cache.GetOrUpload(keep.Get()) != nullptr);
    {
        RefPtr<geometry::StaticMesh> gone = geometry::Primitives::Sphere(DefaultAllocator(), 1.0f, 16, 8);
        REQUIRE(cache.GetOrUpload(gone.Get()) != nullptr);
        CHECK(cache.Size() == 2u);
    } // the sphere is released
    cache.Tick();
    CHECK(cache.Size() == 1u);
    CHECK(cache.RetiredCount() == 1u);
    CHECK(cache.FreeVertexBytes() == 0u); // frames in flight may still draw it
    cache.Tick();
    CHECK(cache.RetiredCount() == 1u); // one more frame
    cache.Tick();
    CHECK(cache.RetiredCount() == 0u);
    const u64 freed = cache.FreeVertexBytes();
    CHECK(freed > 0u);
    RefPtr<geometry::StaticMesh> next = geometry::Primitives::Cube(DefaultAllocator(), 2.0f);
    REQUIRE(cache.GetOrUpload(next.Get()) != nullptr);
    CHECK(cache.FreeVertexBytes() < freed); // placed in what the sphere left
}

TEST_CASE("mesh GPU cache: a mesh rebuilt in place uploads again")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuMeshCache cache(device);
    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    const GpuMesh* first = cache.GetOrUpload(mesh.Get());
    REQUIRE(first != nullptr);
    const u64 firstOffset = first->vertexOffset;
    CHECK(cache.GetOrUpload(mesh.Get())->vertexOffset == firstOffset); // unchanged: the same upload
    RefPtr<geometry::StaticMesh> bigger = geometry::Primitives::Sphere(DefaultAllocator(), 1.0f, 16, 8);
    mesh->vertices = bigger->vertices; // rebuilt in place
    mesh->indices = bigger->indices;
    mesh->ContentChanged();
    const GpuMesh* rebuilt = cache.GetOrUpload(mesh.Get());
    REQUIRE(rebuilt != nullptr);
    CHECK(rebuilt->indexCount == bigger->IndexCount()); // the new geometry, not the cube's
    CHECK(rebuilt->vertexOffset != firstOffset);
    CHECK(cache.RetiredCount() == 1u); // the cube's streams wait for the frames in flight
    CHECK(cache.Size() == 1u);
}
