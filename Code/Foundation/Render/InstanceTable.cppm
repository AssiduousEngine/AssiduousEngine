// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:instance_table` partition.
///
/// The persistent instance table: one GPU-only buffer of every drawn mesh's per-object record
/// (world, last frame's world, tint), kept across frames. A frame writes each mesh's record once,
/// before any pass, and only when it changed; passes read the records by slot. The CPU keeps a
/// mirror of every record, which is also the motion history: a record's previous world is the
/// mirror's world before this frame wrote it.

module;
#include "Core/Prelude.h"

export module foundation.render:instance_table;

import foundation.core;
import foundation.rhi;
import :resources;

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // One drawn instance (StructuredBuffer<GpuInstance> Instances in forward_vs.hlsli,
    // shadow_depth_vs.hlsli and pick_ids_vs.hlsli): the instance table's records, and a MultiMesh
    // set's own instances.
    struct GpuInstance
    {
        Float4x4 world;
        Float4x4 prevWorld;
        Color tint;
    }; // 144 (StructuredBuffer element)

    /// What a frame's upload did, for tests and measurements.
    struct InstanceTableFrameStats
    {
        u32 written = 0;     // records that changed (each written once)
        u32 copies = 0;      // copy commands recorded
        u64 bytes = 0;       // bytes copied into the table
        u32 transitions = 0; // buffer transitions recorded
        bool grew = false;   // the table was made again, larger, and filled whole
    };

    class InstanceTable
    {
    public:
        /// A slot no record holds.
        static constexpr u32 kNoSlot = 0xFFFFFFFFu;
        /// A scene not extracted for this many frames drops its slots (an editor page closed, a
        /// run's scene gone); one extracted again later starts over, its first frame without motion.
        static constexpr u32 kSceneDropFrames = 120;
        /// Above this many runs of changed slots, one copy covers them all, from the first to the
        /// last: scattered changes would otherwise record a copy each.
        static constexpr u32 kMaxCopyRuns = 64;
        static constexpr u32 kInitialSlots = 1024;

        InstanceTable(IAllocator& allocator, rhi::Device& device, u32 framesInFlight)
            : m_allocator(&allocator), m_device(&device),
              m_staging(device, framesInFlight, sizeof(GpuInstance), rhi::BufferUsage::CopySrc,
                        u8"mesh.instances.staging"),
              m_mirror(allocator), m_changedFrame(allocator), m_settled(allocator), m_freeSlots(allocator),
              m_frameSlots(allocator), m_changed(allocator), m_scenes(allocator)
        {
        }
        ~InstanceTable()
        {
            if (m_buffer.IsValid())
            {
                m_device->DestroyBuffer(m_buffer);
            }
        }
        InstanceTable(const InstanceTable&) = delete;
        InstanceTable& operator=(const InstanceTable&) = delete;

        /// A frame begins: the slots last frame gave items with no entity are free again.
        void BeginFrame(u32 frameIndex)
        {
            ++m_frame;
            m_frameIndex = frameIndex;
            for (u32 slot : m_frameSlots)
            {
                FreeSlot(slot);
            }
            m_frameSlots.Clear();
            m_changed.Clear();
            m_stats = {};
        }

        /// What Claim found for an item: its slot, and whether its record is still to be brought up
        /// to date (Refresh). A new slot's record is written by Claim; an entity met already this
        /// frame (a mesh split into items by category) keeps what its first item made.
        struct Claimed
        {
            u32 slot = kNoSlot;
            bool refresh = false;
        };

        /// The slot of the item's entity in `scene`: a new one when the entity has none or another
        /// entity held its index, written at once with no motion on its first frame. An item with no
        /// entity (`entityId` 0) takes a slot for this frame alone. Serial: it hands out slots.
        [[nodiscard]] Claimed Claim(u64 scene, u64 entityId, const Float4x4& world, const Color& tint)
        {
            if (entityId == 0)
            {
                const u32 slot = AllocateSlot();
                m_frameSlots.PushBack(slot);
                Set(slot, GpuInstance{world, world, tint}, true);
                MarkChanged(slot);
                return Claimed{slot, false};
            }
            Scene& slots = SceneOf(scene);
            const u32 index = static_cast<u32>(entityId);
            const u32 generation = static_cast<u32>(entityId >> 32);
            if (index >= slots.entries.Size())
            {
                slots.entries.Resize(index + 1u);
            }
            Entry& entry = slots.entries[index];
            if (entry.slot != kNoSlot && entry.generation == generation && entry.seenFrame == m_frame)
            {
                return Claimed{entry.slot, false};
            }
            entry.seenFrame = m_frame;
            if (entry.slot == kNoSlot || entry.generation != generation)
            {
                if (entry.slot != kNoSlot)
                {
                    FreeSlot(entry.slot); // another entity held this index
                }
                entry.slot = AllocateSlot();
                entry.generation = generation;
                Set(entry.slot, GpuInstance{world, world, tint}, true);
                MarkChanged(entry.slot);
                return Claimed{entry.slot, false};
            }
            return Claimed{entry.slot, true};
        }

        /// Brings a claimed slot's record up to date: written when its world or tint moved, or once
        /// more as its previous world catches up; true when it was written (MarkChanged it, in item
        /// order). A mesh at rest costs one compare of what it carries, and nothing is built. Touches
        /// only that slot's record, so distinct slots refresh on any threads at once.
        [[nodiscard]] bool Refresh(u32 slot, const Float4x4& world, const Color& tint)
        {
            const GpuInstance& kept = m_mirror[slot];
            const bool sameWorld = MemCompare(&kept.world, &world, sizeof(Float4x4)) == 0;
            if (sameWorld && MemCompare(&kept.tint, &tint, sizeof(Color)) == 0)
            {
                if (m_settled[slot] != 0)
                {
                    return false;
                }
                Set(slot, GpuInstance{world, world, tint}, true);
                return true;
            }
            Set(slot, GpuInstance{world, kept.world, tint}, sameWorld);
            return true;
        }

        /// Adds a written slot to this frame's copy (once). Serial; the order given is the order the
        /// slots are copied in, so items in slot order make few runs.
        void MarkChanged(u32 slot)
        {
            if (m_changedFrame[slot] == m_frame)
            {
                return;
            }
            m_changedFrame[slot] = m_frame;
            m_changed.PushBack(slot);
            ++m_stats.written;
        }

        /// Claim, Refresh and MarkChanged in one, for one item at a time.
        [[nodiscard]] u32 Write(u64 scene, u64 entityId, const Float4x4& world, const Color& tint)
        {
            const Claimed claimed = Claim(scene, entityId, world, tint);
            if (claimed.refresh && Refresh(claimed.slot, world, tint))
            {
                MarkChanged(claimed.slot);
            }
            return claimed.slot;
        }

        /// Once every scene of the frame is written: frees the slots of entities a written scene no
        /// longer drew (destroyed, hidden, inactive), and drops the scenes not written for
        /// kSceneDropFrames. A scene not written this frame (a hidden editor page) keeps its slots.
        void EndScenes()
        {
            Array<u64> dropped(*m_allocator);
            for (auto& kv : m_scenes)
            {
                Scene& slots = kv.value;
                if (slots.writtenFrame == m_frame)
                {
                    for (Entry& entry : slots.entries)
                    {
                        if (entry.slot != kNoSlot && entry.seenFrame != m_frame)
                        {
                            FreeSlot(entry.slot);
                            entry = Entry{};
                        }
                    }
                }
                else if (m_frame - slots.writtenFrame > kSceneDropFrames)
                {
                    for (Entry& entry : slots.entries)
                    {
                        if (entry.slot != kNoSlot)
                        {
                            FreeSlot(entry.slot);
                        }
                    }
                    dropped.PushBack(kv.key);
                }
            }
            for (u64 scene : dropped)
            {
                (void)m_scenes.Remove(scene);
            }
            m_lastScene = ~0ull;
            m_lastSceneSlots = nullptr;
        }

        /// Copies this frame's changed records into the table through the staging ring, between a
        /// transition from shader reads to the copy and back: the table is one region every frame
        /// writes, and an earlier frame's passes may still be reading it on the GPU. A frame with
        /// nothing changed records nothing. A table outgrown is made again, larger, and filled whole.
        void Upload(rhi::CommandEncoder& encoder)
        {
            const bool grow = !m_buffer.IsValid() || m_used > m_capacity;
            if (grow)
            {
                if (!Grow())
                {
                    return;
                }
                m_stats.grew = true;
            }
            if (!grow && m_changed.IsEmpty())
            {
                return;
            }
            // The runs to copy: the whole used range after a growth, else the changed slots in the order
            // they were marked (items in slot order make few runs), neighbours merged, or one span over
            // them all when they scatter too far.
            Array<Run> runs(*m_allocator);
            if (grow)
            {
                if (m_used > 0)
                {
                    runs.PushBack(Run{0, m_used, 0});
                }
            }
            else
            {
                AppendRuns(runs);
                if (runs.Size() > kMaxCopyRuns)
                {
                    m_changed.Sort([](u32 a, u32 b) { return a < b; });
                    runs.Clear();
                    AppendRuns(runs);
                }
                if (runs.Size() > kMaxCopyRuns)
                {
                    const u32 first = runs[0].first;
                    const u32 end = runs[runs.Size() - 1].first + runs[runs.Size() - 1].count;
                    runs.Clear();
                    runs.PushBack(Run{first, end - first, 0});
                }
            }
            if (runs.IsEmpty())
            {
                return;
            }
            u32 total = 0;
            for (Run& run : runs)
            {
                run.staged = total;
                total += run.count;
            }
            if (!m_staging.Reserve(total < kInitialSlots ? kInitialSlots : total))
            {
                return;
            }
            m_staging.BeginFrame(m_frameIndex);
            const DynamicUniformRing::Range staged = m_staging.AllocateRange(total);
            if (!staged.ok)
            {
                return;
            }
            StageRecords(Span<const Run>(runs.Data(), runs.Size()), static_cast<GpuInstance*>(staged.ptr), total);
            encoder.TransitionBuffer(m_buffer, grow ? rhi::ResourceState::Undefined : rhi::ResourceState::ShaderRead,
                                     rhi::ResourceState::CopyDst);
            ++m_stats.transitions;
            for (const Run& run : runs)
            {
                const u64 bytes = static_cast<u64>(run.count) * sizeof(GpuInstance);
                encoder.CopyBufferToBuffer(m_staging.Buffer(),
                                           staged.byteOffset + static_cast<u64>(run.staged) * sizeof(GpuInstance), m_buffer,
                                           static_cast<u64>(run.first) * sizeof(GpuInstance), bytes);
                ++m_stats.copies;
                m_stats.bytes += bytes;
            }
            encoder.TransitionBuffer(m_buffer, rhi::ResourceState::CopyDst, rhi::ResourceState::ShaderRead);
            ++m_stats.transitions;
        }

        /// The table the passes bind (whole, as `Instances`); remade when it grows.
        [[nodiscard]] rhi::BufferHandle Buffer() const noexcept { return m_buffer; }
        /// Moves each time the table is made again, so a bind group over it is made again too.
        [[nodiscard]] u32 Generation() const noexcept { return m_generation; }
        [[nodiscard]] u32 Capacity() const noexcept { return m_capacity; }
        /// Slots handed out and not freed.
        [[nodiscard]] u32 LiveSlots() const noexcept { return m_used - static_cast<u32>(m_freeSlots.Size()); }
        /// The record a slot holds, as the mirror has it (what the table holds after Upload).
        [[nodiscard]] const GpuInstance& Record(u32 slot) const noexcept { return m_mirror[slot]; }
        [[nodiscard]] const InstanceTableFrameStats& FrameStats() const noexcept { return m_stats; }

    private:
        struct Entry
        {
            u32 slot = kNoSlot;
            u32 generation = 0; // the entity's, so another entity that takes its index gets its own slot
            u64 seenFrame = 0;  // the frame that last wrote it
        };
        struct Scene
        {
            Array<Entry> entries; // by entity index
            u64 writtenFrame = 0;
        };
        struct Run
        {
            u32 first = 0;  // its first slot
            u32 count = 0;
            u32 staged = 0; // its first record's place in this frame's staging
        };

        Scene& SceneOf(u64 scene)
        {
            if (scene != m_lastScene || m_lastSceneSlots == nullptr)
            {
                m_lastSceneSlots = m_scenes.Find(scene);
                if (m_lastSceneSlots == nullptr)
                {
                    m_lastSceneSlots = &m_scenes.InsertOrAssign(scene, Scene{Array<Entry>(*m_allocator), 0});
                }
                m_lastScene = scene;
            }
            m_lastSceneSlots->writtenFrame = m_frame;
            return *m_lastSceneSlots;
        }

        u32 AllocateSlot()
        {
            if (!m_freeSlots.IsEmpty())
            {
                const u32 slot = m_freeSlots[m_freeSlots.Size() - 1];
                m_freeSlots.PopBack();
                return slot;
            }
            const u32 slot = m_used++;
            m_mirror.Resize(m_used);
            m_changedFrame.Resize(m_used);
            m_settled.Resize(m_used);
            return slot;
        }

        void FreeSlot(u32 slot) { m_freeSlots.PushBack(slot); }

        // A record into the mirror. `settled`: its previous world equals its world, so it needs no
        // write until it moves again.
        void Set(u32 slot, const GpuInstance& record, bool settled)
        {
            m_mirror[slot] = record;
            m_settled[slot] = settled ? 1 : 0;
        }

        void AppendRuns(Array<Run>& runs) const
        {
            for (u32 slot : m_changed)
            {
                if (!runs.IsEmpty() && runs[runs.Size() - 1].first + runs[runs.Size() - 1].count == slot)
                {
                    ++runs[runs.Size() - 1].count;
                }
                else
                {
                    runs.PushBack(Run{slot, 1, 0});
                }
            }
        }

        // The runs' records from the mirror into staging, in pieces the job system copies at once
        // when there are enough of them: a frame where every mesh moved stages megabytes.
        void StageRecords(Span<const Run> runs, GpuInstance* staged, u32 total)
        {
            static constexpr u32 kPieceRecords = 4096;
            const auto copyRange = [this, staged](u32 slot, u32 at, u32 count)
            {
                MemCopy(staged + at, &m_mirror[slot], static_cast<usize>(count) * sizeof(GpuInstance));
            };
            if (total < 2u * kPieceRecords || !HasGlobalJobSystem())
            {
                for (const Run& run : runs)
                {
                    copyRange(run.first, run.staged, run.count);
                }
                return;
            }
            Array<Run> pieces(*m_allocator);
            for (const Run& run : runs)
            {
                for (u32 done = 0; done < run.count; done += kPieceRecords)
                {
                    const u32 count = run.count - done < kPieceRecords ? run.count - done : kPieceRecords;
                    pieces.PushBack(Run{run.first + done, count, run.staged + done});
                }
            }
            GlobalJobs().ParallelFor(static_cast<u32>(pieces.Size()),
                                     [&](u32 i) { copyRange(pieces[i].first, pieces[i].staged, pieces[i].count); }, 1);
        }

        bool Grow()
        {
            u32 capacity = m_capacity < kInitialSlots ? kInitialSlots : m_capacity;
            while (capacity < m_used)
            {
                capacity *= 2u;
            }
            if (m_buffer.IsValid())
            {
                m_device->DestroyBuffer(m_buffer); // the device keeps it while frames in flight read it
            }
            rhi::BufferDesc desc{};
            desc.size = static_cast<u64>(capacity) * sizeof(GpuInstance);
            desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
            desc.memory = rhi::MemoryLocation::GpuOnly;
            desc.label = u8"mesh.instances";
            if (!m_device->CreateBuffer(desc, m_buffer).IsOk())
            {
                m_buffer = {};
                m_capacity = 0;
                return false;
            }
            m_capacity = capacity;
            ++m_generation;
            return true;
        }

        IAllocator* m_allocator;
        rhi::Device* m_device;
        DynamicUniformRing m_staging; // the changed records, a region per frame in flight
        rhi::BufferHandle m_buffer{};
        u32 m_capacity = 0;
        u32 m_generation = 0;
        u32 m_used = 0; // slots ever handed out; the free ones are in m_freeSlots
        u32 m_frameIndex = 0;
        u64 m_frame = 0;
        Array<GpuInstance> m_mirror;   // every slot's record, as the table holds it after Upload
        Array<u64> m_changedFrame;     // by slot: the frame that last wrote it (0: never)
        Array<u8> m_settled;           // by slot: 1 when its previous world equals its world
        Array<u32> m_freeSlots;
        Array<u32> m_frameSlots;       // this frame's slots for items with no entity
        Array<u32> m_changed;          // the slots this frame wrote, in the order it wrote them
        HashMap<u64, Scene> m_scenes;  // by scene serial
        u64 m_lastScene = ~0ull;       // the scene the last Write found, kept for the next
        Scene* m_lastSceneSlots = nullptr;
        InstanceTableFrameStats m_stats;
    };
    static_assert(sizeof(GpuInstance) == 144, "StructuredBuffer<GpuInstance> element layout drift");
}
