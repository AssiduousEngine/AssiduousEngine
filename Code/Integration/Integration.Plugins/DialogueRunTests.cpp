// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the dialogue runner a run holds, staging in the run's scene. A conversation
// fades out, plays a sequence whose binding is named for a speaker and waits for it, fades in, and
// shows a line on the run's bus. The fade is a claim on the scene's fade, held at full until the
// fade in lets it go; the sequence moves the entity the cast names for the speaker, or the one the
// game bound to it.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.run;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import engine.behavior;
import engine.run;
import engine.render;
import engine.sequence;
import dialogue.engine;
import dialogue.pipeline;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;

namespace
{
    // A content database holding a cooked conversation, its cast (the keeper played by "Keeper") and
    // a sequence that walks its binding "keeper" to X 10 over a second.
    struct DialogueStore
    {
        StringView dir = u8"scratch_dialogue_run";
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        UniquePtr<dialogue::DialogueFactory> dialogues;
        UniquePtr<dialogue::DialogueCastFactory> casts;
        UniquePtr<sequence::SequenceFactory> sequences;
        UniquePtr<foundation::resource::ResourceManager> resources;
        Guid gate;
        Guid walk;

        DialogueStore()
        {
            dialogue::RegisterDialogueTypes();
            engine::sequence::RegisterSequenceReflection();
            (void)RemoveDirectoryRecursive(dir);
            (void)CreateDirectories(dir);
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), dir, DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                                  BinarySerializerFactory(), u8".rasset");
            foundation::content::Group& root = *db->RootGroup();

            foundation::content::Instance* walkInstance = root.CreateInstance(u8"walk", sequence::SequenceSource::StaticType());
            walk = walkInstance->Id();
            sequence::SequenceSource walkSource;
            walkSource.sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"keeper")});
            RefPtr<sequence::PropertySequenceTrack> move = MakeRef<sequence::PropertySequenceTrack>(DefaultAllocator());
            move->binding = 0;
            move->property.componentType = String(propertyanimation::kTransformTarget);
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
            walkSource.sequence.tracks.PushBack(move);
            walkSource.sequence.duration = 1.0f;
            REQUIRE(walkInstance->WriteObject(walkSource).IsOk());

            foundation::content::Instance* castInstance = root.CreateInstance(u8"cast", dialogue::DialogueCastSource::StaticType());
            dialogue::DialogueCastSource cast;
            dialogue::DialogueSpeaker& keeper = cast.speakers.EmplaceBack();
            keeper.id = String(u8"keeper");
            keeper.shownName = String(u8"The Keeper");
            keeper.entity = String(u8"Keeper");
            REQUIRE(castInstance->WriteObject(cast).IsOk());

            dialogue::DialogueParse parse = dialogue::ParseDialogue(u8"conversation gate\n"
                                                                    u8"fade out for 1\n"
                                                                    u8"sequence walk and wait\n"
                                                                    u8"fade in for 1\n"
                                                                    u8"@ keeper: Done.\n",
                                                                    {});
            REQUIRE(parse.Ok());
            REQUIRE(parse.conversations[0].steps[1].kind == dialogue::DialogueStepKind::Sequence);
            parse.conversations[0].steps[1].asset = walk; // what the cook resolves through the dialogue assets
            foundation::content::Instance* gateInstance = root.CreateInstance(u8"gate", dialogue::DialogueSource::StaticType());
            gate = gateInstance->Id();
            dialogue::DialogueSource source;
            source.cast = castInstance->Id();
            source.conversations = Move(parse.conversations);
            source.lines = Move(parse.lines);
            REQUIRE(gateInstance->WriteObject(source).IsOk());

            dialogues = MakeUnique<dialogue::DialogueFactory>(DefaultAllocator(), DefaultAllocator());
            casts = MakeUnique<dialogue::DialogueCastFactory>(DefaultAllocator(), DefaultAllocator());
            sequences = MakeUnique<sequence::SequenceFactory>(DefaultAllocator(), DefaultAllocator());
            resources = MakeUnique<foundation::resource::ResourceManager>(DefaultAllocator(), DefaultAllocator(), *db);
            resources->AddFactory(dialogues.Get());
            resources->AddFactory(casts.Get());
            resources->AddFactory(sequences.Get());
        }
        ~DialogueStore()
        {
            resources.Reset();
            db.Reset();
            mount.Reset();
            (void)RemoveDirectoryRecursive(dir);
        }
    };

    // A run with what the runner's host reads (its scripts' host for the resources, its current
    // scene, its save) and its runner, over a running scene that fades and plays sequences.
    struct DialogueRun
    {
        DialogueStore store;
        dialogue::DialogueSettings settings;
        foundation::run::RunBase run{DefaultAllocator(), u8"dialogue"};
        scene::Scene scene{DefaultAllocator(), u8"dialogue"};
        engine::behavior::BehaviorRunHost scripts{DefaultAllocator()};
        engine::render::PostProcessSystem* post = nullptr;
        dialogue::DialogueRunService* runner = nullptr;
        Array<String> lines;

        DialogueRun()
        {
            scene.SetRun(&run);
            engine::sequence::AddSequenceSceneSystems(scene);
            post = scene.AddSystem<engine::render::PostProcessSystem>();
            foundation::resource::ResourceManager* resources = store.resources.Get();
            scripts.Binding().resolveResources = [resources]() { return resources; };
            (void)run.Register<engine::behavior::BehaviorRunHost>(scripts);
            engine::runtime::RunBinding& binding = run.Add<engine::runtime::RunBinding>();
            scene::Scene* current = &scene;
            binding.currentScene = [current]() { return current; };
            (void)run.Add<engine::runtime::RunSave>(DefaultAllocator());
            runner = &run.Add<dialogue::DialogueRunService>(settings, run);
            (void)run.Events().Subscribe(StringHash(dialogue::kDialogueLineEvent),
                                         [this](const Variant& value)
                                         {
                                             if (const auto* notice = value.TryGet<dialogue::DialogueLineNotice>())
                                             {
                                                 lines.PushBack(Format(u8"{}: {}", notice->shownName, notice->text));
                                             }
                                         });
            scene.Start();
            scene.SetSimulationEnabled(true);
        }

        // A frame: the bus, the run's services (the runner), then the scene (its sequences).
        void Frame(f32 seconds)
        {
            run.Events().Drain();
            run.UpdateServices(seconds);
            scene.Update(seconds);
        }
    };
}

