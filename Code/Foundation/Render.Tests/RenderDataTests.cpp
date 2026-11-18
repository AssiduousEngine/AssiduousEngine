// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene-agnostic render-data core (no GPU): the frame arena, ExtractedScene snapshot,
// sort keys and radix sort, the RenderView draw-list build (cull/sort), and the view pool.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;

TEST_CASE("FrameArena: allocations are distinct, aligned, and reset reuses chunks")
{
    FrameArena arena{DefaultAllocator()};
    MeshRenderData* a = arena.New<MeshRenderData>();
    MeshRenderData* b = arena.New<MeshRenderData>();
    MeshRenderData* c = arena.New<MeshRenderData>();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(reinterpret_cast<usize>(a) % alignof(MeshRenderData) == 0);
    CHECK(reinterpret_cast<usize>(b) % alignof(MeshRenderData) == 0);

    const usize chunksAfterFirst = arena.ChunkCount();
    arena.Reset();
    for (int i = 0; i < 3; ++i)
    {
        CHECK(arena.New<MeshRenderData>() != nullptr);
    }
    CHECK(arena.ChunkCount() == chunksAfterFirst); // same load reuses chunks, no growth
}

TEST_CASE("ExtractedScene: Add registers items; Reset empties without freeing chunks")
{
    ExtractedScene scene{DefaultAllocator()};
    CHECK(scene.IsEmpty());
    for (int i = 0; i < 10; ++i)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        REQUIRE(rd != nullptr);
        rd->entityId = static_cast<u64>(i);
    }
    REQUIRE(scene.Size() == 10);
    CHECK(scene.Items()[5]->category == RenderCategories::Opaque);

    scene.Reset();
    CHECK(scene.IsEmpty());
    CHECK(scene.Size() == 0);

    // The scene a snapshot came from is stamped each frame; a reset drops it (a pooled snapshot
    // may serve another scene next).
    scene.SetSceneSerial(42);
    CHECK(scene.SceneSerial() == 42u);
    scene.Reset();
    CHECK(scene.SceneSerial() == 0u);
}

TEST_CASE("sort keys: category dominates, then state, then depth")
{
    // Category is most significant: any opaque key < any transparent key.
    const u64 opaque = MakeSortKey(RenderCategories::Opaque, 0xFFFFFF, 0xFFFFFF);
    const u64 transparent = MakeSortKey(RenderCategories::Transparent, 0, 0);
    CHECK(opaque < transparent);

    // Within a category, smaller depth sorts first (front-to-back for opaque).
    const u64 near =
        MakeSortKey(RenderCategories::Opaque, 7, QuantizeDepth(0.10f, /*invert*/ false));
    const u64 far =
        MakeSortKey(RenderCategories::Opaque, 7, QuantizeDepth(0.90f, /*invert*/ false));
    CHECK(near < far);

    // Inverted depth (transparent) reverses it (back-to-front).
    const u64 tNear =
        MakeSortKey(RenderCategories::Transparent, 0, QuantizeDepth(0.10f, /*invert*/ true));
    const u64 tFar =
        MakeSortKey(RenderCategories::Transparent, 0, QuantizeDepth(0.90f, /*invert*/ true));
    CHECK(tFar < tNear);
}

TEST_CASE("RadixSortDrawItems sorts ascending by key")
{
    Array<DrawItem> items;
    const u64 keys[] = {50, 3, 9999, 0, 42, 7, 0x00FF00FF00FF00FFull, 1, 256, 255};
    for (u64 k : keys)
    {
        items.PushBack(DrawItem{k, nullptr});
    }

    Array<DrawItem> scratch;
    RadixSortDrawItems(items, scratch);

    REQUIRE(items.Size() == 10);
    for (usize i = 1; i < items.Size(); ++i)
    {
        CHECK(items[i - 1].key <= items[i].key);
    }
    CHECK(items[0].key == 0);
    CHECK(items[items.Size() - 1].key == 0x00FF00FF00FF00FFull);
}

