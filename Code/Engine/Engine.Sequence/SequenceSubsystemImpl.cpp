// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.sequence - the scene's plays, where they report, the script surface and the domain.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE

module engine.sequence;

import foundation.core;
import foundation.messaging;
import foundation.profiler;
import foundation.resource;
import foundation.scene;
import foundation.script.facades;
import foundation.sequence;
import foundation.sequence.resource;
import engine.domain;

using namespace foundation::core;

namespace engine::sequence
{
    // ---- the system -----------------------------------------------------------------------------

    // Tells whoever started a play what it reports: its owner as entity events, or the run's bus as
    // notices. An event track bound to an actor posts to that actor.
    class SequenceSystem::Reporter final : public foundation::sequence::ISequenceListener
    {
    public:
        Reporter(scene::Scene& scene, u32 play, const Guid& sequence, scene::EntityHandle owner)
            : m_scene(&scene), m_play(play), m_sequence(sequence), m_owner(owner)
        {
        }

        void OnSequenceEvent(scene::EntityHandle target, StringView name, StringView value) override
        {
            if (m_scene->IsValid(target))
            {
                Post(target, kSequenceEventName, name, value);
                return;
            }
            Report(kSequenceEventName, kSequenceEventBusName, name, value);
        }
        void OnSequenceHeld(StringView marker) override { Report(kSequenceHeldName, kSequenceHeldBusName, marker, {}); }
        void OnSequenceFinished() override { Report(kSequenceFinishedName, kSequenceFinishedBusName, {}, {}); }

    private:
        void Report(StringView event, StringView busEvent, StringView name, StringView value)
        {
            if (m_scene->IsValid(m_owner))
            {
                Post(m_owner, event, name, value);
                return;
            }
            if (foundation::messaging::EventBus* bus = m_scene->Events())
            {
                SequenceNotice notice;
                notice.play = static_cast<i32>(m_play);
                notice.sequence = m_sequence;
                notice.name = String(name);
                notice.value = String(value);
                bus->Publish(StringHash(busEvent), Variant::From(Move(notice)));
            }
        }

        // The values an entity behavior's on<Name> receives: the name and value for an event, the
        // marker for a hold, nothing more for the end.
        void Post(scene::EntityHandle target, StringView event, StringView name, StringView value)
        {
            if (!m_scene->WantsEntityEvents())
            {
                return;
            }
            Variant args[2];
            usize count = 0;
            if (event != kSequenceFinishedName)
            {
                args[count++] = Variant::From<String>(String(name));
            }
            if (event == kSequenceEventName)
            {
                args[count++] = Variant::From<String>(String(value));
            }
            m_scene->PostEntityEvent(scene::EntityEvent{target, event, Span<const Variant>(args, count)});
        }

        scene::Scene* m_scene;
        u32 m_play;
        Guid m_sequence;
        scene::EntityHandle m_owner;
    };

    struct SequenceSystem::Running
    {
        u32 id = 0;
        RefPtr<foundation::sequence::SequenceResource> resource; // kept alive while it plays
        UniquePtr<Reporter> reporter;
        UniquePtr<foundation::sequence::SequencePlay> play;
    };

    SequenceSystem::~SequenceSystem()
    {
        // The scene is going away: nothing is put back or let go of in it.
        for (UniquePtr<Running>& running : m_plays)
        {
            running->play->Abandon();
        }
    }

    u32 SequenceSystem::Play(RefPtr<foundation::sequence::SequenceResource> sequence, const Guid& id, foundation::sequence::SequenceFinish finish, scene::EntityHandle owner)
    {
        if (m_scene == nullptr || !sequence)
        {
            return 0;
        }
        IAllocator& allocator = m_scene->Allocator();
        UniquePtr<Running> running = MakeUnique<Running>(allocator);
        running->id = m_nextId++;
        running->resource = Move(sequence);
        running->reporter = MakeUnique<Reporter>(allocator, *m_scene, running->id, id, owner);
        running->play = MakeUnique<foundation::sequence::SequencePlay>(allocator, allocator, *m_scene, running->resource->sequence);
        running->play->finish = finish;
        running->play->SetListener(running->reporter.Get());
        const u32 playId = running->id;
        foundation::sequence::SequencePlay* play = running->play.Get();
        m_plays.PushBack(Move(running));
        play->Play();
        return playId;
    }

    foundation::sequence::SequencePlay* SequenceSystem::Find(u32 id) noexcept
    {
        for (UniquePtr<Running>& running : m_plays)
        {
            if (running->id == id)
            {
                return running->play.Get();
            }
        }
        return nullptr;
    }

