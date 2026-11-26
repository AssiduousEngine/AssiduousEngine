// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :reflected_fields partition.
//
// A record's fields over MCP, read from its reflection rather than spelled out per tool: the
// project's settings (project_info / project_settings_set) and an export preset
// (export_presets / export_preset_set) are both labelled properties. The kinds a field may be:
// text, a list of texts, an asset's guid or a list of them (the `assetType` attribute names the
// type: an asset type, or what assets cook to, which takes every asset type that cooks to it), a count (within its `range`), a flag, a choice (an enum, by its values' names). The
// answer, the arguments' schema, the check of given values and their write are each one function
// here, so the tools differ only in their own rules (MSAA's levels, a preset's platform).

module;
#include "Core/Prelude.h"

export module editor.mcp:reflected_fields;

import foundation.core;
import foundation.settings; // the settings attributes (label, assetType, ...)
import foundation.json;
import foundation.content;
import foundation.mcp;
import pipeline.core;
import engine.project;
import editor.project;
import :session;
import :scene_reference;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

export namespace editor::mcp::detail
{
    inline StringView PropertyName(const PropertyInfo& property)
    {
        return StringView(reinterpret_cast<const utf8char*>(property.name));
    }

    /// A field holding a list of texts.
    [[nodiscard]] inline bool IsTextListField(const PropertyInfo& property) noexcept
    {
        return property.type == &TypeOf<Array<String>>();
    }

    /// A field holding a colour: a Float4 the inspector shows as one (its `color` attribute).
    [[nodiscard]] inline bool IsColorField(const PropertyInfo& property) noexcept
    {
        return property.type == &TypeOf<Float4>() && FindAttribute(property, u8"color") != nullptr;
    }

    /// A number field's bounds: its reflected `range` (min, max); false when it has none.
    inline bool NumberRange(const PropertyInfo& property, f32& least, f32& most)
    {
        if (const Attribute* range = FindAttribute(property, u8"range"))
        {
            if (const Float4* bounds = range->value.TryGet<Float4>())
            {
                least = bounds->x;
                most = bounds->y;
                return true;
            }
        }
        return false;
    }

    /// The fields of `type` these tools read and set: the labelled properties of a kind above.
    inline Array<const PropertyInfo*> ReflectedFieldsOf(const TypeInfo& type)
    {
        Array<const PropertyInfo*> out;
        for (const PropertyInfo& property : Properties(type))
        {
            const bool kind = property.type == &TypeOf<String>() || IsTextListField(property) ||
                              property.type == &TypeOf<u32>() || property.type == &TypeOf<f32>() ||
                              IsColorField(property) || property.type == &TypeOf<bool>() ||
                              IsEnum(*property.type) || engine::project::IsAssetSetting(property) ||
                              engine::project::IsAssetListSetting(property);
            if (kind && engine::project::SettingAttribute(property, foundation::settings::kSettingLabelAttribute) !=
                            nullptr)
            {
                out.PushBack(&property);
            }
        }
        return out;
    }

    /// A count field's bounds: its reflected `range` (min, max), else every u32.
    inline void CountRange(const PropertyInfo& property, u32& least, u32& most)
    {
        least = 0;
        most = 0xFFFFFFFFu;
        if (const Attribute* range = FindAttribute(property, u8"range"))
        {
            if (const Float4* bounds = range->value.TryGet<Float4>())
            {
                least = static_cast<u32>(bounds->x);
                most = static_cast<u32>(bounds->y);
            }
        }
    }

    /// A choice field's values by name, comma separated (what a refusal lists).
    inline String EnumNames(const TypeInfo& type)
    {
        String names;
        for (const EnumValue& value : Enumerators(type))
        {
            names += names.IsEmpty() ? u8"" : u8", ";
            names += StringView(reinterpret_cast<const utf8char*>(value.name));
        }
        return names;
    }

    /// An asset a field names as {guid, path}; the path is null when the guid names nothing (or
    /// there is no project to look in).
    inline JsonValue FieldAssetJson(editor::EditorProject* project, const Guid& id)
    {
        content::Instance* asset = project != nullptr ? project->SourceDb().GetInstance(id) : nullptr;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set(u8"guid", GuidToJson(id));
        entry.Set(u8"path", asset != nullptr ? JsonValue::MakeString(asset->Path()) : JsonValue::MakeNull());
        return entry;
    }