TEST_CASE("RenderView::BuildDrawList frustum-culls a sphere entirely outside the view, and a "
          "view whose settings opt out of culling keeps everything")
{
    ExtractedScene scene{DefaultAllocator()};
    auto add = [&](Float3 center, f32 radius, u64 tag)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->worldCenter = center;
        rd->worldRadius = radius;
        rd->entityId = tag;
        rd->category = RenderCategories::Opaque;
    };
    add(Float3{0, 0, -5}, 1.0f, 1);    // in front of the camera: kept
    add(Float3{0, 0, 50}, 1.0f, 2);    // behind it: culled
    add(Float3{500, 0, -5}, 1.0f, 3);  // far off to the side: culled
    add(Float3{0, 0, -20}, 1.0f, 4);   // farther in front: kept

    // A real perspective (the default ViewCamera's identity projection makes the frustum the
    // reverse-Z unit box, which culls everything at a distance; the sort tests above never cull).
    ViewCamera camera;
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;
    Array<DrawItem> scratch;

    RenderView culled;
    culled.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
    culled.BuildDrawList(scratch, /*cull*/ true);
    REQUIRE(culled.DrawList().Size() == 2);
    CHECK(culled.CulledCount() == 2);
    for (const DrawItem& item : culled.DrawList())
    {
        const u64 id = static_cast<const MeshRenderData*>(item.data)->entityId;
        CHECK((id == 1 || id == 4));
    }

    // Culling off (the frame's switch off, or a view's frustumCull cleared by the
    // "draw everything" override): the behind-camera sphere is in the list too.
    RenderView all;
    all.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
    all.BuildDrawList(scratch, /*cull*/ false);
    CHECK(all.DrawList().Size() == 4);
    CHECK(all.CulledCount() == 0);

    // The frame gates by both: its global switch (on by default) and the view's setting.
    rhi::null::NullDevice device{DefaultAllocator()};
    RendererRegistry registry; // no renderer needed: AddView only builds the draw list
    RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2);
    CHECK(frame.ViewCulling()); // on by default
    frame.SetViewCulling(true);
    ViewSettings optOut;
    optOut.frustumCull = false;
    RenderView* framed = frame.AddView(scene, camera, optOut, nullptr, rhi::TextureFormat::BGRA8Unorm,
                                       64, 64, nullptr, nullptr, nullptr);
    REQUIRE(framed != nullptr);
    CHECK(framed->DrawList().Size() == 4);
    RenderView* framedCulled = frame.AddView(scene, camera, settings, nullptr,
                                             rhi::TextureFormat::BGRA8Unorm, 64, 64, nullptr,
                                             nullptr, nullptr);
    REQUIRE(framedCulled != nullptr);
    CHECK(framedCulled->DrawList().Size() == 2);
}

// A scene size of its own runs the chain at it and remembers the output rectangle; the
// viewport's own size is no scaling at all, unless a crop shows a slice.
TEST_CASE("RenderView: a scene size splits the scene from the output")
{
    ExtractedScene scene{DefaultAllocator()};
    RenderView view;
    ViewSettings settings;
    settings.viewportX = 160;
    settings.viewportWidth = 960;
    settings.viewportHeight = 540;
    settings.scene = SceneSize(1280, 720);
    view.Bind(scene, ViewCamera{}, settings, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 1280, 540);
    CHECK(view.IsScaled());
    CHECK(view.Width() == 1280u); // the chain runs at the scene size
    CHECK(view.Height() == 720u);
    CHECK(view.ViewportX() == 0);
    CHECK(view.ViewportWidth() == 1280u);
    CHECK(view.ViewportHeight() == 720u);
    CHECK(view.OutputWidth() == 1280u);
    CHECK(view.OutputHeight() == 540u);
    CHECK(view.OutputViewportX() == 160);
    CHECK(view.OutputViewportWidth() == 960u);
    CHECK(view.OutputViewportHeight() == 540u);

    settings.scene = SceneSize(960, 540);
    view.Bind(scene, ViewCamera{}, settings, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 1280, 540);
    CHECK_FALSE(view.IsScaled()); // the viewport's own size is not a scale
    CHECK(view.Width() == 1280u);
    CHECK(view.ViewportX() == 160);

    settings.scene.sourceHeight = 0.75f; // the same size, but a crop's slice of it shows
    view.Bind(scene, ViewCamera{}, settings, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 1280, 540);
    CHECK(view.IsScaled());
}

TEST_CASE("SceneSize: a fit gives the scene size and the slice it shows")
{
    const SceneSize letterbox =
        SceneSize::FromFit(ContentFit{Rectangle{0, 0, 1920, 1080}, Float2{1280, 960}, FitMode::Letterbox});
    CHECK(letterbox.width == 1280u);
    CHECK(letterbox.height == 960u);
    CHECK(letterbox.sourceX == doctest::Approx(0.0f));
    CHECK(letterbox.sourceWidth == doctest::Approx(1.0f));
    CHECK(letterbox.sourceHeight == doctest::Approx(1.0f));
    CHECK_FALSE(letterbox.IsCropped());
    // 4:3 cropped to fill 16:9: the full width, the middle three quarters of the height.
    const SceneSize crop =
        SceneSize::FromFit(ContentFit{Rectangle{0, 0, 1920, 1080}, Float2{1280, 960}, FitMode::Crop});
    CHECK(crop.sourceX == doctest::Approx(0.0f));
    CHECK(crop.sourceWidth == doctest::Approx(1.0f));
    CHECK(crop.sourceHeight == doctest::Approx(0.75f));
    CHECK(crop.sourceY == doctest::Approx(0.125f));
    CHECK(crop.IsCropped());
}

