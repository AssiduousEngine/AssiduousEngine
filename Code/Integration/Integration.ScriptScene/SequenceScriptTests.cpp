// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A behavior plays a sequence in every language: scene.sequences.play(id) loads it through the
// run's resources and returns its play, the behavior fills a name with an entity, and what the play
// reports reaches the behavior's on<Name> as a SequenceNotice naming that play.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import engine.behavior;
import engine.sequence;
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
import foundation.script.angelscript;
#endif
#if defined(INTEGRATION_HAS_LUAU)
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
using namespace engine::behavior;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;

namespace
{
    void RegisterOnce()
    {
        static const bool once = []()
        {
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
            foundation::script::angelscript::RegisterAngelScriptBackend();
#endif
#if defined(INTEGRATION_HAS_LUAU)
            foundation::script::RegisterLuauScriptBackend();
#endif
            RegisterCoreTypes();
            RegisterBehaviorComponentReflection();
            RegisterScriptFacadeReflection();
            engine::sequence::RegisterSequenceScriptFacade();
            return true;
        }();
        (void)once;
    }

    // A content database holding one cooked sequence: "Actor" walks X to 10 over a second, with a
    // cue "gate" at 0.5 on no one, so the play reports it.
    struct SequenceStore
    {
        StringView dir = u8"scratch_sequence_script";
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        UniquePtr<sequence::SequenceFactory> factory;
        UniquePtr<foundation::resource::ResourceManager> resources;
        Guid walk;

        SequenceStore()
        {
            RemoveDirectoryRecursive(dir);
            CreateDirectories(dir);
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), dir, DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                                  BinarySerializerFactory(), u8".rasset");
            foundation::content::Instance* instance = db->RootGroup()->CreateInstance(u8"walk", sequence::SequenceSource::StaticType());
            walk = instance->Id();
            sequence::SequenceSource source;
            source.sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"Actor")});
            RefPtr<sequence::PropertySequenceTrack> move = MakeRef<sequence::PropertySequenceTrack>(DefaultAllocator());
            move->binding = 0;
            move->property.target = String(propertyanimation::kTransformTarget);
            move->property.propertyPath = String(u8"position");
            move->property.kind = propertyanimation::TrackValueKind::Float3;
            CurveKey a;
            a.time = 0.0f;
            a.value = 0.0f;
            CurveKey b;
            b.time = 1.0f;
            b.value = 10.0f;
            move->property.channels[0].AddKey(a);
            move->property.channels[0].AddKey(b);
            source.sequence.tracks.PushBack(move);
            RefPtr<sequence::EventSequenceTrack> cues = MakeRef<sequence::EventSequenceTrack>(DefaultAllocator());
            cues->keys.PushBack(sequence::SequenceEventKey{0.5f, String(u8"cue"), String(u8"gate")});
            source.sequence.tracks.PushBack(cues);
            source.sequence.duration = 1.0f;
            REQUIRE(instance->WriteObject(source).IsOk());
            factory = MakeUnique<sequence::SequenceFactory>(DefaultAllocator(), DefaultAllocator());
            resources = MakeUnique<foundation::resource::ResourceManager>(DefaultAllocator(), DefaultAllocator(), *db);
            resources->AddFactory(factory.Get());
        }
        ~SequenceStore()
        {
            resources.Reset();
            db.Reset();
            mount.Reset();
            RemoveDirectoryRecursive(dir);
        }
    };

    struct SequenceScene
    {
        SequenceStore store;
        foundation::run::RunBase run{DefaultAllocator(), u8"sequences"};
        scene::Scene scene{DefaultAllocator(), u8"sequences"};
        BehaviorRunHost host{DefaultAllocator()};
        BehaviorComponentManager* components = nullptr;

        SequenceScene()
        {
            RegisterOnce();
            scene.SetRun(&run);
            components = scene.AddSystem<BehaviorComponentManager>();
            EntityBehaviorSystem* behaviors = scene.AddSystem<EntityBehaviorSystem>();
            components->SetScriptSystem(behaviors);
            behaviors->SetRunHost(&host);
            engine::sequence::AddSequenceSceneSystems(scene);
            foundation::resource::ResourceManager* resources = store.resources.Get();
            host.Binding().resolveResources = [resources]() { return resources; };
        }

        void Attach(scene::EntityHandle e, StringView language, StringView source)
        {
            RefPtr<BehaviorClass> cls = MakeRef<BehaviorClass>(DefaultAllocator());
            cls->language = String(language);
            cls->className = String(u8"Director");
            cls->kind = BehaviorKind::Entity;
            cls->source = String(source);
            cls->handlers.PushBack(String(u8"onStart"));
            cls->handlers.PushBack(String(u8"onSequenceEvent"));
            cls->handlers.PushBack(String(u8"onSequenceFinished"));
            cls->BuildProfileName();
            AttachedBehavior behavior;
            behavior.behaviorClass = cls;
            components->Add(e).behaviors.PushBack(Move(behavior));
        }

        // Plays a second and a half: the behavior starts the sequence, it runs to its end, and its
        // reports are delivered.
        void Run()
        {
            scene.Start();
            scene.SetSimulationEnabled(true);
            for (int i = 0; i < 8; ++i)
            {
                host.Binding().deltaSeconds = 0.25f;
                run.Events().Drain(); // the run drains at the top of a frame
                scene.Update(0.25f);
            }
        }

        [[nodiscard]] String GuidText() const
        {
            utf8char text[37];
            store.walk.ToChars(text);
            return String(StringView(text, 36));
        }
    };
}

