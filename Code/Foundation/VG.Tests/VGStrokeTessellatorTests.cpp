// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// StrokeTessellator: strips, caps and the miter limit.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("stroketess: solid line produces a quad strip")
{
    Float2 points[2] = {{0, 0}, {10, 0}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 2), false, StrokeStyle(2.0f),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    CHECK(vertices.Size() >= 4u);
    CHECK(indices.Size() >= 6u);
}

TEST_CASE("stroketess: closed path has no caps")
{
    Float2 points[3] = {{0, 0}, {10, 0}, {5, 10}};

    Array<VGVertex> verticesClosed;
    Array<u32> indicesClosed;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), true, StrokeStyle(2.0f),
                                  Span<const f32>{}, false, Color::White, verticesClosed,
                                  indicesClosed);

    Array<VGVertex> verticesOpen;
    Array<u32> indicesOpen;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), false,
                                  StrokeStyle(2.0f, VGLineCap::Square, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, verticesOpen,
                                  indicesOpen);

    CHECK(
        (verticesOpen.Size() > verticesClosed.Size() || indicesOpen.Size() > indicesClosed.Size()));
}

TEST_CASE("stroketess: round cap adds more vertices")
{
    Float2 points[2] = {{0, 0}, {10, 0}};

    Array<VGVertex> vertsButt;
    Array<u32> idxButt;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 2), false,
                                  StrokeStyle(4.0f, VGLineCap::Butt, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, vertsButt, idxButt);

    Array<VGVertex> vertsRound;
    Array<u32> idxRound;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 2), false,
                                  StrokeStyle(4.0f, VGLineCap::Round, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, vertsRound, idxRound);

    CHECK(vertsRound.Size() > vertsButt.Size());
}

TEST_CASE("stroketess: a huge stroke's round caps and joins have a bounded vertex count")
{
    // A width of 1e7 asked for millions of segments per arc (hostile content, a runaway value).
    Float2 points[3] = {{0, 0}, {10, 0}, {10, 10}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), false,
                                  StrokeStyle(1.0e7f, VGLineCap::Round, VGLineJoin::Round),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    CHECK(vertices.Size() > 0u);
    CHECK(vertices.Size() < 2000u);
    // A non-finite width draws nothing past the bound either.
    Array<VGVertex> nanVerts;
    Array<u32> nanIdx;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), false,
                                  StrokeStyle(Sqrt(-1.0f), VGLineCap::Round, VGLineJoin::Round),
                                  Span<const f32>{}, false, Color::White, nanVerts, nanIdx);
    CHECK(nanVerts.Size() < 2000u);
}

TEST_CASE("stroketess: polyline multiple segments")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 4), false, StrokeStyle(2.0f),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    CHECK(vertices.Size() >= 8u);
    CHECK(indices.Size() >= 18u);
}

TEST_CASE("stroketess: a hairpin's miter is bounded by the limit (no vertex escapes)")
{
    // As the corner sharpens the miter length goes to infinity; the miter limit must bevel it.
    // A 100-unit line that turns back on itself: nothing may land past 200 units.
    Float2 hairpin[3] = {{0, 0}, {100, 0}, {0, 0.5f}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(hairpin, 3), false,
                                  StrokeStyle(4.0f, VGLineCap::Butt, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    REQUIRE(!vertices.IsEmpty());
    for (const VGVertex& v : vertices)
    {
        CHECK(Abs(v.position.x) < 200.0f);
        CHECK(Abs(v.position.y) < 200.0f);
    }
}

TEST_CASE("stroketess: a zero-length edge has no direction and produces no NaN")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 0}, {20, 5}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 4), false,
                                  StrokeStyle(2.0f, VGLineCap::Round, VGLineJoin::Round),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    REQUIRE(!vertices.IsEmpty());
    for (const VGVertex& v : vertices)
    {
        CHECK(v.position.x == v.position.x); // a NaN is the one value unequal to itself
        CHECK(v.position.y == v.position.y);
    }
    for (const u32 index : indices)
    {
        CHECK(index < vertices.Size());
    }
}

TEST_CASE("stroketess: a round or bevel join stays inside the stroke's radius; a miter reaches its point")
{
    // Both sides of a corner were offset along one mitred normal, so the outside of a round or bevel
    // join ran out to the miter point, and the join drawn under it never showed.
    const Float2 corner{10.0f, 0.0f};
    const Float2 points[3] = {{0.0f, 0.0f}, corner, {10.0f, 10.0f}}; // a right angle
    const f32 width = 4.0f;
    const auto farthest = [&](VGLineJoin join, bool antiAlias)
    {
        StrokeStyle style(width, VGLineCap::Butt, join);
        Array<VGVertex> vertices;
        Array<u32> indices;
        StrokeTessellator::Tessellate(Span<const Float2>(points, 3), false, style, Span<const f32>{}, antiAlias,
                                      Color::White, vertices, indices);
        f32 reach = 0.0f;
        for (const VGVertex& v : vertices)
        {
            const Float2 d = Float2{v.position.x, v.position.y} - corner;
            if (v.position.x > corner.x - 0.5f && v.position.y < 0.5f) // the corner's outside quadrant
                reach = Max(reach, Sqrt(d.x * d.x + d.y * d.y));
        }
        return reach;
    };
    const f32 miterPoint = width * 0.5f * Sqrt(2.0f);
    CHECK(farthest(VGLineJoin::Miter, false) == doctest::Approx(miterPoint).epsilon(0.01));
    CHECK(farthest(VGLineJoin::Round, false) <= width * 0.5f + 0.01f);
    CHECK(farthest(VGLineJoin::Bevel, false) <= width * 0.5f + 0.01f);
    CHECK(farthest(VGLineJoin::Round, true) <= width * 0.5f + 1.01f); // antialiased: its fringe past it
}

TEST_CASE("stroketess: a right turn's round join is on its outside too, and covers it")
{
    const Float2 corner{10.0f, 0.0f};
    const Float2 points[3] = {{0.0f, 0.0f}, corner, {10.0f, -10.0f}}; // turning right
    StrokeStyle style(4.0f, VGLineCap::Butt, VGLineJoin::Round);
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), false, style, Span<const f32>{}, false,
                                  Color::White, vertices, indices);
    f32 reach = 0.0f;
    bool diagonal = false; // the arc has a vertex out between the segments' square ends
    for (const VGVertex& v : vertices)
    {
        const Float2 d = Float2{v.position.x, v.position.y} - corner;
        if (d.x > -0.5f && d.y > -0.5f)
        {
            const f32 r = Sqrt(d.x * d.x + d.y * d.y);
            reach = Max(reach, r);
            diagonal = diagonal || (r > 1.9f && d.x > 0.3f && d.y > 0.3f); // on the rim, between the two ends
        }
    }
    CHECK(reach <= 2.01f);
    CHECK(diagonal);
}
