// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: an agent authors a cutscene through sequence_edit and reads it back with
// sequence_read, whatever its track kinds: a part, a property track keyed on Transform, a clip
// section with its fields set by name, an event named through its reflected path, a hold. A
// refused edit refuses the whole call and nothing is written.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;

namespace
{
    // One tools/call: the result's object, and whether it was an error (its text then).
    struct Answer
    {
        bool error = false;
        JsonValue value;
        String text;
    };

    Answer Call(McpServer& server, StringView tool, const JsonValue& arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", arguments);
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

    JsonValue Edits(StringView text) { return JsonValue::Parse(String(text)); }
}

TEST_CASE("integration.mcp: an agent authors a cutscene with sequence_edit and reads it back")
{
    pipeline::RegisterPipelineTypes();
    engine::RegisterAllSceneComponentReflection(); // the domains' track kinds
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
    (void)RemoveDirectoryRecursive(u8"mcp_sequence");
    REQUIRE_FALSE(Call(server, u8"project_create", JsonValue::Parse(String(u8"{\"directory\":\"mcp_sequence\",\"name\":\"Cut\"}"))).error);
    REQUIRE_FALSE(Call(server, u8"project_open", JsonValue::Parse(String(u8"{\"directory\":\"mcp_sequence\"}"))).error);
    const Answer made = Call(server, u8"asset_create", JsonValue::Parse(String(u8"{\"type\":\"SequenceAsset\",\"name\":\"Arrival\"}")));
    REQUIRE_FALSE(made.error);
    const String guid = made.value.Get(u8"guid").AsString();
    u32 announced = 0;
    session.onAssetWritten = [&announced](const Guid&) { ++announced; };

    JsonValue args = JsonValue::MakeObject();
    args.Set(u8"guid", JsonValue::MakeString(guid));
    args.Set(u8"edits", Edits(u8"["
                              u8"{\"op\":\"add_part\",\"name\":\"Hero\"},"
                              u8"{\"op\":\"add_track\",\"kind\":\"Property\",\"part\":\"Hero\",\"target\":\"Transform\",\"property\":\"position\"},"
                              u8"{\"op\":\"key\",\"track\":0,\"channel\":\"x\",\"time\":0,\"value\":0},"
                              u8"{\"op\":\"key\",\"track\":0,\"channel\":\"x\",\"time\":2,\"value\":10},"
                              u8"{\"op\":\"add_track\",\"kind\":\"Clip\",\"part\":\"Hero\"},"
                              u8"{\"op\":\"section\",\"track\":1,\"start\":0.5,\"length\":1,\"fields\":{\"speed\":2,\"loop\":true}},"
                              u8"{\"op\":\"add_track\",\"kind\":\"EventSequenceTrack\"},"
                              u8"{\"op\":\"key\",\"track\":2,\"time\":1.5},"
                              u8"{\"op\":\"set\",\"track\":2,\"path\":\"keys.0.name\",\"value\":\"cue\"},"
                              u8"{\"op\":\"set\",\"track\":2,\"path\":\"keys.0.value\",\"value\":\"gate\"},"
                              u8"{\"op\":\"marker\",\"time\":1,\"name\":\"line\",\"hold\":true},"
                              // A scene's settings: a property track on no part.
                              u8"{\"op\":\"add_track\",\"kind\":\"Property\",\"source\":\"settings\",\"target\":\"environment\","
                              u8"\"property\":\"ambientIntensity\",\"valueKind\":\"Float\"}"
                              u8"]"));
    const Answer edited = Call(server, u8"sequence_edit", args);
    CAPTURE(reinterpret_cast<const char*>(edited.text.CStr()));
    REQUIRE_FALSE(edited.error);
    CHECK(announced == 1u);
    CHECK(edited.value.Get(u8"results").At(5).Get(u8"section").AsInt() == 0);

    JsonValue which = JsonValue::MakeObject();
    which.Set(u8"guid", JsonValue::MakeString(guid));
    const Answer read = Call(server, u8"sequence_read", which);
    REQUIRE_FALSE(read.error);
    const JsonValue& sequence = read.value;
    CHECK(sequence.Get(u8"parts").At(0).AsString() == StringView(u8"Hero"));
    CHECK(sequence.Get(u8"length").AsNumber() == doctest::Approx(2.0));
    REQUIRE(sequence.Get(u8"tracks").Count() == 4);
    const JsonValue walk = sequence.Get(u8"tracks").At(0);
    CHECK(walk.Get(u8"kindName").AsString() == StringView(u8"Property"));
    CHECK(walk.Get(u8"part").AsString() == StringView(u8"Hero"));
    CHECK(walk.Get(u8"source").AsString() == StringView(u8"component"));
    CHECK(walk.Get(u8"target").AsString() == StringView(u8"Transform"));
    CHECK(walk.Get(u8"channels").At(0).Get(u8"keys").At(1).Get(u8"value").AsNumber() == doctest::Approx(10.0));
    const JsonValue clip = sequence.Get(u8"tracks").At(1);
    CHECK(clip.Get(u8"kind").AsString() == StringView(u8"ClipSequenceTrack"));
    CHECK(clip.Get(u8"sections").At(0).Get(u8"start").AsNumber() == doctest::Approx(0.5));
    CHECK(clip.Get(u8"fields").Get(u8"sections").At(0).Get(u8"speed").AsNumber() == doctest::Approx(2.0));
    CHECK(clip.Get(u8"fields").Get(u8"sections").At(0).Get(u8"loop").AsBool());
    const JsonValue cues = sequence.Get(u8"tracks").At(2);
    CHECK(cues.Get(u8"part").IsNull());
    CHECK(cues.Get(u8"fields").Get(u8"keys").At(0).Get(u8"name").AsString() == StringView(u8"cue"));
    CHECK(sequence.Get(u8"markers").At(0).Get(u8"hold").AsBool());
    const JsonValue light = sequence.Get(u8"tracks").At(3);
    CHECK(light.Get(u8"part").IsNull());
    CHECK(light.Get(u8"source").AsString() == StringView(u8"settings"));
    CHECK(light.Get(u8"target").AsString() == StringView(u8"environment"));
    bool clipKind = false;
    for (i64 i = 0; i < sequence.Get(u8"kinds").Count(); ++i)
        clipKind = clipKind || sequence.Get(u8"kinds").At(i).Get(u8"kindName").AsString() == StringView(u8"Clip");
    CHECK(clipKind);

    // A refused edit (a field the kind does not have) refuses the call; the edits before it in
    // the same call are not written either.
    JsonValue bad = JsonValue::MakeObject();
    bad.Set(u8"guid", JsonValue::MakeString(guid));
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_part\",\"name\":\"Mentor\"},{\"op\":\"set\",\"track\":1,\"path\":\"sections.0.nothing\",\"value\":1}]"));
    const Answer refused = Call(server, u8"sequence_edit", bad);
    CHECK(refused.error);
    CHECK(refused.text.AsView().Contains(u8"Nothing was written"));
    CHECK(announced == 1u);
    const Answer again = Call(server, u8"sequence_read", which);
    CHECK(again.value.Get(u8"parts").Count() == 1);

    // A kind that plays on a part asks for one; an unknown kind names the known ones.
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_track\",\"kind\":\"Clip\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"give `part`"));
    // So does a property track of a component; a source nobody registered is named.
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_track\",\"kind\":\"Property\",\"target\":\"Transform\",\"property\":\"position\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"give `part`"));
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_track\",\"kind\":\"Property\",\"source\":\"weather\",\"target\":\"rain\","
                             u8"\"property\":\"amount\",\"valueKind\":\"Float\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"no target source 'weather'"));
    // A component named by its shown name is refused, pointed at its type name; one named by nothing too.
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_track\",\"kind\":\"Property\",\"part\":\"Hero\",\"target\":\"Light\","
                             u8"\"property\":\"intensity\",\"valueKind\":\"Float\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"'LightComponent'"));
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_track\",\"kind\":\"Property\",\"part\":\"Hero\",\"target\":\"Lamp\","
                             u8"\"property\":\"intensity\",\"valueKind\":\"Float\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"no component 'Lamp'"));
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"add_track\",\"kind\":\"Juggle\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"Camera Cut"));

    // A key's interpolation and tangent are set by name and read back; a name that is neither is refused.
    JsonValue eased = JsonValue::MakeObject();
    eased.Set(u8"guid", JsonValue::MakeString(guid));
    eased.Set(u8"edits", Edits(u8"[{\"op\":\"key\",\"track\":3,\"time\":0,\"value\":0.3,\"tangent\":\"Flat\"},"
                               u8"{\"op\":\"key\",\"track\":3,\"time\":2,\"value\":0.1,\"tangent\":\"Flat\",\"interpolation\":\"Constant\"}]"));
    const Answer flat = Call(server, u8"sequence_edit", eased);
    REQUIRE_FALSE(flat.error);
    const JsonValue keys = flat.value.Get(u8"tracks").At(3).Get(u8"channels").At(0).Get(u8"keys");
    CHECK(keys.At(0).Get(u8"tangent").AsString() == StringView(u8"Flat"));
    CHECK(keys.At(0).Get(u8"interpolation").AsString() == StringView(u8"Cubic"));
    CHECK(keys.At(1).Get(u8"interpolation").AsString() == StringView(u8"Constant"));
    bad.Set(u8"edits", Edits(u8"[{\"op\":\"key\",\"track\":3,\"time\":1,\"tangent\":\"Wobbly\"}]"));
    CHECK(Call(server, u8"sequence_edit", bad).text.AsView().Contains(u8"Auto, Smooth, Flat"));
    (void)RemoveDirectoryRecursive(u8"mcp_sequence");
}
