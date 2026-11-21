// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the dialogue plugin's Editor part in an editor session. Opening a project that
// enables the plugin adds the cast's and the dialogue assets' pages; a cast opens in its page, its
// speakers a list by id beside the chosen one's rows, where a speaker is added, undone and removed,
// and a dialogue assets list opens in its; closing the project removes the pages again.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import pipeline.core;
import engine.project;
import engine.defaultapp;
import editor.project;
import editor.core;
import editor.app;
import dialogue.pipeline;
import dialogue.editor;

using namespace foundation::core;
namespace runtime = foundation::runtime;
namespace app = editor::app;

namespace
{
    class SessionHost final : public runtime::IApplicationHost
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

TEST_CASE("plugins: in the editor, the dialogue's cast and dialogue assets open in their pages, which leave with the plugin")
{
    app::RegisterEditorProjectSettingsTypes();
    const StringView dir = u8"scratch_plugins_dialogue_editor";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"Dialogue Pages").IsOk());
    {
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        runtime::EnabledPlugin entry;
        entry.id = String(u8"dialogue");
        project->Settings().plugins.PushBack(Move(entry));
        REQUIRE(project->SaveSettings().IsOk());
    }

    SessionHost host;
    engine::runtime::DefaultApplication embedded;
    embedded.Configure(host);
    editor::EditorContext context{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    editor::EditorCookService cook;
    editor::ThumbnailService thumbnails;
    app::AssetPageServicesSource pageServices; // pages with no window: enough to make and close them
    app::EditorSession::Services services;
    services.context = &context;
    services.cook = &cook;
    services.builders = &builders;
    services.thumbnails = &thumbnails;
    services.embeddedHost = &host;
    services.embeddedApp = &embedded;
    services.runtime = &host.Subsystems();
    services.linkedPlugins = runtime::LinkedPluginList();
    services.pageServices = &pageServices;
    {
        app::EditorSession session(DefaultAllocator(), Move(services));
        REQUIRE(session.Open(dir, app::EditorSession::OpenOptions{}));
        REQUIRE(context.Plugins() != nullptr);
        REQUIRE(context.Plugins()->IsLoaded(u8"dialogue"));
        CHECK(context.Pages().FindFactory(dialogue::DialogueCastAsset::StaticType()) != nullptr);
        CHECK(context.Pages().FindFactory(dialogue::DialogueAssetListAsset::StaticType()) != nullptr);

        foundation::content::Group* root = context.Project()->SourceDb().RootGroup();
        REQUIRE(root != nullptr);
        foundation::content::Instance* cast = root->CreateInstance(u8"Cast", dialogue::DialogueCastAsset::StaticType());
        foundation::content::Instance* list = root->CreateInstance(u8"Staging", dialogue::DialogueAssetListAsset::StaticType());
        REQUIRE(cast != nullptr);
        REQUIRE(list != nullptr);
        dialogue::DialogueCastAsset castAsset;
        castAsset.speakers.EmplaceBack().id = String(u8"keeper");
        REQUIRE(cast->WriteObject(castAsset).IsOk());
        dialogue::DialogueAssetListAsset listAsset;
        REQUIRE(list->WriteObject(listAsset).IsOk());

        editor::EditorPage* castPage = context.OpenPage(*cast);
        REQUIRE(castPage != nullptr);
        CHECK(castPage->Title() == StringView(u8"Cast"));
        auto* speakers = static_cast<dialogue::DialogueCastPage*>(castPage); // the dialogue's factory made it
        CHECK(speakers->List() == String(u8"keeper"));
        CHECK(speakers->Chosen() == 0);
        speakers->Add();
        CHECK(speakers->List() == String(u8"keeper\nspeaker"));
        CHECK(speakers->Chosen() == 1);
        CHECK(castPage->IsDirty());
        castPage->Commands().Undo();
        CHECK(speakers->List() == String(u8"keeper"));
        speakers->Add();
        speakers->Add();
        CHECK(speakers->List() == String(u8"keeper\nspeaker\nspeaker_2"));
        speakers->Search(u8"_2");
        CHECK(speakers->List() == String(u8"speaker_2"));
        speakers->Search(u8"");
        speakers->Choose(1);
        CHECK(speakers->RemoveChosen());
        CHECK(speakers->List() == String(u8"keeper\nspeaker_2"));
        editor::EditorPage* listPage = context.OpenPage(*list);
        REQUIRE(listPage != nullptr);
        CHECK(listPage->Title() == StringView(u8"Staging"));
        CHECK(context.OpenPages().Size() == 2u);

        session.Close();
        CHECK(context.Plugins() == nullptr);
        CHECK(context.OpenPages().Size() == 0u);
        CHECK(context.Pages().FindFactory(dialogue::DialogueCastAsset::StaticType()) == nullptr);
        CHECK(context.Pages().FindFactory(dialogue::DialogueAssetListAsset::StaticType()) == nullptr);
    }
    context.Tasks().Finish();
    embedded.OnShutdown(host);
    (void)RemoveDirectoryRecursive(dir);
}