    void SequenceSystem::Stop(u32 id)
    {
        for (usize i = 0; i < m_plays.Size(); ++i)
        {
            if (m_plays[i]->id == id)
            {
                m_plays[i]->play->Stop();
                m_plays.RemoveAt(i);
                return;
            }
        }
    }

    void SequenceSystem::StopAll()
    {
        // Last first, so values two plays changed go back to what was there before both.
        for (usize i = m_plays.Size(); i > 0; --i)
        {
            m_plays[i - 1]->play->Stop();
        }
        m_plays.Clear();
    }

    void SequenceSystem::OnUpdate(scene::ScenePhase phase, f32 deltaTime)
    {
        if (phase != scene::ScenePhase::Update || m_plays.IsEmpty())
        {
            return;
        }
        PROFILE_SCOPE("Sequence.Update");
        // A play started during this loop (none can be: reports are queued, not called) would
        // step next frame; one finished here leaves the list after the loop.
        const usize count = m_plays.Size();
        for (usize i = 0; i < count; ++i)
        {
            m_plays[i]->play->Advance(deltaTime);
        }
        for (usize i = m_plays.Size(); i > 0; --i)
        {
            foundation::sequence::SequencePlay& play = *m_plays[i - 1]->play;
            if (play.IsFinished() && play.finish != foundation::sequence::SequenceFinish::Hold)
            {
                m_plays.RemoveAt(i - 1); // already let go when it finished
            }
        }
    }

    foundation::sequence::SequenceFinish ParseSequenceFinish(StringView name) noexcept
    {
        if (name == StringView(u8"hold"))
        {
            return foundation::sequence::SequenceFinish::Hold;
        }
        if (name == StringView(u8"restore"))
        {
            return foundation::sequence::SequenceFinish::Restore;
        }
        return foundation::sequence::SequenceFinish::Release;
    }

    void AddSequenceSceneSystems(scene::Scene& scene) { (void)scene.AddSystem<SequenceSystem>(); }

    // ---- the script surface ---------------------------------------------------------------------

    foundation::sequence::SequencePlay* SequencePlayHandle::Play() const
    {
        scene::Scene* owner = scene.Get();
        if (owner == nullptr || play == 0)
        {
            return nullptr;
        }
        SequenceSystem* system = owner->GetSystem<SequenceSystem>();
        return system != nullptr ? system->Find(play) : nullptr;
    }

    bool SequencePlayHandle::bind(String name, foundation::script::Entity entity) const
    {
        foundation::sequence::SequencePlay* p = Play();
        return p != nullptr && p->Bind(name.AsView(), entity.Handle());
    }

    void SequencePlayHandle::stop() const
    {
        scene::Scene* owner = scene.Get();
        if (owner != nullptr)
        {
            if (SequenceSystem* system = owner->GetSystem<SequenceSystem>())
            {
                system->Stop(play);
            }
        }
    }

    void SequencePlayHandle::pause() const
    {
        if (foundation::sequence::SequencePlay* p = Play())
        {
            p->Pause();
        }
    }

    void SequencePlayHandle::resume() const
    {
        if (foundation::sequence::SequencePlay* p = Play())
        {
            p->Resume();
        }
    }

    void SequencePlayHandle::seek(f32 time) const
    {
        if (foundation::sequence::SequencePlay* p = Play())
        {
            p->Seek(time);
        }
    }

    void SequencePlayHandle::setSpeed(f32 speed) const
    {
        if (foundation::sequence::SequencePlay* p = Play())
        {
            p->speed = Max(0.0f, speed);
        }
    }

    f32 SequencePlayHandle::time() const
    {
        foundation::sequence::SequencePlay* p = Play();
        return p != nullptr ? p->Time() : 0.0f;
    }

    f32 SequencePlayHandle::duration() const
    {
        foundation::sequence::SequencePlay* p = Play();
        return p != nullptr ? p->Duration() : 0.0f;
    }

    bool SequencePlayHandle::isPlaying() const
    {
        foundation::sequence::SequencePlay* p = Play();
        return p != nullptr && p->IsPlaying();
    }

    bool SequencePlayHandle::isHeld() const
    {
        foundation::sequence::SequencePlay* p = Play();
        return p != nullptr && p->IsHeld();
    }

    SequencePlayHandle SceneSequencesFacade::play(Guid sequence) const { return play(sequence, String(u8"release")); }

