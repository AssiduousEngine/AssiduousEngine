// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Terrain - the `:heighttexture` partition.
//
// The GPU height texture. The renderer's VS fetches per-vertex height with textureLoad, so each
// heightfield's u16 grid is uploaded to an R16Uint 2D texture (one texel per sample, exact). Cached
// by the heightfield's id and version: two terrains referencing one heightfield share one texture,
// and a version bump (a sculpt re-upload) rebuilds it. Foundation::Heightfield stays a CPU grid
// (navigation and physics pay nothing on the GPU); the texture is owned here.

module;
#include "Core/Prelude.h"

export module terrain.engine:heighttexture;

import foundation.core;
import foundation.rhi;
import foundation.render;
import foundation.heightfield;

using namespace foundation::core;

export namespace terrain
{
    namespace rhi = foundation::rhi;
    namespace render = foundation::render;
    namespace heightfield = foundation::heightfield;

    /// Owns and caches the per-heightfield GPU height textures (R16Uint), keyed by id and version.
    class TerrainHeightTextureCache
    {
    public:
        TerrainHeightTextureCache() = default;
        TerrainHeightTextureCache(const TerrainHeightTextureCache&) = delete;
        TerrainHeightTextureCache& operator=(const TerrainHeightTextureCache&) = delete;

        /// The R16Uint height texture view for `hf` at `version`. Keyed by the heightfield's uid,
        /// never its pointer: a freed grid's address can be reused by a fresh one at an equal
        /// version, and a pointer key would serve the dead grid's texture. Two terrains
        /// referencing one heightfield share one texture, and in-memory heightfields with no
        /// resource guid work (the uid is per object, not per resource). Creates, uploads and
        /// caches on a miss; a stale version for the same heightfield rebuilds in place (the
        /// sculpt re-upload path). Returns null on a device failure or an empty grid.
        [[nodiscard]] rhi::TextureViewHandle GetOrCreate(rhi::Device& device,
                                                    const heightfield::Heightfield& hf, u64 version)
        {
            if (hf.IsEmpty())
            {
                return {};
            }
            for (Entry& entry : m_entries)
            {
                if (entry.key == hf.uid)
                {
                    if (entry.version == version && entry.view.IsValid())
                    {
                        return entry.view;
                    }
                    Destroy(device, entry); // stale version: rebuild in place
                    if (!Build(device, hf, entry))
                    {
                        return {};
                    }
                    entry.version = version;
                    return entry.view;
                }
            }
            Entry fresh;
            fresh.key = hf.uid;
            fresh.version = version;
            if (!Build(device, hf, fresh))
            {
                return {};
            }
            m_entries.PushBack(fresh);
            return m_entries[m_entries.Size() - 1].view;
        }

        /// Drops every entry. This runs at scene destroy as well as at shutdown (stopping
        /// play-in-editor destroys the run's scenes mid frame loop), so a live entry may still
        /// sit in a submitted frame's descriptor set: the device keeps the GPU objects until every
        /// frame in flight is done with them (freeing them in place was VUID 01026).
        void Clear(rhi::Device& device)
        {
            for (Entry& entry : m_entries)
            {
                Destroy(device, entry);
            }
            m_entries.Clear();
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            u64 key = 0; // Heightfield::uid, never a pointer (address reuse aliases)
            u64 version = 0;
            rhi::TextureHandle texture{};
            rhi::TextureViewHandle view{};
        };

        [[nodiscard]] static bool Build(rhi::Device& device, const heightfield::Heightfield& hf,
                                        Entry& out)
        {
            const u32 n = static_cast<u32>(hf.Size());
            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::R16Uint;
            td.width = n;
            td.height = n;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"terrain.height";
            if (!device.CreateTexture(td, out.texture).IsOk() || !out.texture.IsValid())
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::R16Uint;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!device.CreateTextureView(out.texture, vd, out.view).IsOk() || !out.view.IsValid())
            {
                device.DestroyTexture(out.texture);
                return false;
            }
            if (rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* batch = nullptr;
                if (queue->CreateTransferBatch(batch).IsOk() && batch != nullptr)
                {
                    const Span<const heightfield::Height> samples = hf.Samples();
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = n * static_cast<u32>(sizeof(heightfield::Height));
                    layout.rowsPerImage = n;
                    batch->WriteTexture(
                        out.texture,
                        Span<const u8>(reinterpret_cast<const u8*>(samples.Data()),
                                       samples.Size() * sizeof(heightfield::Height)),
                        layout, rhi::Extent3D{n, n, 1});
                    (void)batch->Submit();
                    queue->DestroyTransferBatch(batch);
                }
            }
            return true;
        }

        static void Destroy(rhi::Device& device, Entry& entry)
        {
            if (entry.view.IsValid())
            {
                device.DestroyTextureView(entry.view);
                entry.view = {};
            }
            if (entry.texture.IsValid())
            {
                device.DestroyTexture(entry.texture);
                entry.texture = {};
            }
        }

        Array<Entry> m_entries;
    };
}
