// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Core tests: the cook service's observable lifecycle over a scratch project. A
// request runs on the worker and lands through Update (the revision bumps, OnCookFinished
// fires, the summary is readable), a request that arrives mid-cook is remembered and re-issued,
// and Shutdown joins whatever is in flight.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import pipeline.core;
import script.pipeline; // ScriptClassAsset and its builder: a script whose language is not loaded
import foundation.script.resource; // RegisterScriptResource (the builder's product type)
import foundation.content;
import editor.core;

using namespace foundation::core;
using namespace editor;

namespace
{
    // Pumps the service until `done` says so or ~5 s pass (a cook of nothing takes far less).
    template <typename Done>
    bool PumpUntil(EditorCookService& cook, Done done)
    {
        for (u32 i = 0; i < 5000; ++i)
        {
            cook.Update({});
            if (done())
            {
                return true;
            }
            SleepMilliseconds(1);
        }
        return false;
    }
}

TEST_CASE("cook-service: a request lands through Update - revision, OnCookFinished, the summary; "
          "a request mid-cook is remembered; Shutdown joins")
{
    (void)RemoveDirectoryRecursive(u8"cook_service_project");
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"cook_service_project", u8"Cooked").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), u8"cook_service_project");
    REQUIRE(project);
    pipeline::BuilderRegistry builders{DefaultAllocator()}; // nothing buildable: an empty plan

    EditorCookService cook;
    CHECK_FALSE(cook.IsReady());
    cook.Initialize(*project, builders);
    REQUIRE(cook.IsReady());
    CHECK(cook.IsIdle());
    CHECK(cook.Revision() == 0u);
    u32 finished = 0;
    cook.OnCookFinished = [&finished]() { ++finished; };

    // One cook: it runs on the worker and is observed only through Update on this thread.
    cook.RequestCook(false);
    CHECK_FALSE(cook.IsIdle());
    REQUIRE(PumpUntil(cook, [&cook]() { return cook.Revision() == 1u && cook.IsIdle(); }));
    CHECK(finished == 1u);
    const CookSummary& summary = cook.LastCookSummary();
    CHECK(summary.planned == 0u);
    CHECK(summary.cooked == 0u);
    CHECK(summary.failed == 0u);
    CHECK(cook.LastCookedProducts().Size() == 0u);

    // A request while one is in flight is remembered and re-issued: two cooks land.
    cook.RequestCook(false);
    cook.RequestCook(true); // arrives mid-cook (or just after the plan); it is not lost
    REQUIRE(PumpUntil(cook, [&cook]() { return cook.Revision() == 3u && cook.IsIdle(); }));
    CHECK(finished == 3u);

    // Shutdown joins whatever is running; a request after it is ignored.
    cook.RequestCook(false);
    cook.Shutdown();
    CHECK_FALSE(cook.IsReady());
    cook.RequestCook(false);
    CHECK(cook.IsIdle());
    project = nullptr;
    (void)RemoveDirectoryRecursive(u8"cook_service_project");
}

// A script whose language's plugin is not loaded is skipped, not failed: the summary counts it
// apart, and the Assets panel's badge says Skipped.
TEST_CASE("cook-service: a script in a language not loaded is skipped, and its badge says so")
{
    foundation::script::RegisterScriptResource();
    pipeline::RegisterScriptAssets();
    (void)RemoveDirectoryRecursive(u8"cook_service_skipped");
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"cook_service_skipped", u8"Skipped", {}).IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), u8"cook_service_skipped");
    REQUIRE(project);
    const StringView text = u8"anything\n";
    REQUIRE(WriteFile(project->SourceFile(u8"lost.lost").AsView(),
                      Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    foundation::content::Instance* instance =
        project->SourceDb().RootGroup()->CreateInstance(u8"Lost", pipeline::ScriptClassAsset::StaticType());
    REQUIRE(instance != nullptr);
    pipeline::ScriptClassAsset script;
    script.fileName = foundation::vfs::SourcePath(u8"lost.lost");
    script.language = String(u8"lostlang");
    REQUIRE(instance->WriteObject(script).IsOk());

    pipeline::BuilderRegistry builders{DefaultAllocator()};
    builders.Register(UniquePtr<pipeline::IAssetBuilder>(DefaultAllocator().New<pipeline::ScriptClassAssetBuilder>(), DefaultAllocator()));
    EditorCookService cook;
    cook.Initialize(*project, builders);
    REQUIRE(cook.IsReady());
    cook.RequestCook(false);
    REQUIRE(PumpUntil(cook, [&cook]() { return cook.Revision() == 1u && cook.IsIdle(); }));
    const CookSummary& summary = cook.LastCookSummary();
    CHECK(summary.skipped == 1u);
    CHECK(summary.failed == 0u);
    CHECK(cook.BadgeFor(*instance) == CookBadge::Skipped);
    cook.Shutdown();
    project = nullptr;
    (void)RemoveDirectoryRecursive(u8"cook_service_skipped");
}
