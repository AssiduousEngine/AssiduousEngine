// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the dialogue runner a run holds, staging in the run's scene. A conversation
// fades out, plays a sequence whose binding is named for a speaker and waits for it, fades in, and
// shows a line on the run's bus. The fade is a claim on the scene's fade, held at full until the
// fade in lets it go; the sequence moves the entity the cast names for the speaker, or the one the
// game bound to it. And the `Dialogue` facade in every language: a behavior binds a speaker, sets a
// flag, plays a cooked conversation by its file's guid, and answers its line, choice and command
// in its on<Name> handlers until it ends. And a sequence's line track: the play shows each line
// through the run's runner and holds at its end until it is advanced. And the stock box on a real
// GUI: the line typed out, a press finishing it then advancing, the answers pressed, the box gone
// at the end.

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
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
import angelscript.foundation;
#endif
#if defined(INTEGRATION_HAS_LUAU)
import luau.foundation;
#endif
import engine.behavior;
import engine.run;
import engine.render;
import engine.sequence;
import engine.gui;
import foundation.runtime;
import foundation.gui.gamekit;
import foundation.gui;
import foundation.gui.resources;
import dialogue.foundation;
import dialogue.engine;
import dialogue.pipeline;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;
namespace runtime = foundation::runtime;

namespace
{
    constexpr StringView kStaged = u8"conversation gate\n"
                                   u8"fade out for 1\n"
                                   u8"sequence walk and wait\n"
                                   u8"fade in for 1\n"
                                   u8"@ keeper: Done.\n";

    void RegisterScriptsOnce()
    {
        static const bool once = []()
        {
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
            angelscript::RegisterAngelScriptBackend();
#endif
#if defined(INTEGRATION_HAS_LUAU)
            luau::RegisterLuauScriptBackend();
#endif
            RegisterCoreTypes();
            engine::behavior::RegisterBehaviorComponentReflection();
            foundation::script::RegisterScriptFacadeReflection();
            return true;
        }();
        (void)once;
    }

    // A content database holding a cooked conversation (`text`, its `sequence` the walk), its cast
    // (the keeper played by "Keeper") and a sequence that walks its binding "keeper", and "Cart"
    // (no speaker), to X 10 over a second.
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
        Guid castId;

