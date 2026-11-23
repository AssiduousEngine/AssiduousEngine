// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Render - :bind_group_cache partition
//
// The passes build a bind group over this frame's transient textures and keep it while the same
// textures come back. A group stays valid only while every view it binds is the same view of the
// same texture: the render graph's pool hands transients out per frame, and a pooled view can
// return over another texture (its generation tells). A cache that checks only some inputs keeps
// another frame's textures bound, for example a stale motion-vector buffer behind one of TAA's
// two history textures, which shows as edges flickering every other frame.

module;
#include "Core/Prelude.h"

export module foundation.render:bind_group_cache;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // What a bind group was built from: each view it binds that can change between frames, with
    // its texture's generation, and the one buffer it binds that can (a scene's sky lighting, with
    // the context's generation; none by default). Two are equal only when every input is.
    template <usize N>
    struct BindGroupInputs
    {
        rhi::TextureViewHandle views[N] = {};
        u64 generations[N] = {};
        rhi::BufferHandle buffer{};
        u64 bufferGeneration = 0;

        void Set(usize slot, rhi::TextureViewHandle view, u64 generation) noexcept
        {
            views[slot] = view;
            generations[slot] = generation;
        }
        void SetBuffer(rhi::BufferHandle b, u64 generation) noexcept
        {
            buffer = b;
            bufferGeneration = generation;
        }
        // No group can be built while an input is missing.
        [[nodiscard]] bool Complete() const noexcept
        {
            for (usize i = 0; i < N; ++i)
            {
                if (!views[i].IsValid())
                {
                    return false;
                }
            }
            return true;
        }
        [[nodiscard]] bool operator==(const BindGroupInputs&) const = default;
    };

    // Destroys the groups of `groups` whose view its owner has destroyed, and drops their entries.
    // A group binds its view for good, so once the view goes, nothing can ask for the group again.
    inline void DropGroupsOfDestroyedViews(rhi::Device& device,
                                           HashMap<rhi::TextureViewHandle, rhi::BindGroupHandle>& groups)
    {
        for (;;)
        {
            rhi::TextureViewHandle gone{};
            for (auto& kv : groups)
            {
                if (!device.IsLive(kv.key))
                {
                    gone = kv.key;
                    break;
                }
            }
            if (!gone.IsValid())
            {
                return;
            }
            device.DestroyBindGroup(*groups.Find(gone));
            groups.Remove(gone);
        }
    }

    // A pass's per-view state, one entry for each (view, frame in flight): a frame's views each get
    // their own, however many a frame has (split screen, render-texture cameras, probe faces), and
    // the table grows when a frame has more than it has seen. A fixed table indexed modulo its size
    // gave two views one entry, and the second destroyed a bind group the first's commands, recorded
    // earlier in the frame, still used.
    template <typename T>
    class ViewSlots
    {
    public:
        ViewSlots() = default; // on the engine's allocator, as BindGroupCache's map is
        explicit ViewSlots(IAllocator& allocator) : m_entries(allocator) {}

        // How many frames are in flight (each view has that many entries). Set before first use.
        void SetFramesInFlight(u32 frames) noexcept { m_frames = frames == 0 ? 1u : frames; }

        // `view`'s entry for frame `frame`, the table grown to hold the view.
        [[nodiscard]] T& At(u32 view, u32 frame)
        {
            const usize index = static_cast<usize>(view) * m_frames + (frame % m_frames);
            if (index >= m_entries.Size())
                m_entries.Resize((static_cast<usize>(view) + 1) * m_frames);
            return m_entries[index];
        }
        // How many views the table holds now.
        [[nodiscard]] u32 Views() const noexcept { return static_cast<u32>(m_entries.Size() / m_frames); }
        // Every entry, for the owner to release what each holds.
        [[nodiscard]] Span<T> All() noexcept { return m_entries.AsSpan(); }
        void Clear() { m_entries.Clear(); }

    private:
        Array<T> m_entries;
        u32 m_frames = 1;
    };

    // Bind groups keyed by the view that selects one (a history texture, an input that is stable
    // per size), each remembering the inputs it was built from.
    template <usize N>
    class BindGroupCache
    {
    public:
        // The group built for `key` from exactly `inputs`, or null. A group built for `key` from
        // anything else is forgotten and handed back in `stale` for the caller to destroy (the
        // device keeps it while a frame in flight uses it).
        [[nodiscard]] rhi::BindGroupHandle Find(rhi::TextureViewHandle key, const BindGroupInputs<N>& inputs,
                                           rhi::BindGroupHandle& stale)
        {
            stale = {};
            Entry* e = m_entries.Find(key);
            if (e == nullptr)
            {
                return {};
            }
            if (e->bg.IsValid() && e->inputs == inputs)
            {
                return e->bg;
            }
            stale = e->bg;
            m_entries.Remove(key);
            return {};
        }

        void Store(rhi::TextureViewHandle key, const BindGroupInputs<N>& inputs, rhi::BindGroupHandle bg)
        {
            m_entries.InsertOrAssign(key, Entry{bg, inputs});
        }

        // Every group still held, for the owner to destroy, and the cache emptied.
        template <typename Fn>
        void Release(Fn&& destroy)
        {
            for (auto& kv : m_entries)
            {
                if (kv.value.bg.IsValid())
                {
                    destroy(kv.value.bg);
                }
            }
            m_entries.Clear();
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            rhi::BindGroupHandle bg{};
            BindGroupInputs<N> inputs;
        };
        HashMap<rhi::TextureViewHandle, Entry> m_entries;
    };
}
