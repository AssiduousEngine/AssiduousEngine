// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// What the navigation plugin adds to the scene editor: the zone's gizmo, the inspector's Bake
// Navigation action (taken back as the plugin unloads) and the navigation_bake MCP tool.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.content;
import foundation.physics; // MotionKind
import engine.physics;     // RigidBodyComponent (the static ground the bake collects)
import engine.navigation;
import navigation.pipeline; // NavigationZoneAsset
import foundation.gui.toolkit; // IPropertySource (InspectorExtras::AddSection)
import foundation.input;       // ScriptedInputSource (the page interface)
import editor.core;
import editor.camera;
import editor.viewporttools; // ViewportToolManager (the page interface)
import editor.scene;
import editor.mcp;          // PluginTools, ProjectSession
import editor.navigation;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
namespace scene = foundation::scene;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
    };
    Answer Call(McpServer& server, StringView tool, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}",
                                   tool, argumentsJson);
        LineOutcome outcome = server.HandleLine(line.AsView());
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        Answer answer;
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
        {
            answer.payload = json::Parse(text.AsView()).value;
        }
        else
        {
            answer.error = text;
        }
        return answer;
    }

    // A scene page with no window: its scene, its edit context and a simulation flag.
    class HeadlessScenePage final : public EditorPage, public ISceneEditorPage
    {
    public:
        HeadlessScenePage(StringView title, const Guid& asset)
            : EditorPage(DefaultAllocator()), m_title(title),
              m_scene(DefaultAllocator(), u8"headless"), m_edit(m_scene, Commands())
        {
            SetInstanceId(asset);
            Provide<ISceneEditorPage>(*this);
        }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override { return Status{}; }
        [[nodiscard]] SceneEditContext& EditContext() noexcept override { return m_edit; }
        void StartSimulation() override { m_simulating = true; }
        void StopSimulation() override { m_simulating = false; }
        void PauseSimulation(bool) override {}
        [[nodiscard]] bool IsSimulating() const noexcept override { return m_simulating; }
        [[nodiscard]] bool IsPaused() const noexcept override { return false; }
        [[nodiscard]] ViewportToolManager* ViewportTools() noexcept override { return nullptr; }
        [[nodiscard]] GizmoController* Gizmos() noexcept override { return nullptr; }
        [[nodiscard]] bool CameraOwnsInput() const noexcept override { return false; }
        [[nodiscard]] bool MarkersShown() const noexcept override { return true; }
        [[nodiscard]] EditorCamera* ViewportCamera() noexcept override { return nullptr; }
        bool FrameEntities(Span<const Guid>, bool) override { return false; }
        [[nodiscard]] Status RequestProjectThumbnail() override { return Status{ErrorCode::NotSupported}; }
        [[nodiscard]] Status RequestViewportCapture(StringView) override { return Status{ErrorCode::NotSupported}; }
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override { return m_capture; }
        bool BeginScriptedInput(UniquePtr<foundation::input::ScriptedInputSource>) override { return false; }
        void EndScriptedInput() override {}
        [[nodiscard]] f64 ScriptedInputTime() const noexcept override { return -1.0; }
        [[nodiscard]] Float2 ViewportPixels() const noexcept override { return Float2{800.0f, 600.0f}; }
        void SetMarkersShown(bool) override {}
        [[nodiscard]] bool AnimationPanelShown() const noexcept override { return false; }
        void SetAnimationPanelShown(bool) override {}
        void EditAnimationClip(const Guid&, const Guid&) override {}
        [[nodiscard]] bool SequencePanelShown() const noexcept override { return false; }
        void SetSequencePanelShown(bool) override {}
        void EditSequence(const Guid&, const Guid&) override {}
        [[nodiscard]] Result<bool, String> PreviewSequence(const Guid&, Span<const String>, Span<const Guid>, f32) override { return true; }
        void CreatePrefabFromEntity(const Guid&) override {}
        void PickAndSpawnPrefab(const Guid&) override {}
        void ApplyInstanceToPrefab(const Guid&) override {}
        void RevertInstance(const Guid&) override {}

    private:
        String m_title;
        scene::Scene m_scene;
        SceneEditContext m_edit;
        ViewportCapture m_capture;
        bool m_simulating = false;
    };

    // The inspector as an extras provider sees it: one entity, its actions kept by label.
    class Extras final : public InspectorExtras
    {
    public:
        Extras(SceneEditContext& edit, EditorContext& context, Guid entity) : m_edit(&edit), m_context(&context), m_entity(entity) {}
        SceneEditContext& Edit() override { return *m_edit; }
        EditorContext* Context() override { return m_context; }
        Span<const Guid> Entities() const override { return Span<const Guid>(&m_entity, 1); }
        void AddNotice(StringView, Function<String()>) override {}
        void AddAction(StringView, StringView label, StringView, Function<void()> run) override
        {
            labels.PushBack(String(label));
            actions.PushBack(Move(run));
        }
        void AddSection(UniquePtr<foundation::gui::IPropertySource>, Function<void()>) override {}

        Array<String> labels;
        Array<Function<void()>> actions;

    private:
        SceneEditContext* m_edit;
        EditorContext* m_context;
        Guid m_entity;
    };
}

TEST_CASE("navigation-editor: the zone's gizmo draws its box when selected")
{
    GizmoRendererRegistry registry;
    editor::navigation::AddNavigationGizmos(registry);
    IGizmoRenderer* zone = registry.Find(&TypeOf<engine::navigation::NavMeshZoneComponent>());
    REQUIRE(zone != nullptr);
    CHECK_FALSE(zone->DrawWhenUnselected());
}

