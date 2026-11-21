// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cast's reflection (what the inspector shows of a speaker), serialization, lookups and
// factory.

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
    REFLECT_VALUE(DialogueMood, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Mood"));
        builder.Property<&DialogueMood::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .Property<&DialogueMood::portrait>("portrait")
            .PropAttribute("displayName", String(u8"Portrait"))
            .PropAttribute("referenceType", String(u8"Texture"));
    }

    REFLECT_VALUE(DialogueSpeaker, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Speaker"));
        builder.Property<&DialogueSpeaker::id>("id")
            .PropAttribute("displayName", String(u8"Id"))
            .PropAttribute("description", String(u8"What a conversation's lines name the speaker by: one word"))
            .Property<&DialogueSpeaker::shownName>("shownName")
            .PropAttribute("displayName", String(u8"Shown Name"))
            .PropAttribute("description", String(u8"The name the box shows; empty shows none"))
            .Property<&DialogueSpeaker::plate>("plate")
            .PropAttribute("displayName", String(u8"Name Plate"))
            .Property<&DialogueSpeaker::plateColor>("plateColor")
            .PropAttribute("displayName", String(u8"Plate Color"))
            .PropAttribute("color", true)
            .Property<&DialogueSpeaker::moods>("moods")
            .PropAttribute("displayName", String(u8"Moods"))
            .PropAttribute("description", String(u8"The moods a line may name; the first is the default"))
            .Property<&DialogueSpeaker::portrait>("portrait")
            .PropAttribute("displayName", String(u8"Portrait"))
            .PropAttribute("referenceType", String(u8"Texture"))
            .PropAttribute("description", String(u8"For a mood without a portrait of its own"))
            .Property<&DialogueSpeaker::clips>("clips")
            .PropAttribute("displayName", String(u8"Clips"))
            .PropAttribute("referenceType", String(u8"AnimationClip"))
            .PropAttribute("description", String(u8"The animation clips a conversation may play, by name"))
            .Property<&DialogueSpeaker::talk>("talk")
            .PropAttribute("displayName", String(u8"Talk Clip"))
            .PropAttribute("referenceType", String(u8"AnimationClip"))
            .Property<&DialogueSpeaker::voice>("voice")
            .PropAttribute("displayName", String(u8"Voice"))
            .PropAttribute("referenceType", String(u8"AudioClip"))
            .Property<&DialogueSpeaker::entity>("entity")
            .PropAttribute("displayName", String(u8"Entity"))
            .PropAttribute("description", String(u8"The level entity that plays the speaker, by name"));
    }

    void RegisterCastValues()
    {
        RttiRegisterValue_DialogueMood();
        RttiRegisterValue_DialogueSpeaker();
        RegisterArrayType<Guid>();
        RegisterArrayType<DialogueMood>();
        RegisterArrayType<DialogueSpeaker>();
    }

    const DialogueMood* DialogueSpeaker::FindMood(StringView name) const noexcept
    {
        for (const DialogueMood& mood : moods)
        {
            if (mood.name == name)
            {
                return &mood;
            }
        }
        return nullptr;
    }

    void Serialize(ISerializer& ar, DialogueMood& mood)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "name", mood.name);
        foundation::core::Serialize(ar, "portrait", mood.portrait);
        ar.EndObject();
    }

    void Serialize(ISerializer& ar, DialogueSpeaker& speaker)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "id", speaker.id);
        foundation::core::Serialize(ar, "shownName", speaker.shownName);
        foundation::core::Serialize(ar, "plate", speaker.plate);
        foundation::core::Serialize(ar, "plateColor", speaker.plateColor);
        foundation::core::Serialize(ar, "moods", speaker.moods);
        foundation::core::Serialize(ar, "portrait", speaker.portrait);
        foundation::core::Serialize(ar, "clips", speaker.clips);
        foundation::core::Serialize(ar, "talk", speaker.talk);
        foundation::core::Serialize(ar, "voice", speaker.voice);
        foundation::core::Serialize(ar, "entity", speaker.entity);
        ar.EndObject();
    }

    const DialogueSpeaker* DialogueCastResource::Find(StringView id) const noexcept
    {
        for (const DialogueSpeaker& speaker : speakers)
        {
            if (speaker.id == id)
            {
                return &speaker;
            }
        }
        return nullptr;
    }

    void DialogueCastSource::Serialize(ISerializer& ar) { foundation::core::Serialize(ar, "speakers", speakers); }

    RefPtr<Object> DialogueCastFactory::Create(foundation::resource::ResourceManager& manager,
                                               foundation::content::Instance& instance)
    {
        (void)manager;
        RefPtr<ISerializable> object = instance.ReadObject();
        DialogueCastSource* source = Cast<DialogueCastSource>(object.Get());
        if (source == nullptr)
        {
            return RefPtr<Object>{};
        }
        RefPtr<DialogueCastResource> product = MakeRef<DialogueCastResource>(*m_allocator);
        product->speakers = Move(source->speakers);
        return product;
    }

    RTTI_DEFINE_OBJECT(DialogueCastResource, "rtti::dialogue")
    RTTI_DEFINE_OBJECT(DialogueCastSource, "rtti::dialogue")
}
