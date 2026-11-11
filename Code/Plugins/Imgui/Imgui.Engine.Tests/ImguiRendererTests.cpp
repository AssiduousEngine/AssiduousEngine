// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The geometry ImGui draws with as a screen overlay: its window-pixel vertices and clip rectangles
// taken onto a target the window is drawn into turned (a pre-rotated swapchain).

#include "Core/Prelude.h"
#include <doctest/doctest.h>

import foundation.core;
import foundation.render.api;
import imgui.engine;

using namespace foundation::core;
using foundation::render::ScreenOverlayView;

namespace
{
    // Where a window point lands in clip space under `projection` (row vectors).
    Float2 Clip(Float2 point, const Float4x4& projection)
    {
        const Float4 p = Float4{point.x, point.y, 0.0f, 1.0f} * projection;
        return Float2{p.x / p.w, p.y / p.w};
    }

    ScreenOverlayView View(u32 width, u32 height, ContentRotation turn)
    {
        ScreenOverlayView view;
        view.width = width;
        view.height = height;
        view.preRotation = turn;
        return view;
    }
}

TEST_CASE("Imgui projection: an unturned window spans the target, y down")
{
    const ScreenOverlayView view = View(800, 400, ContentRotation::None);
    const Float4x4 proj = imgui::ImguiRenderer::Projection(Float2{800.0f, 400.0f}, view.WindowToTargetTransform());
    const Float2 topLeft = Clip(Float2{0.0f, 0.0f}, proj);
    const Float2 bottomRight = Clip(Float2{800.0f, 400.0f}, proj);
    CHECK(topLeft.x == doctest::Approx(-1.0f));
    CHECK(topLeft.y == doctest::Approx(1.0f));
    CHECK(bottomRight.x == doctest::Approx(1.0f));
    CHECK(bottomRight.y == doctest::Approx(-1.0f));
}

TEST_CASE("Imgui projection: a quarter turn takes the window's top-left to the target's top-right")
{
    // A 400 x 800 window drawn turned into an 800 x 400 target.
    const ScreenOverlayView view = View(800, 400, ContentRotation::Quarter);
    const Float4x4 proj = imgui::ImguiRenderer::Projection(Float2{800.0f, 400.0f}, view.WindowToTargetTransform());
    const Float2 topLeft = Clip(Float2{0.0f, 0.0f}, proj);
    CHECK(topLeft.x == doctest::Approx(1.0f));
    CHECK(topLeft.y == doctest::Approx(1.0f));
    const Float2 topRight = Clip(Float2{400.0f, 0.0f}, proj);
    CHECK(topRight.x == doctest::Approx(1.0f));
    CHECK(topRight.y == doctest::Approx(-1.0f));
    const Float2 bottomLeft = Clip(Float2{0.0f, 800.0f}, proj);
    CHECK(bottomLeft.x == doctest::Approx(-1.0f));
    CHECK(bottomLeft.y == doctest::Approx(1.0f));
}

TEST_CASE("Imgui projection: a half turn takes the window's top-left to the target's bottom-right")
{
    const ScreenOverlayView view = View(800, 400, ContentRotation::Half);
    const Float4x4 proj = imgui::ImguiRenderer::Projection(Float2{800.0f, 400.0f}, view.WindowToTargetTransform());
    const Float2 topLeft = Clip(Float2{0.0f, 0.0f}, proj);
    CHECK(topLeft.x == doctest::Approx(1.0f));
    CHECK(topLeft.y == doctest::Approx(-1.0f));
}

TEST_CASE("Imgui scissor: unturned, a clip rectangle is kept inside the target")
{
    u32 x = 0, y = 0, w = 0, h = 0;
    REQUIRE(imgui::ImguiRenderer::Scissor(Rectangle{-10.0f, 20.0f, 100.0f, 500.0f}, ContentRotation::None,
                                          Float2{800.0f, 400.0f}, 800, 400, x, y, w, h));
    CHECK(x == 0);
    CHECK(y == 20);
    CHECK(w == 90);
    CHECK(h == 380);
}

TEST_CASE("Imgui scissor: a quarter turn turns the clip rectangle with the window")
{
    // The window's top-left 100 x 50 lands on the target's right edge, 50 wide and 100 tall.
    u32 x = 0, y = 0, w = 0, h = 0;
    REQUIRE(imgui::ImguiRenderer::Scissor(Rectangle{0.0f, 0.0f, 100.0f, 50.0f}, ContentRotation::Quarter,
                                          Float2{400.0f, 800.0f}, 800, 400, x, y, w, h));
    CHECK(x == 750);
    CHECK(y == 0);
    CHECK(w == 50);
    CHECK(h == 100);
}

TEST_CASE("Imgui scissor: a clip rectangle wholly off the target draws nothing")
{
    u32 x = 0, y = 0, w = 0, h = 0;
    CHECK_FALSE(imgui::ImguiRenderer::Scissor(Rectangle{900.0f, 0.0f, 50.0f, 50.0f}, ContentRotation::None,
                                              Float2{800.0f, 400.0f}, 800, 400, x, y, w, h));
    CHECK_FALSE(imgui::ImguiRenderer::Scissor(Rectangle{10.0f, 10.0f, 0.0f, 50.0f}, ContentRotation::None,
                                              Float2{800.0f, 400.0f}, 800, 400, x, y, w, h));
}
