// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Sequence.Resource - `foundation.sequence.resource`.
//
// The cooked sequence as a runtime resource, shaped like property animation's:
//   * SequenceResource: the runtime product, a sequence shared by every play of it.
//   * SequenceSource: the cooked form (SerializeSequence: each track as its kind's type id and
//     fields).
//   * SequenceFactory: reads the source into a resource and binds what its tracks refer to.
// And the track kind that refers to a sequence asset: NestedSequenceTrack, a sequence played
// inside another on the same cast.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.sequence.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.scene;
import foundation.sequence;

using namespace foundation::core;

export namespace foundation::sequence
{
    namespace resource = foundation::resource;

    /// The runtime product: a sequence, shared and unchanged by every play of it.
    class SequenceResource final : public Object
    {
        RTTI_OBJECT(SequenceResource, Object)
    public:
        Sequence sequence;
    };

    /// The cooked form.
    class SequenceSource final : public ISerializable
    {
        RTTI_OBJECT(SequenceSource, ISerializable)
    public:
        Sequence sequence;

        void Serialize(ISerializer& ar) override { SerializeSequence(ar, sequence); }
    };

    /// Another sequence, played inside this one over a section of its timeline, on the same cast:
    /// each of its names is filled by this play's entity of that name. Its events reach this
    /// play's listener; its holds are not kept (the outer timeline holds).
    class NestedSequenceTrack final : public SequenceTrack
    {
        RTTI_OBJECT(NestedSequenceTrack, SequenceTrack)
    public:
        resource::Ref<SequenceResource> sequence;
        f32 start = 0.0f;
        f32 length = 0.0f; // 0: the nested sequence's own duration
        bool loop = false; // repeat it over the section; otherwise it stays at its end

        [[nodiscard]] f32 Duration() const noexcept override;
        [[nodiscard]] i32 Order() const noexcept override { return kPropertyTrackOrder; }
        [[nodiscard]] bool NeedsBinding() const noexcept override { return false; }
        [[nodiscard]] UniquePtr<SequenceTrackRunner> Begin(const SequenceTrackContext& context) const override;
        void ResolveResources(resource::ResourceManager& manager) override { sequence.Bind(manager); }
        void Serialize(ISerializer& ar) override;
        /// The section's length: `length`, or the nested sequence's duration.
        [[nodiscard]] f32 SectionLength() const noexcept;
    };

    /// Loads a cooked SequenceSource into a SequenceResource and binds what its tracks refer to.
    class SequenceFactory final : public resource::IResourceFactory
    {
    public:
        explicit SequenceFactory(IAllocator& allocator) : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override { return &SequenceResource::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &SequenceSource::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager, foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            SequenceSource* source = Cast<SequenceSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<SequenceResource> product = MakeRef<SequenceResource>(*m_allocator);
            product->sequence = Move(source->sequence);
            for (RefPtr<SequenceTrack>& track : product->sequence.tracks)
            {
                if (track)
                {
                    track->ResolveResources(manager);
                }
            }
            return product;
        }

    private:
        IAllocator* m_allocator;
    };

    /// Registers the sequence types, the cooked source, the resource and the nested track kind.
    /// Idempotent.
    void RegisterSequenceResource();

    RTTI_DEFINE_OBJECT(SequenceResource, "rtti::sequence")
    RTTI_DEFINE_OBJECT(SequenceSource, "rtti::sequence")
}

export namespace foundation::sequence
{
    /// The sequence resource module: the module the engine composition composes this library's
    /// factory from.
    inline constexpr foundation::resource::ResourceFactoryDesc kSequenceResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<SequenceResource, SequenceSource, SequenceFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kSequenceResourceModule{
        u8"sequence", &RegisterSequenceResource, kSequenceResourceFactories,
        sizeof(kSequenceResourceFactories) / sizeof(kSequenceResourceFactories[0])};
}
