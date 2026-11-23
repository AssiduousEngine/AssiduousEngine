// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of BindGroup.
/// Allocates contiguous descriptor ranges in CPU-visible heaps and writes descriptors.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:bind_group;

import foundation.core;
import foundation.rhi;
import :conversions;
import :bind_group_layout;
import :gpu_descriptor_heap;
import :buffer;
import :texture;
import :texture_view;
import :sampler;
import :accel_struct;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxAccelStructImpl; // forward

    /// Shader-visible sampler tables shared by the bind groups whose samplers are the same, in the
    /// same order. The shader-visible sampler heap holds 2048 descriptors (a D3D12 limit) and every
    /// bind group took a table of its own, so the 2049th group with a sampler failed however few
    /// distinct samplers there were. Keyed by the samplers' descriptions (an address is reused once
    /// a sampler is gone; a description determines the descriptor), counted, freed with the last.
    class DxSamplerTables
    {
    public:
        void init(ID3D12Device* device, DxGpuDescriptorHeap* gpuSamplerHeap)
        {
            m_device = device;
            m_heap = gpuSamplerHeap;
        }

        /// The offset of a table holding `sources` (described by `descs`), made or shared; -1 when
        /// the heap has no room. Slots with no source (an array's unwritten tail) are left as they are.
        i32 acquire(Span<const D3D12_SAMPLER_DESC> descs, Span<const D3D12_CPU_DESCRIPTOR_HANDLE> sources)
        {
            ScopedLock lock(m_mutex);
            for (Table& table : m_tables)
            {
                if (table.descs.Size() == descs.Size() &&
                    MemCompare(table.descs.Data(), descs.Data(), descs.Size() * sizeof(D3D12_SAMPLER_DESC)) == 0)
                {
                    ++table.refs;
                    return table.offset;
                }
            }
            if (m_heap == nullptr)
                return -1;
            const i32 offset = m_heap->allocate(static_cast<u32>(descs.Size()));
            if (offset < 0)
                return -1;
            for (usize i = 0; i < sources.Size(); ++i)
            {
                if (sources[i].ptr != 0)
                    m_device->CopyDescriptorsSimple(1, m_heap->getCpuHandle(static_cast<u32>(offset) + static_cast<u32>(i)),
                                                    sources[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            }
            Table table;
            table.descs.Resize(descs.Size());
            MemCopy(table.descs.Data(), descs.Data(), descs.Size() * sizeof(D3D12_SAMPLER_DESC));
            table.offset = offset;
            table.refs = 1;
            m_tables.PushBack(Move(table));
            return offset;
        }

        void release(i32 offset)
        {
            ScopedLock lock(m_mutex);
            for (usize i = 0; i < m_tables.Size(); ++i)
            {
                if (m_tables[i].offset != offset)
                    continue;
                if (--m_tables[i].refs == 0)
                {
                    m_heap->free(static_cast<u32>(offset), static_cast<u32>(m_tables[i].descs.Size()));
                    m_tables.RemoveAtSwap(i);
                }
                return;
            }
        }

        /// The sampler descriptions of the table at `offset` (empty when there is none).
        [[nodiscard]] Array<D3D12_SAMPLER_DESC> descsOf(i32 offset)
        {
            ScopedLock lock(m_mutex);
            Array<D3D12_SAMPLER_DESC> out;
            for (const Table& table : m_tables)
            {
                if (table.offset == offset)
                {
                    out.Resize(table.descs.Size());
                    MemCopy(out.Data(), table.descs.Data(), table.descs.Size() * sizeof(D3D12_SAMPLER_DESC));
                    break;
                }
            }
            return out;
        }

        [[nodiscard]] usize tableCount()
        {
            ScopedLock lock(m_mutex);
            return m_tables.Size();
        }

    private:
        struct Table
        {
            Array<D3D12_SAMPLER_DESC> descs;
            i32 offset = -1;
            u32 refs = 0;
        };
        ID3D12Device* m_device = nullptr;
        DxGpuDescriptorHeap* m_heap = nullptr;
        Array<Table> m_tables;
        // Bind groups are made and destroyed on whichever thread records (the heap under the
        // tables locks for the same reason), so the table list is guarded as the heap is.
        Mutex m_mutex;
    };

    class DxBindGroupImpl : public BindGroup
    {
    public:
        /// The device's buffers, by which its entries' buffer handles are looked up (set before
        /// init; a bindless update looks them up again).
        const DxBufferTable* buffers = nullptr;
        Status init(ID3D12Device* device, const BindGroupDesc& d, DxGpuDescriptorHeap* cpuSrvHeap,
                    DxGpuDescriptorHeap* cpuSamplerHeap, DxGpuDescriptorHeap* gpuSamplerHeap,
                    DxSamplerTables* samplerTables)
        {
            m_device = device;
            m_samplerTables = samplerTables;
            m_layout = static_cast<DxBindGroupLayoutImpl*>(d.layout);
            m_cpuSrvHeap = cpuSrvHeap;
            m_cpuSamplerHeap = cpuSamplerHeap;
            m_gpuSamplerHeap = gpuSamplerHeap;
            if (!m_layout)
                return ErrorCode::Unknown;

            // Cache counts so cleanup doesn't need to access m_layout (which may be destroyed first).
            m_cachedCbvSrvUavCount = m_layout->cbvSrvUavCount();
            m_cachedSamplerCount = m_layout->samplerCount();

            if (m_cachedCbvSrvUavCount > 0)
            {
                m_cbvSrvUavOffset = cpuSrvHeap->allocate(m_cachedCbvSrvUavCount);
                if (m_cbvSrvUavOffset < 0)
                    return ErrorCode::Unknown;
            }
            if (m_cachedSamplerCount > 0)
            {
                // Samplers are baked into the shader-visible heap once, at creation (the
                // bind group is immutable), and SetBindGroup binds this table directly. The
                // heap's hard 2048 cap cannot fit per-draw copy-on-bind staging, so
                // samplers (unlike CBV/SRV/UAV tables) never go through staging. A group whose
                // samplers are all given shares its table with every group of the same ones; one
                // with bindless sampler ranges (rewritten in place) keeps its own.
                // Only a group with its own table takes slots in the CPU sampler heap (as big as the
                // shader-visible one): a group sharing a table takes none, or the 2049th group of one
                // sampler failed there though its table was shared.
                if (!AcquireSharedSamplers(d))
                {
                    m_samplerOffset = cpuSamplerHeap->allocate(m_cachedSamplerCount);
                    if (m_samplerOffset < 0)
                        return ErrorCode::Unknown;
                    m_gpuSamplerOffset = gpuSamplerHeap->allocate(m_cachedSamplerCount);
                    if (m_gpuSamplerOffset < 0)
                        return ErrorCode::Unknown;
                }
            }

            writeDescriptors(d);
            return ErrorCode::Ok;
        }

        BindGroupLayout* Layout() override { return m_layout; }

        void UpdateBindless(Span<const BindlessUpdateEntry> entries) override
        {
            auto ranges = m_layout->ranges();
            for (usize i = 0; i < entries.Size(); ++i)
            {
                const auto& e = entries[i];
                if (e.layoutIndex >= static_cast<u32>(ranges.Size()))
                    continue;
                const auto& r = ranges[e.layoutIndex];

                BindGroupEntry bgEntry{};
                bgEntry.buffer = e.buffer;
                bgEntry.bufferOffset = e.bufferOffset;
                bgEntry.bufferSize = e.bufferSize;
                bgEntry.textureView = e.textureView;
                bgEntry.sampler = e.sampler;

                if (r.isSampler)
                {
                    // A shared table is every group's with these samplers: rewritten in place, it
                    // would change them all (and writeSampler skipped it, so the update was lost).
                    // The group takes a private table first.
                    if (m_sharedSamplers && !makeSamplersPrivate())
                        continue;
                    writeSampler(bgEntry, r, e.arrayIndex);
                }
                else
                    writeCbvSrvUav(bgEntry, r, e.arrayIndex);
            }
        }

        // Moves the group off its shared sampler table onto a table of its own, made from the shared
        // one's descriptions (a shader-visible heap is no copy source). False, and the group as it
        // was, when the heaps have no room.
        bool makeSamplersPrivate()
        {
            const Array<D3D12_SAMPLER_DESC> descs = m_samplerTables->descsOf(m_gpuSamplerOffset);
            const i32 cpu = m_cpuSamplerHeap->allocate(m_cachedSamplerCount);
            const i32 gpu = cpu >= 0 ? m_gpuSamplerHeap->allocate(m_cachedSamplerCount) : -1;
            if (gpu < 0)
            {
                if (cpu >= 0)
                    m_cpuSamplerHeap->free(static_cast<u32>(cpu), m_cachedSamplerCount);
                return false;
            }
            for (usize i = 0; i < descs.Size() && i < m_cachedSamplerCount; ++i)
            {
                if (descs[i].AddressU == 0)
                    continue; // a slot the shared table left unwritten
                m_device->CreateSampler(&descs[i], m_cpuSamplerHeap->getCpuHandle(static_cast<u32>(cpu) + static_cast<u32>(i)));
                m_device->CreateSampler(&descs[i], m_gpuSamplerHeap->getCpuHandle(static_cast<u32>(gpu) + static_cast<u32>(i)));
            }
            m_samplerTables->release(m_gpuSamplerOffset);
            m_sharedSamplers = false;
            m_samplerOffset = cpu;
            m_gpuSamplerOffset = gpu;
            return true;
        }

        void cleanup()
        {
            if (m_cbvSrvUavOffset >= 0 && m_cachedCbvSrvUavCount > 0)
                m_cpuSrvHeap->free(static_cast<u32>(m_cbvSrvUavOffset), m_cachedCbvSrvUavCount);
            if (m_samplerOffset >= 0 && m_cachedSamplerCount > 0)
                m_cpuSamplerHeap->free(static_cast<u32>(m_samplerOffset), m_cachedSamplerCount);
            if (m_gpuSamplerOffset >= 0 && m_cachedSamplerCount > 0)
            {
                if (m_sharedSamplers)
                    m_samplerTables->release(m_gpuSamplerOffset);
                else
                    m_gpuSamplerHeap->free(static_cast<u32>(m_gpuSamplerOffset), m_cachedSamplerCount);
            }
            m_sharedSamplers = false;
            m_cbvSrvUavOffset = -1;
            m_samplerOffset = -1;
            m_gpuSamplerOffset = -1;
        }

        [[nodiscard]] i32 cbvSrvUavOffset() const { return m_cbvSrvUavOffset; }
        [[nodiscard]] i32 samplerOffset() const { return m_samplerOffset; }
        /// Offset of this group's baked sampler table in the shader-visible sampler heap.
        [[nodiscard]] i32 gpuSamplerOffset() const { return m_gpuSamplerOffset; }
        [[nodiscard]] Span<const u64> dynamicGpuAddresses() const
        {
            return {m_dynAddrs.Data(), m_dynAddrs.Size()};
        }

    private:
        // Takes a shared table for the group's samplers, when every sampler range is a plain one and
        // given; false leaves the group to make its own.
        bool AcquireSharedSamplers(const BindGroupDesc& d)
        {
            if (m_samplerTables == nullptr)
                return false;
            Array<D3D12_SAMPLER_DESC> descs;
            descs.Resize(m_cachedSamplerCount);
            MemZero(descs.Data(), descs.Size() * sizeof(D3D12_SAMPLER_DESC));
            Array<D3D12_CPU_DESCRIPTOR_HANDLE> sources;
            sources.Resize(m_cachedSamplerCount);
            for (D3D12_CPU_DESCRIPTOR_HANDLE& h : sources)
                h.ptr = 0;
            auto ranges = m_layout->ranges();
            usize entryIdx = 0; // walked as writeDescriptors walks them
            for (usize i = 0; i < ranges.Size(); ++i)
            {
                const auto& r = ranges[i];
                if (r.type == BindingType::BindlessSamplers)
                    return false; // rewritten in place by UpdateBindless: its own
                if (r.type == BindingType::BindlessTextures || r.type == BindingType::BindlessStorageBuffers ||
                    r.type == BindingType::BindlessStorageTextures)
                    continue;
                if (entryIdx >= d.entries.Size())
                    break;
                const auto& e = d.entries[entryIdx++];
                if (!r.isSampler || r.hasDynamicOffset)
                    continue;
                auto* s = static_cast<DxSamplerImpl*>(e.sampler);
                if (s == nullptr || r.heapOffset >= m_cachedSamplerCount)
                    return false;
                descs[r.heapOffset] = s->desc();
                sources[r.heapOffset] = s->handle();
            }
            const i32 offset = m_samplerTables->acquire(Span<const D3D12_SAMPLER_DESC>(descs.Data(), descs.Size()),
                                                        Span<const D3D12_CPU_DESCRIPTOR_HANDLE>(sources.Data(), sources.Size()));
            if (offset < 0)
                return false;
            m_gpuSamplerOffset = offset;
            m_sharedSamplers = true;
            return true;
        }

        void writeDescriptors(const BindGroupDesc& d)
        {
            auto ranges = m_layout->ranges();
            usize entryIdx = 0;
            for (usize i = 0; i < ranges.Size(); ++i)
            {
                const auto& r = ranges[i];
                switch (r.type)
                {
                case BindingType::BindlessTextures:
                case BindingType::BindlessSamplers:
                case BindingType::BindlessStorageBuffers:
                case BindingType::BindlessStorageTextures:
                    continue;
                default:
                    break;
                }
                if (entryIdx >= d.entries.Size())
                    break;
                const auto& e = d.entries[entryIdx++];

                if (r.hasDynamicOffset)
                {
                    if (auto* buf = LookUp(buffers, e.buffer))
                        m_dynAddrs.PushBack(buf->gpuAddress() + e.bufferOffset);
                    else
                        m_dynAddrs.PushBack(0);
                    continue;
                }
                if (r.isSampler)
                    writeSampler(e, r);
                else
                    writeCbvSrvUav(e, r);
            }
        }

        void writeCbvSrvUav(const BindGroupEntry& e, const DxBindingRangeInfo& r, u32 arrayIdx = 0)
        {
            u32 off = static_cast<u32>(m_cbvSrvUavOffset) + r.heapOffset + arrayIdx;
            D3D12_CPU_DESCRIPTOR_HANDLE dest = m_cpuSrvHeap->getCpuHandle(off);

            switch (r.type)
            {
            case BindingType::UniformBuffer:
                if (auto* buf = LookUp(buffers, e.buffer))
                {
                    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                    cbv.BufferLocation = buf->gpuAddress() + e.bufferOffset;
                    u64 sz = (e.bufferSize > 0) ? e.bufferSize : buf->desc.size;
                    cbv.SizeInBytes = static_cast<UINT>((sz + 255) & ~u64(255));
                    m_device->CreateConstantBufferView(&cbv, dest);
                }
                break;
            case BindingType::StorageBufferReadOnly:
                if (auto* buf = LookUp(buffers, e.buffer))
                {
                    u64 sz = (e.bufferSize > 0) ? e.bufferSize : buf->desc.size;
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    if (r.storageBufferStride > 0)
                    {
                        srv.Format = DXGI_FORMAT_UNKNOWN;
                        srv.Buffer.FirstElement =
                            static_cast<UINT64>(e.bufferOffset / r.storageBufferStride);
                        srv.Buffer.NumElements = static_cast<UINT>(sz / r.storageBufferStride);
                        srv.Buffer.StructureByteStride = r.storageBufferStride;
                    }
                    else
                    {
                        srv.Format = DXGI_FORMAT_R32_TYPELESS;
                        srv.Buffer.FirstElement = static_cast<UINT64>(e.bufferOffset / 4);
                        srv.Buffer.NumElements = static_cast<UINT>(sz / 4);
                        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                    }
                    m_device->CreateShaderResourceView(buf->handle(), &srv, dest);
                }
                break;
            case BindingType::StorageBufferReadWrite:
                if (auto* buf = LookUp(buffers, e.buffer))
                {
                    u64 sz = (e.bufferSize > 0) ? e.bufferSize : buf->desc.size;
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    if (r.storageBufferStride > 0)
                    {
                        uav.Format = DXGI_FORMAT_UNKNOWN;
                        uav.Buffer.FirstElement =
                            static_cast<UINT64>(e.bufferOffset / r.storageBufferStride);
                        uav.Buffer.NumElements = static_cast<UINT>(sz / r.storageBufferStride);
                        uav.Buffer.StructureByteStride = r.storageBufferStride;
                    }
                    else
                    {
                        uav.Format = DXGI_FORMAT_R32_TYPELESS;
                        uav.Buffer.FirstElement = static_cast<UINT64>(e.bufferOffset / 4);
                        uav.Buffer.NumElements = static_cast<UINT>(sz / 4);
                        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                    }
                    m_device->CreateUnorderedAccessView(buf->handle(), nullptr, &uav, dest);
                }
                break;
            case BindingType::SampledTexture:
            case BindingType::BindlessTextures:
                // A view whose descriptor heap had no room has a null handle: no copy source
                // (copying from it is invalid and can remove the device), so the slot is left.
                if (auto* v = static_cast<DxTextureViewImpl*>(e.textureView); v != nullptr && v->getSrv().ptr != 0)
                    m_device->CopyDescriptorsSimple(1, dest, v->getSrv(),
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                break;
            case BindingType::StorageTextureReadOnly:
            case BindingType::StorageTextureReadWrite:
            case BindingType::BindlessStorageTextures:
                if (auto* v = static_cast<DxTextureViewImpl*>(e.textureView); v != nullptr && v->getUav().ptr != 0)
                    m_device->CopyDescriptorsSimple(1, dest, v->getUav(),
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                break;
            case BindingType::AccelerationStructure:
                if (auto* dxAs = static_cast<DxAccelStructImpl*>(e.accelStruct))
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC asSrv{};
                    asSrv.Format = DXGI_FORMAT_UNKNOWN;
                    asSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                    asSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    asSrv.RaytracingAccelerationStructure.Location = dxAs->DeviceAddress();
                    m_device->CreateShaderResourceView(nullptr, &asSrv, dest);
                }
                break;
            default:
                break;
            }
        }

        void writeSampler(const BindGroupEntry& e, const DxBindingRangeInfo& r, u32 arrayIdx = 0)
        {
            auto* s = static_cast<DxSamplerImpl*>(e.sampler);
            if (!s || m_sharedSamplers)
                return; // a shared table was written when it was made
            u32 off = static_cast<u32>(m_samplerOffset) + r.heapOffset + arrayIdx;
            m_device->CopyDescriptorsSimple(1, m_cpuSamplerHeap->getCpuHandle(off), s->handle(),
                                            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            // Mirror into the baked shader-visible table (bound directly at SetBindGroup).
            u32 gpuOff = static_cast<u32>(m_gpuSamplerOffset) + r.heapOffset + arrayIdx;
            m_device->CopyDescriptorsSimple(1, m_gpuSamplerHeap->getCpuHandle(gpuOff), s->handle(),
                                            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }

        ID3D12Device* m_device = nullptr;
        DxBindGroupLayoutImpl* m_layout = nullptr;
        DxGpuDescriptorHeap* m_cpuSrvHeap = nullptr;
        DxGpuDescriptorHeap* m_cpuSamplerHeap = nullptr;
        DxGpuDescriptorHeap* m_gpuSamplerHeap = nullptr;
        u32 m_cachedCbvSrvUavCount = 0;
        u32 m_cachedSamplerCount = 0;
        i32 m_cbvSrvUavOffset = -1;
        i32 m_samplerOffset = -1;
        i32 m_gpuSamplerOffset = -1; // baked table in the shader-visible sampler heap
        DxSamplerTables* m_samplerTables = nullptr; // the device's (borrowed)
        bool m_sharedSamplers = false;               // the table is a shared one (released, not freed)
        Array<u64> m_dynAddrs;
    };

} // namespace foundation::rhi::dx12
