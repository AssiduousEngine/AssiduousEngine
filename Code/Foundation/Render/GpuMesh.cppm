// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:gpu_mesh` partition.
///
/// GpuMeshCache: uploads a StaticMesh's vertex and index streams to the GPU on first use and
/// caches them by the mesh's uid (so repeated draws reuse the buffers). The streams are
/// sub-allocated from shared vertex and index pools (no per-mesh buffers); a mesh records
/// the pool buffer and its byte offset, and draws bind with that offset. Part of the
/// scene-agnostic renderer: it consumes geometry, not a scene.

module;
#include "Core/Prelude.h"

export module foundation.render:gpu_mesh;

import foundation.core;
import foundation.rhi;
import foundation.geometry;
import :resources;

using namespace foundation::core;
namespace geometry = foundation::geometry;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // GPU location of one mesh, ready to bind and draw: the pooled vertex and index buffers and the
    // byte offsets of this mesh's streams within them. Skinned meshes also carry a parallel skinning
    // stream (joints and weights) in a second vertex buffer, bound at slot 1 by the skinned pipeline.
    struct GpuMesh
    {
        rhi::Buffer* vertexBuffer = nullptr;
        u64 vertexOffset = 0;
        rhi::Buffer* indexBuffer = nullptr;
        u64 indexOffset = 0;
        u32 indexCount = 0;
        rhi::IndexFormat indexFormat = rhi::IndexFormat::UInt32;
        rhi::Buffer* skinBuffer = nullptr; // skinning stream (null for a static mesh)
        u64 skinOffset = 0;
    };

    class GpuMeshCache
    {
    public:
        /// `framesInFlight`: how many frames the ranges of a mesh gone or rebuilt are kept for
        /// before reuse (Tick), since frames still in flight may draw from them.
        explicit GpuMeshCache(rhi::Device& device, u32 framesInFlight = 3) noexcept
            : m_queue(device.GetQueue(rhi::QueueType::Graphics)), m_framesInFlight(framesInFlight < 1 ? 1u : framesInFlight),
              m_vertexPool(device, rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                           kVertexChunk, u8"mesh.vertexPool"),
              m_indexPool(device, rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst, kIndexChunk,
                          u8"mesh.indexPool"),
              m_skinPool(device, rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst, kVertexChunk,
                         u8"mesh.skinPool")
        {
        }

        ~GpuMeshCache() { Clear(); }

        GpuMeshCache(const GpuMeshCache&) = delete;
        GpuMeshCache& operator=(const GpuMeshCache&) = delete;

        // Uploads `mesh` on first request (sub-allocating from the pools), and again when its
        // geometry was rebuilt in place (StaticMesh::contentVersion); returns its cached GPU location
        // (null if empty or allocation failed). The pointer is good for the frame.
        [[nodiscard]] const GpuMesh* GetOrUpload(geometry::StaticMesh* mesh)
        {
            if (mesh == nullptr)
            {
                return nullptr;
            } // no mesh assigned: a normal state, stay silent
            // A non-null mesh with an empty vertex or index stream can't be drawn: the draw path is
            // indexed-only (DrawIndexed), so 0 indices means nothing is rendered. That is almost always
            // a construction bug in whatever produced the mesh, so warn rather than fail silently, but
            // only for the first few, so a broken mesh resubmitted every frame can't flood the console.
            if (mesh->VertexCount() == 0 || mesh->IndexCount() == 0)
            {
                static u32 s_warned = 0;
                constexpr u32 kWarnLimit = 8;
                if (s_warned < kWarnLimit)
                {
                    ++s_warned;
                    const char* nm = mesh->name.IsEmpty()
                                         ? "<unnamed>"
                                         : reinterpret_cast<const char*>(mesh->name.CStr());
                    rhi::LogWarningf(
                        "[GpuMeshCache] mesh '%s' not uploadable: %u vertices, %u indices "
                        "(indexed draw path needs both non-zero) - skipping.%s",
                        nm, mesh->VertexCount(), mesh->IndexCount(),
                        s_warned == kWarnLimit ? " Further such warnings suppressed." : "");
                }
                return nullptr;
            }
            if (Entry* cached = m_cache.Find(mesh->uid))
            {
                if (cached->version == mesh->contentVersion)
                {
                    return &cached->gpu;
                }
                Retire(*cached); // rebuilt in place: its old streams go once the GPU is done with them
                m_cache.Remove(mesh->uid);
            }

            const GpuBufferPool::Alloc v =
                m_vertexPool.Allocate(mesh->VertexDataSize(), kVertexAlign);
            const GpuBufferPool::Alloc idx =
                m_indexPool.Allocate(mesh->indices.DataSize(), kIndexAlign);
            if (!v.ok || !idx.ok)
            {
                return nullptr;
            }

            // Skinned meshes carry a parallel skin stream (joints and weights), uploaded to a second pool.
            const Span<const geometry::VertexSkinning> skin = mesh->SkinningStream();
            GpuBufferPool::Alloc sk{};
            const bool hasSkin = mesh->IsSkinned() && !skin.IsEmpty();
            if (hasSkin)
            {
                sk = m_skinPool.Allocate(skin.Size() * sizeof(geometry::VertexSkinning),
                                         kVertexAlign);
                if (!sk.ok)
                {
                    return nullptr;
                }
            }

            Entry entry;
            if (mesh->IsCounted()) // a mesh on the stack (a test's) is not watched: it stays until Clear
            {
                entry.mesh = WeakRefPtr<geometry::StaticMesh>(RefPtr<geometry::StaticMesh>(mesh));
                entry.watched = true;
            }
            entry.version = mesh->contentVersion;
            entry.vertex = v;
            entry.vertexSize = mesh->VertexDataSize();
            entry.index = idx;
            entry.indexSize = mesh->indices.DataSize();
            if (hasSkin)
            {
                entry.skin = sk;
                entry.skinSize = skin.Size() * sizeof(geometry::VertexSkinning);
            }
            GpuMesh& g = entry.gpu;
            g.vertexBuffer = v.buffer;
            g.vertexOffset = v.offset;
            g.indexBuffer = idx.buffer;
            g.indexOffset = idx.offset;
            g.indexCount = mesh->IndexCount();
            g.indexFormat = (mesh->indices.GetFormat() == geometry::IndexBuffer::Format::U16)
                                ? rhi::IndexFormat::UInt16
                                : rhi::IndexFormat::UInt32;
            if (hasSkin)
            {
                g.skinBuffer = sk.buffer;
                g.skinOffset = sk.offset;
            }

            if (m_queue != nullptr)
            {
                rhi::TransferBatch* tb = nullptr;
                if (m_queue->CreateTransferBatch(tb).IsOk() && tb != nullptr)
                {
                    tb->WriteBuffer(g.vertexBuffer, g.vertexOffset,
                                    Span<const u8>{mesh->VertexData(), mesh->VertexDataSize()});
                    tb->WriteBuffer(
                        g.indexBuffer, g.indexOffset,
                        Span<const u8>{mesh->indices.RawData(), mesh->indices.DataSize()});
                    if (hasSkin)
                    {
                        tb->WriteBuffer(
                            g.skinBuffer, g.skinOffset,
                            Span<const u8>{reinterpret_cast<const u8*>(skin.Data()),
                                           skin.Size() * sizeof(geometry::VertexSkinning)});
                    }
                    (void)tb->Submit();
                    m_queue->DestroyTransferBatch(tb);
                }
            }

            return &m_cache.InsertOrAssign(mesh->uid, Move(entry)).gpu;
        }

        // Once a frame: the streams of meshes that are gone are retired, and retired streams old
        // enough that no frame in flight reads them return to the pools for reuse.
        void Tick()
        {
            for (usize i = 0; i < m_retired.Size();)
            {
                if (m_retired[i].framesLeft <= 1)
                {
                    const Retired& r = m_retired[i];
                    m_vertexPool.Free(r.vertex, r.vertexSize);
                    m_indexPool.Free(r.index, r.indexSize);
                    m_skinPool.Free(r.skin, r.skinSize);
                    m_retired.RemoveAtSwap(i);
                }
                else
                {
                    --m_retired[i].framesLeft;
                    ++i;
                }
            }
            Array<u64> gone;
            for (auto& slot : m_cache)
            {
                if (slot.value.watched && slot.value.mesh.Expired())
                {
                    gone.PushBack(slot.key);
                }
            }
            for (u64 uid : gone)
            {
                Retire(*m_cache.Find(uid));
                m_cache.Remove(uid);
            }
        }

        // Frees all pooled GPU memory and the cache.
        void Clear()
        {
            m_vertexPool.Clear();
            m_indexPool.Clear();
            m_skinPool.Clear();
            m_cache.Clear();
            m_retired.Clear();
        }

        [[nodiscard]] usize Size() const noexcept { return m_cache.Size(); }
        [[nodiscard]] usize RetiredCount() const noexcept { return m_retired.Size(); }
        // Bytes of vertex streams freed for reuse (tests).
        [[nodiscard]] u64 FreeVertexBytes() const noexcept { return m_vertexPool.FreeBytes(); }

    private:
        static constexpr u64 kVertexChunk = 4ull * 1024 * 1024; // 4 MB vertex chunks
        static constexpr u64 kIndexChunk = 1ull * 1024 * 1024;  // 1 MB index chunks
        static constexpr u64 kVertexAlign = 16; // safe vertex-stream offset alignment
        static constexpr u64 kIndexAlign = 4;   // covers u16 and u32 index offsets

        struct Entry
        {
            GpuMesh gpu;
            WeakRefPtr<geometry::StaticMesh> mesh; // expired: the mesh is gone, its streams retire
            bool watched = false;                   // `mesh` holds it (a counted mesh)
            u32 version = 0;                        // the mesh's contentVersion uploaded
            GpuBufferPool::Alloc vertex, index, skin;
            u64 vertexSize = 0, indexSize = 0, skinSize = 0;
        };
        struct Retired
        {
            GpuBufferPool::Alloc vertex, index, skin;
            u64 vertexSize = 0, indexSize = 0, skinSize = 0;
            u32 framesLeft = 0;
        };
        void Retire(const Entry& e)
        {
            m_retired.PushBack(Retired{e.vertex, e.index, e.skin, e.vertexSize, e.indexSize, e.skinSize, m_framesInFlight});
        }

        rhi::Queue* m_queue;
        u32 m_framesInFlight = 3;
        GpuBufferPool m_vertexPool;
        GpuBufferPool m_indexPool;
        GpuBufferPool m_skinPool; // skinning streams (joints and weights) for skinned meshes
        // Keyed by StaticMesh::uid, not the pointer: freed addresses are reused, and a new mesh
        // landing on a dead mesh's address must not inherit its GPU geometry. A dead mesh's entry
        // retires at the next Tick; reloads re-upload under the new object's uid.
        HashMap<u64, Entry> m_cache;
        Array<Retired> m_retired; // streams awaiting the frames in flight, then reused
    };

} // namespace foundation::render