TEST_CASE("RenderView::BuildDrawList sorts opaque front-to-back, transparent back-to-front")
{
    // Three meshes in front of an identity camera at view-space depths 2, 5, 8.
    ExtractedScene scene{DefaultAllocator()};
    auto add = [&](f32 z, u64 tag, RenderCategory cat)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->worldCenter = Float3{0, 0, z}; // camera looks down -z, so z<0 is in front
        rd->entityId = tag;
        rd->category = cat;
    };

    ViewCamera camera; // identity view, farZ 1000
    ViewSettings settings;
    Array<DrawItem> scratch;

    SUBCASE("opaque: nearest first")
    {
        add(-2.0f, /*tag*/ 2, RenderCategories::Opaque);
        add(-8.0f, /*tag*/ 8, RenderCategories::Opaque);
        add(-5.0f, /*tag*/ 5, RenderCategories::Opaque);

        RenderView view;
        view.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
        view.BuildDrawList(scratch);

        const Span<const DrawItem> dl = view.DrawList();
        REQUIRE(dl.Size() == 3);
        CHECK(static_cast<const MeshRenderData*>(dl[0].data)->entityId == 2); // nearest
        CHECK(static_cast<const MeshRenderData*>(dl[1].data)->entityId == 5);
        CHECK(static_cast<const MeshRenderData*>(dl[2].data)->entityId == 8); // farthest
    }

    SUBCASE("transparent: farthest first")
    {
        add(-2.0f, 2, RenderCategories::Transparent);
        add(-8.0f, 8, RenderCategories::Transparent);
        add(-5.0f, 5, RenderCategories::Transparent);

        RenderView view;
        view.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
        view.BuildDrawList(scratch);

        const Span<const DrawItem> dl = view.DrawList();
        REQUIRE(dl.Size() == 3);
        CHECK(static_cast<const MeshRenderData*>(dl[0].data)->entityId == 8); // farthest
        CHECK(static_cast<const MeshRenderData*>(dl[2].data)->entityId == 2); // nearest
    }
}

TEST_CASE("RenderViewPool: Acquire hands out stable views; Begin rewinds")
{
    RenderViewPool pool{DefaultAllocator()};
    pool.Begin();
    RenderView* a = pool.Acquire();
    RenderView* b = pool.Acquire();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    CHECK(pool.ActiveCount() == 2);

    pool.Begin();
    CHECK(pool.ActiveCount() == 0);
    RenderView* a2 = pool.Acquire();
    CHECK(a2 == a); // pooled storage reused, addresses stable
}

TEST_CASE("DynamicUniformRing: per-frame regions are disjoint; exhaustion + grow")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    const u32 framesInFlight = 3;
    DynamicUniformRing ring(device, framesInFlight, /*slotSize*/ 256);
    REQUIRE(ring.Reserve(4)); // 4 slots per frame region
    const u32 gen0 = ring.Generation();

    // Each frame's first slot lands in a distinct region (frameIndex * slotsPerFrame * 256).
    u32 firstOffset[3] = {};
    for (u32 f = 0; f < framesInFlight; ++f)
    {
        ring.BeginFrame(f);
        DynamicUniformRing::Range s = ring.Allocate();
        REQUIRE(s.ok);
        firstOffset[f] = s.byteOffset;
        CHECK(s.slotIndex == f * 4u); // absolute slot index = region base
        ring.EndFrame();
    }
    CHECK(firstOffset[0] == 0u);
    CHECK(firstOffset[1] == 4u * 256u); // region size = slotsPerFrame * slotSize
    CHECK(firstOffset[2] == 8u * 256u);

    // A region holds exactly slotsPerFrame slots; the next Allocate fails (no silent grow).
    ring.BeginFrame(0);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(ring.Allocate().ok);
    }
    CHECK_FALSE(ring.Allocate().ok);
    ring.EndFrame();

    // AllocateRange hands out contiguous runs and also fails past the region.
    ring.BeginFrame(1);
    DynamicUniformRing::Range r = ring.AllocateRange(3);
    REQUIRE(r.ok);
    CHECK(r.slotIndex == 1u * 4u);         // region 1 base
    CHECK_FALSE(ring.AllocateRange(2).ok); // only 1 slot left in the region
    ring.EndFrame();

    // Lazy mapping: a frame that allocates nothing never maps (no flush, no compare on an
    // emulated mapping); the first allocation maps; EndFrame reports the written slot count.
    ring.BeginFrame(2);
    CHECK_FALSE(ring.IsMappedThisFrame());
    CHECK(ring.FrameAllocatedSlots() == 0u);
    ring.EndFrame(); // nothing to flush
    ring.BeginFrame(2);
    CHECK(ring.AllocateRange(2).ok);
    CHECK(ring.IsMappedThisFrame());
    CHECK(ring.FrameAllocatedSlots() == 2u);
    ring.EndFrame();
    CHECK_FALSE(ring.IsMappedThisFrame());
    // Outside a frame nothing allocates (the region base is undefined).
    CHECK_FALSE(ring.Allocate().ok);
    CHECK(ring.SlotsPerFrame() == 4u);

    // Reserving more grows (new generation); a smaller reserve does not.
    REQUIRE(ring.Reserve(16));
    CHECK(ring.Generation() != gen0);
    const u32 gen1 = ring.Generation();
    REQUIRE(ring.Reserve(4));
    CHECK(ring.Generation() == gen1); // no shrink, no realloc
}

