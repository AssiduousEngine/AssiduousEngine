// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The csharp plugin in a host. Its Engine part registers the backend on load and takes it back on
// unload, so a run finds C# only while the plugin is loaded. (A run outliving the unload is the
// registry's, shown with the languages that need no built assembly: Luau's and AngelScript's tests.)

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.script;

using namespace foundation::core;
using namespace foundation::runtime;
using namespace foundation::script;

TEST_CASE("csharp plugin: its backend is registered while it is loaded, and a run after its unload finds no C#")
{
    ScriptBackendRegistry& backends = ScriptBackendRegistry::Get();
    SubsystemManager subsystems(DefaultAllocator());
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(LinkedPluginList()).IsOk());
    CHECK(backends.FindByLanguage(u8"csharp") == nullptr); // linked, not enabled

    EnabledPlugin entry;
    entry.id = String(u8"csharp");
    REQUIRE(host.Load(Span<const EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
    const ScriptBackendDesc* backend = backends.FindByLanguage(u8"csharp");
    REQUIRE(backend != nullptr);
    CHECK(backends.FindByExtension(u8"cs") == backend);

    host.Unload();
    CHECK_FALSE(host.IsLoaded(u8"csharp"));
    CHECK(backends.FindByLanguage(u8"csharp") == nullptr);
}
