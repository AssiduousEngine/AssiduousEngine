// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Engine's runner, playing conversations written as text and parsed here (the parser is
// this suite's), against a stand-in host that records what the runner tells the game and holds the
// save store. Lines, choices and branches; flags under the settings' prefix; waits, staging and
// commands that wait or leave; jumps within a file and into another; resume after a load, and after
// a patch to the conversation; a second play refused, a stop, a host that answers at once.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import dialogue.engine;
import dialogue.pipeline;

using namespace foundation::core;
using namespace dialogue;

namespace
{
    Guid MakeGuid(u64 seed)
    {
        Random rng(seed);
        return Guid::Generate(rng);
    }

    RefPtr<DialogueResource> Compile(StringView text, const Guid& cast = {})
    {
        Array<DialogueCommandForm> forms;
        const StringView texts[] = {u8"cue <event:text> waits", u8"fight <foe:word> leaves", u8"note <what:text>"};
        for (StringView form : texts)
        {
            DialogueCommandForm parsed;
            String error;
            REQUIRE_MESSAGE(ParseCommandForm(form, parsed, error), error);
            forms.PushBack(Move(parsed));
        }
        DialogueParse parse = ParseDialogue(text, Span<const DialogueCommandForm>(forms.Data(), forms.Size()));
        for (const DialogueProblem& problem : parse.problems)
        {
            INFO(problem.line, ": ", problem.message);
            CHECK(problem.warning);
        }
        REQUIRE(parse.Ok());
        RefPtr<DialogueResource> resource = MakeRef<DialogueResource>(DefaultAllocator());
        resource->cast = cast;
        resource->conversations = Move(parse.conversations);
        resource->lines = Move(parse.lines);
        return resource;
    }

    // The game around the runner: its files, its save store, its staging, and a log of what it was
    // told, one entry each.
    class StandIn final : public IDialogueHost
    {
    public:
        struct File
        {
            Guid id;
            RefPtr<DialogueResource> resource;
        };

        Array<File> files;
        Guid castId;
        RefPtr<DialogueCastResource> cast;
        Array<String> keys; // the save store
        Array<String> values;
        Array<String> told;
        Array<u32> staging; // the tokens still going
        u32 nextToken = 1;
        bool advanceAtOnce = false;
        u32 linesEnded = 0;
        f32 clock = 0.0f;
        DialogueRunner* runner = nullptr;
        DialogueChoiceNotice lastChoice;
        DialogueCommandNotice lastCommand;
        DialogueEndedNotice lastEnded;
        DialogueLineNotice lastLine;

        RefPtr<DialogueResource> LoadDialogue(const Guid& id) override
        {
            for (const File& file : files)
            {
                if (file.id == id)
                {
                    return file.resource;
                }
            }
            return {};
        }
        RefPtr<DialogueCastResource> LoadCast(const Guid& id) override { return id == castId ? cast : RefPtr<DialogueCastResource>{}; }

        bool ReadFlag(StringView key) override { return Read(key) == StringView(u8"true"); }
        void WriteFlag(StringView key, bool on) override { Write(key, on ? StringView(u8"true") : StringView(u8"false")); }
        String ReadText(StringView key) override { return Read(key); }
        void WriteText(StringView key, StringView text) override { Write(key, text); }

        String ActionLabel(StringView action) override
        {
            return action == StringView(u8"Interact") ? String(u8"E") : String();
        }

        u32 Stage(const DialogueStep& step, const DialogueCastResource*) override
        {
            told.PushBack(Format(u8"stage {}", step.name.IsEmpty() ? StringView(u8"fade") : step.name.AsView()));
            staging.PushBack(nextToken);
            return nextToken++;
        }
        bool IsStaging(u32 token) override
        {
            for (u32 going : staging)
            {
                if (going == token)
                {
                    return true;
                }
            }
            return false;
        }

        void OnLine(const DialogueLineNotice& notice, const DialogueSpeaker*) override
        {
            lastLine = notice;
            told.PushBack(Format(u8"{}: {}", notice.speaker, notice.text));
            if (advanceAtOnce)
            {
                (void)runner->Advance();
            }
        }
        void OnLineEnded() override { ++linesEnded; }
        void Update(f32 deltaTime) override { clock += deltaTime; }
        void OnChoice(const DialogueChoiceNotice& notice) override
        {
            lastChoice = notice;
            String joined;
            for (const String& answer : notice.answers)
            {
                joined.Append(joined.IsEmpty() ? StringView() : StringView(u8" | "));
                joined.Append(answer.AsView());
            }
            told.PushBack(Format(u8"choice {}", joined));
        }
        void OnCommand(const DialogueCommandNotice& notice) override
        {
            lastCommand = notice;
            told.PushBack(Format(u8"command {}", notice.name));
        }
        void OnEnded(const DialogueEndedNotice& notice) override
        {
            lastEnded = notice;
            told.PushBack(Format(u8"ended {}", notice.conversation));
        }

