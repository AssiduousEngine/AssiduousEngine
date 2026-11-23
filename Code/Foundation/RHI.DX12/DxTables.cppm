// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// The D3D12 device's objects by handle. Encoders, bind groups, transfer batches and the swap
/// chain hold a pointer to the device's tables and look a handle up as they record; a stale
/// handle answers null.

module;
#include "Core/Prelude.h"

export module foundation.rhi.dx12:tables;

import foundation.core;
import foundation.rhi;
import :buffer;
import :texture;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{
    using DxBufferTable = ResourceTable<BufferTag, DxBufferImpl>;
    using DxTextureTable = ResourceTable<TextureTag, DxTextureImpl>;
    using DxTextureViewTable = ResourceTable<TextureViewTag, DxTextureViewImpl>;
    using DxSamplerTable = ResourceTable<SamplerTag, DxSamplerImpl>;

    /// One table per handle type, each freeing its objects through the device (`context`).
    struct DxTables
    {
        DxTables(IAllocator& allocator, SubmissionTracker& tracker, void* context, SubmissionTracker::FreeFn buffer,
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

        DxBufferTable buffers;
        DxTextureTable textures;
        DxTextureViewTable views;
        DxSamplerTable samplers;
    };

    [[nodiscard]] inline DxBufferImpl* LookUp(const DxTables* tables, BufferHandle handle) noexcept
    {
        return tables != nullptr ? tables->buffers.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxTextureImpl* LookUp(const DxTables* tables, TextureHandle handle) noexcept
    {
        return tables != nullptr ? tables->textures.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxTextureViewImpl* LookUp(const DxTables* tables, TextureViewHandle handle) noexcept
    {
        return tables != nullptr ? tables->views.Get(handle) : nullptr;
    }
    [[nodiscard]] inline DxSamplerImpl* LookUp(const DxTables* tables, SamplerHandle handle) noexcept
    {
        return tables != nullptr ? tables->samplers.Get(handle) : nullptr;
    }
} // namespace foundation::rhi::dx12
