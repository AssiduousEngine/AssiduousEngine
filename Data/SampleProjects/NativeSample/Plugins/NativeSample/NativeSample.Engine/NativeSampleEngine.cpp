// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// NativeSample.Engine's entry, the reference game's runtime code. Load is the one registration point:
// it registers the spin component's reflection, and through the engine's service adds the spin
// manager to every scene, live ones included, and the package's domain, whose facade gives scripts
// the component (a host without the service, such as the cook, offers none, and neither is added).
// What it adds is recorded and taken out again when the plugin unloads (a reload, the project
// closing); Unload takes back the script names, which the recording does not hold.

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.scene;
import foundation.script.facades; // ComponentMethods, the script roots and names
import engine.domain;             // DomainModule
import engine.composition;        // EnginePluginServices
import nativesample.engine;

namespace nativesample
{
    REFLECT_VALUE(SpinComponent, "rtti::nativesample")
    {
        builder.Attribute("displayName", foundation::core::String(u8"Spin"))
            .Attribute("category", foundation::core::String(u8"Native Sample"))
            .Attribute("description",
                       foundation::core::String(u8"Turns the entity about an axis of its own while the scene "
                                                u8"simulates."))
            .Property<&SpinComponent::turnsPerSecond>("turnsPerSecond")
            .PropAttribute("displayName", foundation::core::String(u8"Turns Per Second"))
            .Property<&SpinComponent::axis>("axis")
            .PropAttribute("displayName", foundation::core::String(u8"Axis"));
        // SpinComponent.of(entity), addTo and removeFrom, as an engine component has them.
        builder.Apply(&foundation::script::ComponentMethods<SpinComponent>);
    }
}

namespace
{
    // The package's domain: its script facade, run as it is added.
    const engine::DomainModule kNativeSampleDomain{.id = u8"nativesample",
                                                   .registerScriptFacade = &nativesample::RegisterSpinScriptFacade};

    void Load(foundation::runtime::PluginContext& context)
    {
        // The component's reflection: what the inspector, component_add and component_set read.
        nativesample::RttiRegisterValue_SpinComponent();
        context.Types().Register(foundation::core::TypeOf<nativesample::SpinComponent>());
        // The game's behaviors, made by the run host from their Native Behavior assets.
        context.Types().Register(nativesample::Bobber::StaticType());
        context.Types().Register(nativesample::Showcase::StaticType());
        context.Types().Register(nativesample::NativeGame::StaticType());
        engine::EnginePluginServices* engine = context.Find<engine::EnginePluginServices>();
        if (engine == nullptr)
        {
            return;
        }
        foundation::scene::SceneModuleContributions::Contribution spin;
        spin.id = nativesample::kSpinContribution;
        spin.install = [](foundation::scene::Scene& scene) { scene.AddSystem<nativesample::SpinManager>(); };
        spin.systemType = foundation::core::TypeOf<nativesample::SpinManager>().id;
        engine->AddSceneManager(spin);
        (void)engine->AddDomain(kNativeSampleDomain);
        LOG_INFO(u8"Game", u8"NativeSample loaded");
    }

    void Unload(foundation::runtime::PluginContext&) { nativesample::RemoveSpinScriptFacade(); }
}

PLUGIN_PART(&Load, &Unload)