        [[nodiscard]] String Read(StringView key) const
        {
            for (usize i = 0; i < keys.Size(); ++i)
            {
                if (keys[i] == key)
                {
                    return values[i];
                }
            }
            return {};
        }
        void Write(StringView key, StringView value)
        {
            for (usize i = 0; i < keys.Size(); ++i)
            {
                if (keys[i] == key)
                {
                    if (value.IsEmpty())
                    {
                        keys.RemoveAt(i);
                        values.RemoveAt(i);
                    }
                    else
                    {
                        values[i] = String(value);
                    }
                    return;
                }
            }
            if (!value.IsEmpty())
            {
                keys.PushBack(String(key));
                values.PushBack(String(value));
            }
        }

        // What it was told since the last call, one entry a line.
        String Told()
        {
            String out;
            for (const String& entry : told)
            {
                out.Append(entry.AsView());
                out.Append(u8'\n');
            }
            told.Clear();
            return out;
        }
    };

    struct Fixture
    {
        DialogueSettings settings;
        StandIn host;
        DialogueRunner runner{settings, host};
        Guid gate = MakeGuid(1);

        Fixture()
        {
            host.runner = &runner;
            host.castId = MakeGuid(2);
            host.cast = MakeRef<DialogueCastResource>(DefaultAllocator());
            DialogueSpeaker& keeper = host.cast->speakers.EmplaceBack();
            keeper.id = String(u8"keeper");
            keeper.shownName = String(u8"The Keeper");
            keeper.portrait = MakeGuid(3);
            keeper.moods.PushBack(DialogueMood{String(u8"calm"), Guid{}});
            keeper.moods.PushBack(DialogueMood{String(u8"worried"), MakeGuid(4)});
            host.cast->speakers.EmplaceBack().id = String(u8"visitor");
        }

        void Add(const Guid& id, StringView text) { host.files.PushBack(StandIn::File{id, Compile(text, host.castId)}); }
    };
}

TEST_CASE("dialogue runner: lines wait for advance, a choice for an answer, and its flags go in the save store")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"@ keeper: Who goes there? Press {Interact}, {{or}} not.\n"
                  u8"choice\n"
                  u8"> A friend.\n"
                  u8"  set friendly\n"
                  u8"  @ keeper worried: A friend, then.\n"
                  u8"> Nobody.\n"
                  u8"  @ visitor: Nobody at all.\n"
                  u8"end\n"
                  u8"@ keeper: Go on.\n");
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK(f.runner.IsPlaying());
    CHECK(f.runner.Waiting() == DialogueWait::Line);
    CHECK(f.host.Told() == StringView(u8"keeper: Who goes there? Press E, {or} not.\n"));
    CHECK(f.host.lastLine.shownName == StringView(u8"The Keeper"));
    CHECK(f.host.lastLine.mood == StringView(u8"calm")); // the speaker's first, unnamed
    CHECK(f.host.lastLine.portrait == MakeGuid(3));      // the mood has none: the speaker's
    CHECK(f.host.lastLine.lineId == StringView(u8"gate.1"));
    CHECK(f.host.lastLine.dialogue == f.gate);
    CHECK_FALSE(f.runner.Choose(0)); // no choice offered yet

    REQUIRE(f.runner.Advance());
    CHECK(f.host.linesEnded == 1u);
    CHECK(f.runner.Waiting() == DialogueWait::Choice);
    CHECK(f.host.Told() == StringView(u8"choice A friend. | Nobody.\n"));
    CHECK(f.host.lastChoice.lineIds.Size() == 2u);
    CHECK_FALSE(f.runner.Advance()); // no line showing
    CHECK_FALSE(f.runner.Choose(2));

    REQUIRE(f.runner.Choose(0));
    CHECK(f.host.Told() == StringView(u8"keeper: A friend, then.\n"));
    CHECK(f.host.lastLine.portrait == MakeGuid(4)); // the mood's own
    CHECK(f.runner.Flag(u8"friendly"));
    CHECK(f.host.Read(u8"flag.friendly") == StringView(u8"true"));
    CHECK_FALSE(f.host.Read(kDialogueResumeKey).IsEmpty()); // its place, as each line shows

    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"keeper: Go on.\n"));
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"ended gate\n"));
    CHECK_FALSE(f.runner.IsPlaying());
    CHECK_FALSE(f.host.lastEnded.stopped);
    CHECK_FALSE(f.host.lastEnded.left);
    CHECK(f.host.Read(kDialogueResumeKey).IsEmpty()); // ended: nothing to resume
}

