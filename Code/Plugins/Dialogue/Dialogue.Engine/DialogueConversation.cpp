// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The compiled dialogue's serialization, lookups and factory.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.engine;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;

namespace dialogue
{
    void Serialize(ISerializer& ar, DialogueArgument& argument)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "name", argument.name);
        foundation::core::Serialize(ar, "text", argument.text);
        foundation::core::Serialize(ar, "number", argument.number);
        foundation::core::Serialize(ar, "asset", argument.asset);
        foundation::core::Serialize(ar, "present", argument.present);
        ar.EndObject();
    }

    void Serialize(ISerializer& ar, DialogueStep& step)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "kind", step.kind);
        foundation::core::Serialize(ar, "speaker", step.speaker);
        foundation::core::Serialize(ar, "mood", step.mood);
        foundation::core::Serialize(ar, "lineId", step.lineId);
        foundation::core::Serialize(ar, "flag", step.flag);
        foundation::core::Serialize(ar, "negate", step.negate);
        foundation::core::Serialize(ar, "name", step.name);
        foundation::core::Serialize(ar, "file", step.file);
        foundation::core::Serialize(ar, "asset", step.asset);
        foundation::core::Serialize(ar, "seconds", step.seconds);
        foundation::core::Serialize(ar, "waits", step.waits);
        foundation::core::Serialize(ar, "leaves", step.leaves);
        foundation::core::Serialize(ar, "fadeIn", step.fadeIn);
        foundation::core::Serialize(ar, "hasColor", step.hasColor);
        foundation::core::Serialize(ar, "color", step.color);
        foundation::core::Serialize(ar, "next", step.next);
        foundation::core::Serialize(ar, "count", step.count);
        foundation::core::Serialize(ar, "sourceLine", step.sourceLine);
        foundation::core::Serialize(ar, "arguments", step.arguments);
        ar.EndObject();
    }

    void Serialize(ISerializer& ar, DialogueConversation& conversation)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "id", conversation.id);
        foundation::core::Serialize(ar, "level", conversation.level);
        foundation::core::Serialize(ar, "steps", conversation.steps);
        ar.EndObject();
    }

    void Serialize(ISerializer& ar, DialogueLineText& line)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "id", line.id);
        foundation::core::Serialize(ar, "text", line.text);
        ar.EndObject();
    }

    const DialogueConversation* DialogueResource::Find(StringView id) const noexcept
    {
        for (const DialogueConversation& conversation : conversations)
        {
            if (conversation.id == id)
            {
                return &conversation;
            }
        }
        return nullptr;
    }

    StringView DialogueResource::Text(StringView id) const noexcept
    {
        for (const DialogueLineText& line : lines)
        {
            if (line.id == id)
            {
                return line.text.AsView();
            }
        }
        return {};
    }

    void DialogueSource::Serialize(ISerializer& ar)
    {
        foundation::core::Serialize(ar, "cast", cast);
        foundation::core::Serialize(ar, "conversations", conversations);
        foundation::core::Serialize(ar, "lines", lines);
    }

    RefPtr<Object> DialogueFactory::Create(foundation::resource::ResourceManager& manager,
                                           foundation::content::Instance& instance)
    {
        (void)manager;
        RefPtr<ISerializable> object = instance.ReadObject();
        DialogueSource* source = Cast<DialogueSource>(object.Get());
        if (source == nullptr)
        {
            return RefPtr<Object>{};
        }
        RefPtr<DialogueResource> product = MakeRef<DialogueResource>(*m_allocator);
        product->cast = source->cast;
        product->conversations = Move(source->conversations);
        product->lines = Move(source->lines);
        return product;
    }

    RTTI_DEFINE_OBJECT(DialogueResource, "rtti::dialogue")
    RTTI_DEFINE_OBJECT(DialogueSource, "rtti::dialogue")
}
