// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cast: who speaks in a project's conversations. Each speaker is a reflected value (the
// inspector edits it in the cast asset), its assets referred to by guid. The cook writes the cast
// as a DialogueCastSource; the factory loads that into a DialogueCastResource.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module dialogue.engine:cast;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;

export namespace dialogue
{
    /// A mood a speaker's lines may name, and its portrait (a texture; nil uses the speaker's).
    struct DialogueMood
    {
        String name;
        Guid portrait;
    };

    /// One speaker.
    struct DialogueSpeaker
    {
        String id;         // what `@` lines and `play` name: a word
        String shownName;  // the name the box shows; empty shows none
        bool plate = true; // the box shows a name plate, tinted `plateColor`
        Float4 plateColor{0.5f, 0.5f, 0.5f, 1.0f};
        Array<DialogueMood> moods; // the first is the default mood
        Guid portrait;             // for a mood without its own
        Array<Guid> clips;         // the animation clips `play` may name, by the clip's name
        Guid talk;                 // a clip played while the speaker's line shows
        Guid voice;                // a sound played as the text types
        String entity;             // the level entity that plays the speaker, by name

        /// The mood `name`, or null.
        [[nodiscard]] const DialogueMood* FindMood(StringView name) const noexcept;
    };

    void Serialize(ISerializer& ar, DialogueMood& mood);
    void Serialize(ISerializer& ar, DialogueSpeaker& speaker);

    /// The runtime product.
    class DialogueCastResource final : public Object
    {
        RTTI_OBJECT(DialogueCastResource, Object)
    public:
        Array<DialogueSpeaker> speakers;

        /// The speaker `id`, or null.
        [[nodiscard]] const DialogueSpeaker* Find(StringView id) const noexcept;
    };

    /// The cooked form.
    class DialogueCastSource final : public ISerializable
    {
        RTTI_OBJECT(DialogueCastSource, ISerializable)
    public:
        Array<DialogueSpeaker> speakers;

        void Serialize(ISerializer& ar) override;
    };

    /// Loads a cooked DialogueCastSource into a DialogueCastResource.
    class DialogueCastFactory final : public foundation::resource::IResourceFactory
    {
    public:
        explicit DialogueCastFactory(IAllocator& allocator) : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override { return &DialogueCastResource::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &DialogueCastSource::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override;

    private:
        IAllocator* m_allocator;
    };
}