TEST_CASE("dialogue runner: a branch reads a flag, set by script or by a conversation")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"if met\n"
                  u8"  @ keeper: Back again.\n"
                  u8"else\n"
                  u8"  @ keeper: New here?\n"
                  u8"end\n"
                  u8"if not met\n"
                  u8"  set met\n"
                  u8"end\n");
    REQUIRE(f.runner.Play(f.gate, {})); // the first conversation
    CHECK(f.host.Told() == StringView(u8"keeper: New here?\n"));
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"ended gate\n"));
    CHECK(f.runner.Flag(u8"met"));

    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK(f.host.Told() == StringView(u8"keeper: Back again.\n"));
    const u32 ended = f.host.linesEnded;
    f.runner.Stop();
    CHECK(f.host.linesEnded == ended + 1u); // stopped on a line: it ends too
    CHECK(f.host.Told() == StringView(u8"ended gate\n"));
    CHECK(f.host.lastEnded.stopped);

    f.runner.SetFlag(u8"met", false);
    CHECK(f.host.Read(u8"flag.met") == StringView(u8"false"));
    f.settings.flagPrefix = String(u8"story.");
    f.runner.SetFlag(u8"met", true);
    CHECK(f.host.Read(u8"story.met") == StringView(u8"true"));
}

TEST_CASE("dialogue runner: a wait counts down, staging waits when told to, and a fade always")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"wait 1\n"
                  u8"sound creak\n"
                  u8"sequence opening and wait\n"
                  u8"fade out\n"
                  u8"@ keeper: Done.\n");
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK(f.runner.Waiting() == DialogueWait::Time);
    f.runner.Update(0.6f);
    CHECK(f.host.clock == doctest::Approx(0.6f));
    CHECK(f.host.Told().IsEmpty());
    f.runner.Update(0.6f);
    CHECK(f.host.Told() == StringView(u8"stage creak\nstage opening\n")); // the sound does not wait
    CHECK(f.runner.Waiting() == DialogueWait::Staging);
    f.runner.Update(1.0f);
    CHECK(f.host.Told().IsEmpty()); // the sequence is still going
    f.host.staging.Clear();
    f.runner.Update(0.1f);
    CHECK(f.host.Told() == StringView(u8"stage fade\n"));
    CHECK(f.runner.Waiting() == DialogueWait::Staging);
    f.host.staging.Clear();
    f.runner.Update(0.1f);
    CHECK(f.host.Told() == StringView(u8"keeper: Done.\n"));
}

TEST_CASE("dialogue runner: a command that waits holds for done; one that leaves ends with its place kept")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"note the gate\n"
                  u8"cue lights dim\n"
                  u8"@ keeper: Something stirs.\n"
                  u8"fight wolf\n"
                  u8"@ keeper: You won.\n");
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK(f.host.Told() == StringView(u8"command note\ncommand cue\n"));
    CHECK(f.host.lastCommand.waits);
    REQUIRE(f.host.lastCommand.Find(u8"event") != nullptr);
    CHECK(f.host.lastCommand.Find(u8"event")->text == StringView(u8"lights dim"));
    CHECK(f.runner.Waiting() == DialogueWait::Command);
    CHECK_FALSE(f.runner.Advance());
    REQUIRE(f.runner.Done());
    CHECK(f.host.Told() == StringView(u8"keeper: Something stirs.\n"));
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"command fight\nended gate\n"));
    CHECK(f.host.lastCommand.leaves);
    CHECK(f.host.lastEnded.left);
    CHECK_FALSE(f.runner.IsPlaying());

    // The game comes back from the fight (a load between, say): the conversation goes on after it.
    DialogueRunner again(f.settings, f.host);
    f.host.runner = &again;
    REQUIRE(again.Resume());
    CHECK(f.host.Told() == StringView(u8"keeper: You won.\n"));
    REQUIRE(again.Advance());
    CHECK(f.host.Told() == StringView(u8"ended gate\n"));
    CHECK_FALSE(again.Resume()); // nothing left to resume
}

TEST_CASE("dialogue runner: a jump within the file, and into another")
{
    Fixture f;
    const Guid lane = MakeGuid(10);
    f.Add(f.gate, u8"conversation gate\n"
                  u8"@ keeper: This way.\n"
                  u8"jump path\n"
                  u8"conversation path\n"
                  u8"@ keeper: Along the path.\n"
                  u8"jump gate in lane\n");
    f.Add(lane, u8"conversation gate\n" // the same id as the first file's: ids are a file's own
                u8"@ visitor: The lane's gate.\n");
    // The cook resolves `in lane` to the file's guid; here, by hand.
    f.host.files[0].resource->conversations[1].steps[1].asset = lane;

    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"keeper: This way.\nkeeper: Along the path.\n"));
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"visitor: The lane's gate.\n"));
    CHECK(f.host.lastLine.dialogue == lane);
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"ended gate\n"));
    CHECK(f.host.lastEnded.dialogue == lane);
}