namespace
{
    // Warnings in one category, kept for a test to read.
    struct WarningSink final : ILogSink
    {
        explicit WarningSink(StringView category) : m_category(category) {}
        Array<String> lines;
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (level == LogLevel::Warning && category == m_category)
                lines.PushBack(String(message));
        }

    private:
        StringView m_category;
    };
}

TEST_CASE("DynamicUniformRing: a ring that runs out says so once, by its label")
{
    // A fixed ring that ran out dropped what did not fit without a word.
    rhi::null::NullDevice device{DefaultAllocator()};
    DynamicUniformRing ring(device, /*framesInFlight*/ 2, /*slotSize*/ 256, rhi::BufferUsage::Uniform,
                            u8"test.objects");
    REQUIRE(ring.Reserve(2));
    WarningSink sink(u8"Render");
    GlobalLogger().AddSink(&sink);
    for (u32 frame = 0; frame < 3; ++frame)
    {
        ring.BeginFrame(frame);
        CHECK(ring.Allocate().ok);
        CHECK(ring.Allocate().ok);
        CHECK_FALSE(ring.Allocate().ok); // every frame drops the third
        ring.EndFrame();
    }
    GlobalLogger().RemoveSink(&sink);
    REQUIRE(sink.lines.Size() == 1u); // the first time only
    CHECK(sink.lines[0].AsView().Find(StringView(u8"test.objects")) != StringView::kNotFound);
}

TEST_CASE("ViewUniformRing: one slot per distinct view a frame; a frame of more views grows the next")
{
    // A renderer resolves several runs per view, and a slot per run spent a ring of eight within one
    // view, after which the view's remaining runs were dropped.
    rhi::null::NullDevice device{DefaultAllocator()};
    ViewUniformRing ring(device, /*framesInFlight*/ 2, /*slotSize*/ 256, /*minViews*/ 2, u8"test.view");
    Float4x4 views[3] = {Float4x4::Identity(), Float4x4::Translation(Float3{1, 0, 0}),
                         Float4x4::Translation(Float3{2, 0, 0})};
    REQUIRE(ring.BeginFrame(0));
    CHECK(ring.SlotsPerFrame() == 2u);
    u32 first = ~0u, again = ~0u, second = ~0u;
    REQUIRE(ring.SlotFor(&views[0], sizeof(Float4x4), first));
    for (u32 run = 0; run < 20; ++run) // twenty runs of one view: one slot
    {
        REQUIRE(ring.SlotFor(&views[0], sizeof(Float4x4), again));
        CHECK(again == first);
    }
    REQUIRE(ring.SlotFor(&views[1], sizeof(Float4x4), second));
    CHECK(second != first);
    CHECK(ring.FrameSlots() == 2u);
    u32 third = 0;
    CHECK_FALSE(ring.SlotFor(&views[2], sizeof(Float4x4), third)); // a third view: none this frame
    ring.EndFrame();

    // The next frame holds the three, in a new buffer (the bind group over it is rebuilt).
    const u32 generation = ring.Generation();
    REQUIRE(ring.BeginFrame(1));
    CHECK(ring.SlotsPerFrame() == 3u);
    CHECK(ring.Generation() != generation);
    for (const Float4x4& view : views)
    {
        u32 offset = 0;
        CHECK(ring.SlotFor(&view, sizeof(Float4x4), offset));
    }
    CHECK(ring.FrameSlots() == 3u);
    ring.EndFrame();
}

