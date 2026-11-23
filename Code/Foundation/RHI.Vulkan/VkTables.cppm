// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// The Vulkan device's objects by handle. Encoders, bind groups, transfer batches and the swap
/// chain hold a pointer to the device's tables and look a handle up as they record; a stale
/// handle answers null.

module;
#include "Core/Prelude.h"

export module foundation.rhi.vulkan:tables;

import foundation.core;
import foundation.rhi;
import :buffer;
import :texture;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::vk
{
    using VkBufferTable = ResourceTable<BufferTag, VkBufferImpl>;
    using VkTextureTable = ResourceTable<TextureTag, VkTextureImpl>;
    using VkTextureViewTable = ResourceTable<TextureViewTag, VkTextureViewImpl>;
    using VkSamplerTable = ResourceTable<SamplerTag, VkSamplerImpl>;

    /// One table per handle type, each freeing its objects through the device (`context`).
    struct VkTables
    {
        VkTables(IAllocator& allocator, SubmissionTracker& tracker, void* context, SubmissionTracker::FreeFn buffer,
                 SubmissionTracker::FreeFn texture, SubmissionTracker::FreeFn view,
                 SubmissionTracker::FreeFn sampler) noexcept
            : buffers(allocator, tracker, buffer, context), textures(allocator, tracker, texture, context),
              views(allocator, tracker, view, context), samplers(allocator, tracker, sampler, context)
        {
        }

        /// Frees what is still in them; views before the textures they are of.
        void FreeAll()
        {
            samplers.FreeAll();
            views.FreeAll();
            textures.FreeAll();
            buffers.FreeAll();
        }

        VkBufferTable buffers;
        VkTextureTable textures;
        VkTextureViewTable views;
        VkSamplerTable samplers;
    };

    [[nodiscard]] inline VkBufferImpl* LookUp(const VkTables* tables, BufferHandle handle) noexcept
    {
        return tables != nullptr ? tables->buffers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkTextureImpl* LookUp(const VkTables* tables, TextureHandle handle) noexcept
    {
        return tables != nullptr ? tables->textures.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkTextureViewImpl* LookUp(const VkTables* tables, TextureViewHandle handle) noexcept
    {
        return tables != nullptr ? tables->views.Get(handle) : nullptr;
    }
    [[nodiscard]] inline VkSamplerImpl* LookUp(const VkTables* tables, SamplerHandle handle) noexcept
    {
        return tables != nullptr ? tables->samplers.Get(handle) : nullptr;
    }
} // namespace foundation::rhi::vk
