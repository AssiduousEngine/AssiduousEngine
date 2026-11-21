// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:source` partition.
//
// SequenceSource presents a sequence document to the toolkit's dopesheet and curve view, whatever
// its track kinds: each kind says what it holds as key channels or sections (SequenceTrack's
// editing calls), so a kind a domain or a plugin adds edits here with no change. The rows fold by
// path: a binding's name, then the track (its name, or its property for a property track, or its
// kind's name), then the channel when the track has more than one ("Hero.Transform.position.x",
// "Hero.Clip", "Sequence.Events"); tracks bound to no one sit under "Sequence". A track with no key
// channels is one row of sections. The curve view shows the curves of the tracks the panel shows
// (a property's parts, a fade's amount). Every edit goes through the document: a drag is a gesture
// that previews from where it began and becomes one undo step when it ends; a deletion or an
// insertion is one step.

module;
#include "Core/Prelude.h"

export module editor.sequence:source;

import foundation.core;
import foundation.sequence;
import foundation.gui;
import foundation.gui.toolkit;
import :document;

using namespace foundation::core;

export namespace editor
{
    class SequenceSource final : public foundation::gui::ITimelineSource, public foundation::gui::ICurveSource
    {
    public:
        /// A row's channel: its track and, for keys, which of the track's key channels (-1 for the
        /// track's sections).
        struct Channel
        {
            usize Track = 0;
            i32 Keys = -1;
        };

        explicit SequenceSource(SequenceDocument& document);
        ~SequenceSource() override;
        SequenceSource(const SequenceSource&) = delete;
        SequenceSource& operator=(const SequenceSource&) = delete;

        /// Sets the tracks whose curves the curve view shows; empty shows none.
        void ShowTracks(Span<const usize> tracks);
        [[nodiscard]] Span<const usize> ShownTracks() const noexcept { return m_shown.AsSpan(); }

        /// Called by the owner from the document's OnChanged: reads the channels again and notifies
        /// the views.
        void Refresh();

        [[nodiscard]] Span<const Channel> Channels() const noexcept { return m_channels.AsSpan(); }
        /// The row a track's name gives it ("Transform.position", "Clip", "Events").
        [[nodiscard]] static String TrackLabel(const foundation::sequence::SequenceTrack& track);

        // === ITimelineSource ===
        [[nodiscard]] usize ChannelCount() const override { return m_channels.Size(); }
        [[nodiscard]] String GetChannelPath(usize channel) const override;
        void GetKeyTimes(usize channel, Array<f32>& out) const override;
        void BeginKeysMove() override;
        void PreviewKeysMove(Span<const foundation::gui::TimelineKeyRef> keys, f32 delta, Array<foundation::gui::TimelineKeyRef>& moved) override;
        void CommitKeysMove() override;
        void CancelKeysMove() override;
        bool DeleteKeys(Span<const foundation::gui::TimelineKeyRef> keys) override;
        bool InsertKey(usize channel, f32 time, foundation::gui::TimelineKeyRef& made) override;
        [[nodiscard]] bool IsSectionChannel(usize channel) const override;
        void GetSections(usize channel, Array<foundation::gui::TimelineSection>& out) const override;
        void BeginSectionEdit() override;
        void PreviewSectionEdit(const foundation::gui::TimelineSectionRef& section, f32 start, f32 length) override;
        void CommitSectionEdit() override;
        void CancelSectionEdit() override;
        bool DeleteSection(const foundation::gui::TimelineSectionRef& section) override;

        // === ICurveSource ===
        [[nodiscard]] usize CurveCount() const override { return m_curves.Size(); }
        [[nodiscard]] foundation::gui::CurveInfo GetCurveInfo(usize curve) const override;
        [[nodiscard]] Span<const CurveKey> GetKeys(usize curve) const override;
        void BeginKeysEdit() override;
        void PreviewKeys(usize curve, Span<const CurveKey> keys) override;
        void CommitKeysEdit() override;
        void CancelKeysEdit() override;

    private:
        void Rebuild();
        [[nodiscard]] foundation::sequence::SequenceTrack* TrackOf(const Channel& channel) const;

        SequenceDocument* m_document;
        Array<Channel> m_channels; // the dopesheet's rows that hold keys or sections
        Array<Channel> m_curves;   // the curve view's: the shown tracks' curve channels
        Array<usize> m_shown;
    };
}
