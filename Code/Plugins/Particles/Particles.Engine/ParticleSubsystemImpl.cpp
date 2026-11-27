// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles - particles.subsystem implementation unit: the component reflection body.
//
// Kept out of the :components interface partition: REFLECT_* bodies in an interface unit make GCC
// emit an unreadable gcm cluster for consumers. ParticleComponents.cppm declares
// RegisterParticleComponentReflection(); this unit defines it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.particles;
import foundation.particles.resource;

import foundation.core;
import foundation.script.facades; // ComponentMethods<T> (of, addTo, removeFrom), RegisterExtra*

using namespace foundation::core;
namespace core = foundation::core;

namespace engine::particles
{
    REFLECT_VALUE(ParticleEffectComponent, "rtti::engine::particles")
    {
        builder.Attribute("displayName", String(u8"Particle Effect"))
            .Attribute("category", String(u8"Effects"))
            .Attribute("description",
                       String(u8"Plays a particle effect at the entity: billboards, trails, meshes "
                              u8"or lights."))
            // Script: ParticleEffectComponent.of(entity) reaches the live visible/meshScale/light*
            // data. play/stop/setEffect are instance operations, on scene.particles (keyed
            // by entity).
            .Apply(&foundation::script::ComponentMethods<ParticleEffectComponent>)
            .Property<&ParticleEffectComponent::effectAsset>("effect")
            .Property<&ParticleEffectComponent::mesh>("mesh")
            .Property<&ParticleEffectComponent::material>("material")
            .Property<&ParticleEffectComponent::meshScale>("meshScale")
            .Property<&ParticleEffectComponent::lightIntensity>("lightIntensity")
            .Property<&ParticleEffectComponent::lightRange>("lightRange")
            .Property<&ParticleEffectComponent::visible>("visible");
    }

    // The scene-bound particles handle: scene.particles.play/stop/restart/pause/isPlaying/
    // setEffect. `of` returns SceneParticles by value (concrete cross-backend return), like ScenePhysics.
    REFLECT_VALUE(SceneParticlesFacade, "rtti::engine::particles")
    {
        builder.Attribute("scriptName", "SceneParticles");
        builder.Method<&SceneParticlesFacade::play>("play", {"entity"});
        builder.Method<&SceneParticlesFacade::stop>("stop", {"entity"});
        builder.Method<&SceneParticlesFacade::restart>("restart", {"entity"});
        builder.Method<&SceneParticlesFacade::pause>("pause", {"entity", "paused"});
        builder.Method<&SceneParticlesFacade::isPlaying>("isPlaying", {"entity"});
        builder.Method<&SceneParticlesFacade::setEffect>("setEffect", {"entity", "resourceId"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    void RegisterParticleComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_ParticleEffectComponent();
            return true;
        }();
        (void)once;
    }

    void RegisterParticleScriptFacade()
    {
        RegisterParticleComponentReflection(); // ensure component TypeData (including `of`) is built first
        // Surface the particle component (ParticleEffectComponent.of(entity)): register, seed root,
        // name.
        GlobalTypeRegistry().Register(core::TypeOf<ParticleEffectComponent>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<ParticleEffectComponent>());
        foundation::script::RegisterExtraFacadeName(u8"ParticleEffectComponent");

        // The scene-bound particles handle (scene.particles): reflect, register, seed,
        // name.
        RttiRegisterValue_SceneParticlesFacade();
        GlobalTypeRegistry().Register(core::TypeOf<SceneParticlesFacade>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<SceneParticlesFacade>());
        foundation::script::RegisterExtraFacadeName(u8"SceneParticles");
        foundation::script::ExtendScene<&SceneParticlesFacade::ForScene>("particles"); // scene.particles
    }

    void RemoveParticleScriptFacade()
    {
        foundation::script::RetractScene<&SceneParticlesFacade::ForScene>();
        foundation::script::RemoveExtraFacadeName(u8"SceneParticles");
        foundation::script::RemoveExtraScriptRootType(&core::TypeOf<SceneParticlesFacade>());
        foundation::script::RemoveExtraFacadeName(u8"ParticleEffectComponent");
        foundation::script::RemoveExtraScriptRootType(&core::TypeOf<ParticleEffectComponent>());
    }
}
