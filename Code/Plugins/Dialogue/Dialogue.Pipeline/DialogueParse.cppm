// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue format's parser: a dialogue file's text into its conversations (dialogue.foundation's
// DialogueConversation, their blocks compiled to jumps) and the texts of its lines and answers,
// with every problem at its line. And the notation commands are declared in ("move <who:entity>
// to <target:entity> [running] [and wait]"): a form parsed once, then matched against the
// statements that use it. The built-in statements that take arguments are forms in the same
// notation, so an argument with spaces reads the same way in each: it runs to the next literal
// word of its form, the last one to the end of the line, and a quoted name is one word.
//
// What the parser cannot know (the cast, the assets, the level's entities, the flags the rest of
// the project sets) is the cook's to check; the parser keeps each such name as written.

module;

#include "Core/Prelude.h"

export module dialogue.pipeline:parse;

import foundation.core;
import dialogue.foundation;

using namespace foundation::core;

export namespace dialogue
{
    /// What an argument of a form takes.
    enum class DialogueArgumentKind : u8
    {
        Text,    // any words
        Word,    // one word
        Number,  // a number
        OneOf,   // one of the form's `choices`
        Entity,  // a level entity, by name (any words)
        Speaker, // a cast id (one word)
        Flag,    // a flag (one word)
        Asset,   // a dialogue asset of `assetType`, by its name in the project's dialogue assets (one word)
        List,    // one of the named list `list` (any words)
    };

    /// One part of a form: a literal word or an argument, either in an optional group (a run of
    /// parts with the same `group`, 0 for none).
    struct DialogueFormPart
    {
        bool literal = false;
        String word; // a literal's
        String name; // an argument's
        DialogueArgumentKind kind = DialogueArgumentKind::Text;
        Array<String> choices; // OneOf
        String assetType;      // Asset
        String list;           // List
        bool hasDefault = false;
        String defaultValue;
        u32 group = 0;
    };

    /// How a command waits.
    enum class DialogueCommandWait : u8
    {
        Never,  // goes on at once, unless written `and wait` where the form allows it
        Always, // `waits`: holds until the game says it is done
        Leaves, // `leaves`: ends the conversation with its place saved, to be resumed
    };

    /// A declared command's form, parsed.
    struct DialogueCommandForm
    {
        String text;  // as written
        String name;  // its leading literal words ("give coins")
        Array<DialogueFormPart> parts;
        DialogueCommandWait wait = DialogueCommandWait::Never;
        u32 waitGroup = 0; // the optional group that is `[and wait]`; 0 when the form has none
        u32 groupCount = 0;
    };

    /// Parses a command's form. False, with `error` saying why, for a form that is not one: an
    /// unclosed bracket, a nested group, an unknown kind, a name that starts with a statement the
    /// format has already.
    [[nodiscard]] bool ParseCommandForm(StringView text, DialogueCommandForm& out, String& error);

    /// One problem the parser or the cook found, at its line of the source (1 for the first).
    struct DialogueProblem
    {
        u32 line = 0;
        bool warning = false;
        String message;
    };

    /// A dialogue file, parsed.
    struct DialogueParse
    {
        Array<DialogueConversation> conversations;
        Array<DialogueLineText> lines;
        Array<DialogueProblem> problems; // by line

        /// No errors (warnings may remain).
        [[nodiscard]] bool Ok() const noexcept;
    };

    /// Parses a dialogue file's text, matching what is not a built-in statement against `commands`.
    [[nodiscard]] DialogueParse ParseDialogue(StringView text, Span<const DialogueCommandForm> commands);

    /// The words a statement starts with that the format keeps for itself.
    [[nodiscard]] bool IsBuiltInStatement(StringView word) noexcept;
}
