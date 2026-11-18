// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :log_tools partition
//
// The diagnostics tools: log_read, log_write and known_issues. The host registers one
// EditorLogBuffer sink on the global logger first thing in main, so every LOG_* line across the
// engine (cook warnings, scene-load errors, subsystem output) is captured with a monotonic
// sequence. log_read polls it incrementally (sinceSequence); log_write drops an agent marker into
// the same stream, to correlate agent actions with engine output; known_issues returns the
// engine's KnownIssues.md register so an agent can check whether a symptom is already known
// before diagnosing it again. All are read-only against the repo; log_write touches only the
// in-memory log.

module;
#include "Core/Prelude.h"

export module editor.mcp:log_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace editor::mcp::detail
{
    // Level name <-> LogLevel for the tool's enum args (subset an agent filters by).
    inline Array<String> LogLevelChoices()
    {
        Array<String> choices;
        choices.PushBack(String(u8"trace"));
        choices.PushBack(String(u8"debug"));
        choices.PushBack(String(u8"info"));
        choices.PushBack(String(u8"warning"));
        choices.PushBack(String(u8"error"));
        return choices;
    }

    inline LogLevel ParseLogLevel(StringView name, LogLevel fallback)
    {
        if (name == StringView(u8"trace"))
        {
            return LogLevel::Trace;
        }
        if (name == StringView(u8"debug"))
        {
            return LogLevel::Debug;
        }
        if (name == StringView(u8"info"))
        {
            return LogLevel::Info;
        }
        if (name == StringView(u8"warning"))
        {
            return LogLevel::Warning;
        }
        if (name == StringView(u8"error"))
        {
            return LogLevel::Error;
        }
        return fallback;
    }
}

export namespace editor::mcp
{
    // Registers log_read, log_write and known_issues. `logBuffer` is the host's sink (registered
    // on the global logger before anything else logs; it must outlive the server).
    // `knownIssuesPath` is the resolved absolute path of KnownIssues.md ("" when not found; the
    // tool then fails with guidance instead of being absent, so agents learn why). `session` gives
    // log_read the project whose files and assets a line's references name (null: only those the
    // log calls attached).
    inline void RegisterLogTools(foundation::mcp::McpServer& server,
                                 editor::EditorLogBuffer& logBuffer, String knownIssuesPath,
                                 const ProjectSession* session = nullptr)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        editor::EditorLogBuffer* buffer = &logBuffer;

        server.RegisterTool(
            u8"log_read",
            u8"Read the host's captured engine log (cook warnings, scene-load errors, subsystem "
            u8"output, agent markers). Poll incrementally: pass the lastSequence from the "
            u8"previous call as sinceSequence to get only what is new. Returns the newest "
            u8"`limit` matching entries; `dropped` > 0 means the ring overflowed and old "
            u8"entries were lost. An entry that names a file of the project (with its line and "
            u8"column), an asset, or another place has `references`: each a kind (source: file on "
            u8"disk, line, column; asset: guid), its fields, and the part of the message it stands for "
            u8"(start and length in bytes; length 0: the whole line).",
            SchemaBuilder()
                .Number(u8"sinceSequence",
                        u8"only entries with sequence > this (default 0 = everything buffered)")
                .Integer(u8"limit", u8"maximum entries to return, newest kept (default 200)")
                .Enum(u8"minLevel", detail::LogLevelChoices(),
                      u8"only entries at or above this level (default trace = all)")
                .Str(u8"category", u8"only entries with exactly this category (e.g. Cook, Scene)")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [buffer, session](const JsonValue& args) -> ToolResult
            {
                // The recognizers: the host's, else the project's files and assets.
                editor::LogReferenceRecognizers own;
                const editor::LogReferenceRecognizers* recognizers = session != nullptr ? session->logRecognizers : nullptr;
                if (recognizers == nullptr && session != nullptr && session->project != nullptr)
                {
                    editor::EditorProject* project = session->project;
                    (void)own.Add(editor::SourcePathRecognizer(PathAbsolute(project->Directory()).AsView()));
                    (void)own.Add(editor::AssetIdRecognizer([project](const Guid& id) { return project->SourceDb().GetInstance(id) != nullptr; }));
                    recognizers = &own;
                }
                const u64 since = static_cast<u64>(args.Get(u8"sinceSequence").AsNumber());
                const f64 limitRaw = args.Get(u8"limit").AsNumber();
                const usize limit = limitRaw > 0.0 ? static_cast<usize>(limitRaw) : usize(200);
                const LogLevel minLevel = detail::ParseLogLevel(
                    args.Get(u8"minLevel").AsString(), LogLevel::Trace);
                const String category = args.Get(u8"category").AsString();

                Array<editor::EditorLogEntry> collected;
                const u64 lastSequence = buffer->CollectSince(since, collected);

                Array<const editor::EditorLogEntry*> matching;
                for (const editor::EditorLogEntry& entry : collected)
                {
                    if (static_cast<u8>(entry.level) < static_cast<u8>(minLevel))
                    {
                        continue;
                    }
                    if (!category.IsEmpty() && entry.category != category.AsView())
                    {
                        continue;
                    }
                    matching.PushBack(&entry);
                }
                const usize start = matching.Size() > limit ? matching.Size() - limit : 0;

                JsonValue entries = JsonValue::MakeArray();
                for (usize i = start; i < matching.Size(); ++i)
                {
                    const editor::EditorLogEntry& entry = *matching[i];
                    JsonValue e = JsonValue::MakeObject();
                    e.Set(u8"sequence",
                          JsonValue::MakeNumber(static_cast<f64>(entry.sequence)));
                    e.Set(u8"level",
                          JsonValue::MakeString(String(StringView(LogLevelName(entry.level)))));
                    e.Set(u8"category", JsonValue::MakeString(entry.category));
                    e.Set(u8"message", JsonValue::MakeString(entry.message));
                    const Array<LogReference> references =
                        recognizers != nullptr ? recognizers->Of(entry.message.AsView(), entry.references.AsSpan()) : entry.references;
                    if (!references.IsEmpty())
                    {
                        JsonValue list = JsonValue::MakeArray();
                        for (const LogReference& reference : references)
                        {
                            JsonValue r = JsonValue::MakeObject();
                            r.Set(u8"kind", JsonValue::MakeString(reference.kind));
                            JsonValue fields = JsonValue::MakeArray();
                            for (usize f = 0; f < reference.fields.Size(); ++f)
                            {
                                // A script's file is its source path (Code/Player.luau): given on disk.
                                const bool sourcePath = f == 0 && reference.kind == kLogSourceReference && session != nullptr &&
                                                        session->project != nullptr && !PathIsAbsolute(reference.fields[f].AsView());
                                fields.Add(JsonValue::MakeString(sourcePath ? session->project->SourceFile(reference.fields[f].AsView())
                                                                            : reference.fields[f]));
                            }
                            r.Set(u8"fields", Move(fields));
                            r.Set(u8"start", JsonValue::MakeNumber(static_cast<f64>(reference.start)));
                            r.Set(u8"length", JsonValue::MakeNumber(static_cast<f64>(reference.length)));
                            list.Add(Move(r));
                        }
                        e.Set(u8"references", Move(list));
                    }
                    entries.Add(Move(e));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"entries", Move(entries));
                out.Set(u8"lastSequence",
                        JsonValue::MakeNumber(static_cast<f64>(lastSequence)));
                out.Set(u8"dropped",
                        JsonValue::MakeNumber(static_cast<f64>(buffer->DroppedCount())));
                return out;
            });

