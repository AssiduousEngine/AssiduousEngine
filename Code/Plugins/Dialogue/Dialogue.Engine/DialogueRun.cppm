// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The runner a run holds. Every run gets one (a run service the plugin contributes), with the run's
// own host: dialogue files and casts loaded through the run's resources, flags and the resume
// record in the run's save, `{Action}` filled from the player's bindings on the device in hand,
// and what the runner tells the game published on the run's bus as DialogueLine, DialogueChoice,
// DialogueCommand and DialogueEnded (a behavior's `on<Name>` hears them).

module;

#include "Core/Prelude.h"

export module dialogue.engine:run;

import foundation.core;
import foundation.run;
import engine.run; // RunServiceContribution
import :settings;
import :conversation;
import :cast;
import :runner;

using namespace foundation::core;

export namespace dialogue
{
    /// The run's own host for its runner.
    class DialogueRunHost final : public IDialogueHost
    {
    public:
        explicit DialogueRunHost(foundation::run::IRun& run) : m_run(&run) {}

        [[nodiscard]] RefPtr<DialogueResource> LoadDialogue(const Guid& id) override;
        [[nodiscard]] RefPtr<DialogueCastResource> LoadCast(const Guid& id) override;
        [[nodiscard]] bool ReadFlag(StringView key) override;
        void WriteFlag(StringView key, bool on) override;
        [[nodiscard]] String ReadText(StringView key) override;
        void WriteText(StringView key, StringView text) override;
        [[nodiscard]] String ActionLabel(StringView action) override;
        [[nodiscard]] u32 Stage(const DialogueStep& step, const DialogueCastResource* cast) override;
        [[nodiscard]] bool IsStaging(u32 token) override;
        void OnLine(const DialogueLineNotice& notice) override;
        void OnChoice(const DialogueChoiceNotice& notice) override;
        void OnCommand(const DialogueCommandNotice& notice) override;
        void OnEnded(const DialogueEndedNotice& notice) override;

    private:
        // The service the run's scripts would find of type `type`: the run's, then the application's.
        [[nodiscard]] void* Find(TypeId type) const;

        foundation::run::IRun* m_run;
    };

    /// The runner a run holds, with the run's host (`run.Get<DialogueRunService>()`).
    class DialogueRunService final : public DialogueRunner
    {
    public:
        // The base is given the host before the member is made; it only keeps the reference.
        DialogueRunService(const DialogueSettings& settings, foundation::run::IRun& run)
            : DialogueRunner(settings, m_runHost), m_runHost(run)
        {
        }

    private:
        DialogueRunHost m_runHost;
    };

    /// The contribution that gives every run its runner, reading `settings` (the plugin's, which
    /// outlive the contribution).
    [[nodiscard]] engine::runtime::RunServiceContribution DialogueRunContribution(const DialogueSettings& settings);
}
