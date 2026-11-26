// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DialogueLineTrack: lines of dialogue on a sequence's timeline, for a linear cutscene staged as a
// sequence rather than as a conversation. Each section is a line: a speaker of the track's cast, a
// mood, and its text, written in the section or taken by line id from a dialogue file. When the
// playhead enters a section, the run's dialogue runner shows the line as it shows a
// conversation's; at the section's end the play holds until the line is advanced (a track
// runner's hold), so the cutscene waits for the player to read it. The editor's posing shows no
// lines. A line refused (a conversation is playing) holds nothing.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module dialogue.engine:linetrack;

import foundation.core;
import foundation.resource;
import foundation.sequence;
import dialogue.foundation; // the conversations and the cast

using namespace foundation::core;

export namespace dialogue
{
    /// Events run last in a step; a line shows after everything it stages has moved.
    inline constexpr i32 kDialogueLineTrackOrder = 900;

    /// One line on the timeline.
    struct DialogueLineSection
    {
        f32 start = 0.0f;
        f32 length = 2.0f; // the play holds at its end until the line is advanced
        String speaker;    // a cast id
        String mood;       // empty: the speaker's first
        String text;       // the line as written, `{Action}` and all, unless it comes from a file
        foundation::resource::Ref<DialogueResource> dialogue; // a dialogue file whose line `lineId` it shows
        String lineId;

        /// The line's text: the dialogue file's line `lineId` when one is named and loaded, else
        /// `text`.
        [[nodiscard]] StringView Text() const noexcept;
    };

    void Serialize(ISerializer& ar, DialogueLineSection& section);

    class DialogueLineTrack final : public foundation::sequence::SequenceTrack
    {
        RTTI_OBJECT(DialogueLineTrack, foundation::sequence::SequenceTrack)
    public:
        foundation::resource::Ref<DialogueCastResource> cast; // whose speakers the lines name
        Array<DialogueLineSection> sections;                  // by start

        [[nodiscard]] f32 Duration() const noexcept override;
        [[nodiscard]] i32 Order() const noexcept override { return kDialogueLineTrackOrder; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<foundation::sequence::SequenceTrackRunner>
        Begin(const foundation::sequence::SequenceTrackContext& context) const override;
        void ResolveResources(foundation::resource::ResourceManager& manager) override;
        void Serialize(ISerializer& ar) override;
        /// The section running at `time` (its end included); of two overlapping, the later.
        [[nodiscard]] const DialogueLineSection* SectionAt(f32 time) const noexcept;

        [[nodiscard]] usize SectionCount() const override { return sections.Size(); }
        void GetSection(usize index, f32& start, f32& length, String& label) const override;
        void SetSection(usize index, f32 start, f32 length) override;
        void RemoveSection(usize index) override;
        /// A line of no text yet, two seconds long, at `time`.
        i32 AddSection(f32 time) override;
    };

    /// Registers the line track kind (with the plugin's types, on each load).
    void RegisterDialogueLineTrack();
}
