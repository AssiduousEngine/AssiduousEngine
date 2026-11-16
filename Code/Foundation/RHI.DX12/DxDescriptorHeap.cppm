// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Simple CPU-side descriptor heap allocator with a free list.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:descriptor_heap;

import foundation.core;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    /// CPU-side descriptors in pages: when every page is full another of the same size is made, so
    /// an allocation never fails while the device can make heaps (a fixed heap handed out a null
    /// handle when full, and the view written through it took the process down). A null handle comes
    /// back only when a new page cannot be made; callers check for it.
    class DxDescriptorHeapAllocator
    {
    public:
        DxDescriptorHeapAllocator() = default;

        Status init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 maxCount,
                    D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
        {
            m_device = device;
            m_type = type;
            m_flags = flags;
            m_pageSize = maxCount;
            m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
            return AddPage() ? ErrorCode::Ok : ErrorCode::Unknown;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE allocate()
        {
            for (usize p = 0; p < m_pages.Size(); ++p)
            {
                Page& page = m_pages[(m_currentPage + p) % m_pages.Size()];
                if (page.count == m_pageSize)
                    continue;
                for (u32 i = 0; i < m_pageSize; ++i)
                {
                    const u32 idx = (page.searchStart + i) % m_pageSize;
                    if (!page.alive[idx])
                    {
                        page.alive[idx] = 1;
                        ++page.count;
                        page.searchStart = (idx + 1) % m_pageSize;
                        m_currentPage = (m_currentPage + p) % m_pages.Size();
                        D3D12_CPU_DESCRIPTOR_HANDLE h{};
                        h.ptr = page.start.ptr + static_cast<SIZE_T>(idx) * m_descriptorSize;
                        return h;
                    }
                }
            }
            if (!AddPage())
                return {}; // the device made no new heap: the caller reports it
            m_currentPage = m_pages.Size() - 1;
            return allocate();
        }

        void free(D3D12_CPU_DESCRIPTOR_HANDLE handle)
        {
            for (Page& page : m_pages)
            {
                if (handle.ptr < page.start.ptr)
                    continue;
                const SIZE_T offset = (handle.ptr - page.start.ptr) / m_descriptorSize;
                if (offset >= m_pageSize)
                    continue;
                if (page.alive[offset])
                {
                    page.alive[offset] = 0;
                    --page.count;
                }
                return;
            }
        }

        void Destroy()
        {
            m_pages.Clear();
        }

        [[nodiscard]] u32 descriptorSize() const { return m_descriptorSize; }
        [[nodiscard]] usize pageCount() const { return m_pages.Size(); }

    private:
        struct Page
        {
            ComPtr<ID3D12DescriptorHeap> heap;
            D3D12_CPU_DESCRIPTOR_HANDLE start{};
            Array<u8> alive;
            u32 count = 0;
            u32 searchStart = 0;
        };

        bool AddPage()
        {
            if (m_device == nullptr || m_pageSize == 0)
                return false;
            Page page;
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = m_type;
            hd.NumDescriptors = m_pageSize;
            hd.Flags = m_flags;
            if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&page.heap))))
                return false;
            page.start = page.heap->GetCPUDescriptorHandleForHeapStart();
            page.alive.Resize(m_pageSize, static_cast<u8>(0));
            m_pages.PushBack(Move(page));
            return true;
        }

        ID3D12Device* m_device = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        D3D12_DESCRIPTOR_HEAP_FLAGS m_flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        u32 m_pageSize = 0;
        u32 m_descriptorSize = 0;
        usize m_currentPage = 0;
        Array<Page> m_pages;
    };

} // namespace foundation::rhi::dx12
