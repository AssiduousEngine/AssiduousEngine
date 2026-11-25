// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Pipeline: the format parsed into conversations. Lines, choices and branches compiled to
// jumps; the built-in statements; declared commands matched by their forms, names with spaces
// included; line ids; and every problem at its line. The conversations are written for these tests.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import dialogue.engine;
import dialogue.pipeline;

using namespace foundation::core;
using namespace dialogue;

namespace
{
    Array<DialogueCommandForm> Commands()
    {
        Array<DialogueCommandForm> forms;
        const StringView texts[] = {
            u8"move <who:entity> to <target:entity> [running] [and wait]",
            u8"camera <target:entity> [for <seconds:number=2>]",
            u8"shake <how:gentle|strong|violent> [for <seconds:number=1>]",
            u8"fight <foe:list(foes)> leaves",
            u8"give item <item:list(items)> [<count:number=1>]",
            u8"give coins <purse:copper|silver|gold> <count:number>",
            u8"cue <event:text> waits",
        };
        for (StringView text : texts)
        {
            DialogueCommandForm form;
            String error;
            REQUIRE_MESSAGE(ParseCommandForm(text, form, error), error);
            forms.PushBack(Move(form));
        }
        return forms;
    }

    DialogueParse Parse(StringView text)
    {
        const Array<DialogueCommandForm> commands = Commands();
        return ParseDialogue(text, Span<const DialogueCommandForm>(commands.Data(), commands.Size()));
    }

    // The one conversation of a file that must parse cleanly.
    const DialogueConversation& Only(const DialogueParse& parse)
    {
        for (const DialogueProblem& problem : parse.problems)
        {
            INFO(problem.line, ": ", problem.message);
        }
        REQUIRE(parse.Ok());
        REQUIRE(parse.conversations.Size() == 1u);
        return parse.conversations[0];
    }

    const DialogueArgument* Argument(const DialogueStep& step, StringView name)
    {
        for (const DialogueArgument& argument : step.arguments)
        {
            if (argument.name == name)
            {
                return &argument;
            }
        }
        return nullptr;
    }

    // Whether a problem at `line` says something containing `words`.
    bool HasProblem(const DialogueParse& parse, u32 line, StringView words)
    {
        for (const DialogueProblem& problem : parse.problems)
        {
            if (problem.line == line && problem.message.AsView().Contains(words))
            {
                return true;
            }
        }
        return false;
    }

    StringView Text(const DialogueParse& parse, StringView id)
    {
        for (const DialogueLineText& line : parse.lines)
        {
            if (line.id == id)
            {
                return line.text.AsView();
            }
        }
        return {};
    }
}

TEST_CASE("dialogue parse: lines with a speaker and a mood, comments and blank lines left out")
{
    const DialogueParse parse = Parse(u8"# the keeper at the gate\n"
                                      u8"conversation gate_talk\n"
                                      u8"\n"
                                      u8"@ keeper worried: The gate has been quiet since dawn.\n"
                                      u8"   @ visitor: Press {Interact} to knock. #1 on the list.\n"
                                      u8"@ keeper: Time: half past. #line:keeper_time\n");
    const DialogueConversation& talk = Only(parse);
    CHECK(talk.id == StringView(u8"gate_talk"));
    CHECK(talk.level.IsEmpty());
    REQUIRE(talk.steps.Size() == 4u);
    CHECK(talk.steps[0].kind == DialogueStepKind::Line);
    CHECK(talk.steps[0].speaker == StringView(u8"keeper"));
    CHECK(talk.steps[0].mood == StringView(u8"worried"));
    CHECK(talk.steps[0].lineId == StringView(u8"gate_talk.1"));
    CHECK(talk.steps[0].sourceLine == 4u);
    CHECK(talk.steps[1].mood.IsEmpty());
    CHECK(talk.steps[2].lineId == StringView(u8"keeper_time"));
    CHECK(talk.steps[3].kind == DialogueStepKind::End);
    CHECK(Text(parse, u8"gate_talk.1") == StringView(u8"The gate has been quiet since dawn."));
    CHECK(Text(parse, u8"gate_talk.2") == StringView(u8"Press {Interact} to knock. #1 on the list."));
    CHECK(Text(parse, u8"keeper_time") == StringView(u8"Time: half past."));
}

TEST_CASE("dialogue parse: a conversation names its level, and a file holds several")
{
    const DialogueParse parse = Parse(u8"conversation first in village\n"
                                      u8"@ keeper: One.\n"
                                      u8"conversation second\n"
                                      u8"@ keeper: Two.\n");
    REQUIRE(parse.Ok());
    REQUIRE(parse.conversations.Size() == 2u);
    CHECK(parse.conversations[0].level == StringView(u8"village"));
    CHECK(parse.conversations[1].steps[0].lineId == StringView(u8"second.1"));
}

