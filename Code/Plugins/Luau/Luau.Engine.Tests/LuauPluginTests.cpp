// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The luau plugin in a host. Its Engine part registers the backend on load and takes it back on
// unload. The plugin is linked into every host, so its code stays mapped after the unload: a
// manager made while it was loaded keeps running its scripts, and only a new run, which asks the
// registry, finds no Luau.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.script;

using namespace foundation::core;
using namespace foundation::runtime;
using namespace foundation::script;

namespace
{
    constexpr StringView kCounter = u8R"lua(
count = 0
function bump()
    count = count + 1
    return count
end
)lua";

    f64 Bump(IScriptContext& context)
    {
        Result<Variant> result = context.Call(u8"bump", Span<Variant>{});
        REQUIRE(result.HasValue());
        return result.Value().Get<f64>();
    }
}

TEST_CASE("luau plugin: a script running when the plugin unloads keeps running; a new run finds no Luau")
{
    ScriptBackendRegistry& backends = ScriptBackendRegistry::Get();
    SubsystemManager subsystems(DefaultAllocator());
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(LinkedPluginList()).IsOk());
    CHECK(backends.FindByLanguage(u8"luau") == nullptr); // linked, not enabled

    EnabledPlugin entry;
    entry.id = String(u8"luau");
    REQUIRE(host.Load(Span<const EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
    const ScriptBackendDesc* backend = backends.FindByLanguage(u8"luau");
    REQUIRE(backend != nullptr);
    RefPtr<IScriptManager> manager = backend->create(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(kCounter, u8"counter").IsOk());
    CHECK(Bump(*context) == doctest::Approx(1.0));

    host.Unload();
    CHECK_FALSE(host.IsLoaded(u8"luau"));
    CHECK(backends.FindByLanguage(u8"luau") == nullptr); // a new run cannot start Luau

    // The live context runs on with its state, and its manager still makes contexts.
    CHECK(Bump(*context) == doctest::Approx(2.0));
    RefPtr<IScriptContext> another = manager->CreateContext();
    REQUIRE(another->Load(kCounter, u8"counter").IsOk());
    CHECK(Bump(*another) == doctest::Approx(1.0));

    // Loading the plugin again gives the language back.
    REQUIRE(host.Load(Span<const EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
    CHECK(backends.FindByLanguage(u8"luau") != nullptr);
    host.Unload();
}
