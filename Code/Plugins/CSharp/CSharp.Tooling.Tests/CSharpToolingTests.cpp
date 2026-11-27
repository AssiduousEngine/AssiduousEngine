// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The csharp plugin's player hook: a project's C# is compiled into its own player, configured with
// the folder of its C# project; on the web with the bindings and the emsdk; never on Android, which
// the plugin's manifest leaves out.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content; // Instance: the class asset
import foundation.runtime;
import engine.project;
import editor.project;
import script.pipeline; // ScriptClassAsset: the project's C# class
import csharp.tooling;

using namespace foundation::core;

namespace
{
    void SaveText(StringView path, StringView text)
    {
        REQUIRE(CreateDirectories(PathParent(path)));
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }

    // A scratch project enabling `plugins`.
    UniquePtr<editor::EditorProject> MakeProject(StringView dir, Span<const StringView> plugins)
    {
        (void)RemoveDirectoryRecursive(dir);
        REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"CSharp Export", plugins).IsOk());
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        return project;
    }
}

TEST_CASE("csharp tooling: the project's C# project is found, an IDE's output passed over, and two are refused")
{
    const StringView dir = u8"scratch_csharp_tooling_find";
    UniquePtr<editor::EditorProject> project = MakeProject(dir, {});
    String folder;
    String refusal;
    CHECK_FALSE(csharp::FindProjectFolder(*project, folder, refusal)); // no C#

    const String sources = PathJoin(dir, u8"Sources");
    SaveText(PathJoin(sources.AsView(), u8"Game/Game.csproj").AsView(), u8"<Project />");
    SaveText(PathJoin(sources.AsView(), u8"Game/bin/Copy.csproj").AsView(), u8"<Project />"); // an IDE's build output
    SaveText(PathJoin(sources.AsView(), u8"Game/Actors/Mover.cs").AsView(), u8"public class Mover {}");
    REQUIRE(csharp::FindProjectFolder(*project, folder, refusal));
    CHECK(refusal.IsEmpty());
    CHECK(folder == project->SourceFile(u8"Game"));

    SaveText(PathJoin(sources.AsView(), u8"Tools.csproj").AsView(), u8"<Project />");
    REQUIRE(csharp::FindProjectFolder(*project, folder, refusal));
    CHECK(refusal.AsView().Contains(u8"2 C# projects"));
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("csharp tooling: the player hook builds a player with the C# folder, and the web's with the bindings and the emsdk")
{
    const StringView dir = u8"scratch_csharp_tooling_hook";
    const StringView languages[] = {u8"csharp"};
    UniquePtr<editor::EditorProject> project = MakeProject(dir, Span<const StringView>(languages, 1));
    SaveText(PathJoin(dir, u8"Sources/Game/Game.csproj").AsView(), u8"<Project />");
    const String folder = project->SourceFile(u8"Game");
    const String enabledIds[] = {String(u8"csharp")};
    const Span<const String> enabled(enabledIds, 1);

    // Not enabled there: the hook asks for nothing.
    csharp::WebBuild web;
    CHECK_FALSE(csharp::PlayerNeedFor(editor::PlayerHookContext{project.Get(), u8"Linux64", {}}, web).build);

    editor::PlayerNeed need = csharp::PlayerNeedFor(editor::PlayerHookContext{project.Get(), u8"Linux64", enabled}, web);
    CHECK(need.refusal.IsEmpty());
    CHECK(need.build);
    CHECK(need.reason.AsView().Contains(u8"C#"));
    REQUIRE(need.definitions.Size() == 1u);
    CHECK(need.definitions[0] == Format(u8"-DENGINE_PROJECT_CSHARP={}", folder));

    // The web: refused without the bindings or the emsdk, configured with both when they are there.
    const String tools = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_csharp_tooling_web");
    (void)RemoveDirectoryRecursive(tools.AsView());
    web.bindings = PathJoin(tools.AsView(), u8"DotNet/Engine.g.cs");
    web.emsdk = PathJoin(tools.AsView(), u8"emsdk");
    CHECK(csharp::PlayerNeedFor(editor::PlayerHookContext{project.Get(), u8"Web", enabled}, web).refusal.AsView().Contains(u8"bindings"));
    SaveText(web.bindings.AsView(), u8"// bindings");
    CHECK(csharp::PlayerNeedFor(editor::PlayerHookContext{project.Get(), u8"Web", enabled}, web).refusal.AsView().Contains(u8"emsdk"));
    SaveText(PathJoin(web.emsdk.AsView(), u8"emsdk_env.sh").AsView(), u8"# emsdk");
    need = csharp::PlayerNeedFor(editor::PlayerHookContext{project.Get(), u8"Web", enabled}, web);
    CHECK(need.refusal.IsEmpty());
    REQUIRE(need.definitions.Size() == 3u);
    CHECK(need.definitions[1] == Format(u8"-DDOTNET_BINDINGS={}", web.bindings));
    CHECK(need.definitions[2] == Format(u8"-DDOTNET_EMSDK={}", web.emsdk));

    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(tools.AsView());
}

// As an exporting host loads it: the plugin's Tooling part adds the hook to the export's services, the
// export's choice is a player of the project's own, Android is refused by the manifest, and the
// unload takes the hook back.
TEST_CASE("csharp tooling: loaded by an exporting host, the plugin makes the export build the project's player, never on Android")
{
    const StringView dir = u8"scratch_csharp_tooling_export";
    const StringView languages[] = {u8"csharp"};
    UniquePtr<editor::EditorProject> project = MakeProject(dir, Span<const StringView>(languages, 1));
    SaveText(PathJoin(dir, u8"Sources/Game/Game.csproj").AsView(), u8"<Project />");
    pipeline::RegisterScriptAssets();
    foundation::content::Instance* mover = project->SourceDb().RootGroup()->CreateInstance(u8"Mover", pipeline::ScriptClassAsset::StaticType());
    REQUIRE(mover != nullptr);
    pipeline::ScriptClassAsset script;
    script.language = String(u8"csharp");
    REQUIRE(mover->WriteObject(script).IsOk());

    foundation::runtime::SubsystemManager subsystems(DefaultAllocator());
    editor::ExportPluginServices services(DefaultAllocator());
    {
        foundation::runtime::PluginHost host(DefaultAllocator(), subsystems, foundation::runtime::HostRole::Export,
                                             engine::project::kEngineVersionString);
        REQUIRE(services.Attach(host));
        REQUIRE(host.AddLinked(foundation::runtime::LinkedPluginList()).IsOk());
        REQUIRE(host.Load(Span<const foundation::runtime::EnabledPlugin>(project->Settings().plugins.Data(),
                                                                         project->Settings().plugins.Size()))
                    .loaded.Size() == 1u);
        CHECK(services.PlayerHookCount() == 1u);

        editor::ExportTemplate tmpl;
        tmpl.id = String(u8"test-template");
        tmpl.plugins.PushBack(editor::TemplatePlugin{String(u8"csharp"), String(u8"9.9.9")}); // carried: the hook decides
        const editor::detail::PlayerChoice choice = editor::detail::ChoosePlayer(*project, tmpl, u8"Linux64", &services);
        CHECK(choice.refusal.IsEmpty());
        CHECK(choice.build);
        CHECK(choice.reason.AsView().Contains(u8"C#"));
        REQUIRE_FALSE(choice.definitions.IsEmpty());
        CHECK(choice.definitions[0].AsView().StartsWith(u8"-DENGINE_PROJECT_CSHARP="));
        // Android: the manifest leaves it out, so csharp is not enabled there, and the C# class cannot run.
        CHECK(editor::detail::ChoosePlayer(*project, tmpl, u8"Android64", &services).refusal ==
              StringView(u8"script 'Mover' is written in csharp, and the project does not enable plugin 'csharp' on Android64"));
    }
    CHECK(services.PlayerHookCount() == 0u); // the host's unload took it back
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
