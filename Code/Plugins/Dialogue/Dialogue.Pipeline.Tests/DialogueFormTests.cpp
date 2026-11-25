// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Pipeline: the notation commands are declared in, parsed into forms, and what it refuses.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import dialogue.pipeline;

using namespace foundation::core;
using namespace dialogue;

namespace
{
    DialogueCommandForm Parsed(StringView text)
    {
        DialogueCommandForm form;
        String error;
        const bool ok = ParseCommandForm(text, form, error);
        CHECK_MESSAGE(ok, error);
        return form;
    }

    String Refusal(StringView text)
    {
        DialogueCommandForm form;
        String error;
        CHECK_FALSE(ParseCommandForm(text, form, error));
        CHECK_FALSE(error.IsEmpty());
        return error;
    }
}

TEST_CASE("dialogue forms: a form's literal words, arguments and optional parts")
{
    const DialogueCommandForm move = Parsed(u8"move <who:entity> to <target:entity> [running] [and wait]");
    CHECK(move.name == StringView(u8"move"));
    CHECK(move.wait == DialogueCommandWait::Never);
    CHECK(move.groupCount == 2u);
    CHECK(move.waitGroup == 2u);
    REQUIRE(move.parts.Size() == 7u);
    CHECK(move.parts[0].literal);
    CHECK_FALSE(move.parts[1].literal);
    CHECK(move.parts[1].name == StringView(u8"who"));
    CHECK(move.parts[1].kind == DialogueArgumentKind::Entity);
    CHECK(move.parts[2].word == StringView(u8"to"));
    CHECK(move.parts[4].word == StringView(u8"running"));
    CHECK(move.parts[4].group == 1u);
}

TEST_CASE("dialogue forms: every kind, a default, and how a command waits")
{
    const DialogueCommandForm camera = Parsed(u8"camera <target:entity> [for <seconds:number=2>]");
    REQUIRE(camera.parts.Size() == 4u);
    CHECK(camera.parts[3].kind == DialogueArgumentKind::Number);
    CHECK(camera.parts[3].hasDefault);
    CHECK(camera.parts[3].defaultValue == StringView(u8"2"));

    const DialogueCommandForm shake = Parsed(u8"shake <how:gentle|strong|violent>");
    REQUIRE(shake.parts[1].choices.Size() == 3u);
    CHECK(shake.parts[1].kind == DialogueArgumentKind::OneOf);
    CHECK(shake.parts[1].choices[2] == StringView(u8"violent"));

    const DialogueCommandForm fight = Parsed(u8"fight <foe:list(foes)> leaves");
    CHECK(fight.wait == DialogueCommandWait::Leaves);
    CHECK(fight.parts[1].kind == DialogueArgumentKind::List);
    CHECK(fight.parts[1].list == StringView(u8"foes"));

    const DialogueCommandForm show = Parsed(u8"show card <card:asset(TextureAsset)> waits");
    CHECK(show.name == StringView(u8"show card"));
    CHECK(show.wait == DialogueCommandWait::Always);
    CHECK(show.parts[2].kind == DialogueArgumentKind::Asset);
    CHECK(show.parts[2].assetType == StringView(u8"TextureAsset"));

    const DialogueCommandForm note = Parsed(u8"note <who:speaker> <what:flag> <tag:word> <text:text>");
    CHECK(note.parts[1].kind == DialogueArgumentKind::Speaker);
    CHECK(note.parts[2].kind == DialogueArgumentKind::Flag);
    CHECK(note.parts[3].kind == DialogueArgumentKind::Word);
    CHECK(note.parts[4].kind == DialogueArgumentKind::Text);
}

TEST_CASE("dialogue forms: what is not a form is refused, and why")
{
    (void)Refusal(u8"");
    (void)Refusal(u8"<who:entity> waves");             // no name first
    (void)Refusal(u8"wave [at <who:entity>");          // a group not closed
    (void)Refusal(u8"wave at <who:entity>]");          // a close with nothing open
    (void)Refusal(u8"wave [at [<who:entity>]]");       // a group in a group
    (void)Refusal(u8"wave [at [<who:entity>]");        // a group in a group, with every bracket matched but one
    (void)Refusal(u8"wave []");                        // an empty group
    (void)Refusal(u8"wave <who>");                     // no kind
    (void)Refusal(u8"wave <who:person>");              // an unknown kind
    (void)Refusal(u8"wave <who:entity");               // an argument not closed
    (void)Refusal(u8"wave <a b:entity>");              // a name of two words
    (void)Refusal(u8"wave <who:entity> <who:entity>"); // two of one name
    (void)Refusal(u8"wave <how:slow|>");               // an empty choice
    (void)Refusal(u8"wave <who:entity=Cat>");          // a default outside an optional part
    (void)Refusal(u8"wave [for <s:number=soon>]");     // a number's default not a number
    (void)Refusal(u8"wave [and wait] waits");          // waits two ways
    const String builtIn = Refusal(u8"play <who:entity> loudly");
    CHECK(builtIn.AsView().Contains(u8"play"));
    (void)Refusal(u8"@ <who:speaker>");
}