TEST_CASE("navigation-editor: a zone has a Bake Navigation action that says what it needs, and it goes with the plugin")
{
    scene::Scene level{DefaultAllocator()};
    engine::navigation::AddNavigationSceneManagers(level);
    EditorCommandStack commands;
    SceneEditContext edit(level, commands);
    EditorContext context{DefaultAllocator()};
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    const Guid zoneId = edit.CreateEntity(u8"Zone");
    level.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(edit.Resolve(zoneId));

    const TypeInfo& zoneType = TypeOf<engine::navigation::NavMeshZoneComponent>();
    const auto gather = [&]()
    {
        Extras extras(edit, context, zoneId);
        InspectorExtrasRegistry::Get().ForEach(zoneType, [&extras](const InspectorExtrasProvider& provider) { provider(extras); });
        return extras;
    };
    editor::navigation::RegisterNavigationInspectorExtras();
    {
        Extras extras = gather();
        REQUIRE(extras.labels.Size() == 1u);
        CHECK(extras.labels[0] == String(u8"Bake Navigation"));
        extras.actions[0]();
        CHECK(said == String(u8"No project is open."));
    }
    editor::navigation::RemoveNavigationInspectorExtras();
    CHECK(gather().labels.IsEmpty());
}

TEST_CASE("navigation-editor: navigation_bake bakes a page's zone into its asset, and says why not")
{
    pipeline::RegisterNavigationZoneAsset();
    const StringView dir = u8"scratch_navigation_bake_project";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    EditorContext context{DefaultAllocator()};
    McpServer server;
    server.Paths().AddFolder(GetCurrentDirectory().AsView()); // the test's scratch folder
    editor::mcp::ProjectSession session;
    Array<String> registered;
    editor::mcp::PluginTools tools(server, session, registered);
    editor::navigation::RegisterNavigationBakeTool(tools, context);
    CHECK(server.HasTool(u8"navigation_bake"));
    Random rng(77);
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Block", Guid::Generate(rng)), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    scene::Scene& sceneRef = edit.Scene();
    engine::navigation::AddNavigationSceneManagers(sceneRef);
    if (!sceneRef.HasSystem<engine::physics::RigidBodyComponentManager>())
    {
        sceneRef.AddSystem<engine::physics::RigidBodyComponentManager>();
    }

    // A 20 x 20 static ground slab with its top at y 0 (the bake reads static bodies), and a zone
    // over it.
    const Guid groundId = edit.CreateEntity(u8"Ground");
    sceneRef.SetLocalPosition(edit.Resolve(groundId), Float3{0, -0.5f, 0});
    engine::physics::RigidBodyComponent& ground =
        sceneRef.GetSystem<engine::physics::RigidBodyComponentManager>()->Add(edit.Resolve(groundId));
    ground.motion = foundation::physics::MotionKind::Static;
    ground.halfExtents = Float3{10, 0.5f, 10};
    const Guid zoneId = edit.CreateEntity(u8"Zone");
    engine::navigation::NavMeshZoneComponent& zone =
        sceneRef.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(
            edit.Resolve(zoneId));
    zone.extents = Float3{15, 10, 15};

    // With no project the bake is refused.
    Answer bake = Call(server, u8"navigation_bake", u8"{}");
    CHECK_FALSE(bake.ok);
    context.SetProject(project.Get());

    // A zone with no asset is refused, with a message saying what to do.
    bake = Call(server, u8"navigation_bake", u8"{}");
    REQUIRE_FALSE(bake.ok);
    CHECK(bake.error.AsView().ContainsIgnoreCase(u8"Navigation Zone asset"));

    // An entity that is not a zone is refused.
    bake = Call(server, u8"navigation_bake", u8"{\"entity\":\"Ground\"}");
    CHECK_FALSE(bake.ok);

    // With the asset the zone bakes; the scene's only zone is found without naming it.
    foundation::content::Instance* asset = project->SourceDb().RootGroup()->CreateInstance(
        u8"BlockZone", pipeline::NavigationZoneAsset::StaticType());
    REQUIRE(asset != nullptr);
    zone.zone.SetId(asset->Id());
    bake = Call(server, u8"navigation_bake", u8"{}");
    REQUIRE(bake.ok);
    CHECK(bake.payload.Get(u8"baked").AsBool());
    CHECK(bake.payload.Get(u8"triangles").AsNumber() == doctest::Approx(12.0)); // the slab's box
    CHECK(bake.payload.Get(u8"asset").AsString() == Format(u8"{}", asset->Id()));
    pipeline::NavigationZoneAsset readBack;
    {
        RefPtr<ISerializable> object = asset->ReadObject();
        auto* stored = Cast<pipeline::NavigationZoneAsset>(object.Get());
        REQUIRE(stored != nullptr);
        REQUIRE(pipeline::EnsureNavMeshLoaded(*asset, *stored).IsOk());
        CHECK_FALSE(stored->navMeshBlob.IsEmpty());
    }

    // Not while the page simulates.
    page->StartSimulation();
    bake = Call(server, u8"navigation_bake", u8"{\"entity\":\"Zone\"}");
    CHECK_FALSE(bake.ok);
    page->StopSimulation();

    context.SetProject(nullptr);
    context.ClosePage(page);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

