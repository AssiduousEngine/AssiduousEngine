// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DialogueRunner: plays one conversation at a time. It walks a conversation's steps from the first
// and stops at a step that waits: a line until it is advanced, a choice until one answer is chosen,
// a `wait` for its time, staging told to wait until it is done, a command told to wait until the
// game says it is done. A command that leaves ends the conversation with its place saved, for the
// game to resume once it comes back.
//
// What it needs from the game around it goes through IDialogueHost: loading dialogue files and
// casts, the flags and the resume record (the run's save store, under the settings' prefix), the
// staging (a clip, a sound, a fade, a sequence) and what it tells the game (a line to show, the
// answers to offer, a command to carry out, the end). The run's own host is the engine's; a test
// gives it a stand-in.
//
// Resume anchors on a line, not a step index, so a save survives a patch to the conversation: the
// record holds the dialogue file, the conversation, the step to go on at, the id of the last line
// shown and a hash of the conversation's steps. When the hash matches, the runner goes on at the
// step; otherwise at that line, if the conversation still has it; otherwise from the start. The
// record is written as each line shows and when a command leaves, so a game that saves mid
// conversation saves its place too, and it is cleared when the conversation ends.

module;

#include "Core/Prelude.h"

export module dialogue.engine:runner;

import foundation.core;
import foundation.run;
import foundation.scene;
import foundation.script.facades; // Entity: what a command's entity argument reads as in script
import dialogue.foundation; // the settings, conversations and cast

using namespace foundation::core;

export namespace dialogue
{
    /// A line for the game to show.
    struct DialogueLineNotice
    {
        Guid dialogue;       // the dialogue file
        String conversation;
        String speaker;      // a cast id
        String shownName;    // the cast's name for them; empty shows none
        String mood;
        Guid portrait;       // the mood's, else the speaker's; nil for none
        String text;         // its `{Action}`s filled in
        String lineId;
    };

    /// The answers for the game to offer; `Dialogue.choose(i)` takes one.
    struct DialogueChoiceNotice
    {
        Guid dialogue;
        String conversation;
        Array<String> answers; // their texts, `{Action}`s filled in
        Array<String> lineIds;

        // For script:
        [[nodiscard]] i32 answerCount() const noexcept { return static_cast<i32>(answers.Size()); }
        /// Answer `index`'s text; empty past the last.
        [[nodiscard]] String answer(i32 index) const;
        [[nodiscard]] String answerLineId(i32 index) const;
    };

    /// A declared command for the game to carry out.
    struct DialogueCommandNotice
    {
        Guid dialogue;
        String conversation;
        String name;
        bool waits = false;  // the conversation waits for `Dialogue.done()`
        bool leaves = false; // the conversation has ended with its place saved
        Array<DialogueArgument> arguments;
        foundation::scene::WeakSceneRef scene; // where an entity argument is looked for (the run's)

        /// The argument `name`, or null.
        [[nodiscard]] const DialogueArgument* Find(StringView name) const noexcept;

        // For script, each by the argument's name in the command's form:
        /// Whether it was written (an optional part, `[running]`).
        [[nodiscard]] bool has(String name) const;
        /// As written; empty when absent.
        [[nodiscard]] String text(String name) const;
        [[nodiscard]] f32 number(String name) const;
        /// An asset argument's asset; nil when absent.
        [[nodiscard]] Guid asset(String name) const;
        /// An entity argument's entity in the scene, by its name; invalid when there is none.
        [[nodiscard]] foundation::script::Entity entity(String name) const;
    };

    /// The conversation ended: reached its end, was stopped, or left for a command.
    struct DialogueEndedNotice
    {
        Guid dialogue;
        String conversation;
        bool stopped = false; // by `stop()`
        bool left = false;    // by a command that leaves; `resume()` goes on from there
    };

    /// The names the notices go out under on the run's bus (a behavior's `on<Name>` hears them).
    inline constexpr StringView kDialogueLineEvent = u8"DialogueLine";
    inline constexpr StringView kDialogueChoiceEvent = u8"DialogueChoice";
    inline constexpr StringView kDialogueCommandEvent = u8"DialogueCommand";
    inline constexpr StringView kDialogueEndedEvent = u8"DialogueEnded";

    /// The save store's key for the resume record.
    inline constexpr StringView kDialogueResumeKey = u8"dialogue.resume";

    /// What a runner needs from the game around it.
    class IDialogueHost
    {
    public:
        virtual ~IDialogueHost() = default;

        /// The dialogue file `id`, loaded; null when there is none.
        [[nodiscard]] virtual RefPtr<DialogueResource> LoadDialogue(const Guid& id) = 0;
        /// The cast `id`, loaded; null when there is none.
        [[nodiscard]] virtual RefPtr<DialogueCastResource> LoadCast(const Guid& id) = 0;

        /// The save store's boolean `key` (false when absent), and writing it.
        [[nodiscard]] virtual bool ReadFlag(StringView key) = 0;
        virtual void WriteFlag(StringView key, bool on) = 0;
        /// The save store's text `key` (empty when absent), and writing it (empty removes it).
        [[nodiscard]] virtual String ReadText(StringView key) = 0;
        virtual void WriteText(StringView key, StringView text) = 0;

        /// What the input action `action` is bound to, as a line shows it; empty keeps `{action}`.
        [[nodiscard]] virtual String ActionLabel(StringView action) = 0;

