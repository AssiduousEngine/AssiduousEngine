// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// The WebGPU device's objects by handle. Encoders, bind groups, transfer batches and the swap
/// chain hold a pointer to the device's tables and look a handle up as they record; a stale
/// handle answers null.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:tables;

import foundation.core;
import foundation.rhi;
import :buffer;
import :texture;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    using WebGpuBufferTable = ResourceTable<BufferTag, WebGpuBuffer>;
    using WebGpuTextureTable = ResourceTable<TextureTag, WebGpuTexture>;
    using WebGpuTextureViewTable = ResourceTable<TextureViewTag, WebGpuTextureView>;
    using WebGpuSamplerTable = ResourceTable<SamplerTag, WebGpuSampler>;

    /// One table per handle type, each freeing its objects through the device (`context`).
    struct WebGpuTables
    {
        WebGpuTables(IAllocator& allocator, SubmissionTracker& tracker, void* context,
                     SubmissionTracker::FreeFn buffer, SubmissionTracker::FreeFn texture,
                     SubmissionTracker::FreeFn view, SubmissionTracker::FreeFn sampler) noexcept
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

        WebGpuBufferTable buffers;
        WebGpuTextureTable textures;
        WebGpuTextureViewTable views;
        WebGpuSamplerTable samplers;
    };

    [[nodiscard]] inline WebGpuBuffer* LookUp(const WebGpuTables* tables, BufferHandle handle) noexcept
    {
        return tables != nullptr ? tables->buffers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuTexture* LookUp(const WebGpuTables* tables, TextureHandle handle) noexcept
    {
        return tables != nullptr ? tables->textures.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuTextureView* LookUp(const WebGpuTables* tables, TextureViewHandle handle) noexcept
    {
        return tables != nullptr ? tables->views.Get(handle) : nullptr;
    }
    [[nodiscard]] inline WebGpuSampler* LookUp(const WebGpuTables* tables, SamplerHandle handle) noexcept
    {
        return tables != nullptr ? tables->samplers.Get(handle) : nullptr;
    }

    /// The WebGPU object a handle names, or null (a stale handle).
    [[nodiscard]] inline WGPUBuffer WgpuOf(const WebGpuTables* tables, BufferHandle handle) noexcept
    {
        const WebGpuBuffer* buffer = LookUp(tables, handle);
        return buffer != nullptr ? buffer->Handle() : nullptr;
    }
    [[nodiscard]] inline WGPUTexture WgpuOf(const WebGpuTables* tables, TextureHandle handle) noexcept
    {
        const WebGpuTexture* texture = LookUp(tables, handle);
        return texture != nullptr ? texture->Handle() : nullptr;
    }
    [[nodiscard]] inline WGPUTextureView WgpuOf(const WebGpuTables* tables, TextureViewHandle handle) noexcept
    {
        const WebGpuTextureView* view = LookUp(tables, handle);
        return view != nullptr ? view->Handle() : nullptr;
    }
    [[nodiscard]] inline WGPUSampler WgpuOf(const WebGpuTables* tables, SamplerHandle handle) noexcept
    {
        const WebGpuSampler* sampler = LookUp(tables, handle);
        return sampler != nullptr ? sampler->Handle() : nullptr;
    }
}
