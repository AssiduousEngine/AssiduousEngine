// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The run's host for its runner (its staging in the run's scene), and the contribution that
// installs one in every run.

module;

#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module dialogue.engine;

import foundation.core;
import foundation.run;
import foundation.resource;
import foundation.content; // a step's asset, to tell a cue from a clip
import foundation.input;
import foundation.scene;
import foundation.script;
import foundation.script.facades; // ScriptRuntimeBinding: the run's resources
import foundation.sequence;
import foundation.sequence.resource;
import engine.run;
import engine.behavior; // BehaviorRunHost: the services the run's scripts find
import engine.animation; // the one-shot
import engine.audio;
import engine.render; // the scene's fade
import engine.sequence;
import engine.gui;
import foundation.gui.gamekit;
import foundation.gui.resources;

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

    void DialogueRunHost::SetDialogue(const Guid& id, RefPtr<DialogueResource> resource)
    {
        m_previewId = id;
        m_preview = Move(resource);
    }

    bool DialogueRunHost::UseSave(engine::runtime::SaveStoreHandle store)
    {
        auto* saves = m_run->Get<engine::runtime::RunSaves>();
        if (saves == nullptr || saves->Resolve(store) == nullptr)
        {
            LOG_WARNING(u8"Dialogue", u8"useSave: that store is not there (closed, deleted, or never opened); the flags stay where they were");
            return false;
        }
        if (saves->IsReadOnly(store))
        {
            LOG_WARNING(u8"Dialogue", u8"useSave: a slot read (Saves.readSlot) is read only; give the slot opened with Saves.openSlot");
            return false;
        }
        m_store = store;
        m_warnedNoStore = false;
        m_scratchKeys.Clear(); // what played before the store belongs to no playthrough
        m_scratchValues.Clear();
        return true;
    }

    engine::runtime::SaveFile* DialogueRunHost::Store()
    {
        auto* saves = m_run->Get<engine::runtime::RunSaves>();
        engine::runtime::SaveFile* file = saves != nullptr ? saves->Resolve(m_store) : nullptr;
        if (file == nullptr && !m_warnedNoStore)
        {
            m_warnedNoStore = true;
            LOG_WARNING(u8"Dialogue", u8"no save store to keep the flags in (Dialogue.useSave with the game's slot or global store); "
                                      u8"they last this run only");
        }
        return file;
    }

    String* DialogueRunHost::Scratch(StringView key)
    {
        for (usize i = 0; i < m_scratchKeys.Size(); ++i)
        {
            if (m_scratchKeys[i] == key)
            {
                return &m_scratchValues[i];
            }
        }
        return nullptr;
    }

    void DialogueRunHost::WriteScratch(StringView key, StringView value)
    {
        for (usize i = 0; i < m_scratchKeys.Size(); ++i)
        {
            if (m_scratchKeys[i] == key)
            {
                if (value.IsEmpty())
                {
                    m_scratchKeys.RemoveAt(i);
                    m_scratchValues.RemoveAt(i);
                }
                else
                {
                    m_scratchValues[i] = String(value);
                }
                return;
            }
        }
        if (!value.IsEmpty())
        {
            m_scratchKeys.PushBack(String(key));
            m_scratchValues.PushBack(String(value));
        }
    }

    RefPtr<DialogueResource> DialogueRunHost::LoadDialogue(const Guid& id)
    {
        if (m_preview && id == m_previewId)
        {
            return m_preview;
        }
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
        engine::runtime::SaveFile* save = Store();
        if (save == nullptr)
        {
            const String* value = Scratch(key);
            return value != nullptr && *value == StringView(u8"true");
        }
        return save->Values().GetBool(key, false);
    }

    void DialogueRunHost::WriteFlag(StringView key, bool on)
    {
        engine::runtime::SaveFile* save = Store();
        if (save == nullptr)
        {
            WriteScratch(key, on ? StringView(u8"true") : StringView(u8"false"));
        }
        else if (save->Values().SetBool(key, on))
        {
            save->MarkChanged();
        }
    }

    String DialogueRunHost::ReadText(StringView key)
    {
        engine::runtime::SaveFile* save = Store();
        if (save == nullptr)
        {
            const String* value = Scratch(key);
            return value != nullptr ? *value : String();
        }
        return save->Values().GetText(key, {});
    }

    void DialogueRunHost::WriteText(StringView key, StringView text)
    {
        engine::runtime::SaveFile* save = Store();
        if (save == nullptr)
        {
            WriteScratch(key, text);
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

    DialogueRunHost::~DialogueRunHost()
    {
        StopTalking();
        ReleaseFade();
    }

    foundation::script::ServiceResolver DialogueRunHost::Services() const
    {
        const DialogueRunHost* self = this;
        return foundation::script::ServiceResolver{[self](TypeId type) { return self->Find(type); }};
    }

    foundation::resource::ResourceManager* DialogueRunHost::Resources() const
    {
        auto* binding = static_cast<foundation::script::ScriptRuntimeBinding*>(
            Find(TypeOf<foundation::script::ScriptRuntimeBinding>().id));
        return binding != nullptr && binding->resolveResources ? binding->resolveResources() : nullptr;
    }

    foundation::scene::Scene* DialogueRunHost::Scene() const
    {
        if (m_hasScene)
        {
            return m_scene.Get();
        }
        auto* binding = m_run->Get<engine::runtime::RunBinding>();
        return binding != nullptr && binding->currentScene ? binding->currentScene() : nullptr;
    }

    void DialogueRunHost::Bind(StringView speaker, foundation::scene::EntityHandle entity)
    {
        for (usize i = 0; i < m_bindings.Size(); ++i)
        {
            if (m_bindings[i].speaker == speaker)
            {
                if (entity.IsAssigned())
                {
                    m_bindings[i].entity = entity;
                }
                else
                {
                    m_bindings.RemoveAt(i);
                }
                return;
            }
        }
        if (entity.IsAssigned())
        {
            m_bindings.PushBack(Binding{String(speaker), entity});
        }
    }

    foundation::scene::EntityHandle DialogueRunHost::EntityOf(foundation::scene::Scene& scene, StringView speaker,
                                                              const DialogueSpeaker* entry)
    {
        for (const Binding& binding : m_bindings)
        {
            if (binding.speaker == speaker && scene.IsValid(binding.entity))
            {
                return binding.entity;
            }
        }
        if (entry != nullptr && !entry->entity.IsEmpty())
        {
            const foundation::scene::EntityHandle entity = scene.FindEntityByName(entry->entity.AsView());
            if (entity.IsAssigned())
            {
                return entity;
            }
        }
        for (const String& told : m_unplaced)
        {
            if (told == speaker)
            {
                return {};
            }
        }
        m_unplaced.PushBack(String(speaker));
        LOG_WARNING(u8"Dialogue", u8"the speaker `{}` has no entity in the scene: bind one (Dialogue.bind) or name "
                                  u8"one in the cast",
                    speaker);
        return {};
    }

    u32 DialogueRunHost::Stage(const DialogueStep& step, const DialogueCastResource* cast)
    {
        foundation::scene::Scene* scene = Scene();
        if (scene == nullptr)
        {
            return 0;
        }
        // The facades the staging goes through find the run's services through this scope, as a
        // script's call finds them, whether or not the run has a script context.
        const foundation::script::ServiceResolver services = Services();
        foundation::script::ServiceScope scope(&services);
        const auto entryOf = [cast](StringView speaker) { return cast != nullptr ? cast->Find(speaker) : nullptr; };
        switch (step.kind)
        {
        case DialogueStepKind::Play:
        {
            const foundation::scene::EntityHandle entity =
                EntityOf(*scene, step.speaker.AsView(), entryOf(step.speaker.AsView()));
            if (!entity.IsAssigned() || step.asset.IsNil())
            {
                return 0;
            }
            if (m_talker == entity)
            {
                m_talker = {}; // the clip takes over from the talk
            }
            engine::animation::SceneAnimationFacade{scene}.playOneShot(foundation::script::WrapEntity(scene, entity),
                                                                       step.asset);
            const u32 token = m_nextToken++;
            m_staged.PushBack(Staged{token, step.kind, entity, 0});
            return token;
        }
        case DialogueStepKind::Sound:
        {
            foundation::resource::ResourceManager* resources = Resources();
            if (resources == nullptr || step.asset.IsNil())
            {
                return 0;
            }
            const foundation::content::Instance* instance = resources->Database().GetInstance(step.asset);
            const bool cue = instance != nullptr && instance->TypeName() == StringView(u8"SoundCueSource");
            const foundation::scene::EntityHandle at =
                step.speaker.IsEmpty() ? foundation::scene::EntityHandle{}
                                       : EntityOf(*scene, step.speaker.AsView(), entryOf(step.speaker.AsView()));
            if (at.IsAssigned())
            {
                const Float3 position = scene->GetWorldPosition(at);
                (void)(cue ? engine::audio::AudioFacade::playCue3D(step.asset, position)
                           : engine::audio::AudioFacade::playOneShot3D(step.asset, position));
            }
            else
            {
                (void)(cue ? engine::audio::AudioFacade::playCue(step.asset) : engine::audio::AudioFacade::playOneShot(step.asset));
            }
            return 0; // a sound never holds the conversation
        }
        case DialogueStepKind::Fade:
        {
            m_fadeScene = scene;
            m_fadeColor = step.hasColor ? step.color : m_settings->fadeColor;
            // A fade in from nothing held starts at full: a conversation that opens on one.
            m_fadeFrom = step.fadeIn && m_fadeAmount <= 0.0f ? 1.0f : m_fadeAmount;
            m_fadeTo = step.fadeIn ? 0.0f : 1.0f;
            m_fadeSeconds = step.seconds >= 0.0f ? step.seconds : m_settings->fadeSeconds;
            m_fadeTime = 0.0f;
            m_fadeToken = m_nextToken++;
            UpdateFade(0.0f);
            return m_fadeToken;
        }
        case DialogueStepKind::Sequence:
        {
            auto* system = scene->GetSystem<engine::sequence::SequenceSystem>();
            RefPtr<foundation::sequence::SequenceResource> sequence =
                Load<foundation::sequence::SequenceResource>(Resources(), step.asset);
            if (system == nullptr || !sequence)
            {
                LOG_WARNING(u8"Dialogue", u8"sequence: `{}` did not load", step.name);
                return 0;
            }
            // Each of its bindings named for a speaker is that speaker's entity; one named for no
            // speaker is the level entity of its name (a prop the scene moves: a cart, a door).
            Array<Binding> filled;
            for (const foundation::sequence::SequenceBinding& binding : sequence->sequence.bindings)
            {
                const DialogueSpeaker* speaker = entryOf(binding.name.AsView());
                bool boundByGame = false;
                for (const Binding& own : m_bindings)
                {
                    boundByGame = boundByGame || own.speaker == binding.name;
                }
                const foundation::scene::EntityHandle entity = speaker != nullptr || boundByGame
                                                                   ? EntityOf(*scene, binding.name.AsView(), speaker)
                                                                   : scene->FindEntityByName(binding.name.AsView());
                if (entity.IsAssigned())
                {
                    filled.PushBack(Binding{binding.name, entity});
                }
            }
            const u32 play = system->Play(sequence, step.asset, foundation::sequence::SequenceFinish::Release, {},
                                          [&filled](foundation::sequence::SequencePlay& running)
                                          {
                                              for (const Binding& binding : filled)
                                              {
                                                  (void)running.Bind(binding.speaker.AsView(), binding.entity);
                                              }
                                          });
            const u32 token = m_nextToken++;
            m_staged.PushBack(Staged{token, step.kind, {}, play});
            return token;
        }
        default: return 0;
        }
    }

    bool DialogueRunHost::IsStaging(u32 token)
    {
        if (token != 0 && token == m_fadeToken)
        {
            return true;
        }
        for (usize i = 0; i < m_staged.Size(); ++i)
        {
            if (m_staged[i].token != token)
            {
                continue;
            }
            bool going = false;
            if (foundation::scene::Scene* scene = Scene())
            {
                if (m_staged[i].kind == DialogueStepKind::Play)
                {
                    const foundation::script::ServiceResolver services = Services();
                    foundation::script::ServiceScope scope(&services);
                    going = engine::animation::SceneAnimationFacade{scene}.isPlayingOneShot(
                        foundation::script::WrapEntity(scene, m_staged[i].entity));
                }
                else if (auto* system = scene->GetSystem<engine::sequence::SequenceSystem>())
                {
                    const foundation::sequence::SequencePlay* play = system->Find(m_staged[i].play);
                    going = play != nullptr && !play->IsFinished();
                }
            }
            if (!going)
            {
                m_staged.RemoveAt(i);
            }
            return going;
        }
        return false;
    }

    void DialogueRunHost::Update(f32 deltaTime)
    {
        UpdateFade(deltaTime);
        if (!BoxOn() || !m_box.IsShown() || m_runner == nullptr)
        {
            return;
        }
        bool pressed = false;
        if (auto* input = static_cast<foundation::input::ActionRuntime*>(Find(TypeOf<foundation::input::ActionRuntime>().id)))
        {
            const foundation::input::ActionRef action = input->Resolve(m_settings->advanceAction.AsView());
            pressed = input->WasPressed(action);
        }
        const DialogueStockBox::Request request = m_box.Update(deltaTime, pressed);
        if (request.choose >= 0)
        {
            (void)m_runner->Choose(request.choose);
        }
        else if (request.advance)
        {
            (void)m_runner->Advance();
        }
    }

    String DialogueRunHost::BoxSheet() const
    {
        if (!m_settings->stockBoxSheet.IsNil())
        {
            if (RefPtr<foundation::gui::GuiStyleSheet> sheet =
                    Load<foundation::gui::GuiStyleSheet>(Resources(), m_settings->stockBoxSheet))
            {
                return sheet->css;
            }
        }
        return String(DialogueStockBoxSheet());
    }

    bool DialogueRunHost::BoxPlace(engine::gui::GuiSubsystem*& gui, foundation::gui::ScreenStack*& stack) const
    {
        gui = static_cast<engine::gui::GuiSubsystem*>(Find(TypeOf<engine::gui::GuiSubsystem>().id));
        stack = gui != nullptr ? gui->ScreensFor(m_run) : nullptr;
        return stack != nullptr;
    }

    void DialogueRunHost::UpdateFade(f32 deltaTime)
    {
        if (m_fadeToken == 0)
        {
            return;
        }
        m_fadeTime += deltaTime;
        const f32 t = m_fadeSeconds > 0.0f ? Min(1.0f, m_fadeTime / m_fadeSeconds) : 1.0f;
        m_fadeAmount = m_fadeFrom + (m_fadeTo - m_fadeFrom) * t;
        if (t >= 1.0f)
        {
            m_fadeToken = 0;
        }
        foundation::scene::Scene* scene = m_fadeScene.Get();
        auto* post = scene != nullptr ? scene->GetSystem<engine::render::PostProcessSystem>() : nullptr;
        if (post == nullptr)
        {
            return;
        }
        if (m_fadeAmount <= 0.0f && m_fadeToken == 0)
        {
            post->FadeClaims().Release(this); // faded in: nothing held
            return;
        }
        // Over the sequences' fades: what a conversation fades to holds until it fades in again.
        post->FadeClaims().Claim(this, ~u64{0},
                                 engine::render::ScreenFade{Color(m_fadeColor.x, m_fadeColor.y, m_fadeColor.z, m_fadeColor.w),
                                                            m_fadeAmount});
    }

    void DialogueRunHost::ReleaseFade()
    {
        foundation::scene::Scene* scene = m_fadeScene.Get();
        if (auto* post = scene != nullptr ? scene->GetSystem<engine::render::PostProcessSystem>() : nullptr)
        {
            post->FadeClaims().Release(this);
        }
        m_fadeToken = 0;
        m_fadeAmount = 0.0f;
    }

    void DialogueRunHost::StopTalking()
    {
        foundation::scene::Scene* scene = m_talkScene.Get();
        if (scene != nullptr && m_talker.IsAssigned() && scene->IsValid(m_talker))
        {
            const foundation::script::ServiceResolver services = Services();
            foundation::script::ServiceScope scope(&services);
            engine::animation::SceneAnimationFacade{scene}.stopOneShot(foundation::script::WrapEntity(scene, m_talker));
        }
        m_talker = {};
    }

    void DialogueRunHost::OnLine(const DialogueLineNotice& notice, const DialogueSpeaker* speaker)
    {
        StopTalking();
        foundation::scene::Scene* scene = Scene();
        if (scene != nullptr && speaker != nullptr && !speaker->talk.IsNil())
        {
            const foundation::scene::EntityHandle entity = EntityOf(*scene, notice.speaker.AsView(), speaker);
            if (entity.IsAssigned())
            {
                const foundation::script::ServiceResolver services = Services();
                foundation::script::ServiceScope scope(&services);
                engine::animation::SceneAnimationFacade{scene}.playOneShot(foundation::script::WrapEntity(scene, entity),
                                                                           speaker->talk, 1.0f, 0.2f, 0.2f, -1);
                m_talkScene = scene;
                m_talker = entity;
            }
        }
        if (speaker != nullptr && !speaker->voice.IsNil())
        {
            const foundation::script::ServiceResolver services = Services();
            foundation::script::ServiceScope scope(&services);
            (void)engine::audio::AudioFacade::playOneShot(speaker->voice);
        }
        if (BoxOn())
        {
            engine::gui::GuiSubsystem* gui = nullptr;
            foundation::gui::ScreenStack* stack = nullptr;
            if (BoxPlace(gui, stack))
            {
                m_box.ShowLine(*gui, *stack, BoxSheet().AsView(), notice, speaker);
            }
        }
        Publish(*m_run, kDialogueLineEvent, notice);
    }

    void DialogueRunHost::OnLineEnded() { StopTalking(); }
    void DialogueRunHost::OnChoice(const DialogueChoiceNotice& notice)
    {
        if (BoxOn())
        {
            engine::gui::GuiSubsystem* gui = nullptr;
            foundation::gui::ScreenStack* stack = nullptr;
            if (BoxPlace(gui, stack))
            {
                m_box.ShowChoice(*gui, *stack, BoxSheet().AsView(), notice);
            }
        }
        Publish(*m_run, kDialogueChoiceEvent, notice);
    }
    void DialogueRunHost::OnCommand(const DialogueCommandNotice& notice)
    {
        DialogueCommandNotice placed = notice;
        placed.scene = Scene(); // where its entity arguments are
        Publish(*m_run, kDialogueCommandEvent, placed);
    }
    void DialogueRunHost::OnEnded(const DialogueEndedNotice& notice)
    {
        m_box.Hide();
        Publish(*m_run, kDialogueEndedEvent, notice);
    }

    engine::runtime::RunServiceContribution DialogueRunContribution(const DialogueSettings& settings)
    {
        g_runSettings = &settings;
        return engine::runtime::RunServiceContribution{u8"dialogue", TypeOf<DialogueRunService>().id, &InstallRunner};
    }
}
