// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The compiled form of a dialogue file: its conversations, each a flat list of steps with its
// choice and branch blocks compiled to jumps, and the texts of its lines and answers in a table
// keyed by line id, apart from the steps (the seam a translation fills). The cook writes it as a
// DialogueSource; the factory loads that into a DialogueResource the runner plays.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module dialogue.engine:conversation;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;

export namespace dialogue
{
    /// What a step does. The runner walks a conversation's steps from the first; a step that waits
    /// (a line, a choice, a wait, and what is told to wait) stops it until it is let go.
    enum class DialogueStepKind : u8
    {
        Line,     // `speaker` says the line `lineId` in `mood`; waits for advance
        Choice,   // offers the `count` Answer steps that follow; waits for a choice
        Answer,   // one answer, `lineId`; its statements start at `next`
        Skip,     // goes on at `next` (the end of a choice's answer, the end of an if's side)
        If,       // goes on at `next` unless `flag` is set (is clear, with `negate`)
        Set,      // sets `flag`
        Clear,    // clears `flag`
        Wait,     // waits `seconds`
        Play,     // `speaker` plays the clip `asset` (named `name`) once; waits with `waits`
        Sound,    // plays the sound `asset`, at `speaker`'s entity when one is named
        Fade,     // fades in (`fadeIn`) or out over `seconds` to `color`
        Sequence, // plays the sequence `asset`; waits with `waits`
        Jump,     // ends this conversation and plays the one named `name`, in the dialogue file `asset`
                  // (nil: this one)
        Command,  // the declared command `name` with its `arguments`; waits with `waits`, or leaves
        End,      // the conversation's end
    };

    /// One argument of a declared command, converted to its kind by the cook.
    struct DialogueArgument
    {
        String name;   // the form's name for it ("who")
        String text;   // as written (every kind keeps it)
        f32 number = 0.0f;
        Guid asset;    // an asset kind's guid
        bool present = false; // an optional part (`[running]`) was written
    };

    /// One compiled statement. Which fields mean anything depends on the kind (see each kind).
    struct DialogueStep
    {
        DialogueStepKind kind = DialogueStepKind::End;
        String speaker; // a cast id
        String mood;
        String lineId;
        String flag;
        bool negate = false;
        String name;    // a clip's, a conversation's or a command's name
        String file;    // a jump's other dialogue file, by its name in the dialogue assets; empty: this one
        Guid asset;
        f32 seconds = -1.0f; // a wait's, or a fade's length; below 0, the settings' default
        bool waits = false;
        bool leaves = false;
        bool fadeIn = false;
        bool hasColor = false; // a fade names its colour; otherwise the settings'
        Float4 color{0.0f, 0.0f, 0.0f, 1.0f};
        i32 next = -1;  // where a Skip, If or Answer goes on
        u32 count = 0;  // a Choice's answers
        u32 sourceLine = 0; // the line of the source text it came from, for messages
        Array<DialogueArgument> arguments;
    };

    /// A conversation: its id, the level its entity names are checked against (when it names one),
    /// and its steps.
    struct DialogueConversation
    {
        String id;
        String level;
        Array<DialogueStep> steps;
    };

    /// A line's or an answer's text by its id.
    struct DialogueLineText
    {
        String id;
        String text;
    };

    void Serialize(ISerializer& ar, DialogueArgument& argument);
    void Serialize(ISerializer& ar, DialogueStep& step);
    void Serialize(ISerializer& ar, DialogueConversation& conversation);
    void Serialize(ISerializer& ar, DialogueLineText& line);

    /// The runtime product: one dialogue file's conversations and texts, shared by every play.
    class DialogueResource final : public Object
    {
        RTTI_OBJECT(DialogueResource, Object)
    public:
        Guid cast; // the cast its lines' speakers are from (a cooked DialogueCastSource)
        Array<DialogueConversation> conversations;
        Array<DialogueLineText> lines;

        /// The conversation `id`, or null.
        [[nodiscard]] const DialogueConversation* Find(StringView id) const noexcept;
        /// The text of the line or answer `id`; empty when it has none.
        [[nodiscard]] StringView Text(StringView id) const noexcept;
    };

    /// The cooked form.
    class DialogueSource final : public ISerializable
    {
        RTTI_OBJECT(DialogueSource, ISerializable)
    public:
        Guid cast;
        Array<DialogueConversation> conversations;
        Array<DialogueLineText> lines;

        void Serialize(ISerializer& ar) override;
    };

    /// Loads a cooked DialogueSource into a DialogueResource.
    class DialogueFactory final : public foundation::resource::IResourceFactory
    {
    public:
        explicit DialogueFactory(IAllocator& allocator) : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override { return &DialogueResource::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &DialogueSource::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override;

    private:
        IAllocator* m_allocator;
    };
}