    /// The fields' values: an asset {guid, path} (null when unset), a list as an array, a text as
    /// text, a flag as a bool, a choice by its value's name, a count or a number as a number, a
    /// colour as [r, g, b, a].
    inline JsonValue FieldsJson(const Instance& record, Span<const PropertyInfo* const> fields,
                                editor::EditorProject* project)
    {
        JsonValue out = JsonValue::MakeObject();
        for (const PropertyInfo* field : fields)
        {
            const String key(PropertyName(*field));
            const void* address = field->address(record);
            if (engine::project::IsAssetListSetting(*field))
            {
                JsonValue list = JsonValue::MakeArray();
                for (const Guid& id : *static_cast<const Array<Guid>*>(address))
                {
                    list.Add(FieldAssetJson(project, id));
                }
                out.Set(key, Move(list));
            }
            else if (engine::project::IsAssetSetting(*field))
            {
                const Guid& id = *static_cast<const Guid*>(address);
                out.Set(key, id.IsNil() ? JsonValue::MakeNull() : FieldAssetJson(project, id));
            }
            else if (IsTextListField(*field))
            {
                JsonValue list = JsonValue::MakeArray();
                for (const String& text : *static_cast<const Array<String>*>(address))
                {
                    list.Add(JsonValue::MakeString(text));
                }
                out.Set(key, Move(list));
            }
            else if (field->type == &TypeOf<String>())
            {
                out.Set(key, JsonValue::MakeString(*static_cast<const String*>(address)));
            }
            else if (field->type == &TypeOf<bool>())
            {
                out.Set(key, JsonValue::MakeBool(*static_cast<const bool*>(address)));
            }
            else if (field->type == &TypeOf<f32>())
            {
                out.Set(key, JsonValue::MakeNumber(static_cast<f64>(*static_cast<const f32*>(address))));
            }
            else if (IsColorField(*field))
            {
                const Float4& color = *static_cast<const Float4*>(address);
                JsonValue channels = JsonValue::MakeArray();
                const f32 values[] = {color.x, color.y, color.z, color.w};
                for (const f32 channel : values)
                {
                    channels.Add(JsonValue::MakeNumber(static_cast<f64>(channel)));
                }
                out.Set(key, Move(channels));
            }
            else if (IsEnum(*field->type))
            {
                const char* name = EnumValueName(*field->type, ReadEnumValue(address, *field->type));
                out.Set(key, name != nullptr
                                 ? JsonValue::MakeString(String(StringView(reinterpret_cast<const utf8char*>(name))))
                                 : JsonValue::MakeNull());
            }
            else
            {
                out.Set(key, JsonValue::MakeNumber(static_cast<f64>(*static_cast<const u32*>(address))));
            }
        }
        return out;
    }

    /// One argument per field, as reflection describes it.
    /// The asset types a field's `assetType` takes: when it names what assets cook to (the run
    /// behavior's BehaviorClass), every type the builders make it from (a script's, a native
    /// behavior's), as the editor's reference picker reads it; otherwise the type it names.
    inline Array<String> AcceptedAssetTypes(const pipeline::BuilderRegistry& builders, StringView named)
    {
        Array<String> names;
        for (const TypeInfo* asset : SourceAssetTypesForNamed(builders, named))
        {
            names.PushBack(String(StringView(reinterpret_cast<const utf8char*>(asset->name))));
        }
        if (names.IsEmpty())
        {
            names.PushBack(String(named));
        }
        return names;
    }

    /// `names` for a sentence: "A", "A or B", "A, B or C".
    inline String AssetTypesText(const Array<String>& names)
    {
        String text;
        for (usize i = 0; i < names.Size(); ++i)
        {
            if (i > 0)
            {
                text += StringView(i + 1 == names.Size() ? u8" or " : u8", ");
            }
            text += names[i].AsView();
        }
        return text;
    }

