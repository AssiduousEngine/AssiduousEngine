// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the engine's official dialogue plugin, linked here as the engine's hosts link
// it. Its manifest reads and passes the host's checks, an editor links its four parts, and a
// project that enables it loads it: the project's settings for it read into its settings type, its
// domain added to the engine's composition (the factories for a cooked dialogue file and cast),
// a runner in every run whose flags live in the run's save, and all of it taken out again when it
// unloads.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import engine.project;
import engine.defaultapp;
import engine.domain;
import engine.composition;
import engine.run;
import dialogue.engine;

using namespace foundation::core;
namespace runtime = foundation::runtime;

namespace
{
    constexpr char8_t kDialogue[] = u8"dialogue";

    [[nodiscard]] bool HasDomain(StringView id)
    {
        for (const engine::DomainModule* module : engine::FullComposition().Modules())
        {
            if (module->id == id)
            {
                return true;
            }
        }
        return false;
    }

    // A headless application host: no shell, no graphics, no windows.
    class BareHost final : public runtime::IApplicationHost
    {
    public:
        runtime::SubsystemManager& Subsystems() noexcept override { return m_subsystems; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        TaskPool& Tasks() noexcept override
        {
            static TaskPool tasks(DefaultAllocator(), 0u);
            return tasks;
        }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&,
                                                       const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::SubsystemManager m_subsystems{DefaultAllocator()};
    };
}

TEST_CASE("plugins: the dialogue plugin is linked here with its four parts, and its manifest passes the host's checks")
{
    const runtime::LinkedPlugin* found = nullptr;
    for (const runtime::LinkedPlugin& linked : runtime::LinkedPluginList())
    {
        runtime::PluginManifest manifest;
        MemoryStream stream;
        const StringView xml{linked.manifestXml};
        (void)stream.Write(xml.Data(), xml.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        REQUIRE(runtime::ReadPluginManifest(stream, manifest).IsOk());
        if (manifest.id != StringView(kDialogue))
        {
            continue;
        }
        found = &linked;
        for (runtime::PluginPartKind kind : runtime::kPluginPartKinds)
        {
            CHECK(manifest.HasPart(kind));
        }
        String why;
        CHECK(runtime::CheckPluginManifest(manifest, engine::project::kEngineVersionString, why));
        bool here = false;
        for (const runtime::PluginPlatform& platform : manifest.platforms)
        {
            here = here || platform.platform == GetHostPlatformName();
        }
        CHECK(here);
    }
    REQUIRE(found != nullptr);
    CHECK(found->partCount == 4u); // an editor's role links every part
}

TEST_CASE("plugins: a project that enables dialogue loads it, its settings read from the project's entry, its domain "
          "added and a runner in every run")
{
    BareHost host;
    engine::runtime::DefaultApplication embedded;
    embedded.Configure(host);
    runtime::PluginHost* plugins = embedded.CreatePluginHost(runtime::HostRole::Player);
    REQUIRE(plugins != nullptr);

    dialogue::DialogueSettings chosen;
    chosen.providedFlags.PushBack(String(u8"seen_*"));
    chosen.stockBox = true;
    runtime::EnabledPlugin entry;
    entry.id = String(kDialogue);
    REQUIRE(runtime::WritePluginSettings(chosen, entry.settings, DefaultAllocator()).IsOk());
    engine::project::ProjectSettings settings;
    settings.plugins.PushBack(Move(entry));
    const runtime::PluginLoadReport report =
        engine::project::LoadProjectPlugins(*plugins, runtime::LinkedPluginList(), settings);
    REQUIRE(report.loaded.Size() == 1u);
    CHECK(plugins->IsLoaded(kDialogue));

    const TypeInfo& type = dialogue::DialogueSettings::StaticType();
    CHECK(GlobalTypeRegistry().FindById(type.id) != nullptr);
    ISerializable* held = plugins->Settings(kDialogue);
    REQUIRE(held != nullptr);
    REQUIRE(held->GetType() == &type);
    const auto* read = static_cast<const dialogue::DialogueSettings*>(held);
    CHECK(plugins->SettingsRead(kDialogue));
    REQUIRE(read->providedFlags.Size() == 1u);
    CHECK(read->providedFlags[0] == StringView(u8"seen_*"));
    CHECK(read->stockBox);
    CHECK(read->flagPrefix == StringView(u8"flag.")); // what the entry left out keeps its default

    // The domain: its resource types, which the composition builds the factories from.
    CHECK(HasDomain(kDialogue));
    const TypeId cooked = dialogue::DialogueSource::StaticType().id;
    const TypeId castCooked = dialogue::DialogueCastSource::StaticType().id;
    CHECK(GlobalTypeRegistry().FindById(cooked) != nullptr);
    CHECK(GlobalTypeRegistry().FindById(castCooked) != nullptr);

    // Every run gets a runner, its flags in the run's save under the settings' prefix.
    {
        engine::runtime::ApplicationRun run;
        auto* runner = run.Get<dialogue::DialogueRunService>();
        REQUIRE(runner != nullptr);
        CHECK_FALSE(runner->IsPlaying());
        runner->SetFlag(u8"met_keeper", true);
        CHECK(run.Saves().Values().GetBool(u8"flag.met_keeper", false));
        CHECK(run.Saves().HasChanges());
        CHECK(runner->Flag(u8"met_keeper"));
        CHECK_FALSE(runner->Resume()); // nothing saved to resume
        CHECK_FALSE(runner->Play(Guid{}, u8"gate")); // no such file: refused
    }
    engine::runtime::ApplicationRun live;
    REQUIRE(live.Get<dialogue::DialogueRunService>() != nullptr);

    embedded.DestroyPluginHost();
    CHECK(live.Get<dialogue::DialogueRunService>() == nullptr); // its code is going: out of the live run
    CHECK(GlobalTypeRegistry().FindById(type.id) == nullptr); // taken out with the plugin
    CHECK_FALSE(HasDomain(kDialogue));
    CHECK(GlobalTypeRegistry().FindById(cooked) == nullptr);
    embedded.OnShutdown(host);
}