        explicit DialogueStore(StringView text)
        {
            engine::sequence::RegisterSequenceReflection();
            dialogue::RegisterDialogueTypes();
            dialogue::RegisterDialogueLineTrack(); // the Engine part registers it as it loads
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
            walkSource.sequence.tracks.PushBack(move);
            // A second part named for no speaker: the level entity of that name (a prop).
            walkSource.sequence.bindings.PushBack(sequence::SequenceBinding{String(u8"Cart")});
            RefPtr<sequence::PropertySequenceTrack> roll = MakeRef<sequence::PropertySequenceTrack>(DefaultAllocator());
            roll->binding = 1;
            roll->property = move->property;
            walkSource.sequence.tracks.PushBack(roll);
            walkSource.sequence.duration = 1.0f;
            REQUIRE(walkInstance->WriteObject(walkSource).IsOk());

            foundation::content::Instance* castInstance = root.CreateInstance(u8"cast", dialogue::DialogueCastSource::StaticType());
            dialogue::DialogueCastSource cast;
            dialogue::DialogueSpeaker& keeper = cast.speakers.EmplaceBack();
            keeper.id = String(u8"keeper");
            keeper.shownName = String(u8"The Keeper");
            keeper.entity = String(u8"Keeper");
            REQUIRE(castInstance->WriteObject(cast).IsOk());
            castId = castInstance->Id();

            dialogue::DialogueCommandForm cue;
            String error;
            REQUIRE(dialogue::ParseCommandForm(u8"cue <event:text> waits", cue, error));
            dialogue::DialogueParse parse = dialogue::ParseDialogue(text, Span<const dialogue::DialogueCommandForm>(&cue, 1));
            REQUIRE(parse.Ok());
            for (dialogue::DialogueStep& step : parse.conversations[0].steps)
            {
                if (step.kind == dialogue::DialogueStepKind::Sequence)
                {
                    step.asset = walk; // what the cook resolves through the dialogue assets
                }
            }
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
        engine::behavior::BehaviorComponentManager* components = nullptr;
        dialogue::DialogueSettings settings;
        foundation::run::RunBase run{DefaultAllocator(), u8"dialogue"};
        scene::Scene scene{DefaultAllocator(), u8"dialogue"};
        engine::behavior::BehaviorRunHost scripts{DefaultAllocator()};
        engine::render::PostProcessSystem* post = nullptr;
        dialogue::DialogueRunService* runner = nullptr;
        Array<String> lines;

        explicit DialogueRun(StringView text = kStaged) : store(text)
        {
            RegisterScriptsOnce();
            dialogue::RegisterDialogueScriptFacade();
            scene.SetRun(&run);
            components = scene.AddSystem<engine::behavior::BehaviorComponentManager>();
            auto* behaviors = scene.AddSystem<engine::behavior::EntityBehaviorSystem>();
            components->SetScriptSystem(behaviors);
            behaviors->SetRunHost(&scripts);
            scripts.Binding().run = &run;
            engine::sequence::AddSequenceSceneSystems(scene);
            post = scene.AddSystem<engine::render::PostProcessSystem>();
            foundation::resource::ResourceManager* resources = store.resources.Get();
            scripts.Binding().resolveResources = [resources]() { return resources; };
            (void)run.Register<engine::behavior::BehaviorRunHost>(scripts);
            engine::runtime::RunBinding& binding = run.Add<engine::runtime::RunBinding>();
            scene::Scene* current = &scene;
            binding.currentScene = [current]() { return current; };
            engine::runtime::RunSaves& saves = run.Add<engine::runtime::RunSaves>(DefaultAllocator());
            runner = &run.Add<dialogue::DialogueRunService>(settings, run);
            REQUIRE(runner->UseSave(saves.GlobalHandle())); // a game with one save hands it its global store
            (void)run.Events().Subscribe(StringHash(dialogue::kDialogueLineEvent),
                                         [this](const Variant& value)
                                         {
                                             if (const auto* notice = value.TryGet<dialogue::DialogueLineNotice>())
                                             {
                                                 lines.PushBack(Format(u8"{}: {}", notice->shownName, notice->text));
                                             }
                                         });
        }

        ~DialogueRun() { dialogue::RemoveDialogueScriptFacade(); }

        // Starts the scene, its behaviors' onStart with it.
        void Start()
        {
            scene.Start();
            scene.SetSimulationEnabled(true);
        }

        void Attach(scene::EntityHandle e, StringView language, StringView source)
        {
            RefPtr<foundation::script::BehaviorClass> cls = MakeRef<foundation::script::BehaviorClass>(DefaultAllocator());
            cls->language = String(language);
            cls->className = String(u8"Director");
            cls->kind = foundation::script::BehaviorKind::Entity;
            cls->source = String(source);
            const StringView handlers[] = {u8"onStart", u8"onDialogueLine", u8"onDialogueChoice", u8"onDialogueCommand",
                                           u8"onDialogueEnded"};
            for (StringView handler : handlers)
            {
                cls->handlers.PushBack(String(handler));
            }
            cls->BuildProfileName();
            engine::behavior::AttachedBehavior behavior;
            behavior.behaviorClass = cls;
            components->Add(e).behaviors.PushBack(Move(behavior));
        }

        [[nodiscard]] String GateText() const
        {
            utf8char text[37];
            store.gate.ToChars(text);
            return String(StringView(text, 36));
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
    const scene::EntityHandle cart = r.scene.CreateEntity(u8"Cart");
    r.Start();
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
    CHECK(r.scene.GetLocalTransform(cart).position.x == doctest::Approx(10.0f)); // the prop by its name
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
    r.Start();
    REQUIRE(r.runner->Play(r.store.gate, u8"gate"));
    for (int i = 0; i < 16; ++i)
    {
        r.Frame(0.25f);
    }
    CHECK(r.scene.GetLocalTransform(standIn).position.x == doctest::Approx(10.0f));
    CHECK(r.scene.GetLocalTransform(keeper).position.x == doctest::Approx(0.0f));
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Line);
}

namespace
{
    constexpr StringView kTalk = u8"conversation gate\n"
                                 u8"if met\n"
                                 u8"  @ keeper: Welcome back.\n"
                                 u8"end\n"
                                 u8"choice\n"
                                 u8"> Stay.\n"
                                 u8"> Go.\n"
                                 u8"  cue lights out\n"
                                 u8"end\n";

