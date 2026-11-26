// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Dialogue.Tooling's entry, the part the editor, the exporter and the MCP host load. It adds two
// MCP tools that every MCP host serves while the plugin is loaded, over the open project:
//   * dialogue_validate: the cook's checks over one dialogue file or every one, each problem with
//     its line, without cooking.
//   * dialogue_read: one dialogue file's conversations as the cook reads them: their lines with
//     ids, choices, commands, jumps and the flags each reads and writes.
// Writing stays the text's (asset_data_write, or the file itself), then dialogue_validate.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import editor.project;
import editor.mcp; // McpPluginServices
import dialogue.foundation;
import dialogue.pipeline;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace
{
    namespace content = foundation::content;

    void ForEachDialogueFile(content::Group& group, const Function<void(content::Instance&)>& visit)
    {
        for (content::Instance* instance : group.Instances())
        {
            if (instance->TypeName() == StringView(u8"DialogueAsset"))
            {
                visit(*instance);
            }
        }
        for (content::Group* child : group.Groups())
        {
            ForEachDialogueFile(*child, visit);
        }
    }

    [[nodiscard]] String GuidText(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return String(StringView(text, 36));
    }

    // The dialogue file `instance`'s text, checked as the cook checks it; false when its source
    // cannot be read (`error` says why).
    bool Check(editor::EditorProject& project, const dialogue::DialogueSettings& settings, content::Instance& instance,
               dialogue::DialogueParse& parse, String& error)
    {
        RefPtr<ISerializable> object = instance.ReadObject();
        const auto* file = Cast<dialogue::DialogueAsset>(object.Get());
        if (file == nullptr)
        {
            error = String(u8"not a dialogue file");
            return false;
        }
        const dialogue::DialogueSourceReader read = [&project](StringView fileName, String& out)
        {
            Result<Array<byte>> bytes = ReadFile(project.SourceFile(fileName).AsView());
            if (!bytes.HasValue())
            {
                return false;
            }
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            return true;
        };
        String text;
        if (!read(file->fileName.View(), text))
        {
            error = Format(u8"its source {} is missing", file->fileName.View());
            return false;
        }
        parse = dialogue::CheckDialogueText(text.AsView(), settings, instance.Id(), project.SourceDb(), read,
                                            project.Allocator());
        return true;
    }

    [[nodiscard]] JsonValue Problems(const dialogue::DialogueParse& parse, usize& errors)
    {
        JsonValue problems = JsonValue::MakeArray();
        for (const dialogue::DialogueProblem& problem : parse.problems)
        {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set(u8"line", JsonValue::MakeNumber(static_cast<f64>(problem.line)));
            entry.Set(u8"severity", JsonValue::MakeString(String(problem.warning ? u8"warning" : u8"error")));
            entry.Set(u8"message", JsonValue::MakeString(problem.message));
            problems.Add(Move(entry));
            errors += problem.warning ? 0u : 1u;
        }
        return problems;
    }

    [[nodiscard]] JsonValue Strings(const Array<String>& values)
    {
        JsonValue out = JsonValue::MakeArray();
        for (const String& value : values)
        {
            out.Add(JsonValue::MakeString(value));
        }
        return out;
    }

    void AddOnce(Array<String>& list, const String& value)
    {
        for (const String& seen : list)
        {
            if (seen == value)
            {
                return;
            }
        }
        list.PushBack(value);
    }

    // A conversation as dialogue_read gives it.
    [[nodiscard]] JsonValue Conversation(const dialogue::DialogueParse& parse, const dialogue::DialogueConversation& conversation)
    {
        const auto textOf = [&parse](StringView id) -> String
        {
            for (const dialogue::DialogueLineText& line : parse.lines)
            {
                if (line.id == id)
                {
                    return line.text;
                }
            }
            return {};
        };
        JsonValue lines = JsonValue::MakeArray();
        JsonValue choices = JsonValue::MakeArray();
        JsonValue commands = JsonValue::MakeArray();
        JsonValue jumps = JsonValue::MakeArray();
        Array<String> reads;
        Array<String> writes;
        const Array<dialogue::DialogueStep>& steps = conversation.steps;
        for (usize i = 0; i < steps.Size(); ++i)
        {
            const dialogue::DialogueStep& step = steps[i];
            switch (step.kind)
            {
            case dialogue::DialogueStepKind::Line:
            {
                JsonValue line = JsonValue::MakeObject();
                line.Set(u8"id", JsonValue::MakeString(step.lineId));
                line.Set(u8"line", JsonValue::MakeNumber(static_cast<f64>(step.sourceLine)));
                line.Set(u8"speaker", JsonValue::MakeString(step.speaker));
                if (!step.mood.IsEmpty())
                {
                    line.Set(u8"mood", JsonValue::MakeString(step.mood));
                }
                line.Set(u8"text", JsonValue::MakeString(textOf(step.lineId.AsView())));
                lines.Add(Move(line));
                break;
            }
            case dialogue::DialogueStepKind::Choice:
            {
                JsonValue answers = JsonValue::MakeArray();
                for (u32 a = 1; a <= step.count && i + a < steps.Size(); ++a)
                {
                    JsonValue answer = JsonValue::MakeObject();
                    answer.Set(u8"id", JsonValue::MakeString(steps[i + a].lineId));
                    answer.Set(u8"text", JsonValue::MakeString(textOf(steps[i + a].lineId.AsView())));
                    answers.Add(Move(answer));
                }
                JsonValue choice = JsonValue::MakeObject();
                choice.Set(u8"line", JsonValue::MakeNumber(static_cast<f64>(step.sourceLine)));
                choice.Set(u8"answers", Move(answers));
                choices.Add(Move(choice));
                break;
            }
            case dialogue::DialogueStepKind::Command:
            {
                JsonValue arguments = JsonValue::MakeObject();
                for (const dialogue::DialogueArgument& argument : step.arguments)
                {
                    if (argument.present)
                    {
                        arguments.Set(argument.name.AsView(), JsonValue::MakeString(argument.text));
                    }
                }
                JsonValue command = JsonValue::MakeObject();
                command.Set(u8"name", JsonValue::MakeString(step.name));
                command.Set(u8"line", JsonValue::MakeNumber(static_cast<f64>(step.sourceLine)));
                command.Set(u8"arguments", Move(arguments));
                if (step.waits)
                {
                    command.Set(u8"waits", JsonValue::MakeBool(true));
                }
                if (step.leaves)
                {
                    command.Set(u8"leaves", JsonValue::MakeBool(true));
                }
                commands.Add(Move(command));
                break;
            }
            case dialogue::DialogueStepKind::Jump:
                jumps.Add(JsonValue::MakeString(step.file.IsEmpty() ? step.name : Format(u8"{} in {}", step.name, step.file)));
                break;
            case dialogue::DialogueStepKind::If: AddOnce(reads, step.flag); break;
            case dialogue::DialogueStepKind::Set:
            case dialogue::DialogueStepKind::Clear: AddOnce(writes, step.flag); break;
            default: break;
            }
        }
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"id", JsonValue::MakeString(conversation.id));
        if (!conversation.level.IsEmpty())
        {
            out.Set(u8"level", JsonValue::MakeString(conversation.level));
        }
        out.Set(u8"lines", Move(lines));
        out.Set(u8"choices", Move(choices));
        out.Set(u8"commands", Move(commands));
        out.Set(u8"jumps", Move(jumps));
        out.Set(u8"flagsRead", Strings(reads));
        out.Set(u8"flagsWritten", Strings(writes));
        return out;
    }

    // The dialogue file an argument names (its guid), or null with `error` saying why.
    content::Instance* FileArgument(editor::EditorProject& project, const JsonValue& args, String& error)
    {
        const String text = args.Get(u8"asset").AsString();
        Guid id;
        if (!Guid::TryParse(text.AsView(), id))
        {
            error = String(u8"`asset` is a dialogue file's guid");
            return nullptr;
        }
        content::Instance* instance = project.SourceDb().GetInstance(id);
        if (instance == nullptr || instance->TypeName() != StringView(u8"DialogueAsset"))
        {
            error = Format(u8"no dialogue file {}", text);
            return nullptr;
        }
        return instance;
    }

    void Load(foundation::runtime::PluginContext& context)
    {
        editor::mcp::McpPluginServices* tools = context.Find<editor::mcp::McpPluginServices>();
        const dialogue::DialogueSettings* settings = context.Settings<dialogue::DialogueSettings>();
        if (tools == nullptr || settings == nullptr)
        {
            return;
        }
        (void)tools->AddTools(
            [settings](editor::mcp::PluginTools& registry)
            {
                editor::mcp::ProjectSession* session = &registry.Session();
                (void)registry.RegisterTool(
                    String(u8"dialogue_validate"),
                    String(u8"The cook's checks over a dialogue file (`asset`, its guid) or, with no `asset`, every one in "
                           u8"the open project, without cooking: each file's problems with their lines (errors stop its "
                           u8"cook; warnings do not). Run it after writing a dialogue file's text."),
                    foundation::mcp::SchemaBuilder().Str(u8"asset", u8"A dialogue file's guid; none checks every one").Build(),
                    foundation::mcp::ToolAnnotations::ReadOnly(),
                    [session, settings](const JsonValue& args) -> foundation::mcp::ToolResult
                    {
                        if (session->project == nullptr)
                        {
                            return Err(String(u8"no project is open (call project_open first)"));
                        }
                        editor::EditorProject& project = *session->project;
                        Array<content::Instance*> files;
                        if (!args.Get(u8"asset").AsString().IsEmpty())
                        {
                            String error;
                            content::Instance* one = FileArgument(project, args, error);
                            if (one == nullptr)
                            {
                                return Err(Move(error));
                            }
                            files.PushBack(one);
                        }
                        else
                        {
                            ForEachDialogueFile(*project.SourceDb().RootGroup(),
                                                [&files](content::Instance& instance) { files.PushBack(&instance); });
                        }
                        JsonValue results = JsonValue::MakeArray();
                        usize errors = 0;
                        for (content::Instance* instance : files)
                        {
                            JsonValue result = JsonValue::MakeObject();
                            result.Set(u8"asset", JsonValue::MakeString(GuidText(instance->Id())));
                            result.Set(u8"name", JsonValue::MakeString(String(instance->Name())));
                            dialogue::DialogueParse parse;
                            String error;
                            if (Check(project, *settings, *instance, parse, error))
                            {
                                usize fileErrors = 0;
                                result.Set(u8"problems", Problems(parse, fileErrors));
                                result.Set(u8"ok", JsonValue::MakeBool(fileErrors == 0));
                                errors += fileErrors;
                            }
                            else
                            {
                                result.Set(u8"ok", JsonValue::MakeBool(false));
                                result.Set(u8"error", JsonValue::MakeString(error));
                                ++errors;
                            }
                            results.Add(Move(result));
                        }
                        JsonValue out = JsonValue::MakeObject();
                        out.Set(u8"files", Move(results));
                        out.Set(u8"errors", JsonValue::MakeNumber(static_cast<f64>(errors)));
                        return out;
                    });
                (void)registry.RegisterTool(
                    String(u8"dialogue_read"),
                    String(u8"A dialogue file's conversations as the cook reads them (`asset`, its guid): each "
                           u8"conversation's lines with their ids, speakers and texts, its choices and their answers, "
                           u8"its commands with their arguments, its jumps, and the flags it reads and writes; and the "
                           u8"file's problems."),
                    foundation::mcp::SchemaBuilder().Str(u8"asset", u8"A dialogue file's guid", true).Build(),
                    foundation::mcp::ToolAnnotations::ReadOnly(),
                    [session, settings](const JsonValue& args) -> foundation::mcp::ToolResult
                    {
                        if (session->project == nullptr)
                        {
                            return Err(String(u8"no project is open (call project_open first)"));
                        }
                        String error;
                        content::Instance* instance = FileArgument(*session->project, args, error);
                        dialogue::DialogueParse parse;
                        if (instance == nullptr || !Check(*session->project, *settings, *instance, parse, error))
                        {
                            return Err(Move(error));
                        }
                        JsonValue conversations = JsonValue::MakeArray();
                        for (const dialogue::DialogueConversation& conversation : parse.conversations)
                        {
                            conversations.Add(Conversation(parse, conversation));
                        }
                        usize errors = 0;
                        JsonValue out = JsonValue::MakeObject();
                        out.Set(u8"asset", JsonValue::MakeString(GuidText(instance->Id())));
                        out.Set(u8"name", JsonValue::MakeString(String(instance->Name())));
                        out.Set(u8"conversations", Move(conversations));
                        out.Set(u8"problems", Problems(parse, errors));
                        return out;
                    });
            });
    }
}

PLUGIN_PART(&Load, nullptr)
