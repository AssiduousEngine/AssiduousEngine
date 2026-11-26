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
import foundation.vfs;
import foundation.run;
import foundation.scene;
import dialogue.foundation;
import dialogue.engine;
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

TEST_CASE("plugins: in the editor, a dialogue file, the cast and the dialogue assets open in their pages, which leave with "
          "the plugin")
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

        // A dialogue file opens in the text page: its problems as the cook finds them, at their
        // lines, and its conversations in the outline. (With no window the page has no code editor
        // to type in; typing and its checks are the code pages' own.)
        CHECK(context.Pages().FindFactory(dialogue::DialogueAsset::StaticType()) != nullptr);
        const String text(u8"conversation gate\n"
                          u8"@ keeper: Who goes there?\n"
                          u8"conversation lane\n"
                          u8"@ stranger: Me.\n");
        const String path = context.Project()->SourceFile(u8"Dialogue/Gate.dialogue");
        REQUIRE(CreateDirectories(PathParent(path.AsView())));
        REQUIRE(WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
        foundation::content::Instance* file = root->CreateInstance(u8"Gate", dialogue::DialogueAsset::StaticType());
        REQUIRE(file != nullptr);
        dialogue::DialogueAsset fileAsset;
        fileAsset.fileName = foundation::vfs::SourcePath(u8"Dialogue/Gate.dialogue");
        fileAsset.cast = cast->Id();
        REQUIRE(file->WriteObject(fileAsset).IsOk());
        editor::EditorPage* filePage = context.OpenPage(*file);
        REQUIRE(filePage != nullptr);
        auto* textPage = static_cast<dialogue::DialogueTextPage*>(filePage);
        CHECK(textPage->SourceRead());
        CHECK(textPage->Outline() == StringView(u8"gate\nlane\n"));
        CHECK(textPage->OutlineLine(0) == 1u); // choosing a conversation goes to its header
        CHECK(textPage->OutlineLine(1) == 3u);
        REQUIRE(textPage->Found().Size() == 1u);
        CHECK(textPage->Found()[0].line == 3u); // the fourth line, from 0
        CHECK(textPage->Found()[0].message.AsView().Contains(u8"no speaker `stranger`"));
        CHECK(context.OpenPages().Size() == 3u);

        // Preview plays the text as it reads, here in a plain run and scene (the editor plays it in
        // the open scene page's Simulate); text with errors does not play.
        foundation::run::RunBase run(DefaultAllocator(), u8"preview");
        foundation::scene::Scene scene(DefaultAllocator(), u8"preview");
        scene.SetRun(&run);
        CHECK_FALSE(textPage->PlayPreview(scene, run)); // `stranger` is no speaker
        CHECK_FALSE(textPage->IsPreviewing());
        const String clean(u8"conversation talk\n"
                           u8"set seen\n"
                           u8"if seen\n"
                           u8"  @ keeper: Seen.\n"
                           u8"end\n"
                           u8"choice\n"
                           u8"> Yes.\n"
                           u8"> No.\n"
                           u8"end\n");
        const String cleanPath = context.Project()->SourceFile(u8"Dialogue/Talk.dialogue");
        REQUIRE(WriteFile(cleanPath.AsView(), Span<const byte>(reinterpret_cast<const byte*>(clean.Data()), clean.Size())).IsOk());
        foundation::content::Instance* talk = root->CreateInstance(u8"Talk", dialogue::DialogueAsset::StaticType());
        REQUIRE(talk != nullptr);
        dialogue::DialogueAsset talkAsset;
        talkAsset.fileName = foundation::vfs::SourcePath(u8"Dialogue/Talk.dialogue");
        talkAsset.cast = cast->Id();
        REQUIRE(talk->WriteObject(talkAsset).IsOk());
        auto* talkPage = static_cast<dialogue::DialogueTextPage*>(context.OpenPage(*talk));
        REQUIRE(talkPage != nullptr);
        REQUIRE(talkPage->PlayPreview(scene, run));
        REQUIRE(talkPage->PreviewRunner() != nullptr);
        CHECK(talkPage->PreviewRunner()->Waiting() == dialogue::DialogueWait::Line);
        CHECK(talkPage->PreviewRunner()->Flag(u8"seen")); // a flag of the preview's own: the run has no save
        CHECK(talkPage->PreviewState() == StringView(u8"A line shows: Advance goes on"));
        REQUIRE(talkPage->PreviewRunner()->Advance());
        CHECK(talkPage->PreviewRunner()->Waiting() == dialogue::DialogueWait::Choice);
        REQUIRE(talkPage->PreviewRunner()->Choose(1));
        CHECK(talkPage->PreviewState() == StringView(u8"Ended"));
        talkPage->StopPreview();
        CHECK_FALSE(talkPage->IsPreviewing());
        CHECK(talkPage->PreviewState() == StringView(u8"Not playing"));

        session.Close();
        CHECK(context.Plugins() == nullptr);
        CHECK(context.OpenPages().Size() == 0u);
        CHECK(context.Pages().FindFactory(dialogue::DialogueCastAsset::StaticType()) == nullptr);
        CHECK(context.Pages().FindFactory(dialogue::DialogueAssetListAsset::StaticType()) == nullptr);
        CHECK(context.Pages().FindFactory(dialogue::DialogueAsset::StaticType()) == nullptr);
    }
    context.Tasks().Finish();
    embedded.OnShutdown(host);
    (void)RemoveDirectoryRecursive(dir);
}