        /// Starts a staging step (Play, Sound, Fade, Sequence) with the cast `cast` (null: none);
        /// a token to ask after while the conversation waits for it, 0 for nothing to wait on.
        [[nodiscard]] virtual u32 Stage(const DialogueStep& step, const DialogueCastResource* cast) = 0;
        /// Whether the staging `token` is still going.
        [[nodiscard]] virtual bool IsStaging(u32 token) = 0;
        /// The runner's clock, every frame whether or not a conversation plays (a fade goes on
        /// after the conversation that began it).
        virtual void Update(f32 deltaTime) { (void)deltaTime; }

        /// `speaker` is the line's speaker's entry in the cast; null when the cast has none.
        virtual void OnLine(const DialogueLineNotice& notice, const DialogueSpeaker* speaker) = 0;
        /// The line shown has been advanced past, or the conversation ended on it.
        virtual void OnLineEnded() {}
        virtual void OnChoice(const DialogueChoiceNotice& notice) = 0;
        virtual void OnCommand(const DialogueCommandNotice& notice) = 0;
        virtual void OnEnded(const DialogueEndedNotice& notice) = 0;
    };

    /// What the runner is waiting on.
    enum class DialogueWait : u8
    {
        Nothing, // not playing, or between steps
        Line,    // advance()
        Choice,  // choose(i)
        Time,    // a `wait`
        Staging, // a clip, sound, fade or sequence told to wait
        Command, // done()
    };

    class DialogueRunner : public foundation::run::IRunService
    {
    public:
        /// `settings` and `host` outlive the runner.
        DialogueRunner(const DialogueSettings& settings, IDialogueHost& host) : m_settings(&settings), m_host(&host) {}

        /// Plays the conversation `conversation` of the dialogue file `dialogue` (its first when
        /// empty). False, logged, when one is already playing or there is no such conversation.
        bool Play(const Guid& dialogue, StringView conversation);
        [[nodiscard]] bool IsPlaying() const noexcept { return m_playing; }
        [[nodiscard]] DialogueWait Waiting() const noexcept { return m_wait; }

        /// Goes on past the line showing; false when none is.
        bool Advance();
        /// Takes answer `index` of the choice offered; false when none is, or no such answer.
        bool Choose(i32 index);
        /// The command the conversation waits on is done; false when it waits on none.
        bool Done();
        /// Ends the conversation now; its place is not kept.
        void Stop();

        /// Shows one line on its own (a sequence's line track): the game hears it as a
        /// conversation's line, `Advance()` ends it, and DialogueEnded (no conversation) follows.
        /// Its token, or 0, logged, while a conversation plays. A line already shown gives way.
        u32 ShowLine(const DialogueCastResource* cast, StringView speaker, StringView mood, StringView text,
                     StringView lineId);
        /// Whether the line `token` is still shown.
        [[nodiscard]] bool IsShowing(u32 token) const noexcept { return token != 0 && token == m_shownLine; }
        /// Ends the line `token` if it is still shown (its play let go of it).
        void EndLine(u32 token);
        /// Goes on with the conversation the resume record names; false when there is none, or one
        /// is playing.
        bool Resume();

        /// A flag, by its name without the settings' prefix.
        [[nodiscard]] bool Flag(StringView name);
        void SetFlag(StringView name, bool on);

        /// Steps the clock: the host's, then a `wait` counts down and staging is asked after.
        void Update(f32 deltaTime);

        // IRunService
        void OnRunUpdate(foundation::run::IRun&, f32 deltaTime) override { Update(deltaTime); }
        void OnRunStopping(foundation::run::IRun&) override;

        /// The hash of a conversation's steps the resume record keeps.
        [[nodiscard]] static u64 HashSteps(const DialogueConversation& conversation) noexcept;

    private:
        // Starts `conversation` of the loaded file at step `step`.
        void Begin(RefPtr<DialogueResource> file, const Guid& dialogue, const DialogueConversation& conversation,
                   usize step);
        // Runs steps until one waits or the conversation ends.
        void Pump();
        void Finish(bool stopped, bool left);
        // A line's notice: its speaker's name, mood and portrait from `cast`, its text filled.
        [[nodiscard]] DialogueLineNotice LineNotice(const DialogueCastResource* cast, StringView speaker, StringView mood,
                                                    StringView text, StringView lineId,
                                                    const DialogueSpeaker*& entry);
        void WriteResume(usize step);
        [[nodiscard]] String FlagKey(StringView name) const;
        [[nodiscard]] String Fill(StringView text);
        [[nodiscard]] String Text(StringView lineId);

        const DialogueSettings* m_settings;
        IDialogueHost* m_host;
        RefPtr<DialogueResource> m_file;
        RefPtr<DialogueCastResource> m_cast;
        Guid m_dialogue;
        const DialogueConversation* m_conversation = nullptr;
        usize m_step = 0;
        bool m_playing = false;
        DialogueWait m_wait = DialogueWait::Nothing;
        f32 m_timer = 0.0f;
        u32 m_staging = 0;
        String m_lastLine; // the id of the last line shown, the resume record's anchor
        bool m_pumping = false;
        u32 m_shownLine = 0; // a line shown on its own, not a conversation's
        u32 m_nextLine = 1;
    };
}