TEST_CASE("GpuBufferPool sub-allocates within a chunk and grows by adding chunks")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuBufferPool pool(device, rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst, /*chunk*/ 1024,
                       u8"test");

    GpuBufferPool::Alloc a = pool.Allocate(100, 16);
    GpuBufferPool::Alloc b = pool.Allocate(100, 16);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.offset == 0u);
    CHECK(b.offset == 112u);     // 100 rounded up to the next multiple of 16
    CHECK(a.buffer == b.buffer); // same chunk
    CHECK(pool.ChunkCount() == 1u);

    // An allocation that doesn't fit the remaining chunk space opens a new chunk.
    GpuBufferPool::Alloc big = pool.Allocate(2000, 16);
    REQUIRE(big.ok);
    CHECK(big.offset == 0u);
    CHECK(big.buffer != a.buffer);
    CHECK(pool.ChunkCount() == 2u);

    pool.Clear();
    CHECK(pool.ChunkCount() == 0u);
}

TEST_CASE("RendererRegistry routes categories to renderers")
{
    struct FakeRenderer final : Renderer
    {
        Span<const RenderCategory> SupportedCategories() const override
        {
            static constexpr RenderCategory cats[] = {RenderCategories::Opaque,
                                                      RenderCategories::Masked};
            return Span<const RenderCategory>{cats, 2};
        }
        void Resolve(const RenderRecordContext&, Span<const DrawItem>,
                     Array<ResolvedDraw>&) override
        {
        }
    };

    FakeRenderer r;
    RendererRegistry registry;
    registry.Register(&r);

    CHECK(registry.ById(r.RendererId()) == &r);
    CHECK(registry.ById(999) == nullptr); // unregistered id
    CHECK(registry.Unique().Size() == 1);
}

// ---- overlay registries (the two-tier overlay coordination model) ----

namespace
{
    struct FakeSceneOverlay final : foundation::render::ISceneOverlay
    {
        i32 order = 0;
        explicit FakeSceneOverlay(i32 o) : order(o) {}
        [[nodiscard]] i32 OverlayOrder() const noexcept override { return order; }
        void Render(rhi::RenderPassEncoder&, const foundation::render::SceneOverlayView&) override {}
    };
}

TEST_CASE("overlay views: color-only by default (depthStencilFormat Undefined)")
{
    // Sources must treat Undefined as "no stencil in this pass" and fall back to
    // tessellated fills; a pass that attaches a DS advertises its exact format.
    foundation::render::SceneOverlayView sceneView;
    CHECK(sceneView.depthStencilFormat == foundation::rhi::TextureFormat::Undefined);
    foundation::render::ScreenOverlayView screenView;
    CHECK(screenView.depthStencilFormat == foundation::rhi::TextureFormat::Undefined);
}

TEST_CASE("overlay registry: sorted by order, stable ties, idempotent, removable")
{
    foundation::render::OverlayRegistry<foundation::render::ISceneOverlay> registry;
    CHECK(registry.IsEmpty());

    FakeSceneOverlay foreground{10};
    FakeSceneOverlay backgroundA{0};
    FakeSceneOverlay backgroundB{0}; // ties keep registration order
    FakeSceneOverlay middle{5};

    registry.Add(&foreground);
    registry.Add(&backgroundA);
    registry.Add(&backgroundB);
    registry.Add(&middle);
    registry.Add(&middle); // idempotent re-register
    registry.Add(nullptr); // ignored

    REQUIRE(registry.Items().Size() == 4u);
    CHECK(registry.Items()[0] == &backgroundA);
    CHECK(registry.Items()[1] == &backgroundB);
    CHECK(registry.Items()[2] == &middle);
    CHECK(registry.Items()[3] == &foreground);

    registry.Remove(&backgroundB);
    registry.Remove(&backgroundB); // no-op
    REQUIRE(registry.Items().Size() == 3u);
    CHECK(registry.Items()[0] == &backgroundA);
    CHECK(registry.Items()[1] == &middle);
    CHECK(registry.Items()[2] == &foreground);
    CHECK_FALSE(registry.Contains(&backgroundB));
    CHECK(registry.Contains(&middle));
}

TEST_CASE("LightFalloff matches the forward shader's range window and spot cone")
{
    GpuLight point;
    point.type = 1.0f;
    point.positionWS = Float3{0.0f, 2.0f, 0.0f};
    point.range = 10.0f;
    // d = 0.2 of the range: window (1 - d^4)^2 = 0.99680256, over dist^2 (+1e-4 as in the shader).
    CHECK(LightFalloff(point, Float3{0, 0, 0}) == doctest::Approx(0.99680256f / 4.0001f));
    CHECK(LightFalloff(point, Float3{0, 12.0f, 0}) == doctest::Approx(0.0f)); // at the range: none
    point.range = 0.0f; // no range: the shader returns one, no inverse square either
    CHECK(LightFalloff(point, Float3{0, 0, 0}) == doctest::Approx(1.0f));

    GpuLight spot;
    spot.type = 2.0f;
    spot.directionWS = Float3{0.0f, -1.0f, 0.0f};
    spot.innerCos = Cos(0.3f);
    spot.outerCos = Cos(0.5f);
    CHECK(LightFalloff(spot, Float3{0, -2.0f, 0}) == doctest::Approx(1.0f));              // the axis
    CHECK(LightFalloff(spot, Float3{2.0f * Tan(0.7f), -2.0f, 0}) == doctest::Approx(0.0f)); // outside

    GpuLight sun; // directional: everywhere alike
    CHECK(LightFalloff(sun, Float3{5, 5, 5}) == doctest::Approx(1.0f));
}

