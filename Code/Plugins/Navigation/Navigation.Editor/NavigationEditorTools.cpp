// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Navigation.Editor - what the navigation plugin adds to the scene editor: the zone's extents gizmo,
// the inspector's Bake Navigation action, and the navigation_bake MCP tool on a scene page. Each is
// registered by the part's entry and taken back as the plugin unloads.

module;
#include "Core/Prelude.h"

module navigation.editor;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.content;
import navigation.engine;
import editor.core;
import editor.scene; // the gizmo and inspector-extras registries, the scene page tools' lookups
import editor.mcp;   // PluginTools

using namespace foundation::core;
using namespace editor; // its bases
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolResult;
namespace scene = foundation::scene;

namespace navigation
{
    namespace
    {
        /// The navigation zone's extents box (local, entity-oriented): the region the bake samples
        /// geometry from. Read-only, as the extents are edited in the inspector; drawn when the
        /// zone is selected.
        class NavMeshZoneGizmoRenderer final : public IGizmoRenderer
        {
        public:
            [[nodiscard]] const TypeInfo* ComponentType() const override
            {
                return &TypeOf<navigation::NavMeshZoneComponent>();
            }
            void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override
            {
                const auto* zone = component.TryGet<navigation::NavMeshZoneComponent>();
                if (zone == nullptr)
                {
                    return;
                }
                const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
                ctx.debug->DrawTransformedBox(Float3{} - zone->extents, zone->extents, world,
                                              Color{0.20f, 0.85f, 1.0f, 1.0f});
            }
        };

        void AddBake(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddAction(u8"Navigation", u8"Bake Navigation",
                             u8"Bake the zone's static geometry (static bodies, terrain) into its Navigation Zone asset; cook to apply it.",
                             [at]()
                             {
                                 EditorContext* context = at->Context();
                                 SceneEditContext& edit = at->Edit();
                                 if (context == nullptr)
                                     return;
                                 if (context->Project() == nullptr)
                                 {
                                     context->Notify(NoticeKind::Error, u8"No project is open.");
                                     return;
                                 }
                                 auto* zones = edit.Scene().GetSystem<navigation::NavMeshZoneComponentManager>();
                                 for (const Guid& id : at->Entities())
                                 {
                                     const scene::EntityHandle entity = edit.Resolve(id);
                                     const navigation::NavMeshZoneComponent* zone = zones != nullptr && entity.IsAssigned() ? zones->Get(entity) : nullptr;
                                     if (zone == nullptr)
                                         continue;
                                     foundation::content::Instance* target = zone->zone.id.IsNil() ? nullptr : context->Project()->SourceDb().GetInstance(zone->zone.id);
                                     if (target == nullptr)
                                     {
                                         context->Notify(NoticeKind::Warning, u8"Assign a Navigation Zone asset to this zone before baking.");
                                         continue;
                                     }
                                     const BakeResult result = BakeNavigationZone(edit.Scene(), entity, *target, ParallelBakeEnabled(*context));
                                     context->Notify(result.baked ? NoticeKind::Success : NoticeKind::Warning, DescribeBake(result).AsView());
                                 }
                             });
        }
    }

    void AddNavigationGizmos(GizmoRendererRegistry& registry)
    {
        registry.Register(UniquePtr<IGizmoRenderer>(editor::EditorRootAllocator().New<NavMeshZoneGizmoRenderer>(),
                                                    editor::EditorRootAllocator()));
    }

    void RegisterNavigationInspectorExtras()
    {
        InspectorExtrasRegistry::Get().Register(TypeOf<navigation::NavMeshZoneComponent>(),
                                                [](InspectorExtras& extras) { AddBake(extras); });
    }

    void RemoveNavigationInspectorExtras()
    {
        InspectorExtrasRegistry::Get().Remove(TypeOf<navigation::NavMeshZoneComponent>());
    }

