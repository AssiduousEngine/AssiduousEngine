// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: an agent sets up play in editor's saves for a test, as a story is set just before
// a beat: save_write writes a slot and the global store, save_list orders the slots as the game does,
// save_read reads a store back, and what was written is what the game's saves open. A bad value
// refuses the call and writes nothing; the writes wait while a run holds the saves; save_delete
// takes a slot, or every save.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import engine.run;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;

namespace
{
    struct Answer
    {
        bool error = false;
        JsonValue value;
        String text;
    };

    Answer Call(McpServer& server, StringView tool, StringView arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", JsonValue::Parse(String(arguments)));
        JsonValue request = JsonValue::MakeObject();
        request.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        request.Set(u8"id", JsonValue::MakeNumber(1));
        request.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        request.Set(u8"params", Move(params));
        LineOutcome line = server.HandleLine(request.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        const JsonValue response = json::Parse(line.response.AsView()).value;
        REQUIRE(response.Has(u8"result"));
        Answer answer;
        answer.error = response.Get(u8"result").Get(u8"isError").AsBool();
        answer.text = response.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString();
        answer.value = JsonValue::Parse(answer.text);
        return answer;
    }
}

TEST_CASE("integration.mcp: an agent writes play in editor's saves, and the game's saves open what it wrote")
{
    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    (void)pipeline::RegisterAllCreators(creators);
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView());
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer, editor::mcp::EngineToolPaths{}, operations);
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    (void)RemoveDirectoryRecursive(u8"mcp_saves");
    REQUIRE_FALSE(Call(server, u8"project_create", u8"{\"directory\":\"mcp_saves\",\"name\":\"Fair\"}").error);
    REQUIRE_FALSE(Call(server, u8"project_open", u8"{\"directory\":\"mcp_saves\"}").error);

    // Nothing yet.
    Answer listed = Call(server, u8"save_list", u8"{}");
    REQUIRE_FALSE(listed.error);
    CHECK_FALSE(listed.value.Get(u8"global").Get(u8"exists").AsBool());
    CHECK(listed.value.Get(u8"slots").Count() == 0);

    // A story set just before a beat, in slot 2, and an option in the global store.
    Answer wrote = Call(server, u8"save_write",
                        u8"{\"slot\":\"Two\",\"values\":[{\"key\":\"flag.dummies_done\",\"kind\":\"bool\",\"value\":true},"
                        u8"{\"key\":\"world.scene\",\"kind\":\"text\",\"value\":\"Hometown\"},{\"key\":\"errands\",\"kind\":\"int\",\"value\":3},"
                        u8"{\"key\":\"ghost\",\"kind\":\"floats\",\"value\":[1,2.5]}]}");
    REQUIRE_FALSE(wrote.error);
    CHECK(wrote.value.Get(u8"savedAt").AsNumber() > 0.0);
    REQUIRE_FALSE(Call(server, u8"save_write", u8"{\"values\":[{\"key\":\"textSpeed\",\"kind\":\"float\",\"value\":1.5}]}").error);
    REQUIRE_FALSE(Call(server, u8"save_write", u8"{\"slot\":\"one\",\"values\":[{\"key\":\"x\",\"kind\":\"int\",\"value\":1}]}").error);

    listed = Call(server, u8"save_list", u8"{}");
    CHECK(listed.value.Get(u8"global").Get(u8"exists").AsBool());
    REQUIRE(listed.value.Get(u8"slots").Count() == 2);

    // Read back as written, the slot name folded.
    Answer read = Call(server, u8"save_read", u8"{\"slot\":\"two\"}");
    REQUIRE_FALSE(read.error);
    CHECK(read.value.Get(u8"exists").AsBool());
    REQUIRE(read.value.Get(u8"values").Count() == 4);

    // What the game's saves open: the same folder under the project's Editor/.
    {
        engine::runtime::RunSaves saves(DefaultAllocator());
        saves.Open(listed.value.Get(u8"folder").AsString().AsView());
        CHECK(saves.Global().Values().GetFloat(u8"textSpeed", 0.0f) == doctest::Approx(1.5f));
        engine::runtime::SaveFile* two = saves.Resolve(saves.OpenSlot(u8"two"));
        REQUIRE(two != nullptr);
        CHECK(two->Values().GetBool(u8"flag.dummies_done", false));
        CHECK(two->Values().GetText(u8"world.scene", u8"") == StringView(u8"Hometown"));
        CHECK(two->Values().GetInt(u8"errands", 0) == 3);
        CHECK(two->Values().GetFloats(u8"ghost").Size() == 2u);
        CHECK_FALSE(two->Values().Has(u8"textSpeed"));
    }

    // A bad value refuses the call and writes nothing; so does a bad slot name.
    Answer bad = Call(server, u8"save_write", u8"{\"slot\":\"two\",\"clear\":true,\"values\":[{\"key\":\"errands\",\"kind\":\"int\",\"value\":\"three\"}]}");
    CHECK(bad.error);
    CHECK(bad.text.AsView().Contains(u8"Nothing was written"));
    CHECK(Call(server, u8"save_read", u8"{\"slot\":\"two\"}").value.Get(u8"values").Count() == 4);
    CHECK(Call(server, u8"save_write", u8"{\"slot\":\"no spaces\"}").error);

    // A run holding the saves (a Game tab playing, on the editor host): writes wait, reads do not.
    session.savesInUse = []() { return true; };
    CHECK(Call(server, u8"save_write", u8"{\"slot\":\"two\"}").text.AsView().Contains(u8"pie_stop"));
    CHECK(Call(server, u8"save_delete", u8"{\"all\":true}").error);
    CHECK_FALSE(Call(server, u8"save_read", u8"{\"slot\":\"two\"}").error);
    session.savesInUse = nullptr;

    // A slot, then every save.
    CHECK(Call(server, u8"save_delete", u8"{\"slot\":\"one\"}").value.Get(u8"deleted").AsBool());
    CHECK(Call(server, u8"save_list", u8"{}").value.Get(u8"slots").Count() == 1);
    CHECK(Call(server, u8"save_delete", u8"{\"all\":true}").value.Get(u8"deleted").AsBool());
    CHECK_FALSE(Call(server, u8"save_list", u8"{}").value.Get(u8"global").Get(u8"exists").AsBool());
    (void)RemoveDirectoryRecursive(u8"mcp_saves");
}