    // The scene runs a few frames: the behavior starts the conversation and answers each notice.
    [[maybe_unused]] void Talk(DialogueRun& r) // per enabled language
    {
        r.Start();
        for (int i = 0; i < 8; ++i)
        {
            r.Frame(0.25f);
        }
    }
}

#if defined(INTEGRATION_HAS_ANGELSCRIPT)
TEST_CASE("plugins: an AngelScript behavior plays a conversation through Dialogue and answers its notices")
{
    DialogueRun r(kTalk);
    const scene::EntityHandle director = r.scene.CreateEntity(u8"director");
    r.Attach(director, u8"angelscript",
             Format(u8"class Director : EntityBehavior {{\n"
                    u8"    private Entity self;\n"
                    u8"    Director(Entity entity) {{ self = entity; }}\n"
                    u8"    private void say(const string &in what) {{ self.setName(self.name() + \"|\" + what); }}\n"
                    u8"    void onStart() {{\n"
                    u8"        Dialogue::bind(\"keeper\", self);\n"
                    u8"        Dialogue::setFlag(\"met\", true);\n"
                    u8"        if (Dialogue::play(Guid(\"{}\"), \"gate\") && Dialogue::isPlaying()) {{ say(\"playing\"); }}\n"
                    u8"    }}\n"
                    u8"    void onDialogueLine(DialogueLineNotice@ notice) {{\n"
                    u8"        say(notice.shownName + \": \" + notice.text);\n"
                    u8"        Dialogue::advance();\n"
                    u8"    }}\n"
                    u8"    void onDialogueChoice(DialogueChoiceNotice@ notice) {{\n"
                    u8"        say(notice.answer(0) + \" or \" + notice.answer(1));\n"
                    u8"        Dialogue::choose(1);\n"
                    u8"    }}\n"
                    u8"    void onDialogueCommand(DialogueCommandNotice@ notice) {{\n"
                    u8"        say(notice.name + \" \" + notice.text(\"event\"));\n"
                    u8"        if (notice.waits) {{ Dialogue::done(); }}\n"
                    u8"    }}\n"
                    u8"    void onDialogueEnded(DialogueEndedNotice@ notice) {{\n"
                    u8"        say(\"ended \" + notice.conversation + (Dialogue::flag(\"met\") ? \" met\" : \"\"));\n"
                    u8"    }}\n"
                    u8"}}\n",
                    r.GateText())
                 .AsView());
    Talk(r);
    CHECK(r.scene.GetEntityName(director) ==
          StringView(u8"director|playing|The Keeper: Welcome back.|Stay. or Go.|cue lights out|ended gate met"));
    CHECK_FALSE(r.runner->IsPlaying());
}
#endif

#if defined(INTEGRATION_HAS_LUAU)
TEST_CASE("plugins: a Luau behavior plays a conversation through Dialogue and answers its notices")
{
    DialogueRun r(kTalk);
    const scene::EntityHandle director = r.scene.CreateEntity(u8"director");
    r.Attach(director, u8"luau",
             Format(u8"Director = EntityBehavior.class()\n"
                    u8"function Director.new(entity) return setmetatable({{ entity = entity }}, Director) end\n"
                    u8"function Director:say(what) self.entity:setName(self.entity:name() .. \"|\" .. what) end\n"
                    u8"function Director:onStart()\n"
                    u8"    Dialogue.bind(\"keeper\", self.entity)\n"
                    u8"    Dialogue.setFlag(\"met\", true)\n"
                    u8"    if Dialogue.play(Guid.new(\"{}\"), \"gate\") and Dialogue.isPlaying() then self:say(\"playing\") end\n"
                    u8"end\n"
                    u8"function Director:onDialogueLine(notice)\n"
                    u8"    self:say(notice.shownName .. \": \" .. notice.text)\n"
                    u8"    Dialogue.advance()\n"
                    u8"end\n"
                    u8"function Director:onDialogueChoice(notice)\n"
                    u8"    self:say(notice:answer(0) .. \" or \" .. notice:answer(1))\n"
                    u8"    Dialogue.choose(1)\n"
                    u8"end\n"
                    u8"function Director:onDialogueCommand(notice)\n"
                    u8"    self:say(notice.name .. \" \" .. notice:text(\"event\"))\n"
                    u8"    if notice.waits then Dialogue.done() end\n"
                    u8"end\n"
                    u8"function Director:onDialogueEnded(notice)\n"
                    u8"    self:say(\"ended \" .. notice.conversation .. (Dialogue.flag(\"met\") and \" met\" or \"\"))\n"
                    u8"end\n",
                    r.GateText())
                 .AsView());
    Talk(r);
    CHECK(r.scene.GetEntityName(director) ==
          StringView(u8"director|playing|The Keeper: Welcome back.|Stay. or Go.|cue lights out|ended gate met"));
    CHECK_FALSE(r.runner->IsPlaying());
}
#endif

TEST_CASE("plugins: a sequence's line track shows its line through the run's runner and holds until it is advanced")
{
    DialogueRun r;
    // A sequence of one line track, cooked and loaded as any sequence: two lines, the second's text
    // from the dialogue file.
    foundation::content::Instance* instance =
        r.store.db->RootGroup()->CreateInstance(u8"cutscene", sequence::SequenceSource::StaticType());
    sequence::SequenceSource source;
    RefPtr<dialogue::DialogueLineTrack> lines = MakeRef<dialogue::DialogueLineTrack>(DefaultAllocator());
    lines->cast.SetId(r.store.castId);
    dialogue::DialogueLineSection first;
    first.start = 0.5f;
    first.length = 1.0f;
    first.speaker = String(u8"keeper");
    first.text = String(u8"Look.");
    lines->sections.PushBack(first);
    dialogue::DialogueLineSection second;
    second.start = 2.0f;
    second.length = 1.0f;
    second.speaker = String(u8"keeper");
    second.dialogue.SetId(r.store.gate);
    second.lineId = String(u8"gate.1");
    lines->sections.PushBack(second);
    source.sequence.tracks.PushBack(lines);
    source.sequence.duration = 4.0f;
    REQUIRE(instance->WriteObject(source).IsOk());

    bool listed = false;
    for (const TypeInfo* kind : sequence::SequenceTrackKinds())
    {
        listed = listed || kind == &dialogue::DialogueLineTrack::StaticType();
    }
    CHECK(listed); // the sequence panel's Track menu offers it

    r.Start();
    auto* system = r.scene.GetSystem<engine::sequence::SequenceSystem>();
    foundation::resource::Ref<sequence::SequenceResource> loaded;
    loaded.SetId(instance->Id());
    loaded.Bind(*r.store.resources);
    REQUIRE(loaded.Get() != nullptr);
    REQUIRE(loaded.Get()->sequence.tracks.Size() == 1u);
    const u32 play = system->Play(RefPtr<sequence::SequenceResource>(loaded.Get()), instance->Id());
    for (int i = 0; i < 4; ++i)
    {
        r.Frame(0.5f); // past the first line's end at 1.5: held there
    }
    REQUIRE(r.lines.Size() == 1u);
    CHECK(r.lines[0] == StringView(u8"The Keeper: Look."));
    REQUIRE(system->Find(play) != nullptr);
    CHECK(system->Find(play)->Time() == doctest::Approx(1.5f));
    CHECK(system->Find(play)->IsHeld());

    REQUIRE(r.runner->Advance());
    for (int i = 0; i < 4; ++i)
    {
        r.Frame(0.5f);
    }
    REQUIRE(r.lines.Size() == 2u);
    CHECK(r.lines[1] == StringView(u8"The Keeper: Done.")); // the dialogue file's line gate.1
    CHECK(system->Find(play)->Time() == doctest::Approx(3.0f));
    REQUIRE(r.runner->Advance());
    for (int i = 0; i < 4; ++i)
    {
        r.Frame(0.5f);
    }
    CHECK(system->Find(play) == nullptr); // played to its end
}

TEST_CASE("plugins: the stock box types a line, takes a press to finish it and another to advance, and its answers")
{
    DialogueRun r(kTalk);
    runtime::SubsystemManager application{DefaultAllocator()};
    foundation::vfs::NativeFileSystem data(foundation::vfs::FindDataRoot(), DefaultAllocator());
    engine::gui::GuiSubsystem* gui = application.AddSubsystem<engine::gui::GuiSubsystem>(DefaultAllocator(), data);
    application.Startup();
    r.scripts.SetApplicationServices(Function<void*(TypeId)>{[&application](TypeId type) { return application.FindService(type); }});
    r.settings.stockBox = true;
    r.settings.typingSpeed = 10.0f;
    r.runner->SetFlag(u8"met", true);
    dialogue::DialogueStockBox& box = r.runner->Box();
    r.Start();

    REQUIRE(r.runner->Play(r.store.gate, u8"gate"));
    REQUIRE(box.IsShown());
    CHECK(box.IsStyled()); // its sheet over the box alone
    REQUIRE(gui->Screens() != nullptr);
    CHECK(gui->Screens()->Top() == box.ScreenNode());
    CHECK(box.ShownName() == StringView(u8"The Keeper"));
    CHECK(box.ShownText().IsEmpty());
    r.Frame(0.25f);
    CHECK(box.ShownText() == StringView(u8"We")); // ten a second: two and a half shown
    CHECK(box.IsTyping());
    box.PressBox(); // finishes the typing
    r.Frame(0.0f);
    CHECK(box.ShownText() == StringView(u8"Welcome back."));
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Line);
    box.PressBox(); // advances
    r.Frame(0.0f);
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Choice);
    REQUIRE(box.AnswerCount() == 2u);
    CHECK(box.Update(0.0f, true).choose == 0); // the advance action: the focused answer, the first
    box.Press(1); // Go.
    r.Frame(0.0f);
    CHECK(r.runner->Waiting() == dialogue::DialogueWait::Command);
    REQUIRE(r.runner->Done());
    CHECK_FALSE(r.runner->IsPlaying());
    CHECK_FALSE(box.IsShown()); // gone with the conversation
    CHECK(gui->Screens()->Top() == nullptr); // popped
    application.Shutdown();
}
