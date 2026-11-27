// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles.Engine's entry. Load registers the cooked effect's types in every host (a cook reads
// them as a player does) and, where the engine offers its service, adds the particle manager to
// every scene, live ones included, and the particles domain: the component's reflection, the
// `scene.particles` facade and the effect's factory. A host that renders gets the ParticleSubsystem,
// which registers its renderer. What it adds is recorded and taken out when the plugin unloads;
// Unload takes back the subsystem and the script names, which the recording does not hold.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.resource;
import foundation.scene;
import foundation.particles.resource;
import engine.domain;      // DomainModule
import engine.composition; // EnginePluginServices
import engine.render;      // RenderSubsystem: the particle renderer needs one
import engine.particles;

namespace
{
    const foundation::resource::ResourceModule* const kResources[] = {&foundation::particles::kParticlesResourceModule};
    const engine::DomainModule kParticlesDomain{
        .id = u8"particles",
        .registerReflection = &engine::particles::RegisterParticleComponentReflection,
        .registerScriptFacade = &engine::particles::RegisterParticleScriptFacade,
        .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{kResources, 1}};

    void Load(foundation::runtime::PluginContext& context)
    {
        foundation::particles::RegisterParticleEffectResource();
        if (engine::EnginePluginServices* services = context.Find<engine::EnginePluginServices>())
        {
            foundation::scene::SceneModuleContributions::Contribution manager;
            manager.id = u8"particles";
            manager.install = &engine::particles::AddParticleSceneManagers;
            manager.registerReflection = &engine::particles::RegisterParticleComponentReflection;
            manager.systemType = foundation::core::TypeOf<engine::particles::ParticleEffectComponentManager>().id;
            services->AddSceneManager(manager);
            (void)services->AddDomain(kParticlesDomain);
        }
        if (context.Subsystems().GetSubsystem<engine::render::RenderSubsystem>() != nullptr)
        {
            context.Subsystems().AddSubsystem<engine::particles::ParticleSubsystem>();
        }
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        context.Subsystems().RemoveSubsystem<engine::particles::ParticleSubsystem>();
        engine::particles::RemoveParticleScriptFacade();
    }
}

PLUGIN_PART(&Load, &Unload)
