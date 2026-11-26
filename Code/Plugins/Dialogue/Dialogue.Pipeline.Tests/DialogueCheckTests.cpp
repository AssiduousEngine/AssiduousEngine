// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Pipeline: the cook's checks over a parsed file, against a cast, the provided flags, the
// project's other dialogue files and its assets (a table here), and the assets they resolve.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import dialogue.foundation;
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

    // A project's assets: a name and a type for each, by guid.
    class TableAssets final : public IDialogueAssets
    {
    public:
        struct Entry
        {
            String name;
            String type;
            Guid id;
        };
        Array<Entry> entries;

        Guid Add(StringView name, StringView type, u64 seed)
        {
            const Guid id = MakeGuid(seed);
            entries.PushBack(Entry{String(name), String(type), id});
            return id;
        }

        [[nodiscard]] String NameOf(const Guid& id) const override
        {
            for (const Entry& entry : entries)
            {
                if (entry.id == id)
                {
                    return entry.name;
                }
            }
            return {};
        }

        [[nodiscard]] String TypeOf(const Guid& id) const override
        {
            for (const Entry& entry : entries)
            {
                if (entry.id == id)
                {
                    return entry.type;
                }
            }
            return {};
        }
    };

    // A project's levels: the entity names of each.
    class TableLevels final : public IDialogueLevels
    {
    public:
        Guid level;
        Array<String> names;

        [[nodiscard]] const Array<String>* EntityNames(const Guid& id) const override
        {
            return id == level ? &names : nullptr;
        }
    };

    struct Project
    {
        TableAssets assets;
        Array<DialogueSpeaker> cast;
        Array<DialogueAssetEntry> dialogueAssets;
        Array<String> actions;
        TableLevels levels;
        Array<DialogueCommandForm> commands;
        Array<String> provided;
        Guid self;
        Array<DialogueFileConversations> otherFiles;
        Array<String> otherFlagsSet;
        Guid wave;
        Guid creak;
        Guid gateOpen;
        Guid card;
        Guid farewells;

        Project()
        {
            wave = assets.Add(u8"Wave", u8"AnimationClipAsset", 1);
            const Guid nod = assets.Add(u8"Nod", u8"AnimationClipAsset", 2);
            creak = assets.Add(u8"Door Creak", u8"AudioClipAsset", 3);
            gateOpen = assets.Add(u8"Gate Open", u8"SequenceAsset", 4);
            card = assets.Add(u8"Title", u8"TextureAsset", 5);
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"door_creak"), creak});
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"gate_open"), gateOpen});
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"title_card"), card});
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"lost"), MakeGuid(99)}); // refers to nothing
            levels.level = assets.Add(u8"Village", u8"SceneDocument", 6);
            levels.names.PushBack(String(u8"Keeper"));
            levels.names.PushBack(String(u8"Well"));
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"village"), levels.level});
            self = assets.Add(u8"Gate", u8"DialogueAsset", 7);
            farewells = assets.Add(u8"Farewells", u8"DialogueAsset", 8);
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"gate"), self});
            dialogueAssets.PushBack(DialogueAssetEntry{String(u8"farewells"), farewells});
            actions.PushBack(String(u8"Interact"));
            actions.PushBack(String(u8"Move"));
            DialogueSpeaker& keeper = cast.EmplaceBack();
            keeper.id = String(u8"keeper");
            keeper.moods.PushBack(DialogueMood{String(u8"neutral"), Guid{}});
            keeper.moods.PushBack(DialogueMood{String(u8"worried"), Guid{}});
            keeper.clips.PushBack(wave);
            keeper.clips.PushBack(nod);
            keeper.entity = String(u8"Keeper");
            cast.EmplaceBack().id = String(u8"visitor");
            const StringView forms[] = {u8"show card <card:asset(TextureAsset)>", u8"greet <who:speaker>",
                                        u8"walk <who:entity> to <target:entity>"};
            for (StringView text : forms)
            {
                DialogueCommandForm form;
                String error;
                REQUIRE(ParseCommandForm(text, form, error));
                commands.PushBack(Move(form));
            }
            provided.PushBack(String(u8"seen_*"));
            provided.PushBack(String(u8"has_lamp"));
            DialogueFileConversations& other = otherFiles.EmplaceBack();
            other.file = farewells;
            other.conversations.PushBack(String(u8"farewell"));
            other.conversations.PushBack(String(u8"gate")); // a file's ids are its own
            otherFlagsSet.PushBack(String(u8"met_elsewhere"));
        }

        DialogueParse Check(StringView text, bool withCast = true, bool withAssets = true, bool withActions = true)
        {
            DialogueParse parse = ParseDialogue(text, Span<const DialogueCommandForm>(commands.Data(), commands.Size()));
            DialogueCheckInputs inputs;
            inputs.cast = withCast ? &cast : nullptr;
            inputs.dialogueAssets = withAssets ? &dialogueAssets : nullptr;
            inputs.commands = Span<const DialogueCommandForm>(commands.Data(), commands.Size());
            inputs.providedFlags = Span<const String>(provided.Data(), provided.Size());
            inputs.self = self;
            inputs.otherFiles = Span<const DialogueFileConversations>(otherFiles.Data(), otherFiles.Size());
            inputs.otherFlagsSet = Span<const String>(otherFlagsSet.Data(), otherFlagsSet.Size());
            inputs.assets = &assets;
            inputs.actions = withActions ? &actions : nullptr;
            inputs.levels = &levels;
            CheckDialogue(parse, inputs);
            return parse;
        }
    };

    bool HasProblem(const DialogueParse& parse, u32 line, StringView words, bool warning = false)
    {
        for (const DialogueProblem& problem : parse.problems)
        {
            if (problem.line == line && problem.warning == warning && problem.message.AsView().Contains(words))
            {
                return true;
            }
        }
        return false;
    }

    void RequireClean(const DialogueParse& parse)
    {
        for (const DialogueProblem& problem : parse.problems)
        {
            INFO(problem.line, ": ", problem.message);
            CHECK(false);
        }
    }
}