        server.RegisterTool(
            u8"log_write",
            u8"Write a marker line into the host's engine log (category 'Agent'). Use it to "
            u8"correlate your actions with engine output: drop a marker before a risky "
            u8"operation, then log_read from the returned sequence to see exactly what the "
            u8"engine said afterwards.",
            SchemaBuilder()
                .Str(u8"message", u8"the marker text", true)
                .Enum(u8"level", detail::LogLevelChoices(), u8"log level (default info)")
                .Build(),
                foundation::mcp::ToolAnnotations::Adjusts(),
            [buffer](const JsonValue& args) -> ToolResult
            {
                const String message = args.Get(u8"message").AsString();
                if (message.IsEmpty())
                {
                    return Err(String(u8"message must not be empty"));
                }
                const LogLevel level =
                    detail::ParseLogLevel(args.Get(u8"level").AsString(), LogLevel::Info);
                GlobalLogger().Dispatch(level, u8"Agent", message.AsView());

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"written", JsonValue::MakeBool(true));
                // The marker's sequence: the buffer's high-water right after the dispatch.
                out.Set(u8"sequence",
                        JsonValue::MakeNumber(static_cast<f64>(buffer->LatestSequence())));
                return out;
            });

        server.RegisterTool(
            u8"known_issues",
            u8"The engine's curated known-issues register (read-only): user-visible "
            u8"limitations with impact and workaround. Check it when you hit an error or odd "
            u8"behavior BEFORE re-diagnosing: if the symptom matches a recorded issue, report "
            u8"the match and apply its workaround instead of proposing a fix for something "
            u8"already known or deliberately deferred.",
            SchemaBuilder().Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [knownIssuesPath](const JsonValue& /*args*/) -> ToolResult
            {
                if (knownIssuesPath.IsEmpty())
                {
                    return Err(String(u8"KnownIssues.md was not found beside the host executable "
                                      u8"or in the engine checkout's Documentation/Shipping; run "
                                      u8"the MCP host from a distribution or from inside the checkout"));
                }
                FileStream stream(knownIssuesPath.AsView(), FileMode::Read);
                if (!stream.IsValid())
                {
                    return Err(Format(u8"could not read '{}'", knownIssuesPath.AsView()));
                }
                const i64 size = stream.Size();
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(size));
                if (stream.Read(bytes.Data(), bytes.Size()) != static_cast<u64>(size))
                {
                    return Err(Format(u8"could not read '{}'", knownIssuesPath.AsView()));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"path", JsonValue::MakeString(knownIssuesPath));
                out.Set(u8"text",
                        JsonValue::MakeString(String(
                            StringView(reinterpret_cast<const utf8char*>(bytes.Data()),
                                       bytes.Size()))));
                return out;
            });
    }
}
