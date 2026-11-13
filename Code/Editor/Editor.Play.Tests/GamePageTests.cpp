// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Game tab from its markup (Data/Editor/Game.xml) on a headless host with no game running: the
// kit's transport (Play while stopped; Pause and Stop while it runs or waits; the state named; a
// Play waiting for the cook shown as waiting and cancelled by Stop), the resolutions in the
// dropdown, the phone preview kept with the project, and the debugger opened by its toggle; and a
// run's save, written for a game with no game script.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client; // IApplicationHost
import foundation.shell;
import foundation.graphics;
import foundation.settings;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.gui.viewport; // the game's view
import engine.scene;        // a run needs the scenes
import engine.gameinstance; // the run the tab plays
import editor.core;
import editor.play;
import engine.project;      // ProjectSettings: the game's quality levels
import engine.render;       // RenderQualityLevel: the level the Game tab plays at
import foundation.vfs;
import foundation.content;
import foundation.resource;

using namespace foundation::core;
using namespace editor;
namespace runtime = foundation::runtime;
namespace gui = foundation::gui;

namespace
{
    // A host with an empty context: no renderer, no window, no game.
    class BareHost final : public runtime::IApplicationHost
    {
    public:
        ~BareHost() override { m_context.Shutdown(); }
        runtime::SubsystemManager& Subsystems() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::core::TaskPool& Tasks() noexcept override
        {
            static foundation::core::TaskPool tasks(foundation::core::DefaultAllocator(), 0u); // runs what it is given when pumped
            return tasks;
        }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::SubsystemManager m_context{DefaultAllocator()};
    };

