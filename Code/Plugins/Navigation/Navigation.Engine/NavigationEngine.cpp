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
import navigation.foundation;
import engine.domain;      // DomainModule
import engine.composition; // EnginePluginServices
import navigation.engine;

namespace
{
    const foundation::resource::ResourceModule* const kResources[] = {&navigation::kNavigationResourceModule};
    const engine::DomainModule kNavigationDomain{
        .id = u8"navigation",
        .registerReflection = &navigation::RegisterNavigationComponentReflection,
        .registerScriptFacade = &navigation::RegisterNavigationScriptFacade,
        .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{kResources, 1}};

    template <typename Manager>
    void AddManager(engine::EnginePluginServices& services, foundation::core::StringView id)
    {
        foundation::scene::SceneModuleContributions::Contribution contribution;
        contribution.id = id;
        contribution.install = [](foundation::scene::Scene& scene) { scene.AddSystem<Manager>(); };
        contribution.registerReflection = &navigation::RegisterNavigationComponentReflection;
        contribution.systemType = foundation::core::TypeOf<Manager>().id;
        services.AddSceneManager(contribution);
    }

    void Load(foundation::runtime::PluginContext& context)
    {
        navigation::RegisterNavigationResource();
        if (engine::EnginePluginServices* services = context.Find<engine::EnginePluginServices>())
        {
            // The managers AddNavigationSceneManagers installs, in its order.
            AddManager<navigation::NavMeshZoneComponentManager>(*services, u8"navigation.zones");
            AddManager<navigation::NavAgentComponentManager>(*services, u8"navigation.agents");
            AddManager<navigation::NavigationSceneSystem>(*services, u8"navigation.scene");
            (void)services->AddDomain(kNavigationDomain);
        }
        context.Subsystems().AddSubsystem<navigation::NavigationSubsystem>();
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        context.Subsystems().RemoveSubsystem<navigation::NavigationSubsystem>();
        navigation::RemoveNavigationScriptFacade();
    }
}

PLUGIN_PART(&Load, &Unload)
