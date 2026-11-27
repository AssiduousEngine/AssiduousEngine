// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Terrain.Engine's entry. Load registers the cooked terrain, splatmap and vegetation mask types in
// every host (a cook reads them as a player does) and, where the engine offers its service, adds the
// terrain and vegetation managers to every scene, live ones included, and their domains: the
// components' reflection and the resources' factories. A host that renders gets TerrainSubsystem,
// which registers the terrain renderer, and VegetationSubsystem. What it adds is recorded and taken
// out when the plugin unloads; Unload takes back the subsystems, which the recording does not hold.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.resource;
import foundation.scene;
import terrain.foundation;
import engine.domain;      // DomainModule
import engine.composition; // EnginePluginServices
import engine.render;      // RenderSubsystem: the renderers need one
import terrain.engine;

namespace
{
    const foundation::resource::ResourceModule* const kTerrainResources[] = {&terrain::kTerrainResourceModule};
    const engine::DomainModule kTerrainDomain{
        .id = u8"terrain",
        .registerReflection = &terrain::RegisterTerrainComponentReflection,
        .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{kTerrainResources, 1}};
    const foundation::resource::ResourceModule* const kVegetationResources[] = {&terrain::kVegetationResourceModule};
    const engine::DomainModule kVegetationDomain{
        .id = u8"vegetation",
        .registerReflection = &terrain::RegisterVegetationComponentReflection,
        .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{kVegetationResources, 1}};

    void Load(foundation::runtime::PluginContext& context)
    {
        terrain::RegisterTerrainResourceTypes();
        terrain::RegisterSplatmapResourceTypes();
        terrain::RegisterVegetationMaskResourceTypes();
        if (engine::EnginePluginServices* services = context.Find<engine::EnginePluginServices>())
        {
            foundation::scene::SceneModuleContributions::Contribution terrain;
            terrain.id = u8"terrain";
            terrain.install = &terrain::AddTerrainSceneManagers;
            terrain.registerReflection = &terrain::RegisterTerrainComponentReflection;
            terrain.systemType = foundation::core::TypeOf<terrain::TerrainComponentManager>().id;
            services->AddSceneManager(terrain);
            foundation::scene::SceneModuleContributions::Contribution vegetation;
            vegetation.id = u8"vegetation";
            vegetation.install = &terrain::AddVegetationSceneManagers;
            vegetation.registerReflection = &terrain::RegisterVegetationComponentReflection;
            vegetation.systemType = foundation::core::TypeOf<terrain::TerrainVegetationComponentManager>().id;
            services->AddSceneManager(vegetation);
            (void)services->AddDomain(kTerrainDomain);
            (void)services->AddDomain(kVegetationDomain);
        }
        if (context.Subsystems().GetSubsystem<engine::render::RenderSubsystem>() != nullptr)
        {
            context.Subsystems().AddSubsystem<terrain::TerrainSubsystem>();
            context.Subsystems().AddSubsystem<terrain::VegetationSubsystem>();
        }
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        context.Subsystems().RemoveSubsystem<terrain::VegetationSubsystem>();
        context.Subsystems().RemoveSubsystem<terrain::TerrainSubsystem>();
    }
}

PLUGIN_PART(&Load, &Unload)
