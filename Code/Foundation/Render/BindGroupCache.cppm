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
import :resources; // GpuRetireQueue

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
        rhi::TextureView* views[N] = {};
        u64 generations[N] = {};
        rhi::Buffer* buffer = nullptr;
        u64 bufferGeneration = 0;

        void Set(usize slot, rhi::TextureView* view, u64 generation) noexcept
        {
            views[slot] = view;
            generations[slot] = generation;
        }
        void SetBuffer(rhi::Buffer* b, u64 generation) noexcept
        {
            buffer = b;
            bufferGeneration = generation;
        }
        // No group can be built while an input is missing.
        [[nodiscard]] bool Complete() const noexcept
        {
            for (usize i = 0; i < N; ++i)
            {
                if (views[i] == nullptr)
                {
                    return false;
                }
            }
            return true;
        }
        [[nodiscard]] bool operator==(const BindGroupInputs&) const = default;
    };

    // Disposes of a bind group its owner has replaced, such as one a cache handed back as stale. A
    // frame still in flight may have recorded it, so it is retired into `retire` and freed once
    // every such frame has aged out; with no queue wired (a pass used on its own, as the backend
    // probes do), the GPU is idled first, as GpuRetireQueue's consumers do.
    inline void RetireBindGroup(rhi::Device& device, GpuRetireQueue* retire, rhi::BindGroup* group)
    {
        if (group == nullptr)
        {
            return;
        }
        if (retire != nullptr)
        {
            retire->Retire(group);
            return;
        }
        device.WaitIdle();
        device.DestroyBindGroup(group);
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
        // anything else is forgotten and handed back in `stale` for the caller to retire
        // (RetireBindGroup: a frame in flight may still use it).
        [[nodiscard]] rhi::BindGroup* Find(rhi::TextureView* key, const BindGroupInputs<N>& inputs,
                                           rhi::BindGroup*& stale)
        {
            stale = nullptr;
            Entry* e = m_entries.Find(key);
            if (e == nullptr)
            {
                return nullptr;
            }
            if (e->bg != nullptr && e->inputs == inputs)
            {
                return e->bg;
            }
            stale = e->bg;
            m_entries.Remove(key);
            return nullptr;
        }

        void Store(rhi::TextureView* key, const BindGroupInputs<N>& inputs, rhi::BindGroup* bg)
        {
            m_entries.InsertOrAssign(key, Entry{bg, inputs});
        }

        // Every group still held, for the owner to destroy, and the cache emptied.
        template <typename Fn>
        void Release(Fn&& destroy)
        {
            for (auto& kv : m_entries)
            {
                if (kv.value.bg != nullptr)
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
            rhi::BindGroup* bg = nullptr;
            BindGroupInputs<N> inputs;
        };
        HashMap<rhi::TextureView*, Entry> m_entries;
    };
}
