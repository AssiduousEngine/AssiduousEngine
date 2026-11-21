// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:source` partition (implementation).

module;
#include "Core/Prelude.h"

module editor.sequence;

import foundation.core;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;
namespace sequence = foundation::sequence;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        constexpr StringView kUnbound = u8"Sequence"; // the group of tracks bound to no one

        // The kind's name, as its reflection gives it ("Clip", "Camera cut").
        [[nodiscard]] String KindName(const sequence::SequenceTrack& track)
        {
            const TypeInfo* type = track.GetType();
            if (type == nullptr)
                return String(u8"Track");
            if (const Variant* shown = FindAttribute(*type, "displayName"); shown != nullptr && shown->Is<String>())
                return shown->Get<String>();
            return String(StringView(reinterpret_cast<const utf8char*>(type->name)));
        }

        // A key ref's keys grouped by channel, each with its indices.
        struct ChannelKeys
        {
            usize channel = 0;
            Array<usize> indices;
        };
        [[nodiscard]] Array<ChannelKeys> ByChannel(Span<const gui::TimelineKeyRef> keys)
        {
            Array<ChannelKeys> grouped;
            for (const gui::TimelineKeyRef& key : keys)
            {
                ChannelKeys* found = nullptr;
                for (ChannelKeys& group : grouped)
                    if (group.channel == key.Channel)
                        found = &group;
                if (found == nullptr)
                {
                    grouped.PushBack(ChannelKeys{key.Channel, {}});
                    found = &grouped[grouped.Size() - 1];
                }
                found->indices.PushBack(key.Index);
            }
            return grouped;
        }
    }

    SequenceSource::SequenceSource(SequenceDocument& document) : m_document(&document)
    {
        Rebuild();
    }

    SequenceSource::~SequenceSource() = default;

    String SequenceSource::TrackLabel(const sequence::SequenceTrack& track)
    {
        if (!track.name.IsEmpty())
            return track.name;
        if (const auto* property = Cast<sequence::PropertySequenceTrack>(&track); property != nullptr && !property->property.componentType.IsEmpty())
            return Format(u8"{}.{}", property->property.componentType, property->property.propertyPath);
        return KindName(track);
    }

    void SequenceSource::ShowTracks(Span<const usize> tracks)
    {
        m_shown.Clear();
        for (const usize track : tracks)
            m_shown.PushBack(track);
        Refresh();
    }

    void SequenceSource::Refresh()
    {
        Rebuild();
        NotifyTimelineChanged();
        NotifyCurvesChanged();
        NotifyTimeMarkersChanged();
    }

    void SequenceSource::Rebuild()
    {
        const sequence::Sequence& sequence = m_document->Sequence();
        m_channels.Clear();
        // By binding, in the cast's order, then the tracks bound to no one.
        for (i32 binding = 0; binding <= static_cast<i32>(sequence.bindings.Size()); ++binding)
        {
            const i32 wanted = binding < static_cast<i32>(sequence.bindings.Size()) ? binding : -1;
            for (usize t = 0; t < sequence.tracks.Size(); ++t)
            {
                const sequence::SequenceTrack& track = *sequence.tracks[t];
                const bool bound = track.binding >= 0 && track.binding < static_cast<i32>(sequence.bindings.Size());
                if ((bound ? track.binding : -1) != wanted)
                    continue;
                const usize keyChannels = track.KeyChannelCount();
                if (keyChannels == 0)
                {
                    m_channels.PushBack(Channel{t, -1});
                    continue;
                }
                for (usize c = 0; c < keyChannels; ++c)
                    m_channels.PushBack(Channel{t, static_cast<i32>(c)});
            }
        }
        for (usize i = m_shown.Size(); i > 0; --i)
            if (m_shown[i - 1] >= sequence.tracks.Size())
                m_shown.RemoveAt(i - 1);
        m_curves.Clear();
        for (const usize t : m_shown)
            for (usize c = 0; c < sequence.tracks[t]->KeyChannelCount(); ++c)
                if (sequence.tracks[t]->KeyCurve(c) != nullptr)
                    m_curves.PushBack(Channel{t, static_cast<i32>(c)});
    }

    sequence::SequenceTrack* SequenceSource::TrackOf(const Channel& channel) const
    {
        const sequence::Sequence& sequence = m_document->Sequence();
        return channel.Track < sequence.tracks.Size() ? sequence.tracks[channel.Track].Get() : nullptr;
    }

    String SequenceSource::GetChannelPath(usize channel) const
    {
        if (channel >= m_channels.Size())
            return String{};
        const sequence::Sequence& sequence = m_document->Sequence();
        const Channel& at = m_channels[channel];
        const sequence::SequenceTrack& track = *sequence.tracks[at.Track];
        const bool bound = track.binding >= 0 && track.binding < static_cast<i32>(sequence.bindings.Size());
        const StringView group = bound ? sequence.bindings[static_cast<usize>(track.binding)].name.AsView() : kUnbound;
        const String label = TrackLabel(track);
        if (at.Keys < 0 || track.KeyChannelCount() == 1)
            return Format(u8"{}.{}", group, label);
        return Format(u8"{}.{}.{}", group, label, track.KeyChannelName(static_cast<usize>(at.Keys)));
    }

    void SequenceSource::GetKeyTimes(usize channel, Array<f32>& out) const
    {
        out.Clear();
        if (channel >= m_channels.Size() || m_channels[channel].Keys < 0)
            return;
        if (const sequence::SequenceTrack* track = TrackOf(m_channels[channel]))
            track->GetKeyTimes(static_cast<usize>(m_channels[channel].Keys), out);
    }

    void SequenceSource::BeginKeysMove()
    {
        m_document->BeginGesture();
    }

    void SequenceSource::PreviewKeysMove(Span<const gui::TimelineKeyRef> keys, f32 delta, Array<gui::TimelineKeyRef>& moved)
    {
        m_document->RestoreGestureStart(); // each preview moves the keys from where they began
        moved.Clear();
        for (const ChannelKeys& group : ByChannel(keys))
        {
            if (group.channel >= m_channels.Size() || m_channels[group.channel].Keys < 0)
                continue;
            sequence::SequenceTrack* track = TrackOf(m_channels[group.channel]);
            if (track == nullptr)
                continue;
            Array<usize> landed;
            track->MoveKeys(static_cast<usize>(m_channels[group.channel].Keys), group.indices.AsSpan(), delta, landed);
            for (const usize index : landed)
                moved.PushBack(gui::TimelineKeyRef{group.channel, index});
        }
        m_document->PreviewGesture();
    }

    void SequenceSource::CommitKeysMove()
    {
        m_document->CommitGesture(u8"sequence-move-keys");
    }

    void SequenceSource::CancelKeysMove()
    {
        m_document->CancelGesture();
    }

    bool SequenceSource::DeleteKeys(Span<const gui::TimelineKeyRef> keys)
    {
        if (keys.IsEmpty())
            return false;
        const Array<ChannelKeys> grouped = ByChannel(keys);
        const Array<Channel> channels = m_channels; // the rows as the selection saw them
        m_document->Edit(u8"sequence-delete-keys", [&grouped, &channels](sequence::Sequence& sequence)
                         {
                             for (const ChannelKeys& group : grouped)
                                 if (group.channel < channels.Size() && channels[group.channel].Keys >= 0 &&
                                     channels[group.channel].Track < sequence.tracks.Size())
                                     sequence.tracks[channels[group.channel].Track]->DeleteKeys(static_cast<usize>(channels[group.channel].Keys),
                                                                                                group.indices.AsSpan());
                         });
        return true;
    }

    bool SequenceSource::InsertKey(usize channel, f32 time, gui::TimelineKeyRef& made)
    {
        if (channel >= m_channels.Size() || m_channels[channel].Keys < 0)
            return false;
        const Channel at = m_channels[channel];
        i32 index = -1;
        m_document->Edit(u8"sequence-insert-key", [&at, time, &index](sequence::Sequence& sequence)
                         {
                             if (at.Track < sequence.tracks.Size())
                                 index = sequence.tracks[at.Track]->InsertKey(static_cast<usize>(at.Keys), time);
                         });
        if (index < 0)
            return false;
        made = gui::TimelineKeyRef{channel, static_cast<usize>(index)};
        return true;
    }

    bool SequenceSource::IsSectionChannel(usize channel) const
    {
        return channel < m_channels.Size() && m_channels[channel].Keys < 0;
    }

    void SequenceSource::GetSections(usize channel, Array<gui::TimelineSection>& out) const
    {
        out.Clear();
        if (!IsSectionChannel(channel))
            return;
        const sequence::SequenceTrack* track = TrackOf(m_channels[channel]);
        if (track == nullptr)
            return;
        for (usize i = 0; i < track->SectionCount(); ++i)
        {
            gui::TimelineSection section;
            track->GetSection(i, section.Start, section.Length, section.Label);
            out.PushBack(Move(section));
        }
    }

    void SequenceSource::BeginSectionEdit()
    {
        m_document->BeginGesture();
    }

    void SequenceSource::PreviewSectionEdit(const gui::TimelineSectionRef& section, f32 start, f32 length)
    {
        if (!IsSectionChannel(section.Channel))
            return;
        m_document->RestoreGestureStart(); // each preview sets the span from where it began
        if (sequence::SequenceTrack* track = TrackOf(m_channels[section.Channel]))
            track->SetSection(section.Index, start, length);
        m_document->PreviewGesture();
    }

    void SequenceSource::CommitSectionEdit()
    {
        m_document->CommitGesture(u8"sequence-edit-section");
    }

    void SequenceSource::CancelSectionEdit()
    {
        m_document->CancelGesture();
    }

    bool SequenceSource::DeleteSection(const gui::TimelineSectionRef& section)
    {
        if (!IsSectionChannel(section.Channel))
            return false;
        const usize track = m_channels[section.Channel].Track;
        m_document->Edit(u8"sequence-delete-section", [track, &section](sequence::Sequence& sequence)
                         {
                             if (track < sequence.tracks.Size())
                                 sequence.tracks[track]->RemoveSection(section.Index);
                         });
        return true;
    }

    bool SequenceSource::InsertSection(usize channel, f32 time, gui::TimelineSectionRef& made)
    {
        if (!IsSectionChannel(channel))
            return false;
        const usize track = m_channels[channel].Track;
        i32 index = -1;
        m_document->Edit(u8"sequence-insert-section", [track, time, &index](sequence::Sequence& sequence)
                         {
                             if (track < sequence.tracks.Size())
                                 index = sequence.tracks[track]->AddSection(time);
                         });
        if (index < 0)
            return false;
        made = gui::TimelineSectionRef{channel, static_cast<usize>(index)};
        return true;
    }

    gui::CurveInfo SequenceSource::GetCurveInfo(usize curve) const
    {
        gui::CurveInfo info;
        if (curve >= m_curves.Size())
            return info;
        const sequence::SequenceTrack* track = TrackOf(m_curves[curve]);
        if (track == nullptr)
            return info;
        info.Name = Format(u8"{}.{}", TrackLabel(*track), track->KeyChannelName(static_cast<usize>(m_curves[curve].Keys)));
        // x and r red, y and g green, z and b blue, a grey; a lone curve amber.
        static constexpr Color kColors[] = {{0.92f, 0.38f, 0.38f, 1.0f}, {0.42f, 0.82f, 0.45f, 1.0f}, {0.42f, 0.62f, 1.0f, 1.0f}, {0.8f, 0.8f, 0.8f, 1.0f}};
        const usize channel = static_cast<usize>(m_curves[curve].Keys);
        info.StrokeColor = track->KeyChannelCount() == 1 ? Color{0.95f, 0.75f, 0.3f, 1.0f} : kColors[channel < 4 ? channel : 3];
        return info;
    }

    Span<const CurveKey> SequenceSource::GetKeys(usize curve) const
    {
        if (curve >= m_curves.Size())
            return {};
        sequence::SequenceTrack* track = TrackOf(m_curves[curve]);
        const Curve* keys = track != nullptr ? track->KeyCurve(static_cast<usize>(m_curves[curve].Keys)) : nullptr;
        return keys != nullptr ? keys->Keys().AsSpan() : Span<const CurveKey>{};
    }

    void SequenceSource::BeginKeysEdit()
    {
        m_document->BeginGesture();
    }

    void SequenceSource::PreviewKeys(usize curve, Span<const CurveKey> keys)
    {
        if (curve >= m_curves.Size())
            return;
        sequence::SequenceTrack* track = TrackOf(m_curves[curve]);
        Curve* target = track != nullptr ? track->KeyCurve(static_cast<usize>(m_curves[curve].Keys)) : nullptr;
        if (target == nullptr)
            return;
        target->Keys().Clear();
        for (const CurveKey& key : keys)
            target->Keys().PushBack(key);
        m_document->PreviewGesture();
    }

    void SequenceSource::CommitKeysEdit()
    {
        m_document->CommitGesture(u8"sequence-edit-curve");
    }

    void SequenceSource::CancelKeysEdit()
    {
        m_document->CancelGesture();
    }

    // === Markers ===

    usize SequenceSource::MarkerCount() const
    {
        return m_document->Sequence().markers.Size();
    }

    gui::TimeMarker SequenceSource::GetMarker(usize marker) const
    {
        const sequence::Sequence& sequence = m_document->Sequence();
        if (marker >= sequence.markers.Size())
            return {};
        const sequence::SequenceMarker& at = sequence.markers[marker];
        return gui::TimeMarker{at.time, at.hold ? Format(u8"{} (hold)", at.name) : at.name};
    }

    void SequenceSource::BeginMarkerMove(usize marker)
    {
        m_movingMarker = marker;
        m_document->BeginGesture();
    }

    void SequenceSource::PreviewMarkerMove(usize marker, f32 time)
    {
        sequence::Sequence& sequence = m_document->GestureSequence();
        if (marker >= sequence.markers.Size())
            return;
        sequence.markers[marker].time = Max(0.0f, time);
        m_document->PreviewGesture();
    }

    void SequenceSource::CommitMarkerMove()
    {
        // Kept by time, as the play reads them.
        sequence::Sequence& sequence = m_document->GestureSequence();
        for (usize i = 1; i < sequence.markers.Size(); ++i)
            for (usize j = i; j > 0 && sequence.markers[j - 1].time > sequence.markers[j].time; --j)
            {
                sequence::SequenceMarker swap = Move(sequence.markers[j - 1]);
                sequence.markers[j - 1] = Move(sequence.markers[j]);
                sequence.markers[j] = Move(swap);
            }
        m_document->CommitGesture(u8"sequence-move-marker");
    }

    void SequenceSource::CancelMarkerMove()
    {
        m_document->CancelGesture();
    }
}
