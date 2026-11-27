// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the dialogue plugin's assets cooked through the standalone MCP host, as an
// agent or the editor cooks them. A project enables the plugin; its dialogue files each name their
// cast and dialogue assets. A file that names only what the project has cooks to its compiled
// conversations, one with an error fails the cook, and File > New's three creators make their
// assets, a new dialogue file taking the settings' default cast and dialogue assets. The plugin's
// MCP tools check every file or one without cooking (dialogue_validate) and read one's
// conversations (dialogue_read). Conversation
// ids are a file's own: two files may share one, and a jump into another file names it by its
// dialogue asset.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import foundation.vfs;
import pipeline.core;
import pipeline.importer;
import engine.project;
import editor.project;
import editor.mcp;
import dialogue.foundation;
import dialogue.engine;
import dialogue.pipeline;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
namespace runtime = foundation::runtime;
using foundation::json::JsonValue;

namespace
{
    struct Called
    {
        JsonValue value;
        bool error = false;
        String text;
    };

    Called Call(McpServer& server, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = server.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        JsonValue response = json::Parse(line.response.AsView()).value;
        Called called;
        if (response.Has(u8"error"))
        {
            called.error = true;
            called.text = response.Get(u8"error").Get(u8"message").AsString();
            return called;
        }
        JsonValue result = response.Get(u8"result");
        called.error = result.Get(u8"isError").AsBool();
        called.text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (!called.error)
        {
            called.value = JsonValue::Parse(called.text.AsView());
        }
        return called;
    }

    // A dialogue file in the project's sources and its asset, naming `cast` and `assets`; its guid.
    Guid AddDialogueFile(editor::EditorProject& project, foundation::content::Group& group, StringView name, StringView text,
                         const Guid& cast, const Guid& assets)
    {
        const String fileName = Format(u8"Dialogue/{}.dialogue", name);
        const String path = project.SourceFile(fileName.AsView());
        REQUIRE(CreateDirectories(PathParent(path.AsView())));
        REQUIRE(WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
        foundation::content::Instance* instance = group.CreateInstance(name, dialogue::DialogueAsset::StaticType());
        REQUIRE(instance != nullptr);
        dialogue::DialogueAsset asset;
        asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
        asset.cast = cast;
        asset.assets = assets;
        REQUIRE(instance->WriteObject(asset).IsOk());
        return instance->Id();
    }

    // A scratch project enabling dialogue: its cast (a keeper and a visitor), its dialogue assets (the
    // second file, as `lane`) and two clean dialogue files naming them, the first jumping into the
    // second, both with a conversation `gate`; the settings declare a command and a provided flag, and
    // name the cast and the assets as a new file's defaults.
    void MakeProject(StringView dir, Guid& gate, Guid& lane, Guid& castId, Guid& assetsId)
    {
        (void)RemoveDirectoryRecursive(dir);
        REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"Dialogue Cook").IsOk());
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        foundation::content::Group* root = project->SourceDb().RootGroup();
        REQUIRE(root != nullptr);

        foundation::content::Instance* castInstance = root->CreateInstance(u8"Cast", dialogue::DialogueCastAsset::StaticType());
        REQUIRE(castInstance != nullptr);
        dialogue::DialogueCastAsset cast;
        dialogue::DialogueSpeaker& keeper = cast.speakers.EmplaceBack();
        keeper.id = String(u8"keeper");
        keeper.shownName = String(u8"The Keeper");
        keeper.moods.PushBack(dialogue::DialogueMood{String(u8"neutral"), Guid{}});
        keeper.moods.PushBack(dialogue::DialogueMood{String(u8"worried"), Guid{}});
        cast.speakers.EmplaceBack().id = String(u8"visitor");
        REQUIRE(castInstance->WriteObject(cast).IsOk());

        foundation::content::Instance* listInstance =
            root->CreateInstance(u8"Dialogue Assets", dialogue::DialogueAssetListAsset::StaticType());
        REQUIRE(listInstance != nullptr);