#if defined(INTEGRATION_HAS_ANGELSCRIPT)
TEST_CASE("sequences: an AngelScript behavior plays a sequence and hears its notices")
{
    SequenceScene s;
    const scene::EntityHandle actor = s.scene.CreateEntity(u8"actor");
    s.Attach(actor, u8"angelscript",
             Format(u8"class Director : EntityBehavior {{\n"
                    u8"    private Entity self;\n"
                    u8"    private SequencePlay play;\n"
                    u8"    Director(Entity entity) {{ self = entity; }}\n"
                    u8"    void onStart() {{\n"
                    u8"        play = self.scene.sequences.play(Guid(\"{}\"));\n"
                    u8"        play.bind(\"Actor\", self);\n"
                    u8"    }}\n"
                    u8"    void onSequenceEvent(SequenceNotice@ notice) {{\n"
                    u8"        if (notice.play == play.id()) {{ self.setName(notice.name + \" \" + notice.value); }}\n"
                    u8"    }}\n"
                    u8"    void onSequenceFinished(SequenceNotice@ notice) {{\n"
                    u8"        if (notice.play == play.id() && !play.isPlaying()) {{ self.setName(self.name() + \" done\"); }}\n"
                    u8"    }}\n"
                    u8"}}\n",
                    s.GuidText())
                 .AsView());
    s.Run();
    CHECK(s.scene.GetEntityName(actor) == StringView(u8"cue gate done"));
    CHECK(s.scene.GetLocalTransform(actor).position.x == doctest::Approx(10.0f));
}
#endif

#if defined(INTEGRATION_HAS_LUAU)
TEST_CASE("sequences: a Luau behavior plays a sequence and hears its notices")
{
    SequenceScene s;
    const scene::EntityHandle actor = s.scene.CreateEntity(u8"actor");
    s.Attach(actor, u8"luau",
             Format(u8"Director = EntityBehavior.class()\n"
                    u8"function Director.new(entity) return setmetatable({{ entity = entity }}, Director) end\n"
                    u8"function Director:onStart()\n"
                    u8"    self.play = self.entity.scene.sequences:play(Guid.new(\"{}\"))\n"
                    u8"    self.play:bind(\"Actor\", self.entity)\n"
                    u8"end\n"
                    u8"function Director:onSequenceEvent(notice)\n"
                    u8"    if notice.play == self.play:id() then self.entity:setName(notice.name .. \" \" .. notice.value) end\n"
                    u8"end\n"
                    u8"function Director:onSequenceFinished(notice)\n"
                    u8"    if notice.play == self.play:id() and not self.play:isPlaying() then\n"
                    u8"        self.entity:setName(self.entity:name() .. \" done\")\n"
                    u8"    end\n"
                    u8"end\n",
                    s.GuidText())
                 .AsView());
    s.Run();
    CHECK(s.scene.GetEntityName(actor) == StringView(u8"cue gate done"));
    CHECK(s.scene.GetLocalTransform(actor).position.x == doctest::Approx(10.0f));
}
#endif
