// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: a script language's plugin brings File > New's entries for its scripts. An
// editor registers its creators at start, before a project's plugins load, so a language a project
// enables adds its own as it loads (through the pipeline's plugin services) and takes them back as
// it unloads.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import pipeline.core;
import pipeline.importer;
import script.pipeline;
import engine.project;

using namespace foundation::core;

#if defined(HAS_PLUGIN_ANGELSCRIPT) // the plugin this suite links as an editor does
TEST_CASE("plugins: a language plugin adds File > New's script entries as it loads and takes them back as it unloads")
{
    pipeline::RegisterScriptAssets();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    pipeline::PipelinePluginServices services(DefaultAllocator(), pipeline::PipelineRegistries{&builders, &importers, &creators});
    const auto scriptEntries = [&creators]()
    {
        usize count = 0;
        for (const pipeline::AssetCreator& creator : creators.All())
            if (creator.type == &pipeline::ScriptClassAsset::StaticType() && creator.label.AsView().StartsWith(u8"AngelScript "))
                ++count;
        return count;
    };
    CHECK(scriptEntries() == 0u);
    {
        foundation::runtime::SubsystemManager subsystems(DefaultAllocator());
        foundation::runtime::PluginHost host(DefaultAllocator(), subsystems, foundation::runtime::HostRole::Editor,
                                             engine::project::kEngineVersionString);
        REQUIRE(services.Attach(host));
        REQUIRE(host.AddLinked(foundation::runtime::LinkedPluginList()).IsOk());
        foundation::runtime::EnabledPlugin entry;
        entry.id = String(u8"angelscript");
        REQUIRE(host.Load(Span<const foundation::runtime::EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
        CHECK(scriptEntries() == 3u); // an entity, a scene and a run behavior
        host.Unload();
        CHECK(scriptEntries() == 0u);
    }
}
#endif
