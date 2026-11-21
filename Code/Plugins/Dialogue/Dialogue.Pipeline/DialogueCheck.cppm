// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cook's checks over a parsed dialogue file, against what the parser cannot know: the project's
// cast, its dialogue assets, the flags provided from outside the conversations, the flags the
// project's other dialogue files set, and the conversations of the files a jump names. A
// conversation never names an asset by path: a clip by its name among the speaker's clips in the
// cast; a sound, a sequence, a level, another dialogue file or an asset argument by its name in the
// dialogue assets, each of which holds the asset's guid, so a renamed or moved asset is still found.
// Conversation and line ids are a file's own: a script plays a conversation through its file's
// asset, so two files may use the same ids. Each problem is added to the parse at its line, and
// each asset named is resolved to its guid in the steps.

module;

#include "Core/Prelude.h"

export module dialogue.pipeline:check;

import foundation.core;
import dialogue.engine;
import :parse;

using namespace foundation::core;

export namespace dialogue
{
    /// One of the project's dialogue assets: the name a conversation calls it by, and the asset.
    struct DialogueAssetEntry
    {
        String name; // one word
        Guid asset;
    };

    void Serialize(ISerializer& ar, DialogueAssetEntry& entry);

    /// The project's assets as the checks see them, by guid.
    class IDialogueAssets
    {
    public:
        virtual ~IDialogueAssets() = default;
        /// The name of the asset `id`; empty when there is none.
        [[nodiscard]] virtual String NameOf(const Guid& id) const = 0;
        /// The type of the asset `id` ("AudioClipAsset"); empty when there is none.
        [[nodiscard]] virtual String TypeOf(const Guid& id) const = 0;
    };

    /// The project's levels as the checks see them.
    class IDialogueLevels
    {
    public:
        virtual ~IDialogueLevels() = default;
        /// The names of the entities in the scene `level`; null when it does not load.
        [[nodiscard]] virtual const Array<String>* EntityNames(const Guid& level) const = 0;
    };

    /// Another dialogue file's conversations, which a jump into it may name.
    struct DialogueFileConversations
    {
        Guid file; // its DialogueAsset
        Array<String> conversations;
    };

    /// What the checks read beyond the file itself.
    struct DialogueCheckInputs
    {
        const Array<DialogueSpeaker>* cast = nullptr; // null: the file names no cast
        const Array<DialogueAssetEntry>* dialogueAssets = nullptr; // null: the file names none
        Span<const DialogueCommandForm> commands;     // the declared commands the file was parsed with
        Span<const String> providedFlags;             // names, or prefixes ending in `*`
        Guid self;                                    // this file's DialogueAsset; nil when it has none yet
        Span<const DialogueFileConversations> otherFiles; // the project's other dialogue files
        Span<const String> otherFlagsSet;             // the flags the other files' `set` and `clear` write
        const IDialogueAssets* assets = nullptr; // null: the assets' names and types are not checked
        const Array<String>* actions = nullptr;  // the input maps' actions; null: the project has no input map
        const IDialogueLevels* levels = nullptr; // null: a level's entities are not checked
    };

    /// Checks `parse` and adds every problem to it, by line; resolves the clips, sounds and
    /// sequences its steps name into their `asset`.
    void CheckDialogue(DialogueParse& parse, const DialogueCheckInputs& inputs);

    /// Whether `flag` is one of `provided`: named, or under a prefix written `prefix*`.
    [[nodiscard]] bool IsProvidedFlag(StringView flag, Span<const String> provided) noexcept;

    /// What a parsed file contributes to the others' checks: its conversations, and the flags it
    /// writes and reads (each once).
    void CollectDialogueFacts(const DialogueParse& parse, Array<String>& conversations, Array<String>& flagsSet,
                              Array<String>& flagsRead);

    /// The asset types `sound`, `sequence`, a conversation's level and `jump ... in` take.
    inline constexpr StringView kSoundAssetTypes[] = {u8"AudioClipAsset", u8"SoundCueAsset"};
    inline constexpr StringView kSequenceAssetTypes[] = {u8"SequenceAsset"};
    inline constexpr StringView kLevelAssetTypes[] = {u8"SceneDocument"};
    inline constexpr StringView kDialogueFileAssetTypes[] = {u8"DialogueAsset"};
}
