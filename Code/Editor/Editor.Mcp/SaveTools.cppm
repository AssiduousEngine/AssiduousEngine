// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :save_tools partition.
//
// save_list, save_read, save_write and save_delete: play in editor's saves for the open project (its
// Editor/<project>.saves folder: the global store and the slots), so a test save (a story set just
// before a beat) is written by a tool, not by hand. A store's values are typed, each `{key, kind,
// value}` with kind bool, int, float, text or floats. The writes are refused on the editor host while
// a Game tab plays, since its run would write over them as it stops; the headless host has no Game
// tab. A player's saves (in the user data directory) are never touched.

module;
#include "Core/Prelude.h"

export module editor.mcp:save_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.settings;
import editor.project;
import engine.project; // SavesFolder
import engine.run;     // RunSaves, SaveFile: the stores as the game reads them
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

export namespace editor::mcp::detail
{
    inline constexpr StringView kSaveKindNames[] = {u8"bool", u8"int", u8"float", u8"text", u8"floats"};

    /// Play in editor's saves folder for the open project.
    [[nodiscard]] inline String PlaySavesFolder(const editor::EditorProject& project)
    {
        return PathJoin(project.EditorStateRoot().AsView(),
                        engine::project::SavesFolder(project.Settings().name.AsView(), editor::EditorRootAllocator()).AsView());
    }

    /// The file of the store a call names: `slot` (a slot name) or none for the global store. The
    /// reason when the name is not a slot name.
    [[nodiscard]] inline Result<String, String> StoreFile(StringView folder, const JsonValue& args)
    {
        const JsonValue slot = args.Get(u8"slot");
        if (slot.IsNull() || (slot.IsString() && slot.AsString().IsEmpty()))
        {
            return engine::runtime::RunSaves::GlobalFile(folder, editor::EditorRootAllocator());
        }
        const String name = slot.AsString();
        if (!engine::runtime::RunSaves::IsSlotName(name.AsView()))
        {
            return Err(Format(u8"`slot` '{}' is not a slot name (1 to 32 letters, digits, '-' and '_')", name.AsView()));
        }
        return engine::runtime::RunSaves::SlotFile(folder, name.AsView(), editor::EditorRootAllocator());
    }

    [[nodiscard]] inline JsonValue SaveValuesJson(const foundation::settings::SaveValues& values)
    {
        using foundation::settings::SaveValueKind;
        JsonValue out = JsonValue::MakeArray();
        for (usize i = 0; i < values.Count(); ++i)
        {
            const foundation::settings::SaveValues::Entry& entry = values.At(i);
            JsonValue v = JsonValue::MakeObject();
            v.Set(u8"key", JsonValue::MakeString(entry.key));
            v.Set(u8"kind", JsonValue::MakeString(String(kSaveKindNames[static_cast<usize>(entry.kind)])));
            switch (entry.kind)
            {
            case SaveValueKind::Bool: v.Set(u8"value", JsonValue::MakeBool(entry.boolValue)); break;
            case SaveValueKind::Int: v.Set(u8"value", JsonValue::MakeNumber(static_cast<f64>(entry.intValue))); break;
            case SaveValueKind::Float: v.Set(u8"value", JsonValue::MakeNumber(static_cast<f64>(entry.floatValue))); break;
            case SaveValueKind::Text: v.Set(u8"value", JsonValue::MakeString(entry.textValue)); break;
            case SaveValueKind::Floats:
            {
                JsonValue list = JsonValue::MakeArray();
                for (const f32 f : entry.floatsValue)
                    list.Add(JsonValue::MakeNumber(static_cast<f64>(f)));
                v.Set(u8"value", Move(list));
                break;
            }
            }
            out.Add(Move(v));
        }
        return out;
    }

