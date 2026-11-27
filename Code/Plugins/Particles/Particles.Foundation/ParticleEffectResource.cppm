// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles.resource - the cooked ParticleEffectResource (runtime input), its serializer,
// and its resource factory. A ParticleEffectResource is a reflected ISerializable that holds a
// runtime ParticleEffect; the cook (foundation.particles.editor) writes one into the content
// database and the factory reconstructs it at Bind. Polymorphic modules round-trip through the
// serializable registry (Serializables().Create by type id), the same machinery TextureResource uses.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <utility> // std::move

export module particles.foundation:resource;

import foundation.core;
import :types;
import :streams;
import :modules;
import :effect;
import foundation.content;
import foundation.resource;
import foundation.texture;
import foundation.texture.resource;
import foundation.geometry;          // StaticMesh (mesh-mode particles)
import foundation.geometry.resource; // the StaticMesh resource factory (for manager.Bind)
import foundation.materials;          // Material (mesh-mode particles)
import foundation.materials.resource; // the Material resource factory (for manager.Bind)

using namespace foundation::core;
namespace core = foundation::core;
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace texture = foundation::texture;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;

export namespace particles
{
    // ---- Effect serializer (bidirectional) ----------------------------------------------------

    // A polymorphic module: write its reflected type id (u64) and params; on read, reconstruct via the
    // serializable registry, read its params, and add it to the system (which declares its streams).
    inline void SerializeInitializers(ISerializer& ar, ParticleSystem& sys)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        u32 count = reading ? 0u : static_cast<u32>(sys.InitializerCount());
        ar.Key("initializers");
        ar.BeginArray(count);
        for (u32 i = 0; i < count; ++i)
        {
            ar.BeginObject();
            ParticleInitializer* mod = reading ? nullptr : sys.GetInitializer(static_cast<i32>(i));
            u64 typeId = reading ? 0ull : mod->GetType()->id;
            core::Serialize(ar, "type", typeId);
            RefPtr<ParticleInitializer> created;
            if (reading)
            {
                RefPtr<ISerializable> obj = GlobalSerializableRegistry().Create(typeId);
                created = obj ? RefPtr<ParticleInitializer>{Cast<ParticleInitializer>(obj.Get())} : RefPtr<ParticleInitializer>{};
                if (!created)
                {
                    // A module this build cannot construct is not skippable: its parameters stay
                    // in the positional stream and everything after shifts (a garbage particle
                    // budget, a runaway allocation). Refuse the payload, like a stale data
                    // version; the factory binds nothing.
                    ar.FailPayload(ErrorCode::NotSupported);
                    return;
                }
                mod = created.Get();
            }
            mod->Serialize(ar);
            ar.EndObject();
            if (reading && created)
            {
                sys.AddInitializer(std::move(created));
            }
        }
        ar.EndArray();
    }

    inline void SerializeBehaviors(ISerializer& ar, ParticleSystem& sys)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        u32 count = reading ? 0u : static_cast<u32>(sys.BehaviorCount());
        ar.Key("behaviors");
        ar.BeginArray(count);
        for (u32 i = 0; i < count; ++i)
        {
            ar.BeginObject();
            ParticleBehavior* mod = reading ? nullptr : sys.GetBehavior(static_cast<i32>(i));
            u64 typeId = reading ? 0ull : mod->GetType()->id;
            core::Serialize(ar, "type", typeId);
            RefPtr<ParticleBehavior> created;
            if (reading)
            {
                RefPtr<ISerializable> obj = GlobalSerializableRegistry().Create(typeId);
                created = obj ? RefPtr<ParticleBehavior>{Cast<ParticleBehavior>(obj.Get())} : RefPtr<ParticleBehavior>{};
                if (!created)
                {
                    // A module this build cannot construct is not skippable: its parameters stay
                    // in the positional stream and everything after shifts (a garbage particle
                    // budget, a runaway allocation). Refuse the payload, like a stale data
                    // version; the factory binds nothing.
                    ar.FailPayload(ErrorCode::NotSupported);
                    return;
                }
                mod = created.Get();
            }
            mod->Serialize(ar);
            ar.EndObject();
            if (reading && created)
            {
                sys.AddBehavior(std::move(created));
            }
        }
        ar.EndArray();
    }

    // `effectLinks`: the layout before a system owned its sub-emitters (see SerializeEffect), with none.
    inline void SerializeSystem(ISerializer& ar, ParticleEffect& fx, i32 index, bool effectLinks = false)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        i32 maxParticles = reading ? 0 : fx.GetSystem(index)->MaxParticles();
        u64 seed = reading ? 0ull : fx.GetSystem(index)->Seed();
        core::Serialize(ar, "maxParticles", maxParticles);
        core::Serialize(ar, "seed", seed);
        ParticleSystem* sys = reading ? &fx.AddSystem(maxParticles, seed) : fx.GetSystem(index);

        core::Serialize(ar, "name", sys->name);
        core::Serialize(ar, "desiredMode", sys->desiredMode);
        core::Serialize(ar, "simSpace", sys->simulationSpace);
        core::Serialize(ar, "blend", sys->blendMode);
        core::Serialize(ar, "render", sys->renderMode);
        // The references are stored as the ids they name (nil = none); a read leaves them unbound.
        Guid textureId = sys->texture.id;
        core::Serialize(ar, "textureRef", textureId);
        Guid meshId = sys->mesh.id;
        core::Serialize(ar, "meshRef", meshId);
        core::Serialize(ar, "meshScale", sys->meshScale);
        Array<Guid> materialIds;
        for (const resource::Ref<materials::Material>& material : sys->materials)
        {
            materialIds.PushBack(material.id);
        }
        core::Serialize(ar, "materialRefs", materialIds); // per submesh (empty = none)
        if (reading)
        {
            sys->texture.SetId(textureId);
            sys->mesh.SetId(meshId);
            sys->materials.Clear();
            for (const Guid& id : materialIds)
            {
                sys->materials.EmplaceBack().SetId(id);
            }
        }
        core::Serialize(ar, "sort", sys->sortParticles);
        core::Serialize(ar, "soft", sys->softParticles);
        core::Serialize(ar, "softDistance", sys->softDistance);
        core::Serialize(ar, "trail", sys->trail);
        core::Serialize(ar, "flipbook", sys->flipbook);
        core::Serialize(ar, "prewarm", sys->prewarmTime);
        core::Serialize(ar, "lodStart", sys->lodStartDistance);
        core::Serialize(ar, "lodCull", sys->lodCullDistance);
        core::Serialize(ar, "lodMinRate", sys->lodMinRate);

        ar.Key("emitter");
        ar.BeginObject();
        core::Serialize(ar, "mode", sys->emitter.mode);
        core::Serialize(ar, "spawnRate", sys->emitter.spawnRate);
        core::Serialize(ar, "burstCount", sys->emitter.burstCount);
        core::Serialize(ar, "burstInterval", sys->emitter.burstInterval);
        core::Serialize(ar, "burstCycles", sys->emitter.burstCycles);
        core::Serialize(ar, "isEmitting", sys->emitter.isEmitting);
        core::Serialize(ar, "duration", sys->emitter.duration);
        core::Serialize(ar, "looping", sys->emitter.looping);
        ar.EndObject();

        SerializeInitializers(ar, *sys);
        if (!ar.IsPayloadOk())
        {
            return;
        }
        SerializeBehaviors(ar, *sys);
        if (!ar.IsPayloadOk() || effectLinks)
        {
            return;
        }
        // Its sub-emitters, each into the system it names.
        u32 linkCount = reading ? 0u : static_cast<u32>(sys->subEmitters.Size());
        ar.Key("subEmitters");
        ar.BeginArray(linkCount);
        if (reading)
        {
            sys->subEmitters.Clear();
        }
        for (u32 i = 0; i < linkCount; ++i)
        {
            ar.BeginObject();
            SubEmitterLink link = reading ? SubEmitterLink{} : sys->subEmitters[static_cast<usize>(i)];
            Serialize(ar, link);
            ar.EndObject();
            if (reading)
            {
                sys->subEmitters.PushBack(link);
            }
        }
        ar.EndArray();
    }

    /// An effect either way. `effectLinks`: the layout before a system owned its sub-emitters, the
    /// effect's own list of links each into a system on every other system's births or deaths
    /// (the authoring asset's data version 0); read, each becomes a sub-emitter of every system but
    /// the one it spawns into, as it behaved.
    inline void SerializeEffect(ISerializer& ar, ParticleEffect& fx, bool effectLinks = false)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        if (reading)
        {
            fx.Clear(); // read over an effect (an editor's undo), not added to what it had
        }
        core::Serialize(ar, "name", fx.name);

        u32 systemCount = reading ? 0u : static_cast<u32>(fx.SystemCount());
        ar.Key("systems");
        ar.BeginArray(systemCount);
        for (u32 i = 0; i < systemCount; ++i)
        {
            ar.BeginObject();
            SerializeSystem(ar, fx, static_cast<i32>(i), reading && effectLinks);
            if (!ar.IsPayloadOk())
            {
                return; // refused above: do not read the next system's budget from a shifted stream
            }
            ar.EndObject();
        }
        ar.EndArray();

        if (!reading || !effectLinks)
        {
            return;
        }
        u32 linkCount = 0;
        ar.Key("links");
        ar.BeginArray(linkCount);
        for (u32 i = 0; i < linkCount; ++i)
        {
            ar.BeginObject();
            SubEmitterLink link{};
            Serialize(ar, link);
            ar.EndObject();
            for (i32 s = 0; s < fx.SystemCount(); ++s)
            {
                if (link.childSystemIndex >= 0 && link.childSystemIndex < fx.SystemCount() && s != link.childSystemIndex)
                {
                    fx.GetSystem(s)->subEmitters.PushBack(link); // into nothing, it did nothing: dropped
                }
            }
        }
        ar.EndArray();
    }

    // Deep-copy an effect through a serialize round trip (reusing the one serializer; the copy shares
    // no module RefPtrs). This is the core of the bake: the editor clones the authored asset's effect
    // into a fresh cooked resource.
    inline void CloneEffect(const ParticleEffect& src, ParticleEffect& dst)
    {
        MemoryStream buffer(DefaultAllocator());
        {
            BinarySerializer writer(buffer, SerializeMode::Write);
            SerializeEffect(writer,
                            const_cast<ParticleEffect&>(
                                src)); // write pass only reads src (bidirectional API is non-const)
        }
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        SerializeEffect(reader, dst);
    }
}

