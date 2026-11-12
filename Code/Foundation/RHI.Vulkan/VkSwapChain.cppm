// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of SwapChain.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:swap_chain;

import foundation.core;
import foundation.rhi;
import :conversions;
import :adapter;
import :surface;
import :texture;
import :texture_view;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    // Minimal reverse format mapping for swap chain format negotiation.
    inline TextureFormat fromVkFormat(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return TextureFormat::RGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return TextureFormat::RGBA8UnormSrgb;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return TextureFormat::BGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return TextureFormat::BGRA8UnormSrgb;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return TextureFormat::RGBA16Float;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return TextureFormat::RGB10A2Unorm;
        default:
            return TextureFormat::Undefined;
        }
    }

    // Readable names for the swapchain-format negotiation log, so a machine that falls back to a
    // non-sRGB format (a washed-out picture) is diagnosed from one line. const char* for the
    // printf-style RHI logger (LogInfof/LogWarningf).
    inline const char* vkFormatName(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:
            return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB:
            return "B8G8R8A8_SRGB";
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return "A2B10G10R10_UNORM_PACK32";
        default:
            return "(other)";
        }
    }

    inline const char* vkColorSpaceName(VkColorSpaceKHR cs)
    {
        switch (cs)
        {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
            return "SRGB_NONLINEAR";
        default:
            return "(other)";
        }
    }

    inline const char* vkPresentModeName(VkPresentModeKHR mode)
    {
        switch (mode)
        {
        case VK_PRESENT_MODE_FIFO_KHR:
            return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return "FIFO_RELAXED";
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return "IMMEDIATE";
        default:
            return "(other)";
        }
    }

    class VkDeviceImpl; // forward

    class VkSwapChainImpl : public SwapChain
    {
    public:
        Status init(VkDevice device, VkPhysicalDevice physDevice, VkSurfaceKHR surface,
                    const SwapChainDesc& desc, VkDeviceImpl* owner, IAllocator& allocator,
                    bool mutableFormat)
        {
            m_device = device;
            m_physDevice = physDevice;
            m_surface = surface;
            m_owner = owner;
            m_allocator = &allocator;
            m_presentMode = desc.presentMode;
            m_mutableFormat = mutableFormat;
            m_requestedAlternate = desc.alternateViewFormat;
            m_requestedFormat = desc.format;
            m_preRotate = desc.preRotate;
            return CreateSwapChain(desc.width, desc.height, desc.format, desc.bufferCount,
                                   VK_NULL_HANDLE);
        }

        // ---- SwapChain interface ----
        TextureFormat Format() const override { return m_format; }
        u32 Width() const override { return m_width; }
        u32 Height() const override { return m_height; }
        u32 BufferCount() const override { return m_bufferCount; }
        u32 CurrentImageIndex() const override { return m_currentImageIndex; }

        Status AcquireNextImage() override;
        Texture* CurrentTexture() override
        {
            return m_currentImageIndex < m_textures.Size() ? m_textures[m_currentImageIndex]
                                                           : nullptr;
        }
        TextureView* CurrentTextureView() override
        {
            return m_currentImageIndex < m_views.Size() ? m_views[m_currentImageIndex] : nullptr;
        }
        TextureFormat AlternateViewFormat() const override { return m_alternate; }
        ContentRotation PreRotation() const override { return m_rotation; }
        TextureView* CurrentAlternateTextureView() override
        {
            return m_currentImageIndex < m_alternateViews.Size() ? m_alternateViews[m_currentImageIndex]
                                                                 : nullptr;
        }
        Status Present(Queue* queue) override;
        Status Resize(u32 w, u32 h) override;

        void cleanup();

        // ---- Internal ----
        [[nodiscard]] VkSwapchainKHR handle() const { return m_swapchain; }

        // Semaphore accessors for queue submit integration.
        VkSemaphore currentAcquireSemaphore() const { return m_acquireSems[m_frameIndex]; }
        VkSemaphore currentPresentSemaphore() const { return m_presentSems[m_currentImageIndex]; }

    private:
        Status CreateSwapChain(u32 w, u32 h, TextureFormat reqFormat, u32 reqCount,
                               VkSwapchainKHR old);
        Status retrieveImages(VkFormat format);
        void createSyncObjects();
        void cleanupImages();
        void destroySyncObjects();

        VkSurfaceFormatKHR chooseSurfaceFormat(TextureFormat requested);
        VkPresentModeKHR choosePresentMode(PresentMode requested);

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkDeviceImpl* m_owner = nullptr;
        IAllocator* m_allocator = nullptr;

        TextureFormat m_format = TextureFormat::Undefined;
        PresentMode m_presentMode = PresentMode::Fifo;
        u32 m_width = 0, m_height = 0, m_bufferCount = 0;
        u32 m_currentImageIndex = 0;
        u32 m_frameIndex = 0;

        Array<VkTextureImpl*> m_textures;
        Array<VkTextureViewImpl*> m_views;
        Array<VkTextureViewImpl*> m_alternateViews; // in m_alternate, when honoured
        bool m_mutableFormat = false;                // VK_KHR_swapchain_mutable_format enabled
        TextureFormat m_requestedAlternate = TextureFormat::Undefined;
        // The format first asked for: a rebuild asks it again, so it negotiates as the first did and
        // the alternate view asked as its twin follows (the format negotiated may be another).
        TextureFormat m_requestedFormat = TextureFormat::Undefined;
        TextureFormat m_alternate = TextureFormat::Undefined;
        Array<VkSemaphore> m_acquireSems;
        Array<VkSemaphore> m_presentSems;
        // Pre-rotation (SwapChainDesc::preRotate): the turn the images are drawn with, the surface
        // transform the swapchain was made for, the window size it was asked for (to make it again
        // when only the transform changes), and whether a present said the surface may have turned.
        bool m_preRotate = false;
        ContentRotation m_rotation = ContentRotation::None;
        VkSurfaceTransformFlagBitsKHR m_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        u32 m_askedWidth = 0, m_askedHeight = 0;
        bool m_checkTransform = false;
    };

    // The clockwise turn a surface transform asks of the picture drawn for it; a mirrored one is not
    // drawn turned (the display turns it).
    [[nodiscard]] inline ContentRotation RotationOf(VkSurfaceTransformFlagBitsKHR transform) noexcept
    {
        switch (transform)
        {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            return ContentRotation::Quarter;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            return ContentRotation::Half;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            return ContentRotation::ThreeQuarter;
        default:
            return ContentRotation::None;
        }
    }

    // The surface format a swapchain asked for `desired` takes from what the surface offers: the
    // format itself; else an 8-bit sRGB format in the sRGB colour space, in either channel order (a
    // desktop's surface offers BGRA, an Android phone's RGBA only); else an 8-bit UNORM one; else
    // the first offered.
    [[nodiscard]] inline VkSurfaceFormatKHR ChooseSurfaceFormat(Span<const VkSurfaceFormatKHR> offered,
                                                                VkFormat desired) noexcept
    {
        if (offered.IsEmpty())
        {
            return VkSurfaceFormatKHR{desired, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        }
        for (const VkSurfaceFormatKHR& f : offered)
        {
            if (f.format == desired)
            {
                return f;
            }
        }
        const VkFormat srgb[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
        for (const VkFormat wanted : srgb)
        {
            for (const VkSurfaceFormatKHR& f : offered)
            {
                if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    return f;
                }
            }
        }
        const VkFormat unorm[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        for (const VkFormat wanted : unorm)
        {
            for (const VkSurfaceFormatKHR& f : offered)
            {
                if (f.format == wanted)
                {
                    return f;
                }
            }
        }
        return offered[0];
    }

    // The alternate view a swapchain of `negotiated` images honours, asked for `requestedAlternate`
    // with `requested` images: the one asked when it is the negotiated format's sRGB or UNORM twin;
    // asked as the twin of the format requested, the twin of the one negotiated (the surface had
    // RGBA where BGRA was asked); else none.
    [[nodiscard]] inline TextureFormat AlternateViewFor(TextureFormat requested, TextureFormat requestedAlternate,
                                                        TextureFormat negotiated) noexcept
    {
        if (requestedAlternate == TextureFormat::Undefined)
        {
            return TextureFormat::Undefined;
        }
        if (requestedAlternate == SrgbTwin(negotiated))
        {
            return requestedAlternate;
        }
        if (requestedAlternate == SrgbTwin(requested))
        {
            return SrgbTwin(negotiated);
        }
        return TextureFormat::Undefined;
    }

    // ---- Implementation (inline in module) ----

    inline Status VkSwapChainImpl::CreateSwapChain(u32 w, u32 h, TextureFormat reqFormat,
                                                   u32 reqCount, VkSwapchainKHR old)
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &caps);
        m_askedWidth = w;
        m_askedHeight = h;
        m_checkTransform = false;

        // The picture is drawn as the window stands and the display turns it, unless pre-rotation
        // was asked: then the images are made in the display's orientation, the surface's current
        // transform their pre-transform, and the caller draws the window's picture turned into them
        // (PreRotation), so the display has no turning to do.
        m_transform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
        m_rotation = ContentRotation::None;
        if (m_preRotate && RotationOf(caps.currentTransform) != ContentRotation::None &&
            (caps.supportedTransforms & caps.currentTransform) != 0)
        {
            m_transform = caps.currentTransform;
            m_rotation = RotationOf(caps.currentTransform);
        }

        // The size asked (the window's pixels), within what the surface allows. Most surfaces allow
        // only their current extent, so it decides; Android allows a range and scales the images to
        // the window, and its current extent lags a turn of the phone: taken as reported, a window
        // just turned upright gets landscape images, stretched. Both are the window's way round
        // (Android reports its extent turned with the phone), so a quarter of pre-rotation swaps
        // them for the images.
        const bool asked = w != 0 && h != 0;
        u32 wantW = asked || caps.currentExtent.width == ~0u ? w : caps.currentExtent.width;
        u32 wantH = asked || caps.currentExtent.width == ~0u ? h : caps.currentExtent.height;
        if (TurnSwapsSides(m_rotation))
        {
            const u32 swap = wantW;
            wantW = wantH;
            wantH = swap;
        }
        if (!asked && caps.currentExtent.width != ~0u)
        {
            m_width = wantW;
            m_height = wantH;
        }
        else
        {
            m_width = Clamp(wantW, caps.minImageExtent.width, caps.maxImageExtent.width);
            m_height = Clamp(wantH, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (m_width == 0 || m_height == 0)
            return ErrorCode::Unknown;

        auto presentMode = choosePresentMode(m_presentMode);

        // Mailbox needs a 3rd image to race ahead of the display without stalling; 2 is fine for
        // FIFO/Immediate.
        u32 wantCount = reqCount;
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR && wantCount < 3)
        {
            wantCount = 3;
        }
        m_bufferCount = Max(wantCount, caps.minImageCount);
        if (caps.maxImageCount > 0)
            m_bufferCount = Min(m_bufferCount, caps.maxImageCount);
        // How the display paces frames: under FIFO the CPU waits in the acquire for an image the
        // compositor gives back, and how many images there are bounds how far it can run ahead, so
        // a long acquire in a capture is read against these.
        if (m_preRotate)
            LogInfof("[RHI] swapchain pre-rotation: surface transform 0x%x, drawn turned %d quarter(s), images %ux%u",
                     static_cast<unsigned>(caps.currentTransform), static_cast<int>(m_rotation), m_width, m_height);
        LogInfof("[RHI] swapchain present mode: requested %s, chosen %s; %u images (the surface allows %u to %u)",
                 vkPresentModeName(toVkPresentMode(m_presentMode)), vkPresentModeName(presentMode), m_bufferCount,
                 caps.minImageCount, caps.maxImageCount);

        auto surfFmt = chooseSurfaceFormat(reqFormat);
        m_format = fromVkFormat(surfFmt.format);
        if (m_format == TextureFormat::Undefined)
            m_format = reqFormat;

        // Surface-format negotiation can wash out the picture: if a machine's surface lacks our
        // sRGB target, chooseSurfaceFormat falls back to a UNORM format while the final pass still
        // assumes encode-on-write. Log the request and the result, and warn on a non-sRGB
        // negotiation, so such a report is a one-line diagnosis.
        LogInfof("[RHI] swapchain surface format: requested %s, negotiated %s [%s]",
                 vkFormatName(toVkFormat(reqFormat)), vkFormatName(surfFmt.format),
                 vkColorSpaceName(surfFmt.colorSpace));
        if (!IsSrgb(m_format))
        {
            LogWarningf("[RHI] swapchain negotiated a NON-sRGB format (%s) - the surface offered no "
                        "sRGB target; output may look washed out where the final pass assumes "
                        "encode-on-write",
                        vkFormatName(surfFmt.format));
        }

        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VkCompositeAlphaFlagBitsKHR compositeAlpha =
            (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = m_surface;
        ci.minImageCount = m_bufferCount;
        ci.imageFormat = surfFmt.format;
        ci.imageColorSpace = surfFmt.colorSpace;
        ci.imageExtent = {m_width, m_height};
        ci.imageArrayLayers = 1;
        ci.imageUsage = usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Identity, or with pre-rotation the surface's turn: claiming a turn the picture was not
        // drawn with would show it turned twice.
        ci.preTransform = m_transform;
        ci.compositeAlpha = compositeAlpha;
        ci.presentMode = presentMode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;

        // The images viewable in their sRGB or UNORM twin too, when asked and the device can.
        m_alternate = m_mutableFormat ? AlternateViewFor(reqFormat, m_requestedAlternate, m_format)
                                      : TextureFormat::Undefined;
        if (m_requestedAlternate != TextureFormat::Undefined)
        {
            LogInfof("[RHI] swapchain alternate view format %s: %s%s", vkFormatName(toVkFormat(m_requestedAlternate)),
                     m_alternate != TextureFormat::Undefined ? "honoured as "
                     : m_mutableFormat ? "not the swapchain format's sRGB or UNORM twin"
                                       : "unavailable (no VK_KHR_swapchain_mutable_format)",
                     m_alternate != TextureFormat::Undefined ? vkFormatName(toVkFormat(m_alternate)) : "");
        }
        VkFormat viewFormats[2] = {surfFmt.format, toVkFormat(m_alternate)};
        VkImageFormatListCreateInfo formatList{};
        if (m_alternate != TextureFormat::Undefined)
        {
            formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            formatList.viewFormatCount = 2;
            formatList.pViewFormats = viewFormats;
            ci.pNext = &formatList;
            ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        }

        if (vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain) != VK_SUCCESS)
            return ErrorCode::Unknown;
        if (old != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(m_device, old, nullptr);

        if (retrieveImages(surfFmt.format) != ErrorCode::Ok)
            return ErrorCode::Unknown;
        createSyncObjects();
        m_frameIndex = 0;
        return ErrorCode::Ok;
    }

    inline VkSurfaceFormatKHR VkSwapChainImpl::chooseSurfaceFormat(TextureFormat requested)
    {
        u32 count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &count, nullptr);
        Array<VkSurfaceFormatKHR> fmts(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &count, fmts.Data());
        return ChooseSurfaceFormat(Span<const VkSurfaceFormatKHR>(fmts.Data(), count), toVkFormat(requested));
    }

    inline VkPresentModeKHR VkSwapChainImpl::choosePresentMode(PresentMode requested)
    {
        u32 count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &count, nullptr);
        Array<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &count, modes.Data());
        auto has = [&](VkPresentModeKHR m)
        {
            for (auto x : modes)
                if (x == m)
                    return true;
            return false;
        };

        const VkPresentModeKHR desired = toVkPresentMode(requested);
        if (has(desired))
        {
            return desired;
        }

        // Requested mode unavailable: preserve the request's intent rather than dropping straight
        // to vsync. Immediate and Mailbox both mean "don't block on the refresh", so fall back to
        // whichever uncapped mode the surface does expose (Wayland commonly offers Mailbox but not
        // Immediate) before settling for FIFO, the only mode the Vulkan spec guarantees.
        VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
        if (requested == PresentMode::Immediate || requested == PresentMode::Mailbox)
        {
            if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            {
                chosen = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            {
                chosen = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }
        else if (requested == PresentMode::FifoRelaxed && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
        {
            chosen = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        }

        String msg;
        AppendFormat(msg, u8"[swapchain] requested present mode (vk {}) unavailable; using vk {}\n",
                     static_cast<u32>(desired), static_cast<u32>(chosen));
        ConsoleWrite(msg.AsView());
        return chosen;
    }

    inline Status VkSwapChainImpl::retrieveImages(VkFormat format)
    {
        u32 imgCount = 0;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, nullptr);
        Array<VkImage> images(imgCount);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, images.Data());
        m_bufferCount = imgCount;

        TextureFormat texFmt = fromVkFormat(format);
        if (texFmt == TextureFormat::Undefined)
            texFmt = m_format;

        for (u32 i = 0; i < imgCount; ++i)
        {
            TextureDesc td{};
            td.dimension = TextureDimension::Texture2D;
            td.format = texFmt;
            td.width = m_width;
            td.height = m_height;
            td.arrayLayerCount = 1;
            td.mipLevelCount = 1;
            td.sampleCount = 1;
            td.usage = TextureUsage::RenderTarget;
            td.alternateViewFormat = m_alternate;

            auto* tex = m_allocator->New<VkTextureImpl>();
            tex->initFromExisting(images[i], td);
            m_textures.PushBack(tex);

            TextureViewDesc vd{};
            vd.format = texFmt;
            vd.dimension = TextureViewDimension::Texture2D;
            vd.mipLevelCount = 1;
            vd.arrayLayerCount = 1;
            auto* view = m_allocator->New<VkTextureViewImpl>();
            if (view->init(m_device, tex, vd) != ErrorCode::Ok)
            {
                m_allocator->Delete(view);
                return ErrorCode::Unknown;
            }
            m_views.PushBack(view);

            if (m_alternate != TextureFormat::Undefined)
            {
                TextureViewDesc ad = vd;
                ad.format = m_alternate;
                auto* alternate = m_allocator->New<VkTextureViewImpl>();
                if (alternate->init(m_device, tex, ad) != ErrorCode::Ok)
                {
                    m_allocator->Delete(alternate);
                    return ErrorCode::Unknown;
                }
                m_alternateViews.PushBack(alternate);
            }
        }
        return ErrorCode::Ok;
    }

    inline void VkSwapChainImpl::createSyncObjects()
    {
        VkSemaphoreCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (u32 i = 0; i < m_bufferCount; ++i)
        {
            VkSemaphore a = VK_NULL_HANDLE, p = VK_NULL_HANDLE;
            vkCreateSemaphore(m_device, &ci, nullptr, &a);
            vkCreateSemaphore(m_device, &ci, nullptr, &p);
            m_acquireSems.PushBack(a);
            m_presentSems.PushBack(p);
        }
    }

    inline void VkSwapChainImpl::cleanupImages()
    {
        for (auto* v : m_views)
        {
            v->cleanup(m_device);
            m_allocator->Delete(v);
        }
        m_views.Clear();
        for (auto* v : m_alternateViews)
        {
            v->cleanup(m_device);
            m_allocator->Delete(v);
        }
        m_alternateViews.Clear();
        for (auto* t : m_textures)
        {
            t->cleanup(m_device);
            m_allocator->Delete(t);
        }
        m_textures.Clear();
    }

    inline void VkSwapChainImpl::destroySyncObjects()
    {
        for (auto s : m_acquireSems)
        {
            vkDestroySemaphore(m_device, s, nullptr);
        }
        m_acquireSems.Clear();
        for (auto s : m_presentSems)
        {
            vkDestroySemaphore(m_device, s, nullptr);
        }
        m_presentSems.Clear();
    }

    inline Status VkSwapChainImpl::Resize(u32 w, u32 h)
    {
        vkDeviceWaitIdle(m_device);
        cleanupImages();
        destroySyncObjects();
        return CreateSwapChain(w, h, m_requestedFormat, m_bufferCount, m_swapchain);
    }

    inline void VkSwapChainImpl::cleanup()
    {
        vkDeviceWaitIdle(m_device);
        cleanupImages();
        destroySyncObjects();
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

} // namespace foundation::rhi::vk