TEST_CASE("dialogue runner: resume after a load, and after a patch to the conversation")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"@ keeper: One.\n"
                  u8"@ keeper: Two. #line:two\n"
                  u8"@ keeper: Three.\n");
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    REQUIRE(f.runner.Advance());
    CHECK(f.host.Told() == StringView(u8"keeper: One.\nkeeper: Two.\n"));

    // Saved on the second line; loaded into a new run: the line shows again.
    {
        DialogueRunner loaded(f.settings, f.host);
        f.host.runner = &loaded;
        REQUIRE(loaded.Resume());
        CHECK(f.host.Told() == StringView(u8"keeper: Two.\n"));
        CHECK_FALSE(loaded.Resume()); // already playing
    }

    // A patch inserts a line above it: the step moved, the line is found by its id.
    f.host.files[0].resource = Compile(u8"conversation gate\n"
                                       u8"@ keeper: Zero.\n"
                                       u8"@ keeper: One.\n"
                                       u8"@ keeper: Two. #line:two\n"
                                       u8"@ keeper: Three.\n",
                                       f.host.castId);
    {
        DialogueRunner patched(f.settings, f.host);
        f.host.runner = &patched;
        REQUIRE(patched.Resume());
        CHECK(f.host.Told() == StringView(u8"keeper: Two.\n"));
    }

    // A patch that drops the line: the conversation starts again.
    f.host.files[0].resource = Compile(u8"conversation gate\n"
                                       u8"@ keeper: Only this.\n",
                                       f.host.castId);
    {
        DialogueRunner dropped(f.settings, f.host);
        f.host.runner = &dropped;
        REQUIRE(dropped.Resume());
        CHECK(f.host.Told() == StringView(u8"keeper: Only this.\n"));
    }
}

TEST_CASE("dialogue runner: one conversation at a time; a host may answer a line at once")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"@ keeper: One.\n"
                  u8"@ keeper: Two.\n"
                  u8"conversation other\n"
                  u8"@ visitor: Other.\n");
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK_FALSE(f.runner.Play(f.gate, u8"other"));
    CHECK_FALSE(f.runner.Play(f.gate, u8"missing"));
    f.runner.Stop();
    (void)f.host.Told();
    CHECK_FALSE(f.runner.Play(f.gate, u8"missing"));
    CHECK_FALSE(f.runner.Play(MakeGuid(99), u8"gate"));

    f.host.advanceAtOnce = true;
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK(f.host.Told() == StringView(u8"keeper: One.\nkeeper: Two.\nended gate\n"));
    CHECK_FALSE(f.runner.IsPlaying());
}

TEST_CASE("dialogue runner: a line shown on its own (a sequence's) ends on advance, and gives way to a conversation")
{
    Fixture f;
    f.Add(f.gate, u8"conversation gate\n"
                  u8"@ visitor: A conversation.\n");
    const u32 line = f.runner.ShowLine(f.host.cast.Get(), u8"keeper", {}, u8"Press {Interact}.", u8"cutscene.1");
    REQUIRE(line != 0u);
    CHECK(f.runner.IsShowing(line));
    CHECK_FALSE(f.runner.IsPlaying());
    CHECK(f.host.Told() == StringView(u8"keeper: Press E.\n"));
    CHECK(f.host.lastLine.shownName == StringView(u8"The Keeper"));
    CHECK(f.host.lastLine.conversation.IsEmpty());
    REQUIRE(f.runner.Advance());
    CHECK_FALSE(f.runner.IsShowing(line));
    CHECK(f.host.Told() == StringView(u8"ended \n")); // no conversation
    CHECK_FALSE(f.runner.Advance());

    // A conversation takes over from a line still shown.
    const u32 second = f.runner.ShowLine(f.host.cast.Get(), u8"keeper", {}, u8"Wait.", {});
    REQUIRE(f.runner.Play(f.gate, u8"gate"));
    CHECK_FALSE(f.runner.IsShowing(second));
    CHECK(f.host.Told() == StringView(u8"keeper: Wait.\nended \nvisitor: A conversation.\n"));

    // While it plays, a sequence's line is refused.
    CHECK(f.runner.ShowLine(f.host.cast.Get(), u8"keeper", {}, u8"Refused.", {}) == 0u);
    f.runner.EndLine(second); // long gone: nothing happens
    CHECK(f.host.Told().IsEmpty());
}