    /// Sets one `{key, kind, value}` in `values`; the reason when it is not one.
    [[nodiscard]] inline Result<bool, String> SetSaveValue(foundation::settings::SaveValues& values, const JsonValue& entry)
    {
        const String key = entry.Get(u8"key").AsString();
        const String kind = entry.Get(u8"kind").AsString();
        const JsonValue value = entry.Get(u8"value");
        if (key.IsEmpty())
            return Err(String(u8"each of `values` needs a `key`"));
        if (kind == StringView(u8"bool") && value.IsBool())
            return values.SetBool(key.AsView(), value.AsBool());
        if (kind == StringView(u8"int") && value.IsNumber())
            return values.SetInt(key.AsView(), static_cast<i32>(value.AsInt()));
        if (kind == StringView(u8"float") && value.IsNumber())
            return values.SetFloat(key.AsView(), static_cast<f32>(value.AsNumber()));
        if (kind == StringView(u8"text") && value.IsString())
            return values.SetText(key.AsView(), value.AsString().AsView());
        if (kind == StringView(u8"floats") && value.IsArray())
        {
            Array<f32> list;
            for (i64 i = 0; i < value.Count(); ++i)
                list.PushBack(static_cast<f32>(value.At(i).AsNumber()));
            return values.SetFloats(key.AsView(), Span<const f32>(list.Data(), list.Size()));
        }
        return Err(Format(u8"'{}': `kind` is bool, int, float, text or floats, with a `value` of that kind", key.AsView()));
    }

    /// The reason a write is refused now, or empty: no project, or a Game tab playing.
    [[nodiscard]] inline String SaveWriteRefusal(const ProjectSession& session)
    {
        if (session.project == nullptr)
            return String(u8"no project is open");
        if (session.savesInUse && session.savesInUse())
            return String(u8"a Game tab is playing, and its run writes the saves as it stops: pie_stop first");
        return String();
    }
}

export namespace editor::mcp
{
    // The number of tools RegisterSaveTools registers (part of kEngineToolCount).
    inline constexpr usize kSaveToolCount = 4;