    inline void AddFieldsToSchema(foundation::mcp::SchemaBuilder& schema, Span<const PropertyInfo* const> fields,
                                  const pipeline::BuilderRegistry& builders)
    {
        for (const PropertyInfo* field : fields)
        {
            const String* label = engine::project::SettingAttribute(*field, foundation::settings::kSettingLabelAttribute);
            const String key(PropertyName(*field));
            const String* assetType =
                engine::project::SettingAttribute(*field, foundation::settings::kSettingAssetTypeAttribute);
            const String accepted = assetType != nullptr ? AssetTypesText(AcceptedAssetTypes(builders, assetType->AsView()))
                                                         : String();
            if (engine::project::IsAssetListSetting(*field))
            {
                schema.Arr(key, u8"string",
                           Format(u8"{}: the guids of assets of type {}, in order; the whole list, [] for none",
                                  label->AsView(), accepted.AsView()));
            }
            else if (assetType != nullptr)
            {
                const String* emptyText =
                    engine::project::SettingAttribute(*field, foundation::settings::kSettingEmptyTextAttribute);
                schema.Str(key, Format(u8"{}: the guid of an asset of type {}; \"\" clears it ({})", label->AsView(),
                                       accepted.AsView(), emptyText != nullptr ? emptyText->AsView() : StringView()));
            }
            else if (IsTextListField(*field))
            {
                schema.Arr(key, u8"string", Format(u8"{}: the whole list, [] for none", label->AsView()));
            }
            else if (field->type == &TypeOf<String>())
            {
                schema.Str(key, *label);
            }
            else if (field->type == &TypeOf<bool>())
            {
                schema.Boolean(key, *label);
            }
            else if (field->type == &TypeOf<f32>())
            {
                f32 least = 0.0f;
                f32 most = 0.0f;
                schema.Number(key, NumberRange(*field, least, most) ? Format(u8"{}: {} to {}", label->AsView(), least, most)
                                                                    : *label);
            }
            else if (IsColorField(*field))
            {
                schema.Arr(key, u8"number", Format(u8"{}: [r, g, b] or [r, g, b, a], each 0 to 1", label->AsView()));
            }
            else if (IsEnum(*field->type))
            {
                Array<String> names;
                for (const EnumValue& value : Enumerators(*field->type))
                {
                    names.PushBack(String(StringView(reinterpret_cast<const utf8char*>(value.name))));
                }
                schema.Enum(key, Move(names), *label);
            }
            else
            {
                u32 least = 0;
                u32 most = 0;
                CountRange(*field, least, most);
                schema.Integer(key, most == 0xFFFFFFFFu ? *label
                                                        : Format(u8"{}: {} to {}", label->AsView(), least, most));
            }
        }
    }

    /// One given value, checked and held until every one is (a refusal leaves the record as it
    /// was).
    struct FieldChange
    {
        const PropertyInfo* field = nullptr;
        Guid id;
        Array<Guid> ids;    // an asset list's whole list
        Array<String> texts; // a text list's whole list
        bool flag = false;
        i64 choice = 0;
        String text;
        u32 number = 0;
        f32 real = 0.0f;
        Float4 color{};
    };

