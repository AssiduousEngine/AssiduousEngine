// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DialogueSettings: the dialogue plugin's project settings, read from the project's entry for the
// plugin when it loads (PluginContext::Settings) and edited in Project Settings.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module dialogue.engine:settings;

import foundation.core;

using namespace foundation::core;

export namespace dialogue
{
    class DialogueSettings final : public ISerializable
    {
        RTTI_OBJECT(DialogueSettings, ISerializable)
    public:
        /// The cast (a DialogueCastAsset) a new dialogue file names; each file names its own.
        Guid cast;
        /// The dialogue assets (a DialogueAssetListAsset) a new dialogue file names; each file names
        /// its own.
        Guid assets;
        /// The commands the game declares, each a form: literal words, `<name:kind>` arguments,
        /// `[optional parts]` and how it waits ("move <who:entity> to <target:entity> [and wait]").
        Array<String> commands;
        /// Flags provided from outside the conversations (set by the application's own code), so a
        /// conversation may read them though none sets them: a name, or a prefix ending in `*`
        /// ("seen_*").
        Array<String> providedFlags;
        /// Where flags live in the run's save store: `<prefix><flag>`.
        String flagPrefix = String(u8"flag.");
        /// A `fade` that names no length takes this many seconds.
        f32 fadeSeconds = 0.5f;
        /// A `fade` that names no colour fades to this one.
        Float4 fadeColor{0.0f, 0.0f, 0.0f, 1.0f};
        /// The plugin's own dialogue box shows lines and choices.
        bool stockBox = false;
        /// The stock box types a line out at this many characters a second; 0 shows it whole.
        f32 typingSpeed = 40.0f;
        /// The input map action that advances a line in the stock box.
        String advanceAction = String(u8"Interact");
        /// The stock box shows the speaker's portrait on the right.
        bool portraitOnRight = false;

        void Serialize(ISerializer& ar) override;
    };
}
