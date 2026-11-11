// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App: EditorExport. An export needs a project and only one runs at a time. The templates
// folder is the user's override when there is one. A resource report with no project writes a line
// to the log rather than crashing. A project's saved presets are read back when it opens again. A
// cook that fails an asset stops the export. An Android app signed with a keystore asks for the
// keystore's password when the environment lacks it.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.settings;
import foundation.content;
import pipeline.core;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

namespace
{
    // An asset whose cook always fails.
    class BrokenAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(BrokenAsset, pipeline::Asset)
    };

    class BrokenBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override { return &BrokenAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &BrokenAsset::StaticType(); }
        [[nodiscard]] Status Build(const pipeline::Asset&, pipeline::AssetBuildContext&) override { return Status{ErrorCode::Unknown}; }
    };
}

RTTI_DEFINE_OBJECT(BrokenAsset, "rtti::editor::app::test")

TEST_CASE("export: one asked for with no project says to open one; the templates follow the user's override")
{
    RegisterEditorSettingsTypes();
    EditorContext context{DefaultAllocator()};
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    EditorCookService cook;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    foundation::settings::Settings user(DefaultAllocator());

    app::EditorExport::Services services;
    services.context = &context;
    services.cook = &cook;
    services.tasks = &context.Tasks();
    services.lane = context.BuildLane();
    services.builders = &builders;
    services.userSettings = &user;
    app::EditorExport exporter(DefaultAllocator(), Move(services));

    exporter.Run(u8"Linux", false);
    CHECK(said == String(u8"Open a project first."));
    CHECK_FALSE(exporter.IsPending());

    user.Section<EditorExportSettings>().templatesRoot = String(u8"/shared/templates");
    CHECK(exporter.TemplatesRoot() == String(u8"/shared/templates"));

    app::EditorExport::ReportResourceMemory(nullptr); // no project: said in the log
    context.Tasks().Finish();
}

TEST_CASE("export: a project's presets are read back when it opens again")
{
    RegisterEditorSettingsTypes();
    const String dir(u8"scratch_export_presets_project");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    EditorContext context{DefaultAllocator()};
    EditorCookService cook;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    const auto make = [&]()
    {
        app::EditorExport::Services services;
        services.context = &context;
        services.cook = &cook;
        services.tasks = &context.Tasks();
        services.lane = context.BuildLane();
        services.builders = &builders;
        return MakeUnique<app::EditorExport>(DefaultAllocator(), DefaultAllocator(), Move(services));
    };
    {
        UniquePtr<app::EditorExport> first = make();
        first->SetProject(project.Get());
        const usize defaults = first->Presets().Count();
        ExportPreset deck;
        deck.name = String(u8"Steam Deck");
        deck.platform = String(u8"Linux64");
        (void)first->Presets().Add(deck);
        first->SavePresets();
        REQUIRE(first->Presets().Count() == defaults + 1);
    }
    // The next session: a new export over the same project.
    UniquePtr<app::EditorExport> again = make();
    again->SetProject(project.Get());
    bool found = false;
    for (const ExportPreset& preset : again->Presets().Set().presets)
        found = found || preset.name == StringView(u8"Steam Deck");
    CHECK(found);
    project.Reset();
    context.Tasks().Finish();
    (void)RemoveDirectoryRecursive(dir.AsView());
}