TEST_CASE("dialogue check: a file that names only what the project has is clean, its assets resolved")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation gate\n"
                                              u8"@ keeper worried: Who goes there?\n"
                                              u8"@ visitor: Only me.\n"
                                              u8"play keeper Wave and wait\n"
                                              u8"sound door_creak at keeper\n"
                                              u8"sequence gate_open and wait\n"
                                              u8"show card title_card\n"
                                              u8"greet visitor\n"
                                              u8"if seen_gate\n"
                                              u8"  set met\n"
                                              u8"end\n"
                                              u8"if has_lamp\n"
                                              u8"end\n"
                                              u8"if met_elsewhere\n"
                                              u8"end\n"
                                              u8"if met\n"
                                              u8"end\n"
                                              u8"jump farewell in farewells\n");
    RequireClean(parse);
    const Array<DialogueStep>& steps = parse.conversations[0].steps;
    CHECK(steps[steps.Size() - 2].kind == DialogueStepKind::Jump);
    CHECK(steps[steps.Size() - 2].asset == project.farewells);
    CHECK(steps[2].asset == project.wave);
    CHECK(steps[3].asset == project.creak);
    CHECK(steps[4].asset == project.gateOpen);
    REQUIRE(steps[5].arguments.Size() == 1u);
    CHECK(steps[5].arguments[0].asset == project.card);
}

