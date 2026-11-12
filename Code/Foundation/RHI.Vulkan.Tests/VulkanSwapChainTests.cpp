// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vulkan backend: the swapchain's surface format and its alternate view, from what a surface
// offers. A desktop's surface offers BGRA formats, an Android phone's RGBA only; asked for BGRA
// sRGB, a phone must still get an sRGB format, and its UNORM view to match. Pure; no device.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "RHI.Vulkan/VkIncludes.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;

using namespace foundation::core;
using namespace foundation::rhi;

namespace
{
    constexpr VkColorSpaceKHR kSrgbSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

TEST_CASE("vk.swapchain: the format asked for is taken when the surface offers it")
{
    const VkSurfaceFormatKHR offered[] = {{VK_FORMAT_B8G8R8A8_UNORM, kSrgbSpace}, {VK_FORMAT_B8G8R8A8_SRGB, kSrgbSpace}};
    const VkSurfaceFormatKHR chosen = vk::ChooseSurfaceFormat(Span<const VkSurfaceFormatKHR>(offered, 2), VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(chosen.format == VK_FORMAT_B8G8R8A8_SRGB);
}

TEST_CASE("vk.swapchain: a surface with RGBA formats only gives an sRGB one for a BGRA sRGB request")
{
    // A surface like an Android phone's: RGBA formats, UNORM first (the Pixel 6 lists R8G8B8A8_UNORM first).
    const VkSurfaceFormatKHR offered[] = {{VK_FORMAT_R8G8B8A8_UNORM, kSrgbSpace},
                                          {VK_FORMAT_R8G8B8A8_SRGB, kSrgbSpace},
                                          {VK_FORMAT_R5G6B5_UNORM_PACK16, kSrgbSpace}};
    const VkSurfaceFormatKHR chosen = vk::ChooseSurfaceFormat(Span<const VkSurfaceFormatKHR>(offered, 3), VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(chosen.format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(chosen.colorSpace == kSrgbSpace);
}

TEST_CASE("vk.swapchain: with no sRGB format offered, an 8-bit UNORM one, else the first")
{
    const VkSurfaceFormatKHR unorm[] = {{VK_FORMAT_R5G6B5_UNORM_PACK16, kSrgbSpace}, {VK_FORMAT_R8G8B8A8_UNORM, kSrgbSpace}};
    CHECK(vk::ChooseSurfaceFormat(Span<const VkSurfaceFormatKHR>(unorm, 2), VK_FORMAT_B8G8R8A8_SRGB).format ==
          VK_FORMAT_R8G8B8A8_UNORM);
    const VkSurfaceFormatKHR other[] = {{VK_FORMAT_A2B10G10R10_UNORM_PACK32, kSrgbSpace}};
    CHECK(vk::ChooseSurfaceFormat(Span<const VkSurfaceFormatKHR>(other, 1), VK_FORMAT_B8G8R8A8_SRGB).format ==
          VK_FORMAT_A2B10G10R10_UNORM_PACK32);
}

TEST_CASE("vk.swapchain: the alternate view follows the format negotiated")
{
    // Asked as the requested format's twin: the twin of what the surface gave.
    CHECK(vk::AlternateViewFor(TextureFormat::BGRA8UnormSrgb, TextureFormat::BGRA8Unorm, TextureFormat::BGRA8UnormSrgb) ==
          TextureFormat::BGRA8Unorm);
    CHECK(vk::AlternateViewFor(TextureFormat::BGRA8UnormSrgb, TextureFormat::BGRA8Unorm, TextureFormat::RGBA8UnormSrgb) ==
          TextureFormat::RGBA8Unorm);
    // Asked for the negotiated format's own twin: honoured as asked.
    CHECK(vk::AlternateViewFor(TextureFormat::BGRA8UnormSrgb, TextureFormat::RGBA8Unorm, TextureFormat::RGBA8UnormSrgb) ==
          TextureFormat::RGBA8Unorm);
    // None asked, or a negotiated format with no twin: none.
    CHECK(vk::AlternateViewFor(TextureFormat::BGRA8UnormSrgb, TextureFormat::Undefined, TextureFormat::BGRA8UnormSrgb) ==
          TextureFormat::Undefined);
    CHECK(vk::AlternateViewFor(TextureFormat::BGRA8UnormSrgb, TextureFormat::BGRA8Unorm, TextureFormat::RGB10A2Unorm) ==
          TextureFormat::Undefined);
}