    inline void RegisterSaveTools(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"save_list",
            u8"Play in editor's saves for the open project (its Editor/<project>.saves folder; a player's "
            u8"are never touched): the global store (`global`: whether it exists, `savedAt` in Unix "
            u8"seconds) and each slot (`slots`: `name`, `savedAt`, 0 for never saved), the slot saved last "
            u8"first, as Saves.slots() orders them. save_read shows a store's values.",
            foundation::mcp::SchemaBuilder().Build(), foundation::mcp::ToolAnnotations::ReadOnly(),
            [s](const JsonValue&) -> ToolResult
            {
                if (s->project == nullptr)
                    return Err(String(u8"no project is open"));
                const String folder = detail::PlaySavesFolder(*s->project);
                engine::runtime::RunSaves saves(editor::EditorRootAllocator());
                saves.Open(folder.AsView()); // reads only: nothing is written unless flushed
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"folder", JsonValue::MakeString(folder));
                JsonValue global = JsonValue::MakeObject();
                global.Set(u8"exists", JsonValue::MakeBool(FileExists(engine::runtime::RunSaves::GlobalFile(folder.AsView(), editor::EditorRootAllocator()).AsView())));
                global.Set(u8"savedAt", JsonValue::MakeNumber(static_cast<f64>(saves.Global().SavedAt())));
                out.Set(u8"global", Move(global));
                JsonValue slots = JsonValue::MakeArray();
                for (const String& name : saves.Slots())
                {
                    JsonValue slot = JsonValue::MakeObject();
                    slot.Set(u8"name", JsonValue::MakeString(name));
                    slot.Set(u8"savedAt", JsonValue::MakeNumber(static_cast<f64>(saves.SlotTime(name.AsView()))));
                    slots.Add(Move(slot));
                }
                out.Set(u8"slots", Move(slots));
                return out;
            });

        server.RegisterTool(
            u8"save_read",
            u8"One of play in editor's save stores: `slot` (a slot's name) or none for the global store. "
            u8"Returns `exists`, `savedAt` (Unix seconds, 0 for never saved) and `values`, each `{key, "
            u8"kind, value}` (kind bool, int, float, text or floats) as save_write takes them. The "
            u8"dialogue plugin's flags are `flag.<name>` bools in the store the game gives it.",
            foundation::mcp::SchemaBuilder().Str(u8"slot", u8"a slot's name; none: the global store").Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                    return Err(String(u8"no project is open"));
                Result<String, String> file = detail::StoreFile(detail::PlaySavesFolder(*s->project).AsView(), args);
                if (!file.HasValue())
                    return Err(Move(file.Error()));
                engine::runtime::SaveFile store(editor::EditorRootAllocator());
                const bool exists = FileExists(file.Value().AsView());
                if (exists && !store.Read(file.Value().AsView()))
                    return Err(Format(u8"could not read '{}'", file.Value().AsView()));
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"exists", JsonValue::MakeBool(exists));
                out.Set(u8"savedAt", JsonValue::MakeNumber(static_cast<f64>(store.SavedAt())));
                out.Set(u8"values", detail::SaveValuesJson(store.Values()));
                return out;
            });

        server.RegisterTool(
            u8"save_write",
            u8"Writes one of play in editor's save stores: `slot` (made when there is none) or none for "
            u8"the global store. `clear` (true) empties its values first; `remove` lists keys to take out; "
            u8"`values` sets each `{key, kind, value}` (kind bool, int, float, text or floats). Checked in "
            u8"full before anything is written, then saved with the current time, so the slot becomes the "
            u8"one saved last (Continue's). The player's bindings in the global store are kept. Refused "
            u8"while a Game tab plays: its run would write over it as it stops. The next play reads it.",
            foundation::mcp::SchemaBuilder()
                .Str(u8"slot", u8"a slot's name; none: the global store")
                .Boolean(u8"clear", u8"empty the store's values first")
                .Arr(u8"remove", u8"string", u8"keys to take out")
                .Arr(u8"values", u8"object", u8"{key, kind, value} to set")
                .Build(),
            foundation::mcp::ToolAnnotations::Overwrites(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (const String refused = detail::SaveWriteRefusal(*s); !refused.IsEmpty())
                    return Err(refused);
                Result<String, String> file = detail::StoreFile(detail::PlaySavesFolder(*s->project).AsView(), args);
                if (!file.HasValue())
                    return Err(Move(file.Error()));
                engine::runtime::SaveFile store(editor::EditorRootAllocator());
                if (!store.Open(file.Value().AsView()))
                    return Err(Format(u8"could not read '{}', nor move it aside", file.Value().AsView()));
                foundation::settings::SaveValues& values = store.Values();
                if (args.Get(u8"clear").AsBool())
                    (void)values.Clear();
                const JsonValue remove = args.Get(u8"remove");
                for (i64 i = 0; i < remove.Count(); ++i)
                    (void)values.Remove(remove.At(i).AsString().AsView());
                const JsonValue set = args.Get(u8"values");
                for (i64 i = 0; i < set.Count(); ++i)
                {
                    Result<bool, String> applied = detail::SetSaveValue(values, set.At(i));
                    if (!applied.HasValue())
                        return Err(Format(u8"{}. Nothing was written.", applied.Error().AsView()));
                }
                store.MarkChanged(); // saved now, so it is the store saved last
                if (!store.Flush(GetUnixTime()))
                    return Err(Format(u8"could not write '{}'", file.Value().AsView()));
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"file", JsonValue::MakeString(file.Value()));
                out.Set(u8"savedAt", JsonValue::MakeNumber(static_cast<f64>(store.SavedAt())));
                out.Set(u8"values", detail::SaveValuesJson(store.Values()));
                return out;
            });

        server.RegisterTool(
            u8"save_delete",
            u8"Deletes one of play in editor's saves: `slot` (a slot's name), or `all` (true) for the whole "
            u8"saves folder (the global store, with the player's bindings, and every slot): a new game. "
            u8"Refused while a Game tab plays. Returns whether there was anything to delete.",
            foundation::mcp::SchemaBuilder()
                .Str(u8"slot", u8"the slot to delete")
                .Boolean(u8"all", u8"delete the whole saves folder")
                .Build(),
            foundation::mcp::ToolAnnotations::Deletes(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (const String refused = detail::SaveWriteRefusal(*s); !refused.IsEmpty())
                    return Err(refused);
                const String folder = detail::PlaySavesFolder(*s->project);
                JsonValue out = JsonValue::MakeObject();
                if (args.Get(u8"all").AsBool())
                {
                    const bool had = DirectoryExists(folder.AsView());
                    if (had && !RemoveDirectoryRecursive(folder.AsView()))
                        return Err(Format(u8"could not delete '{}'", folder.AsView()));
                    out.Set(u8"deleted", JsonValue::MakeBool(had));
                    return out;
                }
                if (args.Get(u8"slot").IsNull())
                    return Err(String(u8"give `slot` (a slot's name), or `all` for the whole saves folder"));
                Result<String, String> file = detail::StoreFile(folder.AsView(), args);
                if (!file.HasValue())
                    return Err(Move(file.Error()));
                out.Set(u8"deleted", JsonValue::MakeBool(FileDelete(file.Value().AsView())));
                return out;
            });
    }
}
