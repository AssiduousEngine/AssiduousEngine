// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :native_behavior_tools partition
//
// native_behavior_list: the native behavior classes this host knows (registered by the project's
// plugins), each as the Native Behavior cook reads it (its kind, properties with their defaults, and
// handlers, or why it would not cook), with the open project's Native Behavior assets naming it. It
// is what an agent reads before asset_create's Native Behavior: the asset named like the class
// names it.

module;
#include "Core/Prelude.h"

export module editor.mcp:native_behavior_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.content;
import foundation.script;
import foundation.script.resource;
import script.pipeline;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace editor::mcp::detail
{
    [[nodiscard]] inline StringView PropertyTypeName(foundation::script::BehaviorPropertyType type)
    {
        using foundation::script::BehaviorPropertyType;
        switch (type)
        {
        case BehaviorPropertyType::Float: return u8"float";
        case BehaviorPropertyType::Int: return u8"int";
        case BehaviorPropertyType::Bool: return u8"bool";
        case BehaviorPropertyType::String: return u8"string";
        case BehaviorPropertyType::Color: return u8"color";
        case BehaviorPropertyType::Vec3: return u8"vec3";
        case BehaviorPropertyType::Entity: return u8"entity";
        case BehaviorPropertyType::Asset: return u8"asset";
        case BehaviorPropertyType::None:
        default: return u8"none";
        }
    }

    // A Native Behavior asset and the class it names.
    struct NativeAsset
    {
        String className;
        Guid id;
    };

    // Every Native Behavior asset under `group`.
    inline void CollectNativeAssets(const foundation::content::Group& group, Array<NativeAsset>& out)
    {
        for (foundation::content::Instance* instance : group.Instances())
        {
            if (instance->TypeName() != StringView(u8"NativeBehaviorAsset"))
            {
                continue;
            }
            RefPtr<ISerializable> read = instance->ReadObject();
            if (const auto* asset = Cast<pipeline::NativeBehaviorAsset>(read.Get()))
            {
                out.PushBack(NativeAsset{asset->className, instance->Id()});
            }
        }
        for (foundation::content::Group* child : group.Groups())
        {
            CollectNativeAssets(*child, out);
        }
    }
}

export namespace editor::mcp
{
    inline void RegisterNativeBehaviorTools(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;

        server.RegisterTool(
            u8"native_behavior_list",
            u8"LIST the native behavior classes (C++ classes a project's plugin registers, deriving from "
            u8"RunBehavior, SceneBehavior or EntityBehavior): each one's className (the qualified name a "
            u8"Native Behavior asset names), name, kind, properties (the fields it marks behaviorProperty: "
            u8"name, type, default) and handlers (its on<Name> methods), as the cook reads it, or `error` "
            u8"when it would not cook; and `assets`, the open project's Native Behavior assets naming it. "
            u8"asset_create with creator 'Native Behavior' and the class's name makes its asset.",
            SchemaBuilder().Build(), foundation::mcp::ToolAnnotations::ReadOnly(),
            [&session](const JsonValue&) -> ToolResult
            {
                Array<detail::NativeAsset> assets;
                if (session.project != nullptr)
                {
                    if (const foundation::content::Group* root = session.project->SourceDb().RootGroup())
                    {
                        detail::CollectNativeAssets(*root, assets);
                    }
                }
                JsonValue classes = JsonValue::MakeArray();
                for (const TypeInfo* type : GlobalTypeRegistry().All())
                {
                    if (pipeline::NativeBehaviorKindOf(*type) == foundation::script::BehaviorKind::None)
                    {
                        continue;
                    }
                    foundation::script::BehaviorClassSource harvested;
                    String why;
                    const bool cooks = pipeline::HarvestNativeBehaviorClass(*type, editor::EditorRootAllocator(), harvested, why);
                    if (cooks && harvested.className.IsEmpty())
                    {
                        continue;
                    }
                    const String className = Format(u8"{}::{}", StringView(reinterpret_cast<const utf8char*>(type->namespaceName)),
                                                    StringView(reinterpret_cast<const utf8char*>(type->name)));
                    JsonValue entry = JsonValue::MakeObject();
                    entry.Set(u8"className", JsonValue::MakeString(className));
                    entry.Set(u8"name", JsonValue::MakeString(String(StringView(reinterpret_cast<const utf8char*>(type->name)))));
                    entry.Set(u8"kind", JsonValue::MakeString(String(foundation::script::BehaviorKinds::TypeName(pipeline::NativeBehaviorKindOf(*type)))));
                    if (!cooks)
                    {
                        entry.Set(u8"error", JsonValue::MakeString(why));
                    }
                    JsonValue properties = JsonValue::MakeArray();
                    for (const foundation::script::BehaviorProperty& property : harvested.properties)
                    {
                        JsonValue p = JsonValue::MakeObject();
                        p.Set(u8"name", JsonValue::MakeString(property.name));
                        p.Set(u8"type", JsonValue::MakeString(String(detail::PropertyTypeName(property.type))));
                        if (property.type == foundation::script::BehaviorPropertyType::Float ||
                            property.type == foundation::script::BehaviorPropertyType::Int)
                        {
                            p.Set(u8"default", JsonValue::MakeNumber(property.defaultValue.number));
                        }
                        else if (property.type == foundation::script::BehaviorPropertyType::Bool)
                        {
                            p.Set(u8"default", JsonValue::MakeBool(property.defaultValue.boolean));
                        }
                        else if (property.type == foundation::script::BehaviorPropertyType::String)
                        {
                            p.Set(u8"default", JsonValue::MakeString(property.defaultValue.text));
                        }
                        if (!property.assetType.IsEmpty())
                        {
                            p.Set(u8"assetType", JsonValue::MakeString(property.assetType));
                        }
                        properties.Add(Move(p));
                    }
                    entry.Set(u8"properties", Move(properties));
                    JsonValue handlers = JsonValue::MakeArray();
                    for (const String& handler : harvested.handlers)
                    {
                        handlers.Add(JsonValue::MakeString(handler));
                    }
                    entry.Set(u8"handlers", Move(handlers));
                    JsonValue named = JsonValue::MakeArray();
                    for (const detail::NativeAsset& asset : assets)
                    {
                        if (asset.className == className)
                        {
                            named.Add(JsonValue::MakeString(Format(u8"{}", asset.id)));
                        }
                    }
                    entry.Set(u8"assets", Move(named));
                    classes.Add(Move(entry));
                }
                JsonValue result = JsonValue::MakeObject();
                result.Set(u8"classes", Move(classes));
                return result;
            });
    }
}