        gate = AddDialogueFile(*project, *root, u8"Gate",
                               u8"conversation gate\n"
                               u8"@ keeper worried: Who goes there?\n"
                               u8"choice\n"
                               u8"> A friend.\n"
                               u8"  set friendly\n"
                               u8"> Nobody. #line:nobody\n"
                               u8"end\n"
                               u8"if seen_gate\n"
                               u8"  @ visitor: Again.\n"
                               u8"end\n"
                               u8"cue lights dim\n"
                               u8"jump lane in lane\n",
                               castInstance->Id(), listInstance->Id());
        lane = AddDialogueFile(*project, *root, u8"Lane",
                               u8"conversation gate\n"
                               u8"@ visitor: The other gate.\n"
                               u8"conversation lane\n"
                               u8"@ visitor: Down the lane.\n",
                               castInstance->Id(), listInstance->Id());
        dialogue::DialogueAssetListAsset list;
        list.entries.PushBack(dialogue::DialogueAssetEntry{String(u8"lane"), lane});
        REQUIRE(listInstance->WriteObject(list).IsOk());
        castId = castInstance->Id();
        assetsId = listInstance->Id();

        dialogue::DialogueSettings settings;
        settings.cast = castInstance->Id();
        settings.assets = listInstance->Id();
        settings.commands.PushBack(String(u8"cue <event:text>"));
        settings.providedFlags.PushBack(String(u8"seen_*"));
        runtime::EnabledPlugin entry;
        entry.id = String(u8"dialogue");
        REQUIRE(runtime::WritePluginSettings(settings, entry.settings, DefaultAllocator()).IsOk());
        project->Settings().plugins.PushBack(Move(entry));
        REQUIRE(project->SaveSettings().IsOk());
    }
}