// A level is one word, a dialogue asset's name: more words, or `in` with none, is no header.
TEST_CASE("dialogue parse: a conversation's level is one word")
{
    const DialogueParse spaced = Parse(u8"conversation square in Old Village\n"
                                       u8"@ keeper: One.\n");
    CHECK(HasProblem(spaced, 1, u8"`conversation <id> [in <level>]`"));
    const DialogueParse alone = Parse(u8"conversation alone in\n"
                                      u8"@ keeper: One.\n");
    CHECK(HasProblem(alone, 1, u8"`conversation <id> [in <level>]`"));
}

TEST_CASE("dialogue parse: a choice compiles to its answers' table, then each answer's statements")
{
    const DialogueParse parse = Parse(u8"conversation ask\n"
                                      u8"choice\n"
                                      u8"  > Who are you?\n"
                                      u8"    @ keeper: The keeper.\n"
                                      u8"  > Goodbye. #line:bye\n"
                                      u8"end\n"
                                      u8"@ keeper: Well then.\n");
    const DialogueConversation& ask = Only(parse);
    // Choice, Answer, Answer, [Line, Skip], [Skip], Line, End
    REQUIRE(ask.steps.Size() == 8u);
    CHECK(ask.steps[0].kind == DialogueStepKind::Choice);
    CHECK(ask.steps[0].count == 2u);
    CHECK(ask.steps[1].kind == DialogueStepKind::Answer);
    CHECK(ask.steps[1].lineId == StringView(u8"ask.1"));
    CHECK(ask.steps[1].next == 3);
    CHECK(ask.steps[2].lineId == StringView(u8"bye"));
    CHECK(ask.steps[2].next == 5);
    CHECK(ask.steps[3].kind == DialogueStepKind::Line);
    CHECK(ask.steps[3].lineId == StringView(u8"ask.2"));
    CHECK(ask.steps[4].kind == DialogueStepKind::Skip);
    CHECK(ask.steps[4].next == 6);
    CHECK(ask.steps[5].kind == DialogueStepKind::Skip);
    CHECK(ask.steps[5].next == 6);
    CHECK(ask.steps[6].kind == DialogueStepKind::Line);
    CHECK(Text(parse, u8"ask.1") == StringView(u8"Who are you?"));
    CHECK(Text(parse, u8"bye") == StringView(u8"Goodbye."));
}

TEST_CASE("dialogue parse: an if with and without else, and blocks nested in an answer")
{
    const DialogueParse parse = Parse(u8"conversation branch\n"
                                      u8"if not met\n"
                                      u8"  set met\n"
                                      u8"else\n"
                                      u8"  clear met\n"
                                      u8"end\n"
                                      u8"choice\n"
                                      u8"> Ask\n"
                                      u8"  if met\n"
                                      u8"    @ keeper: Again?\n"
                                      u8"  end\n"
                                      u8"end\n");
    const DialogueConversation& branch = Only(parse);
    // 0 If, 1 Set, 2 Skip, 3 Clear, 4 Choice, 5 Answer, 6 If, 7 Line, 8 Skip, 9 End
    REQUIRE(branch.steps.Size() == 10u);
    CHECK(branch.steps[0].kind == DialogueStepKind::If);
    CHECK(branch.steps[0].negate);
    CHECK(branch.steps[0].flag == StringView(u8"met"));
    CHECK(branch.steps[0].next == 3); // to the else side
    CHECK(branch.steps[1].kind == DialogueStepKind::Set);
    CHECK(branch.steps[2].kind == DialogueStepKind::Skip);
    CHECK(branch.steps[2].next == 4); // past the else side
    CHECK(branch.steps[3].kind == DialogueStepKind::Clear);
    CHECK(branch.steps[5].next == 6);
    CHECK(branch.steps[6].kind == DialogueStepKind::If);
    CHECK_FALSE(branch.steps[6].negate);
    CHECK(branch.steps[6].next == 8); // the answer's if, moved to where its answer starts
    CHECK(branch.steps[8].kind == DialogueStepKind::Skip);
    CHECK(branch.steps[8].next == 9);
}

