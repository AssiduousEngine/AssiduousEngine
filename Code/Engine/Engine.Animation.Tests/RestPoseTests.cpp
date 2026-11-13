// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The rest pose: a skinned mesh fed no palette draws its raw vertices, which an FBX file keeps in
// its authoring tool's axes (the bind matrices carry the turn upright). So an animator feeds its
// meshes the player's standing pose even while the scene stands still (the editor's edit mode, a
// thumbnail), the clip waiting for the simulation, and a graph player starts in the rest pose.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;
import foundation.scene;
import engine.animation;
import engine.render;
import foundation.render;   // ExtractedScene, MeshRenderData: what drawing is handed
import foundation.geometry; // Primitives: a mesh to draw

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;

namespace
{
    // One bone at the origin whose inverse bind turns a quarter about X: the palette at rest is
    // that turn, what stands a Z-up FBX mesh upright.
    RefPtr<animation::Skeleton> TurnedRig()
    {
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
        skeleton->Bones()[0].index = 0;
        skeleton->Bones()[0].parentIndex = -1;
        skeleton->Bones()[0].inverseBindPose = Float4x4::RotationX(-kHalfPi);
        skeleton->FindRootBones();
        skeleton->BuildChildIndices();
        return skeleton;
    }

    Float3 Moved(const Float4x4& m) { return TransformPoint(Float3{0, 0, 1}, m); }
}

namespace
{
    // Whether the animator poses `mesh`, and its first bone's matrix (as drawing asks for them).
    bool IsPosed(scene::Scene& level, scene::EntityHandle mesh)
    {
        scene::SkinningPalette palette;
        return scene::FindSkinningPalette(level, mesh, palette);
    }
    Float4x4 PosedBone(scene::Scene& level, scene::EntityHandle mesh)
    {
        scene::SkinningPalette palette;
        REQUIRE(scene::FindSkinningPalette(level, mesh, palette));
        return palette.current[0];
    }
}

TEST_CASE("rest pose: a standing scene's animator feeds the rest pose and holds its clip")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Lift", 1.0f, true);
    clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 2, 0});
    clip->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 2, 0});

    scene::Scene level{DefaultAllocator(), u8"rest"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body);
    auto& animator = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(body);
    animator.skeleton.SetDirect(skeleton);
    animator.clip.SetDirect(clip);

    level.SetSimulationEnabled(false);
    level.Update(0.5f);
    scene::SkinningPalette palette;
    REQUIRE(scene::FindSkinningPalette(level, body, palette));
    REQUIRE(palette.boneCount == 1u);
    const Float3 rest = Moved(Float4x4::RotationX(-kHalfPi));
    CHECK(Moved(palette.current[0]).y == doctest::Approx(rest.y)); // turned, not raw
    CHECK(Moved(palette.current[0]).z == doctest::Approx(rest.z));
    REQUIRE(animator.player.Get() != nullptr);
    CHECK(animator.player->CurrentClip() == nullptr); // the clip waits for the simulation
    CHECK(animator.player->CurrentTime() == doctest::Approx(0.0f));

    // Simulating, the clip plays: the bone lifts by two.
    level.SetSimulationEnabled(true);
    level.Update(0.25f);
    CHECK(animator.player->CurrentClip() == clip.Get());
    CHECK(Moved(PosedBone(level, body)).y == doctest::Approx(rest.y + 2.0f));
}

TEST_CASE("rest pose: an animator with no clip feeds the rest pose while simulating too")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    scene::Scene level{DefaultAllocator(), u8"rest"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body);
    level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(body).skeleton.SetDirect(skeleton);
    level.Update(0.5f);
    REQUIRE(IsPosed(level, body));
    CHECK(Moved(PosedBone(level, body)).y == doctest::Approx(Moved(Float4x4::RotationX(-kHalfPi)).y));
}

TEST_CASE("rest pose: a graph player starts in the rig's rest pose, fed while the scene stands still")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    skeleton->Bones()[0].localBindPose.position = Float3{0, 3, 0};
    RefPtr<animation::AnimationGraph> graph = MakeRef<animation::AnimationGraph>(DefaultAllocator());

    scene::Scene level{DefaultAllocator(), u8"rest"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body);
    auto& component = level.GetSystem<engine::animation::AnimationGraphComponentManager>()->Add(body);
    component.skeleton.SetDirect(skeleton);
    component.graph.SetDirect(graph);
    level.SetSimulationEnabled(false);
    level.Update(0.5f);
    REQUIRE(IsPosed(level, body));
    // Its rest position (three up) under the turn, not the identity a default pose gives.
    CHECK(Moved(PosedBone(level, body)).y ==
          doctest::Approx(Moved(Float4x4::RotationX(-kHalfPi)).y + 3.0f));
}

TEST_CASE("skinning: a mesh whose animator is removed draws static, never the animator's freed matrices")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    scene::Scene level{DefaultAllocator(), u8"removed"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body).mesh = foundation::geometry::Primitives::Quad(DefaultAllocator());
    auto* animators = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>();
    animators->Add(body).skeleton.SetDirect(skeleton);
    level.Update(0.5f);
    REQUIRE(IsPosed(level, body));
    {
        foundation::render::ExtractedScene posed{DefaultAllocator()};
        engine::render::ExtractSceneInto(level, posed);
        REQUIRE(posed.Size() == 1u);
        CHECK(static_cast<const foundation::render::MeshRenderData*>(posed.Items()[0])->boneMatrices != nullptr);
    }
    animators->RemoveComponent(body); // its player, and the matrices, go with it
    CHECK_FALSE(IsPosed(level, body));
    foundation::render::ExtractedScene still{DefaultAllocator()};
    engine::render::ExtractSceneInto(level, still);
    REQUIRE(still.Size() == 1u);
    const auto* md = static_cast<const foundation::render::MeshRenderData*>(still.Items()[0]);
    CHECK(md->boneMatrices == nullptr);
    CHECK(md->boneCount == 0u);
}
