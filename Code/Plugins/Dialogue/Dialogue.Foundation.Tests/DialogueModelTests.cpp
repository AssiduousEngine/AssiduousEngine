// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Engine: the compiled conversations and the cast, as the cook writes them and the
// factories read them, and their lookups.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import dialogue.foundation;

using namespace foundation::core;
using namespace dialogue;

namespace
{
    template <typename T>
    void RoundTrip(T& written, T& read)
    {
        MemoryStream stream;
        {
            BinarySerializer writer(stream, SerializeMode::Write);
            written.Serialize(writer);
        }
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(stream, SerializeMode::Read);
        read.Serialize(reader);
        REQUIRE(reader.IsOk());
    }

    Guid MakeGuid(u64 seed)
    {
        Random rng(seed);
        return Guid::Generate(rng);
    }

    DialogueStep Step(DialogueStepKind kind)
    {
        DialogueStep step;
        step.kind = kind;
        return step;
    }
}

TEST_CASE("dialogue model: a compiled dialogue file reads back as it was written")
{
    RegisterDialogueTypes();
    DialogueSource written;
    written.cast = MakeGuid(41);
    DialogueConversation& talk = written.conversations.EmplaceBack();
    talk.id = String(u8"gate_talk");
    talk.level = String(u8"Levels/Village");
    DialogueStep line = Step(DialogueStepKind::Line);
    line.speaker = String(u8"keeper");
    line.mood = String(u8"worried");
    line.lineId = String(u8"gate_talk.1");
    line.sourceLine = 2;
    talk.steps.PushBack(Move(line));
    DialogueStep test = Step(DialogueStepKind::If);
    test.flag = String(u8"met_keeper");
    test.negate = true;
    test.next = 4;
    talk.steps.PushBack(Move(test));
    DialogueStep fade = Step(DialogueStepKind::Fade);
    fade.seconds = 1.5f;
    fade.hasColor = true;
    fade.color = Float4{1.0f, 1.0f, 1.0f, 1.0f};
    talk.steps.PushBack(Move(fade));
    DialogueStep command = Step(DialogueStepKind::Command);
    command.name = String(u8"move");
    command.waits = true;
    DialogueArgument who;
    who.name = String(u8"who");
    who.text = String(u8"Grey Cat");
    command.arguments.PushBack(Move(who));
    DialogueArgument running;
    running.name = String(u8"running");
    running.present = true;
    command.arguments.PushBack(Move(running));
    talk.steps.PushBack(Move(command));
    talk.steps.PushBack(Step(DialogueStepKind::End));
    written.lines.PushBack(DialogueLineText{String(u8"gate_talk.1"), String(u8"The gate has been quiet since dawn.")});

    DialogueSource read;
    RoundTrip(written, read);
    CHECK(read.cast == written.cast);
    REQUIRE(read.conversations.Size() == 1u);
    const DialogueConversation& back = read.conversations[0];
    CHECK(back.id == StringView(u8"gate_talk"));
    CHECK(back.level == StringView(u8"Levels/Village"));
    REQUIRE(back.steps.Size() == 5u);
    CHECK(back.steps[0].kind == DialogueStepKind::Line);
    CHECK(back.steps[0].speaker == StringView(u8"keeper"));
    CHECK(back.steps[0].mood == StringView(u8"worried"));
    CHECK(back.steps[0].sourceLine == 2u);
    CHECK(back.steps[1].negate);
    CHECK(back.steps[1].next == 4);
    CHECK(back.steps[2].seconds == doctest::Approx(1.5f));
    CHECK(back.steps[2].hasColor);
    CHECK(back.steps[2].color.x == doctest::Approx(1.0f));
    REQUIRE(back.steps[3].arguments.Size() == 2u);
    CHECK(back.steps[3].waits);
    CHECK(back.steps[3].arguments[0].text == StringView(u8"Grey Cat"));
    CHECK(back.steps[3].arguments[1].present);
    CHECK(back.steps[4].kind == DialogueStepKind::End);
    REQUIRE(read.lines.Size() == 1u);
    CHECK(read.lines[0].text == StringView(u8"The gate has been quiet since dawn."));
}

TEST_CASE("dialogue model: a resource finds a conversation and a line's text by id")
{
    DialogueResource resource;
    resource.conversations.EmplaceBack().id = String(u8"first");
    resource.conversations.EmplaceBack().id = String(u8"second");
    resource.lines.PushBack(DialogueLineText{String(u8"first.1"), String(u8"Hello.")});
    REQUIRE(resource.Find(u8"second") != nullptr);
    CHECK(resource.Find(u8"second")->id == StringView(u8"second"));
    CHECK(resource.Find(u8"third") == nullptr);
    CHECK(resource.Text(u8"first.1") == StringView(u8"Hello."));
    CHECK(resource.Text(u8"first.2").IsEmpty());
}

TEST_CASE("dialogue model: a cast reads back as it was written, and finds a speaker and a mood")
{
    RegisterDialogueTypes();
    DialogueCastSource written;
    DialogueSpeaker& keeper = written.speakers.EmplaceBack();
    keeper.id = String(u8"keeper");
    keeper.shownName = String(u8"The Keeper");
    keeper.plateColor = Float4{0.2f, 0.4f, 0.6f, 1.0f};
    keeper.moods.PushBack(DialogueMood{String(u8"neutral"), Guid{}});
    keeper.moods.PushBack(DialogueMood{String(u8"worried"), MakeGuid(7u)});
    keeper.clips.PushBack(MakeGuid(8u));
    keeper.entity = String(u8"Keeper");
    DialogueSpeaker& narrator = written.speakers.EmplaceBack();
    narrator.id = String(u8"narrator");
    narrator.plate = false;

    DialogueCastSource read;
    RoundTrip(written, read);
    REQUIRE(read.speakers.Size() == 2u);
    const DialogueSpeaker& back = read.speakers[0];
    CHECK(back.shownName == StringView(u8"The Keeper"));
    CHECK(back.plateColor.y == doctest::Approx(0.4f));
    REQUIRE(back.moods.Size() == 2u);
    CHECK(back.moods[1].portrait == keeper.moods[1].portrait);
    REQUIRE(back.clips.Size() == 1u);
    CHECK(back.clips[0] == keeper.clips[0]);
    CHECK(back.entity == StringView(u8"Keeper"));
    CHECK_FALSE(read.speakers[1].plate);

    DialogueCastResource cast;
    cast.speakers = Move(read.speakers);
    REQUIRE(cast.Find(u8"keeper") != nullptr);
    CHECK(cast.Find(u8"keeper")->FindMood(u8"worried") != nullptr);
    CHECK(cast.Find(u8"keeper")->FindMood(u8"angry") == nullptr);
    CHECK(cast.Find(u8"stranger") == nullptr);
}

TEST_CASE("dialogue model: a speaker's fields are reflected for the inspector")
{
    RegisterDialogueTypes();
    const TypeInfo& speaker = TypeOf<DialogueSpeaker>();
    const char* const names[] = {"id", "shownName", "plate", "plateColor", "moods",
                                 "portrait", "clips", "talk", "voice", "entity"};
    for (const char* name : names)
    {
        CHECK_MESSAGE(FindProperty(speaker, name) != nullptr, name);
    }
    CHECK(FindProperty(TypeOf<DialogueMood>(), "portrait") != nullptr);
}