TEST_CASE("plugins: through the MCP host, a dialogue file cooks to its conversations, one with an error does not, and "
          "File > New makes the dialogue's assets")
{
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::McpPluginServices pluginTools{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView());
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     editor::mcp::EngineToolPaths{}, operations);
    pluginTools.AttachServer(server, session);
    editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, StringView{},
                                          editor::mcp::ProjectPlugins{runtime::LinkedPluginList(),
                                                                      pipeline::PipelineRegistries{&builders, &importers, &creators},
                                                                      &pluginTools});
    const StringView dir = u8"scratch_plugins_dialogue_cook";
    Guid gate;
    Guid lane;
    Guid castId;
    Guid assetsId;
    MakeProject(dir, gate, lane, castId, assetsId);

    JsonValue openArgs = JsonValue::MakeObject();
    openArgs.Set(u8"directory", JsonValue::MakeString(String(dir)));
    REQUIRE_FALSE(Call(server, u8"project_open", Move(openArgs)).error);
    REQUIRE(session.plugins != nullptr);
    CHECK(session.plugins->IsLoaded(u8"dialogue"));
    CHECK(builders.Find(&dialogue::DialogueAsset::StaticType()) != nullptr);
    CHECK(builders.Find(&dialogue::DialogueCastAsset::StaticType()) != nullptr);

    // The clean file cooks: its conversation compiled, its texts by line id; the cast cooks too.
    const Called cooked = Call(server, u8"asset_cook", JsonValue::MakeObject());
    REQUIRE_FALSE(cooked.error);
    CHECK(cooked.value.Get(u8"failed").AsNumber() == doctest::Approx(0.0));
    RefPtr<ISerializable> read = session.project->CookedDb().ReadObject(gate);
    auto* source = read ? Cast<dialogue::DialogueSource>(read.Get()) : nullptr;
    REQUIRE(source != nullptr);
    CHECK(source->cast == castId); // the runner loads it for the lines' speakers
    REQUIRE(source->conversations.Size() == 1u);
    const dialogue::DialogueConversation& conversation = source->conversations[0];
    CHECK(conversation.id == StringView(u8"gate"));
    CHECK(conversation.steps[0].kind == dialogue::DialogueStepKind::Line);
    CHECK(conversation.steps.Back().kind == dialogue::DialogueStepKind::End);
    const dialogue::DialogueStep& jump = conversation.steps[conversation.steps.Size() - 2];
    CHECK(jump.kind == dialogue::DialogueStepKind::Jump);
    CHECK(jump.name == StringView(u8"lane"));
    CHECK(jump.asset == lane);
    RefPtr<ISerializable> laneRead = session.project->CookedDb().ReadObject(lane);
    const auto* laneSource = laneRead ? Cast<dialogue::DialogueSource>(laneRead.Get()) : nullptr;
    REQUIRE(laneSource != nullptr);
    CHECK(laneSource->conversations.Size() == 2u);
    bool cue = false;
    for (const dialogue::DialogueStep& step : conversation.steps)
    {
        cue = cue || (step.kind == dialogue::DialogueStepKind::Command && step.name == StringView(u8"cue"));
    }
    CHECK(cue);
    bool nobody = false;
    for (const dialogue::DialogueLineText& line : source->lines)
    {
        nobody = nobody || (line.id == StringView(u8"nobody") && line.text == StringView(u8"Nobody."));
    }
    CHECK(nobody);

    // A file naming a mood the keeper lacks does not cook.
    {
        foundation::content::Group* root = session.project->SourceDb().RootGroup();
        REQUIRE(root != nullptr);
        (void)AddDialogueFile(*session.project, *root, u8"Wrong",
                              u8"conversation wrong\n"
                              u8"@ keeper angry: Out!\n",
                              castId, assetsId);
    }
    const Called failing = Call(server, u8"asset_cook", JsonValue::MakeObject());
    REQUIRE_FALSE(failing.error);
    CHECK(failing.value.Get(u8"failed").AsNumber() == doctest::Approx(1.0));

    // Its settings as the MCP tools read and set them: numbers and colours too.
    {
        const Called listed = Call(server, u8"plugin_list", JsonValue::MakeObject());
        REQUIRE_FALSE(listed.error);
        JsonValue settings;
        const JsonValue& plugins = listed.value.Get(u8"plugins");
        for (usize i = 0; i < static_cast<usize>(plugins.Count()); ++i)
        {
            if (plugins.At(i).Get(u8"id").AsString() == String(u8"dialogue"))
            {
                settings = plugins.At(i).Get(u8"settings");
            }
        }
        CHECK(settings.Get(u8"typingSpeed").AsNumber() == doctest::Approx(40.0));
        CHECK(settings.Get(u8"fadeSeconds").AsNumber() == doctest::Approx(0.5));
        REQUIRE(settings.Get(u8"fadeColor").Count() == 4);
        CHECK(settings.Get(u8"fadeColor").At(3).AsNumber() == doctest::Approx(1.0));
        const auto set = [&server](JsonValue values)
        {
            JsonValue args = JsonValue::MakeObject();
            args.Set(u8"id", JsonValue::MakeString(String(u8"dialogue")));
            args.Set(u8"settings", Move(values));
            return Call(server, u8"plugin_settings_set", Move(args));
        };
        JsonValue tooFast = JsonValue::MakeObject();
        tooFast.Set(u8"typingSpeed", JsonValue::MakeNumber(500.0));
        CHECK(set(Move(tooFast)).text.AsView().Contains(u8"`typingSpeed` takes a number from 0 to 200"));
        JsonValue badColour = JsonValue::MakeObject();
        badColour.Set(u8"fadeColor", JsonValue::MakeString(String(u8"white")));
        CHECK(set(Move(badColour)).error);
        JsonValue values = JsonValue::MakeObject();
        values.Set(u8"typingSpeed", JsonValue::MakeNumber(25.0));
        JsonValue white = JsonValue::MakeArray();
        for (int i = 0; i < 3; ++i)
        {
            white.Add(JsonValue::MakeNumber(1.0));
        }
        values.Set(u8"fadeColor", Move(white));
        REQUIRE_FALSE(set(Move(values)).error);
        const auto* held = static_cast<const dialogue::DialogueSettings*>(session.plugins->Settings(u8"dialogue"));
        REQUIRE(held != nullptr);
        CHECK(held->typingSpeed == doctest::Approx(25.0f));
        CHECK(held->fadeColor.x == doctest::Approx(1.0f));
        CHECK(held->fadeColor.w == doctest::Approx(1.0f)); // three channels: opaque
    }

    // The plugin's tools: the checks over every file, without cooking, and one file's conversations.
    const Called validated = Call(server, u8"dialogue_validate", JsonValue::MakeObject());
    REQUIRE_FALSE(validated.error);
    CHECK(validated.value.Get(u8"files").Count() == 3); // Gate, Lane, Wrong
    CHECK(validated.value.Get(u8"errors").AsNumber() == doctest::Approx(1.0));
    bool wrongTold = false;
    for (i64 i = 0; i < validated.value.Get(u8"files").Count(); ++i)
    {
        const JsonValue file = validated.value.Get(u8"files").At(i);
        if (file.Get(u8"name").AsString() == String(u8"Wrong"))
        {
            CHECK_FALSE(file.Get(u8"ok").AsBool());
            REQUIRE(file.Get(u8"problems").Count() == 1);
            CHECK(file.Get(u8"problems").At(0).Get(u8"line").AsNumber() == doctest::Approx(2.0));
            CHECK(file.Get(u8"problems").At(0).Get(u8"message").AsString().AsView().Contains(u8"no mood `angry`"));
            wrongTold = true;
        }
    }
    CHECK(wrongTold);
    utf8char gateText[37];
    gate.ToChars(gateText);
    JsonValue one = JsonValue::MakeObject();
    one.Set(u8"asset", JsonValue::MakeString(String(StringView(gateText, 36))));
    const Called gateValid = Call(server, u8"dialogue_validate", one);
    REQUIRE_FALSE(gateValid.error);
    REQUIRE(gateValid.value.Get(u8"files").Count() == 1);
    CHECK(gateValid.value.Get(u8"files").At(0).Get(u8"ok").AsBool());

    const Called readOne = Call(server, u8"dialogue_read", one);
    REQUIRE_FALSE(readOne.error);
    REQUIRE(readOne.value.Get(u8"conversations").Count() == 1);
    const JsonValue talk = readOne.value.Get(u8"conversations").At(0);
    CHECK(talk.Get(u8"id").AsString() == String(u8"gate"));
    REQUIRE(talk.Get(u8"lines").Count() == 2);
    CHECK(talk.Get(u8"lines").At(0).Get(u8"speaker").AsString() == String(u8"keeper"));
    CHECK(talk.Get(u8"lines").At(0).Get(u8"text").AsString() == String(u8"Who goes there?"));
    REQUIRE(talk.Get(u8"choices").Count() == 1);
    CHECK(talk.Get(u8"choices").At(0).Get(u8"answers").At(1).Get(u8"id").AsString() == String(u8"nobody"));
    REQUIRE(talk.Get(u8"commands").Count() == 1);
    CHECK(talk.Get(u8"commands").At(0).Get(u8"arguments").Get(u8"event").AsString() == String(u8"lights dim"));
    CHECK(talk.Get(u8"jumps").At(0).AsString() == String(u8"lane in lane"));
    CHECK(talk.Get(u8"flagsWritten").At(0).AsString() == String(u8"friendly"));
    CHECK(talk.Get(u8"flagsRead").At(0).AsString() == String(u8"seen_gate"));
    CHECK(Call(server, u8"dialogue_read", JsonValue::MakeObject()).error); // no asset named

    // File > New: the three creators make their types.
    const auto create = [&server](StringView creator, StringView name)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"creator", JsonValue::MakeString(String(creator)));
        args.Set(u8"name", JsonValue::MakeString(String(name)));
        const Called made = Call(server, u8"asset_create", Move(args));
        REQUIRE_FALSE(made.error);
        return made.value;
    };
    const JsonValue village = create(dialogue::kDialogueCreator, u8"Village");
    CHECK(village.Get(u8"type").AsString() == String(u8"DialogueAsset"));
    Guid villageId;
    REQUIRE(Guid::TryParse(village.Get(u8"guid").AsString().AsView(), villageId));
    RefPtr<ISerializable> villageRead = session.project->SourceDb().ReadObject(villageId);
    const auto* villageAsset = villageRead ? Cast<dialogue::DialogueAsset>(villageRead.Get()) : nullptr;
    REQUIRE(villageAsset != nullptr);
    CHECK(villageAsset->cast == castId); // the settings' defaults
    CHECK(villageAsset->assets == assetsId);
    CHECK(create(dialogue::kDialogueCastCreator, u8"Second Cast").Get(u8"type").AsString() == String(u8"DialogueCastAsset"));
    CHECK(create(dialogue::kDialogueAssetsCreator, u8"More Assets").Get(u8"type").AsString() ==
          String(u8"DialogueAssetListAsset"));

    session.plugins = nullptr;
    owner.plugins.Reset();
    session.project = nullptr;
    owner.project.Reset();
    pluginTools.DetachServer(server);
    (void)RemoveDirectoryRecursive(dir);
}