TEST_CASE("ShadowReach: a view drawn from behind its eye reaches as far past the eye")
{
    // The cascades start at the draw eye; an orthographic editor view pulled back 40 m must reach
    // 40 m further from there, or its shadows would end 40 m short of where they did.
    ViewCamera cam;
    cam.farZ = 1000.0f;
    CHECK(ShadowReach(cam, 120.0f) == doctest::Approx(120.0f));
    CHECK(ShadowReach(cam, 0.0f) == doctest::Approx(1.0f)); // at least a metre
    cam.pullBack = 40.0f;
    cam.farZ = 1040.0f;
    CHECK(ShadowReach(cam, 120.0f) == doctest::Approx(160.0f));
    CHECK(ShadowReach(cam, 5000.0f) == doctest::Approx(1040.0f)); // never past the far plane
}

TEST_CASE("CasterShadowReachesView: a caster counts when its shadow can fall on what the view receives")
{
    // A camera at the origin looking down -Z (60 degrees, near 0.1, far 100), shadows to 50 m, the sun
    // straight down and carried 30 m. The receivers are the view's frustum cut at 50 m.
    ViewCamera cam;
    cam.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    const BoundingFrustum receivers = ShadowReceiverFrustum(cam, 50.0f);
    const Float3 down{0.0f, -1.0f, 0.0f};
    const f32 reach = 30.0f;

    CHECK(CasterShadowReachesView(receivers, down, reach, Float4{0, 0, -10, 1})); // in view
    // Above the view, out of it, but its shadow falls through it: kept (the view culling a caster
    // list must never do, the reason shadows take their casters from the scene).
    CHECK(CasterShadowReachesView(receivers, down, reach, Float4{0, 20, -10, 1}));
    // The same caster higher than the light carries a shadow: nothing of it can reach the view.
    CHECK_FALSE(CasterShadowReachesView(receivers, down, reach, Float4{0, 40, -10, 1}));
    // Just within reach once its diameter counts: the shadow pass clamps a caster's depth, so the
    // part sticking out toward the light still casts (30 m from its centre alone would fall short).
    CHECK(CasterShadowReachesView(receivers, down, reach, Float4{0, 37, -10, 1}));
    // Behind the camera, its shadow falling straight down behind it too.
    CHECK_FALSE(CasterShadowReachesView(receivers, down, reach, Float4{0, 0, 20, 1}));
    // In front but past the shadow distance (inside the camera's far plane): no shadow is drawn there.
    CHECK_FALSE(CasterShadowReachesView(receivers, down, reach, Float4{0, 0, -80, 1}));
    // Its radius counts: the same caster just beyond the cut, but large enough to reach back into it.
    CHECK(CasterShadowReachesView(receivers, down, reach, Float4{0, 0, -55, 6}));

    // The view turned to the sky from 2 m up: the forest below it shadows nothing it sees.
    ViewCamera sky;
    sky.view = Float4x4::LookAtRH(Float3{0, 2, 0}, Float3{0, 3, 0}, Float3{0, 0, -1});
    sky.projection = cam.projection;
    const BoundingFrustum skyward = ShadowReceiverFrustum(sky, 50.0f);
    for (f32 x = -40.0f; x <= 40.0f; x += 10.0f)
    {
        for (f32 z = -40.0f; z <= 40.0f; z += 10.0f)
        {
            CHECK_FALSE(CasterShadowReachesView(skyward, down, reach, Float4{x, 0.5f, z, 1.0f}));
        }
    }
    // A caster up in the view (a branch overhead) still casts.
    CHECK(CasterShadowReachesView(skyward, down, reach, Float4{0, 10, 0, 1}));
}