TEST_CASE("plugins: a run's dialogue runner fades, plays a sequence bound to the cast's entity and waits for it")
{
    DialogueRun r;
    const scene::EntityHandle keeper = r.scene.CreateEntity(u8"Keeper");
    REQUIRE(r.runner->Play(r.store.gate, u8"gate"));

    r.Frame(0.5f);
    CHECK(r.post->Fade().amount == doctest::Approx(0.5f));
    r.Frame(0.5f); // the fade out ends; the sequence starts, held at full black
    CHECK(r.post->Fade().amount == doctest::Approx(1.0f));
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Staging);
    for (int i = 0; i < 5; ++i)
    {
        r.Frame(0.25f);
    }
    CHECK(r.scene.GetLocalTransform(keeper).position.x == doctest::Approx(10.0f));
    CHECK(r.post->Fade().amount == doctest::Approx(0.5f)); // the sequence ended; half way into the fade in
    CHECK(r.lines.IsEmpty());

    for (int i = 0; i < 3; ++i)
    {
        r.Frame(0.25f);
    }
    CHECK(r.post->Fade().amount == doctest::Approx(0.0f)); // faded in: the claim let go
    CHECK(r.post->FadeClaims().WinningValue() == nullptr);
    r.Frame(0.0f); // the line's notice is delivered
    REQUIRE(r.lines.Size() == 1u);
    CHECK(r.lines[0] == StringView(u8"The Keeper: Done."));
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Line);
}

TEST_CASE("plugins: a speaker the game binds plays as that entity, over the one the cast names")
{
    DialogueRun r;
    const scene::EntityHandle keeper = r.scene.CreateEntity(u8"Keeper");
    const scene::EntityHandle standIn = r.scene.CreateEntity(u8"Stand-in");
    r.runner->Bind(u8"keeper", standIn);
    REQUIRE(r.runner->Play(r.store.gate, u8"gate"));
    for (int i = 0; i < 16; ++i)
    {
        r.Frame(0.25f);
    }
    CHECK(r.scene.GetLocalTransform(standIn).position.x == doctest::Approx(10.0f));
    CHECK(r.scene.GetLocalTransform(keeper).position.x == doctest::Approx(0.0f));
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Line);
}