TEST_CASE("export: a cook that fails an asset stops the export, said, and no export job runs")
{
    RegisterEditorSettingsTypes();
    GlobalTypeRegistry().Register(BrokenAsset::StaticType());
    RegisterSerializable<BrokenAsset>();
    const String dir(u8"scratch_export_cook_failed");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    foundation::content::Instance* broken = project->SourceDb().RootGroup()->CreateInstance(u8"Broken", BrokenAsset::StaticType());
    REQUIRE(broken != nullptr);
    BrokenAsset asset;
    REQUIRE(broken->WriteObject(asset).IsOk());

    EditorContext context{DefaultAllocator()};
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    builders.Register(UniquePtr<pipeline::IAssetBuilder>(DefaultAllocator().New<BrokenBuilder>(), DefaultAllocator()));
    EditorCookService cook;
    cook.Initialize(*project, builders);
    app::EditorExport::Services services;
    services.context = &context;
    services.cook = &cook;
    services.tasks = &context.Tasks();
    services.lane = context.BuildLane();
    services.builders = &builders;
    app::EditorExport exporter(DefaultAllocator(), Move(services));
    exporter.SetProject(project.Get());

    exporter.Run(u8"Linux", false);
    REQUIRE(exporter.IsPending());
    for (u32 i = 0; i < 5000 && exporter.IsPending(); ++i)
    {
        cook.Update({});
        exporter.Pump();
        SleepMilliseconds(1);
    }
    CHECK_FALSE(exporter.IsPending());
    CHECK(said.AsView().StartsWith(u8"Export stopped: 1 asset(s) failed to cook"));
    CHECK_FALSE(context.Tasks().IsLaneBusy(context.BuildLane())); // no export job was submitted

    cook.Shutdown();
    project.Reset();
    context.Tasks().Finish();
    (void)RemoveDirectoryRecursive(dir.AsView());
}

TEST_CASE("export: an Android preset signed with a keystore asks for its password when the environment lacks it")
{
    RegisterEditorSettingsTypes();
    const String dir(u8"scratch_export_keystore_project");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    const Optional<String> was = GetEnvironmentVariable(kAndroidStorePasswordVariable);
    (void)SetEnvironmentVariable(kAndroidStorePasswordVariable, u8"");
    EditorContext context{DefaultAllocator()};
    EditorCookService cook;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    Array<app::NameRequest> asked;
    const auto make = [&]()
    {
        app::EditorExport::Services services;
        services.context = &context;
        services.cook = &cook;
        services.tasks = &context.Tasks();
        services.lane = context.BuildLane();
        services.builders = &builders;
        services.askName = [&asked](app::NameRequest request) { asked.PushBack(Move(request)); };
        UniquePtr<app::EditorExport> exporter = MakeUnique<app::EditorExport>(DefaultAllocator(), DefaultAllocator(), Move(services));
        exporter->SetProject(project.Get());
        return exporter;
    };
    {
        UniquePtr<app::EditorExport> first = make();
        ExportPreset phone;
        phone.name = String(u8"Phone");
        phone.platform = String(u8"Android64");
        phone.androidKeystore = String(u8"release.jks");
        (void)first->Presets().Add(phone);
        ExportPreset debugKey;
        debugKey.name = String(u8"Phone Test");
        debugKey.platform = String(u8"Android64");
        (void)first->Presets().Add(debugKey);
        first->SavePresets();
    }

    // The debug key needs no password: the export goes ahead.
    {
        UniquePtr<app::EditorExport> exporter = make();
        exporter->Run(u8"Phone Test", false);
        CHECK(asked.IsEmpty());
        CHECK(exporter->IsPending());
    }

    // The keystore's password is asked for, masked, and the export waits for it.
    UniquePtr<app::EditorExport> exporter = make();
    exporter->Run(u8"Phone", false);
    REQUIRE(asked.Size() == 1u);
    CHECK(asked[0].secret);
    CHECK_FALSE(exporter->IsPending());
    CHECK_FALSE(asked[0].refusal(u8"").IsEmpty());
    // Given, it is kept in the environment, where the export's build reads it, and the export runs.
    asked[0].accepted(u8"secret");
    const Optional<String> kept = GetEnvironmentVariable(kAndroidStorePasswordVariable);
    REQUIRE(kept.HasValue());
    CHECK(*kept == StringView(u8"secret"));
    CHECK(exporter->IsPending());
    CHECK(asked.Size() == 1u);

    (void)SetEnvironmentVariable(kAndroidStorePasswordVariable, was.HasValue() ? was->AsView() : StringView{});
    exporter.Reset();
    project.Reset();
    context.Tasks().Finish();
    (void)RemoveDirectoryRecursive(dir.AsView());
}