    void RegisterNavigationBakeTool(editor::mcp::PluginTools& tools, EditorContext& context)
    {
        EditorContext* ctx = &context;
        tools.RegisterTool(
            u8"navigation_bake",
            u8"Bake a scene page's navigation zone, as the inspector's Bake Navigation button does: "
            u8"collect the static geometry inside the zone's box (static rigid bodies and terrain; "
            u8"render meshes are not read) and write the navmesh into "
            u8"the zone's Navigation Zone asset (the zone component's `zone`; assign one first, "
            u8"with component_set). `entity` names the zone's entity; without it, the scene's only "
            u8"zone. The scene is not changed; cook (asset_cook) for the game to see the new "
            u8"navmesh. Refused while the page simulates. Returns {page, entity, asset, baked, "
            u8"triangles, message}: baked false with the reason when nothing walkable came out.",
            SchemaBuilder()
                .Str(u8"page", kScenePageArgument)
                .Str(u8"entity", u8"the zone's entity: a guid, a name or a slash path (default: "
                                 u8"the scene's only navigation zone)")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                const AddressedPage& page = addressed.Value();
                if (page.scene->IsSimulating())
                {
                    return Err(Format(u8"page '{}' is simulating - stop it (simulate_stop) "
                                      u8"before baking",
                                      page.page->Title()));
                }
                if (ctx->Project() == nullptr)
                {
                    return Err(String(u8"no project is open"));
                }
                SceneEditContext& edit = page.scene->EditContext();
                scene::Scene& sceneRef = edit.Scene();
                auto* zones =
                    sceneRef.GetSystem<navigation::NavMeshZoneComponentManager>();
                scene::EntityHandle zoneEntity{};
                const String named = args.Get(u8"entity").AsString();
                if (!named.IsEmpty())
                {
                    Result<Guid, String> id = ResolveSceneEntity(page, named.AsView());
                    if (!id.HasValue())
                    {
                        return Err(Move(id.Error()));
                    }
                    zoneEntity = edit.Resolve(id.Value());
                    if (zones == nullptr || zones->Get(zoneEntity) == nullptr)
                    {
                        return Err(Format(u8"'{}' has no navigation zone component",
                                          named.AsView()));
                    }
                }
                else
                {
                    usize count = 0;
                    if (zones != nullptr)
                    {
                        zones->ForEach(
                            [&](const navigation::NavMeshZoneComponent&, scene::EntityHandle e)
                            {
                                zoneEntity = e;
                                ++count;
                            });
                    }
                    if (count != 1)
                    {
                        return Err(Format(u8"the scene has {} navigation zones - name one with "
                                          u8"`entity`",
                                          count));
                    }
                }
                const navigation::NavMeshZoneComponent* zone = zones->Get(zoneEntity);
                foundation::content::Instance* target =
                    zone->zone.id.IsNil() ? nullptr
                                          : ctx->Project()->SourceDb().GetInstance(zone->zone.id);
                if (target == nullptr)
                {
                    return Err(String(u8"the zone has no Navigation Zone asset - create one "
                                      u8"(asset_create) and set it as the zone component's "
                                      u8"`zone` (component_set) before baking"));
                }
                sceneRef.UpdateTransforms();
                const BakeResult result = BakeNavigationZone(
                    sceneRef, zoneEntity, *target, ParallelBakeEnabled(*ctx));
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"page", PageJson(*page.page));
                out.Set(u8"entity", JsonValue::MakeString(Format(u8"{}", sceneRef.GetEntityId(zoneEntity))));
                out.Set(u8"asset", JsonValue::MakeString(Format(u8"{}", zone->zone.id)));
                out.Set(u8"baked", JsonValue::MakeBool(result.baked));
                out.Set(u8"triangles", JsonValue::MakeNumber(static_cast<f64>(result.triangleCount)));
                out.Set(u8"message", JsonValue::MakeString(DescribeBake(result)));
                return out;
            });
    }
}