TEST_CASE("dialogue parse: the built-in statements")
{
    const DialogueParse parse = Parse(u8"conversation staged\n"
                                      u8"wait 1.5\n"
                                      u8"play keeper Wave Hello and wait\n"
                                      u8"play keeper Nod\n"
                                      u8"sound door_creak at keeper\n"
                                      u8"sound wind\n"
                                      u8"fade out\n"
                                      u8"fade in for 0.5 #FFFFFF\n"
                                      u8"sequence gate_open and wait\n"
                                      u8"jump farewell\n"
                                      u8"jump farewell in farewells\n");
    const DialogueConversation& staged = Only(parse);
    REQUIRE(staged.steps.Size() == 11u);
    CHECK(staged.steps[0].kind == DialogueStepKind::Wait);
    CHECK(staged.steps[0].seconds == doctest::Approx(1.5f));
    CHECK(staged.steps[1].kind == DialogueStepKind::Play);
    CHECK(staged.steps[1].speaker == StringView(u8"keeper"));
    CHECK(staged.steps[1].name == StringView(u8"Wave Hello"));
    CHECK(staged.steps[1].waits);
    CHECK_FALSE(staged.steps[2].waits);
    CHECK(staged.steps[3].kind == DialogueStepKind::Sound);
    CHECK(staged.steps[3].name == StringView(u8"door_creak"));
    CHECK(staged.steps[3].speaker == StringView(u8"keeper"));
    CHECK(staged.steps[4].speaker.IsEmpty());
    CHECK(staged.steps[5].kind == DialogueStepKind::Fade);
    CHECK_FALSE(staged.steps[5].fadeIn);
    CHECK(staged.steps[5].seconds < 0.0f); // the settings' length
    CHECK_FALSE(staged.steps[5].hasColor);
    CHECK(staged.steps[6].fadeIn);
    CHECK(staged.steps[6].seconds == doctest::Approx(0.5f));
    CHECK(staged.steps[6].hasColor);
    CHECK(staged.steps[6].color.z == doctest::Approx(1.0f));
    CHECK(staged.steps[6].color.w == doctest::Approx(1.0f)); // six digits: opaque
    CHECK(staged.steps[7].kind == DialogueStepKind::Sequence);
    CHECK(staged.steps[7].name == StringView(u8"gate_open"));
    CHECK(staged.steps[7].waits);
    CHECK(staged.steps[8].kind == DialogueStepKind::Jump);
    CHECK(staged.steps[8].name == StringView(u8"farewell"));
    CHECK(staged.steps[8].file.IsEmpty());
    CHECK(staged.steps[9].kind == DialogueStepKind::Jump);
    CHECK(staged.steps[9].name == StringView(u8"farewell"));
    CHECK(staged.steps[9].file == StringView(u8"farewells"));
}

TEST_CASE("dialogue parse: a declared command, its arguments run to the next literal word")
{
    const DialogueParse parse = Parse(u8"conversation walk\n"
                                      u8"move Grey Cat to Player running and wait\n"
                                      u8"move Player to Old Well 01\n"
                                      u8"camera Old Well 01 for 2.5\n"
                                      u8"camera Garden Gate\n"
                                      u8"move Player to \"Path to Mill\"\n"
                                      u8"move \"Path to Mill\" to Player\n");
    const DialogueConversation& walk = Only(parse);
    REQUIRE(walk.steps.Size() == 7u);
    const DialogueStep& cat = walk.steps[0];
    CHECK(cat.kind == DialogueStepKind::Command);
    CHECK(cat.name == StringView(u8"move"));
    CHECK(Argument(cat, u8"who")->text == StringView(u8"Grey Cat"));
    CHECK(Argument(cat, u8"target")->text == StringView(u8"Player"));
    CHECK(Argument(cat, u8"running")->present);
    CHECK(cat.waits);
    const DialogueStep& well = walk.steps[1];
    CHECK(Argument(well, u8"target")->text == StringView(u8"Old Well 01"));
    CHECK_FALSE(Argument(well, u8"running")->present);
    CHECK_FALSE(well.waits);
    CHECK(Argument(walk.steps[2], u8"target")->text == StringView(u8"Old Well 01"));
    CHECK(Argument(walk.steps[2], u8"seconds")->number == doctest::Approx(2.5f));
    CHECK(Argument(walk.steps[3], u8"seconds")->number == doctest::Approx(2.0f)); // the default
    CHECK_FALSE(Argument(walk.steps[3], u8"seconds")->present);
    CHECK(Argument(walk.steps[4], u8"target")->text == StringView(u8"Path to Mill"));
    CHECK(Argument(walk.steps[5], u8"who")->text == StringView(u8"Path to Mill"));
}

