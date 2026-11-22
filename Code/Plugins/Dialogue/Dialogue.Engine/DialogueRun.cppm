// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The runner a run holds. Every run gets one (a run service the plugin contributes), with the run's
// own host: dialogue files and casts loaded through the run's resources, flags and the resume
// record in the run's save, `{Action}` filled from the player's bindings on the device in hand,
// and what the runner tells the game published on the run's bus as DialogueLine, DialogueChoice,
// DialogueCommand and DialogueEnded (a behavior's `on<Name>` hears them).
//
// The staging is in the run's scene (its current one). A speaker plays as an entity: the one the
// game bound to it (`Dialogue.bind`), else the level entity the cast names, else none (told once).
//   * `play`: the clip once over what the entity animates (the animation's one-shot).
//   * `sound`: the clip or cue through the run's audio, at the speaker's entity when named.
//   * `fade`: a claim on the scene's fade, over the sequences' own, held at full until a fade in.
//   * `sequence`: a play in the scene, each of its bindings named for a speaker filled with that
//     speaker's entity; `and wait` holds until it ends.
// While a line shows, its speaker plays their talk clip (the cast's `talk`), looping, and their
// voice (the cast's `voice`) once as it starts.
//
// With the settings' `stockBox` on, the host shows what it tells the game in the plugin's own box
// as well (DialogueStockBox), on the run's screen stack, and takes the player's advance and answers
// from it.

module;

#include "Core/Prelude.h"

export module dialogue.engine:run;

import foundation.core;
import foundation.run;
import engine.run; // RunServiceContribution
import foundation.scene;
import foundation.resource;
import foundation.script; // IScriptContext
import foundation.gui.gamekit; // the stock box's screen stack
import engine.gui;
import :settings;
import :conversation;
import :cast;
import :runner;
import :stockbox;

using namespace foundation::core;

export namespace dialogue
{
    /// The run's own host for its runner.
    class DialogueRunHost final : public IDialogueHost
    {
    public:
        DialogueRunHost(const DialogueSettings& settings, foundation::run::IRun& run)
            : m_settings(&settings), m_run(&run), m_box(settings)
        {
        }
        ~DialogueRunHost() override;

        /// Plays the speaker `speaker` as `entity` in what follows (unassigned: the cast's again).
        void Bind(StringView speaker, foundation::scene::EntityHandle entity);
        /// The runner the stock box advances and answers.
        void SetRunner(DialogueRunner& runner) noexcept { m_runner = &runner; }
        [[nodiscard]] DialogueStockBox& Box() noexcept { return m_box; }

        [[nodiscard]] RefPtr<DialogueResource> LoadDialogue(const Guid& id) override;
        [[nodiscard]] RefPtr<DialogueCastResource> LoadCast(const Guid& id) override;
        [[nodiscard]] bool ReadFlag(StringView key) override;
        void WriteFlag(StringView key, bool on) override;
        [[nodiscard]] String ReadText(StringView key) override;
        void WriteText(StringView key, StringView text) override;
        [[nodiscard]] String ActionLabel(StringView action) override;
        [[nodiscard]] u32 Stage(const DialogueStep& step, const DialogueCastResource* cast) override;
        [[nodiscard]] bool IsStaging(u32 token) override;
        void Update(f32 deltaTime) override;
        void OnLine(const DialogueLineNotice& notice, const DialogueSpeaker* speaker) override;
        void OnLineEnded() override;
        void OnChoice(const DialogueChoiceNotice& notice) override;
        void OnCommand(const DialogueCommandNotice& notice) override;
        void OnEnded(const DialogueEndedNotice& notice) override;

    private:
        struct Binding
        {
            String speaker;
            foundation::scene::EntityHandle entity;
        };
        struct Staged
        {
            u32 token = 0;
            DialogueStepKind kind = DialogueStepKind::End;
            foundation::scene::EntityHandle entity; // a one-shot's
            u32 play = 0;                           // a sequence's
        };

        // The service the run's scripts would find of type `type`: the run's, then the application's.
        [[nodiscard]] void* Find(TypeId type) const;
        [[nodiscard]] foundation::script::IScriptContext* Context() const;
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const;
        [[nodiscard]] foundation::scene::Scene* Scene() const;
        // The entity that plays `speaker` (`entry` its cast entry, null for none) in `scene`:
        // the game's binding, else the entity the cast names; unassigned, told once, when none.
        [[nodiscard]] foundation::scene::EntityHandle EntityOf(foundation::scene::Scene& scene, StringView speaker,
                                                               const DialogueSpeaker* entry);
        void StopTalking();
        void ReleaseFade();
        void UpdateFade(f32 deltaTime);
        // The box's sheet: the project's (`stockBoxSheet`) when it loads, else the plugin's.
        [[nodiscard]] String BoxSheet() const;
        // The GUI and the run's screen stack the box goes on; false when there are none.
        [[nodiscard]] bool BoxPlace(engine::gui::GuiSubsystem*& gui, foundation::gui::ScreenStack*& stack) const;

        const DialogueSettings* m_settings;
        foundation::run::IRun* m_run;
        Array<Binding> m_bindings;
        Array<String> m_unplaced; // speakers told to have no entity
        Array<Staged> m_staged;
        u32 m_nextToken = 1;
        // The fade: its claim's scene, where it is and where it goes, and its token while it moves.
        foundation::scene::WeakSceneRef m_fadeScene;
        Float4 m_fadeColor{0.0f, 0.0f, 0.0f, 1.0f};
        f32 m_fadeAmount = 0.0f;
        f32 m_fadeFrom = 0.0f;
        f32 m_fadeTo = 0.0f;
        f32 m_fadeSeconds = 0.0f;
        f32 m_fadeTime = 0.0f;
        u32 m_fadeToken = 0;
        // The speaker talking while their line shows.
        foundation::scene::WeakSceneRef m_talkScene;
        foundation::scene::EntityHandle m_talker;
        DialogueRunner* m_runner = nullptr;
        DialogueStockBox m_box;
    };

    /// The runner a run holds, with the run's host (`run.Get<DialogueRunService>()`).
    class DialogueRunService final : public DialogueRunner
    {
    public:
        // The base is given the host before the member is made; it only keeps the reference.
        DialogueRunService(const DialogueSettings& settings, foundation::run::IRun& run)
            : DialogueRunner(settings, m_runHost), m_runHost(settings, run)
        {
            m_runHost.SetRunner(*this);
        }

        /// The stock box (for tests).
        [[nodiscard]] DialogueStockBox& Box() noexcept { return m_runHost.Box(); }

        /// Plays the speaker `speaker` as `entity` (unassigned: the cast's entity again).
        void Bind(StringView speaker, foundation::scene::EntityHandle entity) { m_runHost.Bind(speaker, entity); }

    private:
        DialogueRunHost m_runHost;
    };

    /// The contribution that gives every run its runner, reading `settings` (the plugin's, which
    /// outlive the contribution).
    [[nodiscard]] engine::runtime::RunServiceContribution DialogueRunContribution(const DialogueSettings& settings);
}
