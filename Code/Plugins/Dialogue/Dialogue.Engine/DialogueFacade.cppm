// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The `Dialogue` script facade: the calling script's run's runner, the same shape in every language
// (`Dialogue::play(gate, "gate")` in AngelScript, `Dialogue.play(gate, "gate")` in Luau). A
// dialogue file is reached by its guid, as every asset is: a property tagged
// `"asset:DialogueResource"`. What the runner tells the game reaches a behavior's handlers as
// `onDialogueLine(DialogueLineNotice)`, `onDialogueChoice(DialogueChoiceNotice)`,
// `onDialogueCommand(DialogueCommandNotice)` and `onDialogueEnded(DialogueEndedNotice)`. A context
// outside every run (an editor tool) reads every call as refused and does nothing.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module dialogue.engine:facade;

import foundation.core;
import foundation.script.facades;
import :run;

using namespace foundation::core;

export namespace dialogue
{
    class DialogueFacade final : public Object
    {
        RTTI_OBJECT(DialogueFacade, Object)
    public:
        /// The calling script's run's runner, or null.
        [[nodiscard]] static DialogueRunService* Resolve();

        /// Plays the dialogue file's first conversation, or the one named; false when one is
        /// playing or there is no such conversation.
        static bool play(Guid dialogue);
        static bool play(Guid dialogue, String conversation);
        [[nodiscard]] static bool isPlaying();
        /// Goes on past the line showing; false when none is.
        static bool advance();
        /// Takes answer `index` (from 0) of the choice offered.
        static bool choose(i32 index);
        /// The command the conversation waits on is done.
        static bool done();
        static void stop();
        /// Goes on with the conversation the save holds a place in (after a load, or back from a
        /// command that left).
        static bool resume();
        [[nodiscard]] static bool flag(String name);
        static void setFlag(String name, bool on);
        /// Plays the speaker `speaker` as `entity` in what follows (an invalid entity: the cast's
        /// again).
        static void bind(String speaker, foundation::script::Entity entity);
    };

    /// Registers the facade and the notices for script. Called on each load (the domain's
    /// registerScriptFacade), so a plugin loaded again registers again.
    void RegisterDialogueScriptFacade();
    /// Takes back the script names and roots it added, as the plugin unloads.
    void RemoveDialogueScriptFacade();
}