TEST_CASE("dialogue check: what the project does not have, each at its line")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation farewell\n"          // 1: another file's id too, which is fine
                                              u8"@ stranger: Hello.\n"             // 2
                                              u8"@ keeper angry: Leave.\n"         // 3
                                              u8"play keeper Dance\n"              // 4
                                              u8"play stranger Wave\n"             // 5
                                              u8"sound missing\n"                  // 6
                                              u8"sound gate_open\n"                // 7: not a sound
                                              u8"sequence lost\n"                 // 8: refers to nothing
                                              u8"show card door_creak\n"          // 9: not a texture
                                              u8"greet stranger\n"                 // 10
                                              u8"if never_set\n"                   // 11
                                              u8"end\n"                            // 12
                                              u8"@ keeper: Taken. #line:taken\n"   // 13
                                              u8"jump nowhere\n"                   // 14
                                              u8"jump nowhere in farewells\n"      // 15
                                              u8"jump farewell in door_creak\n"    // 16: not a dialogue file
                                              u8"jump farewell in elsewhere\n");   // 17
    CHECK_FALSE(HasProblem(parse, 1, u8""));
    CHECK(HasProblem(parse, 2, u8"no speaker `stranger`"));
    CHECK(HasProblem(parse, 3, u8"no mood `angry` (has neutral, worried)"));
    CHECK(HasProblem(parse, 4, u8"no clip `Dance` (has Wave, Nod)"));
    CHECK(HasProblem(parse, 5, u8"no speaker `stranger`"));
    CHECK(HasProblem(parse, 6, u8"no dialogue asset `missing`"));
    CHECK(HasProblem(parse, 7, u8"`gate_open` is of type SequenceAsset, which is no sound"));
    CHECK(HasProblem(parse, 8, u8"`lost` refers to no asset"));
    CHECK(HasProblem(parse, 9, u8"is of type AudioClipAsset, which is no TextureAsset"));
    CHECK(HasProblem(parse, 10, u8"no speaker `stranger`"));
    CHECK(HasProblem(parse, 11, u8"`never_set` is read but nothing sets it"));
    CHECK_FALSE(HasProblem(parse, 13, u8""));
    CHECK(HasProblem(parse, 14, u8"no conversation `nowhere` in this file"));
    CHECK(HasProblem(parse, 15, u8"no conversation `nowhere` in `farewells`"));
    CHECK(HasProblem(parse, 16, u8"`door_creak` is of type AudioClipAsset, which is no dialogue file"));
    CHECK(HasProblem(parse, 17, u8"no dialogue asset `elsewhere`"));
    CHECK_FALSE(parse.Ok());
}

TEST_CASE("dialogue check: a jump within the file, or into this file by its own name, takes no asset")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation gate\n"
                                              u8"jump lane\n"
                                              u8"conversation lane\n"
                                              u8"jump gate in gate\n");
    RequireClean(parse);
    CHECK(parse.conversations[0].steps[0].asset == Guid{});
    CHECK(parse.conversations[1].steps[0].file == StringView(u8"gate"));
    CHECK(parse.conversations[1].steps[0].asset == Guid{});
}

TEST_CASE("dialogue check: with no dialogue assets, the file is told once")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation bare\n"
                                              u8"sound door_creak\n"
                                              u8"sequence gate_open\n",
                                              true, false);
    REQUIRE(parse.problems.Size() == 1u);
    CHECK(HasProblem(parse, 2, u8"names no dialogue assets"));
}

TEST_CASE("dialogue check: with no cast, the file is told once")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation alone\n"
                                              u8"@ keeper: One.\n"
                                              u8"@ keeper: Two.\n",
                                              false);
    REQUIRE(parse.problems.Size() == 1u);
    CHECK(HasProblem(parse, 2, u8"names no cast"));
}

TEST_CASE("dialogue check: what nothing reaches is a warning, not an error")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation early\n"
                                              u8"@ keeper: Off you go.\n"
                                              u8"jump farewell in farewells\n"
                                              u8"@ keeper: Never said.\n");
    CHECK(parse.Ok());
    CHECK(HasProblem(parse, 4, u8"nothing reaches", true));
    CHECK_FALSE(HasProblem(parse, 2, u8"nothing reaches", true));
}

TEST_CASE("dialogue check: a provided flag by name or by prefix")
{
    Array<String> provided;
    provided.PushBack(String(u8"seen_*"));
    provided.PushBack(String(u8"has_lamp"));
    const Span<const String> span(provided.Data(), provided.Size());
    CHECK(IsProvidedFlag(u8"seen_gate", span));
    CHECK(IsProvidedFlag(u8"seen_", span));
    CHECK(IsProvidedFlag(u8"has_lamp", span));
    CHECK_FALSE(IsProvidedFlag(u8"has_lamps", span));
    CHECK_FALSE(IsProvidedFlag(u8"unseen", span));
}

