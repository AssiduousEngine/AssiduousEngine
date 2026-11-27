// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Navigation.Engine's entry. Load registers the cooked navigation zone in every host (a cook reads it
// as a player does) and, where the engine offers its service, adds the zone, agent and navigation
// managers to every scene, live ones included, and the navigation domain: the components'
// reflection, the NavAgentComponent script surface and the zone's factory. Every host with a run
// gets the NavigationSubsystem. What it adds is recorded and taken out when the plugin unloads;
// Unload takes back the subsystem and the script names, which the recording does not hold.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.resource;
import foundation.scene;
import foundation.navigation.resource;
import engine.domain;      // DomainModule
import engine.composition; // EnginePluginServices
import engine.navigation;

namespace
{
    const foundation::resource::ResourceModule* const kResources[] = {&foundation::navigation::kNavigationResourceModule};
    const engine::DomainModule kNavigationDomain{
        .id = u8"navigation",
        .registerReflection = &engine::navigation::RegisterNavigationComponentReflection,
        .registerScriptFacade = &engine::navigation::RegisterNavigationScriptFacade,
        .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{kResources, 1}};

    template <typename Manager>
    void AddManager(engine::EnginePluginServices& services, foundation::core::StringView id)
    {
        foundation::scene::SceneModuleContributions::Contribution contribution;
        contribution.id = id;
        contribution.install = [](foundation::scene::Scene& scene) { scene.AddSystem<Manager>(); };
        contribution.registerReflection = &engine::navigation::RegisterNavigationComponentReflection;
        contribution.systemType = foundation::core::TypeOf<Manager>().id;
        services.AddSceneManager(contribution);
    }

    void Load(foundation::runtime::PluginContext& context)
    {
        foundation::navigation::RegisterNavigationResource();
        if (engine::EnginePluginServices* services = context.Find<engine::EnginePluginServices>())
        {
            // The managers AddNavigationSceneManagers installs, in its order.
            AddManager<engine::navigation::NavMeshZoneComponentManager>(*services, u8"navigation.zones");
            AddManager<engine::navigation::NavAgentComponentManager>(*services, u8"navigation.agents");
            AddManager<engine::navigation::NavigationSceneSystem>(*services, u8"navigation.scene");
            (void)services->AddDomain(kNavigationDomain);
        }
        context.Subsystems().AddSubsystem<engine::navigation::NavigationSubsystem>();
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        context.Subsystems().RemoveSubsystem<engine::navigation::NavigationSubsystem>();
        engine::navigation::RemoveNavigationScriptFacade();
    }
}

PLUGIN_PART(&Load, &Unload)