    // A host whose context has the scenes a run starts in, and nothing else.
    class SceneHost final : public runtime::IApplicationHost
    {
    public:
        SceneHost()
        {
            (void)m_context.AddSubsystem<engine::scene::SceneSubsystem>();
            m_context.Startup();
        }
        ~SceneHost() override { m_context.Shutdown(); }
        runtime::SubsystemManager& Subsystems() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::core::TaskPool& Tasks() noexcept override
        {
            static foundation::core::TaskPool tasks(foundation::core::DefaultAllocator(), 0u);
            return tasks;
        }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::SubsystemManager m_context{DefaultAllocator()};
    };

    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(TEST_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    struct Bed
    {
        String dir = String(u8"scratch_game_page_project");
        UniquePtr<EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()};
        EditorContext context{DefaultAllocator()};
        BareHost host;
        UniquePtr<GameEditorPage> page;

        Bed()
        {
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            GameEditorPage::Services services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            page = MakeUnique<GameEditorPage>(DefaultAllocator(), context, host, Move(services), nullptr, nullptr, u8"game-page");
        }
        ~Bed()
        {
            page->OnClose();
            page.Reset();
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        template <typename T>
        T* Find(StringView id) { return FindId<T>(*page->Content(), id); }
    };
}

TEST_CASE("game tab: the kit's transport over a stopped run; a Play waiting for the cook says so and Stop cancels it")
{
    Bed bed;
    auto* play = bed.Find<gui::ButtonBase>(u8"game-play");
    auto* pause = bed.Find<gui::ButtonBase>(u8"game-pause");
    auto* stop = bed.Find<gui::ButtonBase>(u8"game-stop");
    auto* state = bed.Find<gui::Label>(u8"game-state");
    auto* cook = bed.Find<gui::Label>(u8"game-cook");
    REQUIRE(play != nullptr);
    REQUIRE(pause != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(state != nullptr);
    REQUIRE(cook != nullptr);
    CHECK(play->IsEnabled());
    CHECK_FALSE(pause->IsEnabled());
    CHECK_FALSE(stop->IsEnabled());
    CHECK(state->GetText() == StringView(u8"Stopped"));
    CHECK_FALSE(cook->IsDisplayed());

    // The cook is busy: Play waits for it, and says so.
    bed.context.CookBusy = []() { return true; };
    (void)play->HandleActivate();
    bed.page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(bed.page->IsStarting());
    CHECK(cook->IsDisplayed());
    CHECK_FALSE(play->IsEnabled());
    CHECK(stop->IsEnabled());
    (void)stop->HandleActivate();
    bed.page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(bed.page->IsStarting());
    CHECK_FALSE(cook->IsDisplayed());
    CHECK(play->IsEnabled());
}

TEST_CASE("game tab: the resolutions in the dropdown, a pick and the phone kept with the project; the debugger beside the game by its toggle, its share kept")
{
    Bed bed;
    auto* resolution = bed.Find<gui::ComboBox>(u8"game-resolution");
    REQUIRE(resolution != nullptr);
    const Span<const GameResolutionChoice> choices = bed.page->ResolutionChoices();
    REQUIRE(choices.Size() >= 2u);
    CHECK(resolution->ItemCount() == choices.Size());
    CHECK(resolution->GetSelectedText() == choices[0].label.AsView()); // the project's entry
    resolution->SelectByUser(1);
    CHECK(bed.page->ResolutionKey() == choices[1].key.AsView());
    const GamePageSettings* saved = bed.store.Find<GamePageSettings>();
    REQUIRE(saved != nullptr);
    CHECK(saved->resolutionKey == choices[1].key);

    auto* phone = bed.Find<gui::ButtonBase>(u8"game-phone");
    REQUIRE(phone != nullptr);
    bed.page->SelectPhonePreview(true);
    CHECK(phone->IsChecked());
    CHECK(bed.page->PhonePreview());
    CHECK(bed.store.Find<GamePageSettings>()->phonePreview);

    // The debugger stays out of the split until its toggle puts it beside the game, and it comes
    // back at the share it was given.
    auto* stage = bed.Find<gui::Splitter>(u8"game-stage");
    auto* toggle = bed.Find<gui::ButtonBase>(u8"game-debugger-toggle");
    REQUIRE(stage != nullptr);
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(bed.page->DebuggerShown()); // it takes room the game wants
    CHECK(stage->ChildCount() >= 1u);
    CHECK(bed.Find<gui::Node>(u8"game-debugger") == nullptr);
    (void)toggle->HandleActivate();
    CHECK(toggle->IsChecked());
    CHECK(bed.page->DebuggerShown());
    auto* debuggerState = bed.Find<gui::Label>(u8"game-debugger-state");
    REQUIRE(debuggerState != nullptr);
    CHECK(debuggerState->GetText() == StringView(u8"Not running"));
    const f32 given[2] = {0.6f, 0.4f};
    stage->SetWeights(Span<const f32>(given, 2u));
    (void)toggle->HandleActivate();
    CHECK_FALSE(bed.page->DebuggerShown());
    (void)toggle->HandleActivate();
    REQUIRE(stage->GetWeights().Size() == 2u);
    CHECK(stage->GetWeights()[1] == doctest::Approx(0.4f));
}

TEST_CASE("game tab: tabs side by side are told apart: Game, then Game 2, Game 3")
{
    CHECK(GameEditorPage::TitleFor(u8"game-page") == StringView(u8"Game"));
    CHECK(GameEditorPage::TitleFor(u8"game-page-1") == StringView(u8"Game 2"));
    CHECK(GameEditorPage::TitleFor(u8"game-page-2") == StringView(u8"Game 3"));
    Bed bed; // the bed's tab is the first one
    CHECK(bed.page->Title() == StringView(u8"Game"));
}

TEST_CASE("game tab: Shift+F1 gives the keyboard back: the game view lets the focus go, and captures no more until it is clicked")
{
    Bed bed;
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{800.0f, 600.0f});
    root->AddChild(bed.page->Content().Get());
    (void)root->UpdateLayout();
    gui::Viewport* view = bed.page->View();
    REQUIRE(view != nullptr);
    // As a running game's view: focused (by a click) and capturing every key.
    view->RequestFocus();
    view->SetCapturesAllKeys(true);
    REQUIRE(view->IsFocused());
    root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(gui::KeyCode::F1), gui::KeyModShift);
    CHECK_FALSE(view->GetCapturesAllKeys());
    CHECK_FALSE(view->IsFocused()); // the game reads keys only while its view has the focus
    // The next frame does not arm it again while the view is not focused.
    bed.page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(view->GetCapturesAllKeys());
    root->RemoveChild(bed.page->Content().Get());
}