TEST_CASE("ComputeCascades: casterReach is the deepest cascade's depth, as far as any shadow is carried")
{
    ViewCamera cam;
    cam.view = Float4x4::LookAtRH(Float3{0, 2, 0}, Float3{0, 2, -1}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 16.0f / 9.0f, 0.1f, 250.0f);
    const u32 resolution = 1024;
    const ShadowCascades cascades =
        ComputeCascades(cam, Normalized(Float3{0.3f, -1.0f, 0.2f}), 200.0f, resolution);
    // Each cascade's box is six radii deep along the light, its radius half its width in texels.
    f32 deepest = 0.0f;
    for (u32 c = 0; c < ShadowCascades::kCount; ++c)
    {
        deepest = Max(deepest, cascades.texelWorldSize[c] * static_cast<f32>(resolution) * 0.5f * 6.0f);
    }
    CHECK(cascades.casterReach > 0.0f);
    CHECK(cascades.casterReach == doctest::Approx(deepest));
}

TEST_CASE("ComputeCascades: fitted to the scene, a cascade shrinks to what of its slice holds anything")
{
    // A view 2 m up looking down -Z over a scene 30 m tall that ends partway along it.
    ViewCamera cam;
    cam.view = Float4x4::LookAtRH(Float3{0, 2, 0}, Float3{0, 2, -1}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 250.0f);
    const Float3 light = Normalized(Float3{0.3f, -1.0f, 0.2f});
    const u32 resolution = 1024;
    const ShadowCascades slices = ComputeCascades(cam, light, 200.0f, resolution);
    // The scene ends just before the last cascade's slice begins.
    const AABB scene{Float3{-50.0f, -13.0f, -(slices.splitFar[2] - 1.0f)}, Float3{50.0f, 17.0f, 10.0f}};
    const ShadowCascades fitted = ComputeCascades(cam, light, 200.0f, resolution, 0.5f, &scene);

    // The near cascade's slice lies wholly in the scene: unchanged.
    CHECK(fitted.hasReceivers[0]);
    CHECK(fitted.texelWorldSize[0] == doctest::Approx(slices.texelWorldSize[0]));
    // The last slice is past the scene: nothing to shadow there.
    CHECK_FALSE(fitted.hasReceivers[3]);
    // The third slice spans far more height than the scene's 30 m: it shrinks to the overlap, its
    // texels finer, and still holds what of the scene is in it.
    CHECK(fitted.hasReceivers[2]);
    CHECK(fitted.texelWorldSize[2] < slices.texelWorldSize[2]);
    const f32 mid = (slices.splitFar[1] + slices.splitFar[2]) * 0.5f;
    const BoundingFrustum box{fitted.viewProj[2]};
    const Float3 inScene[] = {Float3{0, 0, -mid}, Float3{-10, 15, -mid}, Float3{10, -12, -mid}};
    for (const Float3& p : inScene)
    {
        CHECK(box.Contains(p) != ContainmentType::Disjoint);
    }
    // A fitted box reaches no deeper than the slice's would: as far toward the light, less beyond.
    CHECK(fitted.casterReach > 0.0f);
    CHECK(fitted.casterReach <= slices.casterReach + 1e-3f);

    // No bounds (a scene that draws nothing) fits nothing: the boxes stay the slices'.
    const AABB nothing = AABB::Empty();
    const ShadowCascades none = ComputeCascades(cam, light, 200.0f, resolution, 0.5f, &nothing);
    for (u32 c = 0; c < ShadowCascades::kCount; ++c)
    {
        CHECK(none.hasReceivers[c]);
        CHECK(none.texelWorldSize[c] == doctest::Approx(slices.texelWorldSize[c]));
    }
}

TEST_CASE("CascadeReceiverDepths: a cascade shades its slice and the blend band before it")
{
    ShadowCascades cascades;
    cascades.splitFar[0] = 10.0f;
    cascades.splitFar[1] = 30.0f;
    cascades.splitFar[2] = 60.0f;
    cascades.splitFar[3] = 100.0f;
    f32 nearDepth = 0.0f;
    f32 farDepth = 0.0f;
    CascadeReceiverDepths(cascades, 0.1f, 0, nearDepth, farDepth);
    CHECK(nearDepth == doctest::Approx(0.1f)); // the first starts at the view's near plane
    CHECK(farDepth == doctest::Approx(10.0f));
    // The second also shades the last 15% of the first, where SampleCSM blends into it.
    CascadeReceiverDepths(cascades, 0.1f, 1, nearDepth, farDepth);
    CHECK(nearDepth == doctest::Approx(10.0f - 10.0f * kCascadeBlendBand));
    CHECK(farDepth == doctest::Approx(30.0f));
    CascadeReceiverDepths(cascades, 0.1f, 3, nearDepth, farDepth);
    CHECK(nearDepth == doctest::Approx(60.0f - 30.0f * kCascadeBlendBand));
    CHECK(farDepth == doctest::Approx(100.0f));

    // A slice of the view: a point between its depths is inside, one nearer or further is not.
    ViewCamera cam;
    cam.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 250.0f);
    const BoundingFrustum slice = ShadowSliceFrustum(cam, 30.0f, 60.0f);
    CHECK(slice.Contains(Float3{0, 0, -45}) != ContainmentType::Disjoint);
    CHECK(slice.Contains(Float3{0, 0, -20}) == ContainmentType::Disjoint);
    CHECK(slice.Contains(Float3{0, 0, -70}) == ContainmentType::Disjoint);
}