TEST_CASE("dialogue parse: commands of two words, choices, and how a command waits or leaves")
{
    const DialogueParse parse = Parse(u8"conversation reward\n"
                                      u8"give item Brass Key\n"
                                      u8"give item Apple 3\n"
                                      u8"give coins silver 12\n"
                                      u8"shake strong\n"
                                      u8"cue lights dim\n"
                                      u8"fight Grey Wolf\n");
    const DialogueConversation& reward = Only(parse);
    REQUIRE(reward.steps.Size() == 7u);
    CHECK(reward.steps[0].name == StringView(u8"give item"));
    CHECK(Argument(reward.steps[0], u8"item")->text == StringView(u8"Brass Key"));
    CHECK(Argument(reward.steps[0], u8"count")->number == doctest::Approx(1.0f));
    // The fewest words that let the rest match: the optional count takes the 3.
    CHECK(Argument(reward.steps[1], u8"item")->text == StringView(u8"Apple"));
    CHECK(Argument(reward.steps[1], u8"count")->number == doctest::Approx(3.0f));
    CHECK(reward.steps[2].name == StringView(u8"give coins"));
    CHECK(Argument(reward.steps[2], u8"purse")->text == StringView(u8"silver"));
    CHECK(Argument(reward.steps[2], u8"count")->number == doctest::Approx(12.0f));
    CHECK(Argument(reward.steps[3], u8"how")->text == StringView(u8"strong"));
    CHECK(reward.steps[4].waits);
    CHECK(Argument(reward.steps[4], u8"event")->text == StringView(u8"lights dim"));
    CHECK(reward.steps[5].leaves);
    CHECK_FALSE(reward.steps[5].waits);
}

TEST_CASE("dialogue parse: every problem at its line")
{
    const DialogueParse parse = Parse(u8"@ keeper: Before any conversation.\n" // 1
                                      u8"conversation trouble\n"               // 2
                                      u8"@ keeper Hello there\n"               // 3: no colon
                                      u8"@ keeper:\n"                          // 4: says nothing
                                      u8"@ two words here: Hi\n"               // 5: three words
                                      u8"> An answer alone\n"                  // 6
                                      u8"else\n"                               // 7
                                      u8"end\n"                                // 8
                                      u8"choice\n"                             // 9: no answers
                                      u8"end\n"                                // 10
                                      u8"wait soon\n"                          // 11
                                      u8"fade sideways\n"                      // 12
                                      u8"fade in #12345\n"                     // 13
                                      u8"shake wildly\n"                       // 14
                                      u8"dance a jig\n"                        // 15
                                      u8"move Player to \"Unclosed\n"          // 16
                                      u8"if\n"                                 // 17
                                      u8"end\n"                                // 18
                                      u8"choice\n"                             // 19: not closed
                                      u8"> Leave\n"                            // 20
                                      u8"conversation trouble\n"               // 21: a second
                                      u8"if met\n"                             // 22: not closed
                                      u8"@ keeper: Hi. #line:same\n"           // 23
                                      u8"@ keeper: Hi. #line:same\n");         // 24
    CHECK_FALSE(parse.Ok());
    CHECK(HasProblem(parse, 1, u8"before any"));
    CHECK(HasProblem(parse, 3, u8"a line is"));
    CHECK(HasProblem(parse, 4, u8"says nothing"));
    CHECK(HasProblem(parse, 5, u8"a line is"));
    CHECK(HasProblem(parse, 6, u8"outside a choice"));
    CHECK(HasProblem(parse, 7, u8"outside an `if`"));
    CHECK(HasProblem(parse, 8, u8"nothing open"));
    CHECK(HasProblem(parse, 9, u8"needs"));
    CHECK(HasProblem(parse, 11, u8"`wait` is"));
    CHECK(HasProblem(parse, 12, u8"`fade` is"));
    CHECK(HasProblem(parse, 13, u8"colour"));
    CHECK(HasProblem(parse, 14, u8"the nearest is `shake"));
    CHECK(HasProblem(parse, 15, u8"`dance` is no statement"));
    CHECK(HasProblem(parse, 16, u8"quote"));
    CHECK(HasProblem(parse, 17, u8"`if [not] <flag>`"));
    CHECK(HasProblem(parse, 19, u8"not closed by `end`"));
    CHECK(HasProblem(parse, 21, u8"a second conversation"));
    CHECK(HasProblem(parse, 22, u8"not closed by `end`"));
    CHECK(HasProblem(parse, 24, u8"a second line"));
    for (usize i = 1; i < parse.problems.Size(); ++i)
    {
        CHECK(parse.problems[i - 1].line <= parse.problems[i].line);
    }
}

// A quoted word is an argument's text, never one of the form's words: "to" in quotes does not stand
// for the form's `to`.
TEST_CASE("dialogue parse: a quoted word never matches a form's literal word")
{
    const DialogueParse parse = Parse(u8"conversation quoted\n"
                                      u8"move Player \"to\" Old Well\n");
    CHECK_FALSE(parse.Ok());
    CHECK(HasProblem(parse, 2, u8"the nearest is `move"));
}

TEST_CASE("dialogue parse: an empty file has nothing and no problem")
{
    const DialogueParse parse = Parse(u8"# nothing yet\n\n");
    CHECK(parse.Ok());
    CHECK(parse.conversations.IsEmpty());
}