export namespace particles
{
    // ---- Cooked resource ---------------------------------------------------------------------
    // Both the cooked record and the runtime product (no GPU transform needed): holds a template
    // ParticleEffect. A component instantiates its own ParticleEffectInstance over this effect.
    class ParticleEffectResource final : public ISerializable
    {
        RTTI_OBJECT(ParticleEffectResource, ISerializable)
    public:
        [[nodiscard]] ParticleEffect& Effect() noexcept { return m_effect; }
        [[nodiscard]] const ParticleEffect& Effect() const noexcept { return m_effect; }
        void Serialize(ISerializer& ar) override { SerializeEffect(ar, m_effect); }

    private:
        ParticleEffect m_effect;
    };

    // Binds each system's texture, mesh and materials through `manager`. Bind records the
    // dependency edges, so re-cooking a referenced asset reloads this effect and the reference
    // follows it. Shared by the cooked factory and by the editor's live preview, which binds an
    // in-memory resource built from the authored effect, so a preview draws as a cooked build does.
    inline void ResolveParticleEffectResources(ParticleEffectResource& res,
                                               resource::ResourceManager& manager)
    {
        ParticleEffect& fx = res.Effect();
        for (i32 s = 0; s < fx.SystemCount(); ++s)
        {
            ParticleSystem* sys = fx.GetSystem(s);
            if (sys == nullptr)
            {
                continue;
            }
            if (!sys->texture.id.IsNil())
            {
                sys->texture.Bind(manager);
            }
            if (!sys->mesh.id.IsNil())
            {
                sys->mesh.Bind(manager);
            }
            for (resource::Ref<materials::Material>& material : sys->materials)
            {
                if (!material.id.IsNil())
                {
                    material.Bind(manager);
                }
            }
        }
    }

