// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :builtin_inspector_extras partition.
//
// The inspector's extras for the engine's own components and settings blocks, registered by type
// into the inspector's registry by RegisterSceneEditor:
//   - a rigid body whose shape is Cooked with no collision shape set has no collider, and a note
//     under its header says so while that holds (it follows an edit, not only a rebuild);
//   - a script component's behaviours, as a source of their own (BehaviorsSource);
//   - a navigation zone's Bake, which bakes the static geometry inside the zone, writes it into
//     the zone's asset and reports the outcome. It runs at once on the main thread, as the bake's
//     interface is one call; running it as a job needs that call split (collect here, bake on a
//     worker);
//   - the physics block's collision groups and the scene script block's level properties, as
//     sources of their own (SettingsExtraSources).

module;
#include "Core/Prelude.h"

export module editor.scene:builtin_inspector_extras;

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.physics;
import engine.physics;
import engine.animation; // PropertyAnimatorComponent (its Edit Clip)
import engine.sequence;  // SequenceComponent (its Edit Sequence)
import editor.core;
import :edit;
import :inspector_extras;
import :scene_page_interface; // the animation panel an animator's clip opens in
import :behaviors_source;
import :settings_extra_sources;
import engine.behavior;

using namespace foundation::core;

export namespace editor
{
    /// BuiltinInspectorExtras::kNoColliderText, at namespace scope for another library to read
    /// (MSVC exports no class's static constexpr member from a shared library).
    inline constexpr StringView kBuiltinInspectorExtrasNoColliderText =
        u8"Shape is Cooked but no collision shape is set - this body has no collider. Assign one (import a mesh with Generate "
        u8"collision, or create a Collision Shape asset).";

    // A struct of static functions rather than a function: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct BuiltinInspectorExtras
    {
        static constexpr StringView kNoColliderText = kBuiltinInspectorExtrasNoColliderText;

        static void Register(InspectorExtrasRegistry& registry)
        {
            registry.Register(TypeOf<engine::physics::RigidBodyComponent>(), [](InspectorExtras& extras) { AddNoCollider(extras); });
            registry.Register(TypeOf<engine::behavior::BehaviorComponent>(), [](InspectorExtras& extras) { AddBehaviors(extras); });
            registry.Register(TypeOf<engine::physics::PhysicsSceneSettings>(), [](InspectorExtras& extras) { AddCollisionGroups(extras); });
            registry.Register(TypeOf<engine::behavior::SceneBehaviorSettings>(), [](InspectorExtras& extras) { AddLevelProperties(extras); });
            registry.Register(TypeOf<engine::animation::PropertyAnimatorComponent>(), [](InspectorExtras& extras) { AddEditClip(extras); });
            registry.Register(TypeOf<engine::sequence::SequenceComponent>(), [](InspectorExtras& extras) { AddEditSequence(extras); });
        }

    private:
        // A script component's behaviours: a source of their own (reflection does not show them).
        static void AddBehaviors(InspectorExtras& extras)
        {
            auto source = MakeUnique<BehaviorsSource>(EditorRootAllocator(), extras.Edit(), extras.Context(), extras.Entities());
            BehaviorsSource* raw = source.Get();
            extras.AddSection(Move(source), [raw]() { raw->Refresh(); });
        }
        // The physics block's collision groups (reflection does not show them).
        static void AddCollisionGroups(InspectorExtras& extras)
        {
            auto source = MakeUnique<CollisionGroupsSource>(EditorRootAllocator(), extras.Edit());
            CollisionGroupsSource* raw = source.Get();
            extras.AddSection(Move(source), [raw]() { raw->Refresh(); });
        }
        // The scene script block's Level class's properties (its overrides are not reflected).
        static void AddLevelProperties(InspectorExtras& extras)
        {
            auto source = MakeUnique<LevelScriptSource>(EditorRootAllocator(), extras.Edit(), extras.Context());
            LevelScriptSource* raw = source.Get();
            extras.AddSection(Move(source), [raw]() { raw->Refresh(); });
        }
        // An animator's clip opens in the page's animation panel, animating its entity. With no
        // clip, the panel is bound to the animator so one can be made.
        static void AddEditClip(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddAction(u8"Animation", u8"Edit Clip", u8"Open the clip in the animation panel under the viewport, animating this entity",
                             [at]()
                             {
                                 EditorContext* context = at->Context();
                                 ISceneEditorPage* page = context != nullptr ? ServiceOf<ISceneEditorPage>(context->ActivePage()) : nullptr;
                                 if (page == nullptr || at->Entities().IsEmpty())
                                     return;
                                 SceneEditContext& edit = at->Edit();
                                 const Guid entity = at->Entities()[0];
                                 auto* animators = edit.Scene().GetSystem<engine::animation::PropertyAnimatorComponentManager>();
                                 const foundation::scene::EntityHandle handle = edit.Resolve(entity);
                                 engine::animation::PropertyAnimatorComponent* animator = animators != nullptr && handle.IsAssigned() ? animators->Get(handle) : nullptr;
                                 page->EditAnimationClip(animator != nullptr ? animator->clip.id : Guid{}, entity);
                             });
        }

        // A sequence component's sequence opens in the page's sequence panel, its cast filled from
        // the component's.
        static void AddEditSequence(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddAction(u8"Sequence", u8"Edit Sequence", u8"Open the sequence in the sequence panel under the viewport, cast as this component casts it",
                             [at]()
                             {
                                 EditorContext* context = at->Context();
                                 ISceneEditorPage* page = context != nullptr ? ServiceOf<ISceneEditorPage>(context->ActivePage()) : nullptr;
                                 if (page == nullptr || at->Entities().IsEmpty())
                                     return;
                                 SceneEditContext& edit = at->Edit();
                                 const Guid entity = at->Entities()[0];
                                 auto* components = edit.Scene().GetSystem<engine::sequence::SequenceComponentManager>();
                                 const foundation::scene::EntityHandle handle = edit.Resolve(entity);
                                 const engine::sequence::SequenceComponent* component =
                                     components != nullptr && handle.IsAssigned() ? components->Get(handle) : nullptr;
                                 page->EditSequence(component != nullptr ? component->sequence.id : Guid{}, entity);
                             });
        }

        static void AddNoCollider(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddNotice(u8"Collision", [at]() -> String
                             {
                                 SceneEditContext& edit = at->Edit();
                                 auto* bodies = edit.Scene().GetSystem<engine::physics::RigidBodyComponentManager>();
                                 for (const Guid& id : at->Entities())
                                 {
                                     const foundation::scene::EntityHandle e = edit.Resolve(id);
                                     engine::physics::RigidBodyComponent* body = bodies != nullptr && e.IsAssigned() ? bodies->Get(e) : nullptr;
                                     if (body != nullptr && body->shape == foundation::physics::ShapeKind::Cooked && body->collisionShape.id.IsNil())
                                         return String(kNoColliderText);
                                 }
                                 return String{};
                             });
        }
    };
}
