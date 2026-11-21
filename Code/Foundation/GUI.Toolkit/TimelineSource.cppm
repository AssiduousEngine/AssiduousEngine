// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :timeline_source partition
//
// What a dopesheet reads: channels named by paths ("Lamp.color.r"), which the dopesheet folds into
// rows by their segments (component, property, channel), and each channel's key times. A key is
// identified by (channel, index). An edit through the source returns the keys' new references, so a
// selection survives the source sorting its keys again. A move is a gesture, as in the other
// sources: it begins, previews from where the keys were, and then commits once or cancels. A
// deletion or an insertion is one change each. The undo belongs to the source.
//
// A channel may hold sections instead of keys: spans of time (a clip, a sound, a shot), which the
// dopesheet draws as bars on the channel's row and moves or trims. A source of keys alone leaves
// the section calls as they are.

module;
#include "Core/Prelude.h"

export module foundation.gui.toolkit:timeline_source;

import foundation.core;
import foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// Identifies a key of a timeline by its channel and its index among that channel's keys
    /// (sorted by time).
    struct TimelineKeyRef
    {
        usize Channel = 0;
        usize Index = 0;
        [[nodiscard]] bool operator==(const TimelineKeyRef&) const noexcept = default;
    };

    /// A span on a section channel: where it starts, how long it is, and what it says on its bar.
    struct TimelineSection
    {
        f32 Start = 0.0f;
        f32 Length = 0.0f;
        core::String Label;
    };

    /// Identifies a section by its channel and its index among that channel's sections.
    struct TimelineSectionRef
    {
        usize Channel = 0;
        usize Index = 0;
        [[nodiscard]] bool operator==(const TimelineSectionRef&) const noexcept = default;
    };

    /// The notifications a timeline source sends its views.
    class ITimelineSourceClient
    {
    public:
        virtual ~ITimelineSourceClient() = default;
        /// Called when the channels or their keys changed; the view reads them again.
        virtual void OnTimelineChanged() = 0;
    };

    class ITimelineSource
    {
    public:
        virtual ~ITimelineSource() = default;

        [[nodiscard]] virtual usize ChannelCount() const = 0;
        /// Returns the channel's path, with segments split by '.', which decides the rows it folds
        /// into.
        [[nodiscard]] virtual core::String GetChannelPath(usize channel) const = 0;
        /// Fills `out` with the channel's key times, sorted.
        virtual void GetKeyTimes(usize channel, core::Array<f32>& out) const = 0;

        /// Begins a move. Each preview moves the keys from where they are now, and a cancel restores
        /// them there.
        virtual void BeginKeysMove() = 0;
        /// Moves `keys` (as they were when the move began) by `delta` seconds, live, and fills `moved`
        /// with where they are now.
        virtual void PreviewKeysMove(core::Span<const TimelineKeyRef> keys, f32 delta, core::Array<TimelineKeyRef>& moved) = 0;
        /// Ends the move as last previewed, as one change.
        virtual void CommitKeysMove() = 0;
        /// Cancels the move and puts the keys back as they were.
        virtual void CancelKeysMove() = 0;
        /// Deletes `keys` as one change. Returns false when none was deleted.
        virtual bool DeleteKeys(core::Span<const TimelineKeyRef> keys) = 0;
        /// Adds a key to `channel` at `time` as one change. The source picks its value, usually the
        /// channel's value there. Sets `made` to where the key went, and returns false when it
        /// cannot add one (the channel is full or locked).
        virtual bool InsertKey(usize channel, f32 time, TimelineKeyRef& made) = 0;

        // === Sections (a source of keys alone keeps these) ===
        /// Whether the channel holds sections rather than keys.
        [[nodiscard]] virtual bool IsSectionChannel(usize channel) const
        {
            (void)channel;
            return false;
        }
        /// Fills `out` with the channel's sections, in the order the source keeps them.
        virtual void GetSections(usize channel, core::Array<TimelineSection>& out) const
        {
            (void)channel;
            out.Clear();
        }
        /// Begins a move or a trim, as a key move does: each preview starts from where the section
        /// was, and a cancel puts it back.
        virtual void BeginSectionEdit() {}
        /// Sets the section's start and length, live. A trim of its start moves its start and
        /// keeps its end; the source decides what moves inside (a clip's offset, so what plays
        /// stays where it was).
        virtual void PreviewSectionEdit(const TimelineSectionRef& section, f32 start, f32 length)
        {
            (void)section;
            (void)start;
            (void)length;
        }
        virtual void CommitSectionEdit() {}
        virtual void CancelSectionEdit() {}
        /// Deletes the section as one change. Returns false when it cannot.
        virtual bool DeleteSection(const TimelineSectionRef& section)
        {
            (void)section;
            return false;
        }

        void AddTimelineClient(ITimelineSourceClient* client) { m_timelineClients.PushBack(client); }
        void RemoveTimelineClient(ITimelineSourceClient* client)
        {
            for (usize i = 0; i < m_timelineClients.Size(); ++i)
                if (m_timelineClients[i] == client)
                {
                    m_timelineClients.RemoveAt(i);
                    return;
                }
        }

    protected:
        void NotifyTimelineChanged()
        {
            for (usize i = 0; i < m_timelineClients.Size(); ++i)
                m_timelineClients[i]->OnTimelineChanged();
        }

    private:
        core::Array<ITimelineSourceClient*> m_timelineClients; // non-owning: each view leaves before it goes away
    };
}