TEST_CASE("dialogue check: the actions a line names are the input maps'")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation hint\n"
                                              u8"@ keeper: Press {Interact} to open, {{or not}}.\n" // 2: clean
                                              u8"@ keeper: Press {Jump}.\n"                       // 3
                                              u8"choice\n"
                                              u8"> {Move} away\n"                                // 5: clean
                                              u8"> {Fly} away\n"                                 // 6
                                              u8"end\n"
                                              u8"@ keeper: An open {brace.\n");                  // 8
    CHECK_FALSE(HasProblem(parse, 2, u8""));
    CHECK(HasProblem(parse, 3, u8"no action `Jump`"));
    CHECK_FALSE(HasProblem(parse, 5, u8""));
    CHECK(HasProblem(parse, 6, u8"no action `Fly`"));
    CHECK(HasProblem(parse, 8, u8"a `{` with no `}`"));

    const DialogueParse bare = project.Check(u8"conversation bare\n"
                                             u8"@ keeper: Press {Interact}.\n"
                                             u8"@ keeper: Press {Move}.\n",
                                             true, true, false);
    REQUIRE(bare.problems.Size() == 1u);
    CHECK(HasProblem(bare, 2, u8"`{Interact}` names an action, but the project has no input map"));
}

TEST_CASE("dialogue check: a conversation's level: its entities named, a missing one a warning")
{
    Project project;
    const DialogueParse parse = project.Check(u8"conversation square in village\n"
                                              u8"@ keeper: Here.\n"                  // the keeper's entity is there
                                              u8"walk Keeper to Well\n"              // both there
                                              u8"walk Keeper to Old Mill\n"          // 4: not there
                                              u8"walk Old Mill to Keeper\n");        // 5: told once
    CHECK(parse.Ok());
    CHECK(HasProblem(parse, 4, u8"no entity `Old Mill` (`walk`'s target) in the level `village`", true));
    CHECK_FALSE(HasProblem(parse, 5, u8"Old Mill", true));

    project.cast[0].entity = String(u8"Gatekeeper");
    const DialogueParse moved = project.Check(u8"conversation square in village\n"
                                              u8"@ keeper: Here.\n");
    CHECK(HasProblem(moved, 2, u8"no entity `Gatekeeper` (the speaker `keeper`'s)", true));

    const DialogueParse wrong = project.Check(u8"conversation square in door_creak\n"
                                              u8"@ keeper: Here.\n");
    CHECK(HasProblem(wrong, 1, u8"`door_creak` is of type AudioClipAsset, which is no level"));
}

// What a file tells the others' checks: its conversations, and each flag it writes (set or clear) and
// reads (if), once each, whatever the order or the repeats.
TEST_CASE("dialogue check: a file's facts are its conversations and the flags it sets and reads, once each")
{
    const DialogueParse parse = ParseDialogue(u8"conversation gate\n"
                                              u8"set met\n"
                                              u8"if met\n"
                                              u8"@ keeper: Again.\n"
                                              u8"end\n"
                                              u8"clear met\n"
                                              u8"conversation farewell\n"
                                              u8"if met\n"
                                              u8"@ keeper: Bye.\n"
                                              u8"end\n"
                                              u8"if not asked\n"
                                              u8"set asked\n"
                                              u8"end\n",
                                              {});
    REQUIRE(parse.Ok());
    Array<String> conversations;
    Array<String> flagsSet;
    Array<String> flagsRead;
    CollectDialogueFacts(parse, conversations, flagsSet, flagsRead);
    REQUIRE(conversations.Size() == 2u);
    CHECK(conversations[0] == StringView(u8"gate"));
    CHECK(conversations[1] == StringView(u8"farewell"));
    REQUIRE(flagsSet.Size() == 2u); // met (set, then cleared: once), asked
    CHECK(flagsSet[0] == StringView(u8"met"));
    CHECK(flagsSet[1] == StringView(u8"asked"));
    REQUIRE(flagsRead.Size() == 2u); // met (read twice: once), asked
    CHECK(flagsRead[0] == StringView(u8"met"));
    CHECK(flagsRead[1] == StringView(u8"asked"));
}
