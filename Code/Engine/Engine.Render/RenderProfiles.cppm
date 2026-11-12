// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Render - :profiles partition: the cooked records, factories and resource module of the
// render profiles (an Environment Profile, a Post Process Profile) and of the quality levels (a
// Render Quality). A profile is a shared data asset carrying the value fields of a scene's
// environment or post settings block; a block whose source is Profile uses its values
// (RenderComponents.cppm: SettingsSource, Effective()). A Render Quality is one level a project
// offers: what the scenes' look costs on it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.render:profiles;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.texture.resource;
import :components;

using namespace foundation::core;

export namespace engine::render
{
    // The cooked Environment Profile: the environment's value fields (no source: a profile is the
    // source), written by the same serializer as the scene block.
    class EnvironmentProfileSource final : public ISerializable
    {
        RTTI_OBJECT(EnvironmentProfileSource, ISerializable)
    public:
        EnvironmentSettings values;
        void Serialize(ISerializer& ar) override { SerializeEnvironmentValues(ar, values, true); }
    };

    // The cooked Post Process Profile. Data version 1 has no antialiasing mode (the quality
    // level's); a version 0 record, cooked before, still reads.
    class PostProcessProfileSource final : public ISerializable
    {
        RTTI_OBJECT(PostProcessProfileSource, ISerializable)
    public:
        PostProcessSettings values;
        void Serialize(ISerializer& ar) override
        {
            SerializePostValues(ar, values, ar.Mode() == SerializeMode::Read && ar.Version() < 1);
        }
    };

    // The cooked Render Quality: a quality level's values.
    class RenderQualitySource final : public ISerializable
    {
        RTTI_OBJECT(RenderQualitySource, ISerializable)
    public:
        RenderQualitySettings values;
        void Serialize(ISerializer& ar) override { SerializeQualityValues(ar, values); }
    };

    // Loads a profile, binding its texture references through the manager (the bind records the
    // dependency, so a texture's reload reaches the profile).
    class EnvironmentProfileFactory final : public foundation::resource::IResourceFactory
    {
    public:
        explicit EnvironmentProfileFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &EnvironmentProfile::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &EnvironmentProfileSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* source = Cast<EnvironmentProfileSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<EnvironmentProfile> profile = MakeRef<EnvironmentProfile>(*m_allocator);
            profile->values = source->values;
            profile->values.skyTexture.Bind(manager);
            return profile;
        }

    private:
        IAllocator* m_allocator;
    };

    class PostProcessProfileFactory final : public foundation::resource::IResourceFactory
    {
    public:
        explicit PostProcessProfileFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &PostProcessProfile::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &PostProcessProfileSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* source = Cast<PostProcessProfileSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<PostProcessProfile> profile = MakeRef<PostProcessProfile>(*m_allocator);
            profile->values = source->values;
            profile->values.gradingLut.Bind(manager);
            return profile;
        }

    private:
        IAllocator* m_allocator;
    };

    // Loads a quality level (it references nothing).
    class RenderQualityFactory final : public foundation::resource::IResourceFactory
    {
    public:
        explicit RenderQualityFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}
        [[nodiscard]] const TypeInfo* ProductType() const override { return &RenderQuality::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &RenderQualitySource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* source = Cast<RenderQualitySource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<RenderQuality> quality = MakeRef<RenderQuality>(*m_allocator);
            quality->values = source->values;
            return quality;
        }

    private:
        IAllocator* m_allocator;
    };

    // Registers the cooked records and products (content-DB construction by type name). Idempotent.
    inline void RegisterRenderProfileResources()
    {
        GlobalTypeRegistry().Register(EnvironmentProfileSource::StaticType());
        RegisterSerializable<EnvironmentProfileSource>();
        GlobalTypeRegistry().Register(EnvironmentProfile::StaticType());
        GlobalTypeRegistry().Register(PostProcessProfileSource::StaticType());
        RegisterSerializable<PostProcessProfileSource>();
        GlobalTypeRegistry().Register(PostProcessProfile::StaticType());
        GlobalTypeRegistry().Register(RenderQualitySource::StaticType());
        RegisterSerializable<RenderQualitySource>();
        GlobalTypeRegistry().Register(RenderQuality::StaticType());
    }

    RTTI_DEFINE_OBJECT(EnvironmentProfileSource, "rtti::engine::render")
    // PostProcessProfileSource's reflection (its data version) is in RenderComponentsImpl.cpp.
    RTTI_DEFINE_OBJECT(RenderQualitySource, "rtti::engine::render")

    inline constexpr foundation::resource::ResourceFactoryDesc kRenderProfileFactories[] = {
        foundation::resource::FactoryWithAllocator<EnvironmentProfile, EnvironmentProfileSource,
                                               EnvironmentProfileFactory>(),
        foundation::resource::FactoryWithAllocator<PostProcessProfile, PostProcessProfileSource,
                                               PostProcessProfileFactory>(),
        foundation::resource::FactoryWithAllocator<RenderQuality, RenderQualitySource, RenderQualityFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kRenderProfileResourceModule{
        u8"render.profiles", &RegisterRenderProfileResources, kRenderProfileFactories,
        sizeof(kRenderProfileFactories) / sizeof(kRenderProfileFactories[0])};
}