    /// Checks every given argument that names one of `fields` against its kind: an asset must
    /// exist in `project` and be of the field's type (an asset list's entries once each), a count
    /// within its range, a choice one of its values, a flag a bool. The changes, or the refusal.
    inline Result<Array<FieldChange>, String> CheckFields(const JsonValue& args, Span<const PropertyInfo* const> fields,
                                                          editor::EditorProject& project,
                                                          const pipeline::BuilderRegistry& builders)
    {
        Array<FieldChange> changes;
        for (const PropertyInfo* field : fields)
        {
            const String key(PropertyName(*field));
            if (!args.Has(key))
            {
                continue;
            }
            const JsonValue value = args.Get(key);
            FieldChange change;
            change.field = field;
            const String* assetType =
                engine::project::SettingAttribute(*field, foundation::settings::kSettingAssetTypeAttribute);
            const Array<String> accepted =
                assetType != nullptr ? AcceptedAssetTypes(builders, assetType->AsView()) : Array<String>();
            // One guid naming an asset of a type the field takes: the refusal, empty when it does;
            // `where` names the field in it.
            const auto checkAsset = [&project, &accepted](StringView where, StringView text, Guid& id) -> String
            {
                if (!Guid::TryParse(text, id))
                {
                    return Format(u8"`{}`: '{}' is not a valid guid", where, text);
                }
                content::Instance* asset = project.SourceDb().GetInstance(id);
                if (asset == nullptr)
                {
                    return Format(u8"`{}`: no asset with guid {} in the project", where, text);
                }
                bool takes = false;
                for (const String& type : accepted)
                {
                    takes = takes || asset->TypeName() == type.AsView();
                }
                if (!takes)
                {
                    return Format(u8"`{}` takes an asset of type {}; '{}' is of type {}", where,
                                  AssetTypesText(accepted).AsView(), asset->Name(), asset->TypeName());
                }
                return String();
            };
            if (engine::project::IsAssetListSetting(*field))
            {
                if (!value.IsArray())
                {
                    return Err(Format(u8"`{}` takes an array of {} guids", key.AsView(), AssetTypesText(accepted).AsView()));
                }
                for (i64 i = 0; i < value.Count(); ++i)
                {
                    Guid id;
                    const String text = value.At(i).AsString();
                    String refused = checkAsset(Format(u8"{}[{}]", key.AsView(), i).AsView(), text.AsView(), id);
                    if (!refused.IsEmpty())
                    {
                        return Err(Move(refused));
                    }
                    bool listed = false;
                    for (const Guid& other : change.ids)
                    {
                        listed = listed || other == id;
                    }
                    if (!listed)
                    {
                        change.ids.PushBack(id); // once each
                    }
                }
            }
            else if (assetType != nullptr)
            {
                const String text = value.AsString();
                if (!text.IsEmpty())
                {
                    String refused = checkAsset(key.AsView(), text.AsView(), change.id);
                    if (!refused.IsEmpty())
                    {
                        return Err(Move(refused));
                    }
                }
            }
            else if (IsTextListField(*field))
            {
                if (!value.IsArray())
                {
                    return Err(Format(u8"`{}` takes an array of texts", key.AsView()));
                }
                for (i64 i = 0; i < value.Count(); ++i)
                {
                    change.texts.PushBack(value.At(i).AsString());
                }
            }
            else if (field->type == &TypeOf<String>())
            {
                change.text = value.AsString();
            }
            else if (field->type == &TypeOf<bool>())
            {
                if (!value.IsBool())
                {
                    return Err(Format(u8"`{}` takes true or false", key.AsView()));
                }
                change.flag = value.AsBool();
            }
            else if (field->type == &TypeOf<f32>())
            {
                f32 least = 0.0f;
                f32 most = 0.0f;
                const bool bounded = NumberRange(*field, least, most);
                if (!value.IsNumber() || (bounded && (value.AsNumber() < static_cast<f64>(least) ||
                                                      value.AsNumber() > static_cast<f64>(most))))
                {
                    return Err(bounded ? Format(u8"`{}` takes a number from {} to {}", key.AsView(), least, most)
                                       : Format(u8"`{}` takes a number", key.AsView()));
                }
                change.real = static_cast<f32>(value.AsNumber());
            }
            else if (IsColorField(*field))
            {
                f32 channels[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                bool valid = value.IsArray() && (value.Count() == 3 || value.Count() == 4);
                for (i64 i = 0; valid && i < value.Count(); ++i)
                {
                    const JsonValue channel = value.At(i);
                    valid = channel.IsNumber() && channel.AsNumber() >= 0.0 && channel.AsNumber() <= 1.0;
                    channels[i] = static_cast<f32>(channel.AsNumber());
                }
                if (!valid)
                {
                    return Err(Format(u8"`{}` takes a colour as [r, g, b] or [r, g, b, a], each 0 to 1", key.AsView()));
                }
                change.color = Float4{channels[0], channels[1], channels[2], channels[3]};
            }
            else if (IsEnum(*field->type))
            {
                const String name = value.AsString();
                if (!EnumValueByName(*field->type, reinterpret_cast<const char*>(name.CStr()), change.choice))
                {
                    return Err(Format(u8"`{}` takes {}", key.AsView(), EnumNames(*field->type).AsView()));
                }
            }
            else
            {
                u32 least = 0;
                u32 most = 0;
                CountRange(*field, least, most);
                if (!value.IsNumber() || value.AsNumber() < static_cast<f64>(least) ||
                    value.AsNumber() > static_cast<f64>(most))
                {
                    return Err(most == 0xFFFFFFFFu ? Format(u8"`{}` takes a count from {}", key.AsView(), least)
                                                   : Format(u8"`{}` takes {} to {}", key.AsView(), least, most));
                }
                change.number = static_cast<u32>(value.AsInt());
            }
            changes.PushBack(Move(change));
        }
        return changes;
    }

    /// Writes checked changes into `record`.
    inline void ApplyFields(const Instance& record, Span<const FieldChange> changes)
    {
        for (const FieldChange& change : changes)
        {
            void* address = change.field->address(record);
            if (engine::project::IsAssetListSetting(*change.field))
            {
                *static_cast<Array<Guid>*>(address) = change.ids;
            }
            else if (engine::project::IsAssetSetting(*change.field))
            {
                *static_cast<Guid*>(address) = change.id;
            }
            else if (IsTextListField(*change.field))
            {
                *static_cast<Array<String>*>(address) = change.texts;
            }
            else if (change.field->type == &TypeOf<String>())
            {
                *static_cast<String*>(address) = change.text;
            }
            else if (change.field->type == &TypeOf<bool>())
            {
                *static_cast<bool*>(address) = change.flag;
            }
            else if (change.field->type == &TypeOf<f32>())
            {
                *static_cast<f32*>(address) = change.real;
            }
            else if (IsColorField(*change.field))
            {
                *static_cast<Float4*>(address) = change.color;
            }
            else if (IsEnum(*change.field->type))
            {
                WriteEnumValue(address, *change.field->type, change.choice);
            }
            else
            {
                *static_cast<u32*>(address) = change.number;
            }
        }
    }
}