    SequencePlayHandle SceneSequencesFacade::play(Guid sequence, String finish) const
    {
        SequencePlayHandle handle;
        scene::Scene* owner = scene.Get();
        foundation::resource::ResourceManager* resources = foundation::script::CurrentRunResources();
        if (owner == nullptr || resources == nullptr || sequence.IsNil())
        {
            return handle;
        }
        SequenceSystem* system = owner->GetSystem<SequenceSystem>();
        if (system == nullptr)
        {
            return handle;
        }
        foundation::resource::Ref<foundation::sequence::SequenceResource> ref;
        ref.SetId(sequence);
        ref.Bind(*resources);
        if (ref.Get() == nullptr)
        {
            LOG_WARNING(u8"Sequence", u8"scene.sequences.play: no sequence asset with that id");
            return handle;
        }
        handle.scene = scene;
        handle.play = system->Play(RefPtr<foundation::sequence::SequenceResource>(ref.Get()), sequence, ParseSequenceFinish(finish.AsView()));
        return handle;
    }

    void SceneSequencesFacade::stopAll() const
    {
        if (scene::Scene* owner = scene.Get())
        {
            if (SequenceSystem* system = owner->GetSystem<SequenceSystem>())
            {
                system->StopAll();
            }
        }
    }

    REFLECT_VALUE(SequencePlayHandle, "rtti::engine::sequence")
    {
        builder.Attribute("scriptName", "SequencePlay");
        builder.Method<&SequencePlayHandle::id>("id");
        builder.Method<&SequencePlayHandle::bind>("bind", {"name", "entity"});
        builder.Method<&SequencePlayHandle::stop>("stop");
        builder.Method<&SequencePlayHandle::pause>("pause");
        builder.Method<&SequencePlayHandle::resume>("resume");
        builder.Method<&SequencePlayHandle::seek>("seek", {"time"});
        builder.Method<&SequencePlayHandle::setSpeed>("setSpeed", {"speed"});
        builder.Method<&SequencePlayHandle::time>("time");
        builder.Method<&SequencePlayHandle::duration>("duration");
        builder.Method<&SequencePlayHandle::isPlaying>("isPlaying");
        builder.Method<&SequencePlayHandle::isHeld>("isHeld");
    }

    REFLECT_VALUE(SequenceNotice, "rtti::engine::sequence")
    {
        builder.Property<&SequenceNotice::play>("play");
        builder.Property<&SequenceNotice::sequence>("sequence");
        builder.Property<&SequenceNotice::name>("name");
        builder.Property<&SequenceNotice::value>("value");
    }

    REFLECT_VALUE(SceneSequencesFacade, "rtti::engine::sequence")
    {
        builder.Attribute("scriptName", "SceneSequences");
        builder.Method<static_cast<SequencePlayHandle (SceneSequencesFacade::*)(Guid) const>(&SceneSequencesFacade::play)>(
            "play", {"sequence"});
        builder.Method<static_cast<SequencePlayHandle (SceneSequencesFacade::*)(Guid, String) const>(&SceneSequencesFacade::play)>(
            "play", {"sequence", "finish"});
        builder.Method<&SceneSequencesFacade::stopAll>("stopAll");
    }

    void RegisterSequenceReflection()
    {
        static const bool once = []()
        {
            foundation::sequence::RegisterSequenceResource();
            RttiRegisterValue_SequencePlayHandle();
            GlobalTypeRegistry().Register(TypeOf<SequencePlayHandle>());
            RttiRegisterValue_SequenceNotice();
            GlobalTypeRegistry().Register(TypeOf<SequenceNotice>());
            RttiRegisterValue_SceneSequencesFacade();
            GlobalTypeRegistry().Register(TypeOf<SceneSequencesFacade>());
            return true;
        }();
        (void)once;
    }

    void RegisterSequenceScriptFacade()
    {
        static const bool once = []()
        {
            RegisterSequenceReflection();
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SequencePlayHandle>());
            foundation::script::RegisterExtraFacadeName(u8"SequencePlay");
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SequenceNotice>());
            foundation::script::RegisterExtraFacadeName(u8"SequenceNotice");
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SceneSequencesFacade>());
            foundation::script::RegisterExtraFacadeName(u8"SceneSequences");
            foundation::script::ExtendScene<&SceneSequencesFacade::ForScene>("sequences"); // scene.sequences
            return true;
        }();
        (void)once;
    }

    const engine::DomainModule& SequenceDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {&foundation::sequence::kSequenceResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"sequence",
            .installScene = &AddSequenceSceneSystems,
            .registerReflection = &RegisterSequenceReflection,
            .registerScriptFacade = &RegisterSequenceScriptFacade,
            .resources = Span<const foundation::resource::ResourceModule* const>{kResources, 1}};
        return kModule;
    }
}
