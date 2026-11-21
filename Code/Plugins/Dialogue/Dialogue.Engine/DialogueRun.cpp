// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The run's host for its runner, and the contribution that installs one in every run.

module;

#include "Core/Prelude.h"

module dialogue.engine;

import foundation.core;
import foundation.run;
import foundation.resource;
import foundation.input;
import foundation.script.facades; // ScriptRuntimeBinding: the run's resources
import engine.run;
import engine.behavior; // BehaviorRunHost: the services the run's scripts find

using namespace foundation::core;

namespace dialogue
{
    namespace
    {
        // The plugin's settings, which the contribution's install reads: set as the contribution is
        // made, and read only while it is installed, which ends as the plugin unloads.
        const DialogueSettings* g_runSettings = nullptr;

        void InstallRunner(foundation::run::RunBase& run)
        {
            if (g_runSettings != nullptr)
            {
                (void)run.Add<DialogueRunService>(*g_runSettings, run);
            }
        }

        template <typename T>
        [[nodiscard]] RefPtr<T> Load(foundation::resource::ResourceManager* resources, const Guid& id)
        {
            if (resources == nullptr || id.IsNil())
            {
                return {};
            }
            foundation::resource::Ref<T> ref;
            ref.SetId(id);
            ref.Bind(*resources);
            return RefPtr<T>(ref.Get());
        }

        template <typename Notice>
        void Publish(foundation::run::IRun& run, StringView event, const Notice& notice)
        {
            run.Events().Publish(StringHash(event), Variant::From(notice));
        }
    }

    void* DialogueRunHost::Find(TypeId type) const
    {
        // The behavior host resolves as its scripts do: the run's services, then the application's.
        if (auto* scripts = m_run->Get<engine::behavior::BehaviorRunHost>())
        {
            return scripts->FindService(type);
        }
        return m_run->FindService(type);
    }

    RefPtr<DialogueResource> DialogueRunHost::LoadDialogue(const Guid& id)
    {
        auto* binding = static_cast<foundation::script::ScriptRuntimeBinding*>(
            Find(TypeOf<foundation::script::ScriptRuntimeBinding>().id));
        return Load<DialogueResource>(binding != nullptr && binding->resolveResources ? binding->resolveResources() : nullptr, id);
    }

    RefPtr<DialogueCastResource> DialogueRunHost::LoadCast(const Guid& id)
    {
        auto* binding = static_cast<foundation::script::ScriptRuntimeBinding*>(
            Find(TypeOf<foundation::script::ScriptRuntimeBinding>().id));
        return Load<DialogueCastResource>(binding != nullptr && binding->resolveResources ? binding->resolveResources() : nullptr,
                                          id);
    }

    bool DialogueRunHost::ReadFlag(StringView key)
    {
        auto* save = m_run->Get<engine::runtime::RunSave>();
        return save != nullptr && save->Values().GetBool(key, false);
    }

    void DialogueRunHost::WriteFlag(StringView key, bool on)
    {
        if (auto* save = m_run->Get<engine::runtime::RunSave>(); save != nullptr && save->Values().SetBool(key, on))
        {
            save->MarkChanged();
        }
    }

    String DialogueRunHost::ReadText(StringView key)
    {
        auto* save = m_run->Get<engine::runtime::RunSave>();
        return save != nullptr ? save->Values().GetText(key, {}) : String();
    }

    void DialogueRunHost::WriteText(StringView key, StringView text)
    {
        auto* save = m_run->Get<engine::runtime::RunSave>();
        if (save == nullptr)
        {
            return;
        }
        if (text.IsEmpty() ? save->Values().Remove(key) : save->Values().SetText(key, text))
        {
            save->MarkChanged();
        }
    }

    String DialogueRunHost::ActionLabel(StringView action)
    {
        namespace input = foundation::input;
        auto* runtime = static_cast<input::ActionRuntime*>(Find(TypeOf<input::ActionRuntime>().id));
        if (runtime == nullptr)
        {
            return {};
        }
        const auto* kinds = static_cast<const input::InputKindTracker*>(Find(TypeOf<input::InputKindTracker>().id));
        const input::InputKind kind = kinds != nullptr ? kinds->Kind() : input::InputKind::Pointer;
        const Optional<input::Binding> binding = input::ShownBinding::For(*runtime, action, {}, kind);
        return binding.HasValue() ? input::DescribeBinding(binding.Value()) : String();
    }

    u32 DialogueRunHost::Stage(const DialogueStep& step, const DialogueCastResource* cast)
    {
        // TODO(dialogue): the staging (a clip, a sound, a fade, a sequence) through the run's scene.
        (void)step;
        (void)cast;
        return 0;
    }

    bool DialogueRunHost::IsStaging(u32 token)
    {
        (void)token;
        return false;
    }

    void DialogueRunHost::OnLine(const DialogueLineNotice& notice) { Publish(*m_run, kDialogueLineEvent, notice); }
    void DialogueRunHost::OnChoice(const DialogueChoiceNotice& notice) { Publish(*m_run, kDialogueChoiceEvent, notice); }
    void DialogueRunHost::OnCommand(const DialogueCommandNotice& notice) { Publish(*m_run, kDialogueCommandEvent, notice); }
    void DialogueRunHost::OnEnded(const DialogueEndedNotice& notice) { Publish(*m_run, kDialogueEndedEvent, notice); }

    engine::runtime::RunServiceContribution DialogueRunContribution(const DialogueSettings& settings)
    {
        g_runSettings = &settings;
        return engine::runtime::RunServiceContribution{u8"dialogue", TypeOf<DialogueRunService>().id, &InstallRunner};
    }
}