TEST_CASE("ShadowSweepLength: a shadow runs along the light no further than the scene's bounds")
{
    // A forest 200 m across, from the ground (0) to the treetops (20 m).
    const AABB scene{Float3{-100.0f, 0.0f, -100.0f}, Float3{100.0f, 20.0f, 100.0f}};
    const f32 reach = 480.0f; // what the cascades alone would allow
    // Straight down from 10 m up (radius 1): to the ground, and its own radius past it.
    CHECK(ShadowSweepLength(Float4{0, 10, 0, 1}, Float3{0, -1, 0}, reach, scene) == doctest::Approx(11.0f));
    // Sideways from the middle: to the far side of the scene, not the cascades' whole reach.
    CHECK(ShadowSweepLength(Float4{0, 10, 0, 1}, Float3{1, 0, 0}, reach, scene) == doctest::Approx(101.0f));
    // A low sun: whichever bound the sweep meets first.
    const Float3 low = Normalized(Float3{1.0f, -0.5f, 0.0f});
    CHECK(ShadowSweepLength(Float4{0, 10, 0, 1}, low, reach, scene) == doctest::Approx(11.0f / 0.5f * Length(Float3{1.0f, -0.5f, 0.0f})).epsilon(0.001));
    // Already past the scene along the light: nothing left for its shadow to land on.
    CHECK(ShadowSweepLength(Float4{0, -50, 0, 1}, Float3{0, -1, 0}, reach, scene) == doctest::Approx(0.0f));
    // A scene with no bounds leaves the cascades' reach.
    CHECK(ShadowSweepLength(Float4{0, 10, 0, 1}, Float3{0, -1, 0}, reach, AABB::Empty()) == doctest::Approx(reach));
}

TEST_CASE("ExtractedScene: past the per-view budget, the lights that matter to the view are kept")
{
    // The forward and the cluster build shade the first kMaxLightsPerView lights in extraction
    // order: with one more, a bright light beside the camera extracted last was the one cut.
    ExtractedScene scene{DefaultAllocator()};
    scene.SetViewOrigin(Float3{0, 0, 0});
    for (u32 i = 0; i < kMaxLightsPerView; ++i)
    {
        GpuLight dim;
        dim.type = 1.0f; // point
        dim.positionWS = Float3{500.0f + static_cast<f32>(i), 0, 0};
        dim.range = 5.0f;
        dim.intensity = 0.5f;
        scene.AddLight(dim);
    }
    GpuLight sun;
    sun.type = 0.0f;
    sun.intensity = 0.1f;
    scene.AddLight(sun);
    GpuLight lamp;
    lamp.type = 2.0f; // spot, beside the view
    lamp.positionWS = Float3{1, 2, 0};
    lamp.range = 10.0f;
    lamp.intensity = 20.0f;
    lamp.shadowIndex = 3.0f;
    scene.AddLight(lamp);
    REQUIRE(scene.Lights().Size() == kMaxLightsPerView + 2);

    scene.RankLights();
    const Span<const GpuLight> lights = scene.Lights();
    REQUIRE(lights.Size() == kMaxLightsPerView + 2); // ranked, not cut: the views cut
    CHECK(lights[0].type == 0.0f);                    // the sun first
    CHECK(lights[1].type == 2.0f);                    // then the lamp, with its own shadow index
    CHECK(lights[1].shadowIndex == 3.0f);
    // The dim ones keep their extraction order among themselves (the sort is stable).
    CHECK(lights[2].positionWS.x == 500.0f);
    CHECK(lights[kMaxLightsPerView + 1].positionWS.x == 500.0f + static_cast<f32>(kMaxLightsPerView - 1));
}

TEST_CASE("ExtractedScene: within the budget, lights keep extraction order")
{
    ExtractedScene scene{DefaultAllocator()};
    scene.SetViewOrigin(Float3{0, 0, 0});
    GpuLight far;
    far.type = 1.0f;
    far.positionWS = Float3{100, 0, 0};
    GpuLight near = far;
    near.positionWS = Float3{1, 0, 0};
    scene.AddLight(far);
    scene.AddLight(near);
    scene.RankLights();
    CHECK(scene.Lights()[0].positionWS.x == 100.0f);
}