TEST_CASE("game tab: a run with no game script still keeps its save, written to the project's Editor/ folder when it stops")
{
    // A game whose scripts are all on its scenes and entities saves through the same Save facade
    // as a game script; its save was only named for a run with a startup script, so it lasted that
    // run alone.
    const String dir(u8"scratch_game_page_save");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->Settings().startupScriptId.IsNil());
    foundation::settings::Settings store{DefaultAllocator()};
    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    context.SetProjectEditorSettings(&store);
    SceneHost host;
    engine::runtime::GameInstance instance;
    {
        GameEditorPage::Services services;
        services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
        GameEditorPage page(context, host, Move(services), nullptr, &instance, u8"game-page");
        page.Play();
        page.OnUpdate(host, 1.0f / 60.0f);
        REQUIRE(page.IsRunning());
        REQUIRE(instance.Saves().IsOpen());
        instance.Saves().Values().SetInt(u8"best.level", 7);
        instance.Saves().MarkChanged();
        page.Stop();
        page.OnUpdate(host, 1.0f / 60.0f);
        page.OnClose();
    }
    const String path = PathJoin(project->EditorStateRoot().AsView(), u8"P.save.xml");
    CHECK(FileExists(path.AsView()));

    // The next run reads it back.
    engine::runtime::RunSave again(DefaultAllocator());
    again.Open(path.AsView());
    CHECK(again.Values().GetInt(u8"best.level", 0) == 7);

    context.SetProjectEditorSettings(nullptr);
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir.AsView());
}