    // ---- Factory -----------------------------------------------------------------------------
    // Data factory: deserialize the record; no GPU upload. The systems' references (textures,
    // meshes, materials) are bound through ResolveParticleEffectResources.
    class ParticleEffectFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ParticleEffectResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &ParticleEffectResource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            content::Instance& instance) override
        {
            RefPtr<ISerializable> obj = instance.ReadObject();
            if (ParticleEffectResource* res = Cast<ParticleEffectResource>(obj.Get()))
            {
                ResolveParticleEffectResources(*res, manager);
            }
            return obj;
        }
    };

    // Register the cooked resource type and all module types. Call once at startup (tooling and runtime).
    inline void RegisterParticleEffectResource()
    {
        RegisterParticleModules();
        GlobalTypeRegistry().Register(ParticleEffectResource::StaticType());
        RegisterSerializable<ParticleEffectResource>();
    }

    // v1: curves of Core's keys and gradients, size as `size`, `separateAxes` and `sizeY`. v2: each
    // system owns its sub-emitters; there are no effect-level links. Each bump cooks effects again.
    RTTI_DEFINE_OBJECT_VERSIONED(ParticleEffectResource, "rtti::particles", 2)
}

export namespace particles
{
    /// The particles resource module: what the engine composition builds this library's
    /// factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kParticlesResourceFactories[] = {
        foundation::resource::FactoryByDefault<ParticleEffectResource, ParticleEffectResource, ParticleEffectFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kParticlesResourceModule{
        u8"particles", &RegisterParticleEffectResource, kParticlesResourceFactories,
        sizeof(kParticlesResourceFactories) / sizeof(kParticlesResourceFactories[0])};
}