TEST_CASE("game tab: its run offers the project's levels to scripts; a script's choice moves the quality dropdown")
{
    // RenderQuality in a Game tab: the run lists the project's levels and reads the one the tab
    // plays at; a script's set picks that level in the dropdown, for the session, as the user would.
    engine::render::RegisterRenderProfileResources();
    const String dir(u8"scratch_game_page_quality_choice");
    const String levelsDir(u8"scratch_game_page_quality_choice_db");
    (void)RemoveDirectoryRecursive(dir.AsView());
    (void)RemoveDirectoryRecursive(levelsDir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    {
        foundation::vfs::NativeFileSystem mount(levelsDir.AsView(), DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        const auto write = [&db](StringView name) -> Guid
        {
            engine::render::RenderQualitySource source;
            source.values.label = String(name);
            foundation::content::Instance* instance = db.RootGroup()->CreateInstance(name, engine::render::RenderQualitySource::StaticType());
            REQUIRE(instance != nullptr);
            REQUIRE(instance->WriteObject(source).IsOk());
            return instance->Id();
        };
        const Guid low = write(u8"Low");
        const Guid high = write(u8"High");
        engine::render::RenderQualityFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager resources(DefaultAllocator(), db);
        resources.AddFactory(&factory);
        project->Settings().qualityLevelIds.PushBack(low);
        project->Settings().qualityLevelIds.PushBack(high);
        project->Settings().defaultQualityId = high;

        foundation::settings::Settings store{DefaultAllocator()};
        EditorContext context{DefaultAllocator()};
        context.SetProject(project.Get());
        context.SetProjectEditorSettings(&store);
        context.SetResources(&resources);
        SceneHost host;
        engine::runtime::GameInstance instance;
        {
            GameEditorPage::Services services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            GameEditorPage page(context, host, Move(services), nullptr, &instance, u8"game-page");
            page.Play();
            page.OnUpdate(host, 1.0f / 60.0f);
            REQUIRE(page.IsRunning());

            engine::render::RenderQualityChoice* choice = instance.Get<engine::render::RenderQualityChoice>();
            REQUIRE(choice != nullptr);
            REQUIRE(choice->levels.Size() == 2u);
            CHECK(choice->levels[0].values.label == StringView(u8"Low"));
            CHECK(choice->levels[1].values.label == StringView(u8"High"));
            CHECK(choice->current == high); // the dropdown's preset entry: the project's starting level

            // A script's choice is the dropdown's.
            CHECK(choice->Choose(u8"Low"));
            CHECK(page.QualityKey() == Format(u8"{}", low).AsView());
            page.OnUpdate(host, 1.0f / 60.0f);
            CHECK(choice->current == low);
            CHECK(GameQualityLevel(project->Settings(), page.PlayedQualityLevel()) == low);

            // The user's pick back to the preset's reaches the scripts.
            CHECK(page.SelectQuality(kPresetQualityKey));
            page.OnUpdate(host, 1.0f / 60.0f);
            CHECK(choice->current == high);
            page.Stop();
            page.OnClose();
        }
        CHECK(instance.Get<engine::render::RenderQualityChoice>() == nullptr); // the page's, gone with it
        context.SetResources(nullptr);
        context.SetProjectEditorSettings(nullptr);
        context.SetProject(nullptr);
    }
    project.Reset();
    (void)RemoveDirectoryRecursive(dir.AsView());
    (void)RemoveDirectoryRecursive(levelsDir.AsView());
}

TEST_CASE("game page: the Game tab plays at the project's starting quality level")
{
    // The player starts at the project's level; the Game tab plays the game at the same one.
    engine::render::RegisterRenderProfileResources();
    const StringView dir = u8"scratch_game_page_quality_db";
    (void)RemoveDirectoryRecursive(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        engine::render::RenderQualitySource low;
        low.values.label = String(u8"Low");
        low.values.msaaSamples = 1;
        low.values.antialiasing = engine::render::AaMode::FXAA;
        low.values.ambientOcclusion = engine::render::QualityTier::Off;
        auto* lowInstance = db.RootGroup()->CreateInstance(u8"low", engine::render::RenderQualitySource::StaticType());
        REQUIRE(lowInstance->WriteObject(low).IsOk());
        engine::render::RenderQualityFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);

        // No levels: the engine's own highest.
        engine::project::ProjectSettings settings;
        CHECK(editor::GameQuality(settings, &manager).msaaSamples == 4u);
        // The project's level, read through the editor's resources.
        settings.qualityLevelIds.PushBack(lowInstance->Id());
        const engine::render::RenderQualitySettings quality = editor::GameQuality(settings, &manager);
        CHECK(quality.label == StringView(u8"Low"));
        CHECK(quality.msaaSamples == 1u);
        CHECK(quality.antialiasing == engine::render::AaMode::FXAA);
        CHECK(quality.ambientOcclusion == engine::render::QualityTier::Off);
        // A level the tab chose (its dropdown's, or the previewed preset's) while the project lists
        // it; one it does not list is ignored for the project's starting level.
        settings.qualityLevelIds.PushBack(Guid{0x93, 0x94});
        CHECK(editor::GameQuality(settings, &manager, lowInstance->Id()).label == StringView(u8"Low"));
        CHECK(editor::GameQuality(settings, &manager, Guid{0x95, 0x96}).label == StringView(u8"High")); // unlisted: the last, unresolved
        settings.qualityLevelIds.PopBack();
        // Without the resources, or a level that does not resolve: the engine's own highest.
        CHECK(editor::GameQuality(settings, nullptr).msaaSamples == 4u);
        settings.qualityLevelIds[0] = Guid{0x91, 0x92};
        CHECK(editor::GameQuality(settings, &manager).msaaSamples == 4u);
    }
    (void)RemoveDirectoryRecursive(dir);
}
