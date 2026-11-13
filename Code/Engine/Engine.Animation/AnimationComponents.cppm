// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - the `:components` partition.
///
/// The scene-facing side of skeletal animation. Two components, each with a manager that ticks its
/// players every frame and feeds the resulting skinning matrices into the target MeshComponents
/// for GPU skinning: SkeletalAnimationComponent (a single clip via AnimationPlayer) and
/// AnimationGraphComponent (a state machine and blend trees via AnimationGraphPlayer). The engine
/// animates skinned meshes from the scene tick, so app code does not drive players by hand.
///
/// This partition depends on both foundation.animation and the render components; neither of
/// those depends back on it.

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.animation:components;

import foundation.core;
import foundation.profiler;
import foundation.resource;
import foundation.scene;
import foundation.animation; // Skeleton, AnimationClip, AnimationPlayer, AnimationGraph, AnimationGraphPlayer
import engine.render; // MeshComponentManager / MeshComponent (the feed target)
import foundation.script.facades; // script::Entity/Scene and CurrentRunResources (the SceneAnimation handle)

using namespace foundation::core;

export namespace engine::animation
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace scene = foundation::scene;
    namespace animation = foundation::animation;
    namespace script = foundation::script;
    using namespace foundation::animation; // bare foundation animation types (BoneTransform, ...)


    // What an animator does with the root motion its clips carry.
    enum class RootMotionMode : u8
    {
        Ignore,    // nothing: the character stays where its game puts it (the default)
        Entity,    // the animator's entity moves and turns by it (a non-physics actor)
        Character, // the nearest character at or above it walks by it (still colliding) and turns
        Manual,    // held for game code or a script to apply (rootMotionTranslation / rootMotionYaw)
    };

    // An animator's root motion as of its last tick (Manual mode reads it; the others use it).
    struct RootMotionRuntime
    {
        Float3 worldTranslation{}; // the last tick's travel, in the world
        f32 yaw = 0.0f;            // and its turn about up, radians
        bool drivingCharacter = false; // a character's move is ours to zero when we stop
        scene::EntityHandle character = scene::EntityHandle::Invalid();
    };

    namespace root_motion_apply
    {
        [[nodiscard]] inline scene::ISceneCharacterMotion* Mover(scene::Scene& scene)
        {
            scene::ISceneCharacterMotion* found = nullptr;
            scene.ForEachSystem(
                [&](scene::SceneSystem& system)
                {
                    if (found == nullptr)
                    {
                        found = system.AsCharacterMotion();
                    }
                });
            return found;
        }

        /// A character we were walking stops: one zero move, so it does not walk on by our last.
        inline void Release(scene::Scene& scene, RootMotionRuntime& rt)
        {
            if (rt.drivingCharacter && scene.IsValid(rt.character))
            {
                if (scene::ISceneCharacterMotion* mover = Mover(scene))
                {
                    mover->MoveCharacter(rt.character, Float3{});
                }
            }
            rt.drivingCharacter = false;
            rt.character = scene::EntityHandle::Invalid();
        }

        /// The entity whose world is the skeleton's model space (the first mesh it feeds, else its own).
        [[nodiscard]] inline scene::EntityHandle ModelEntity(scene::Scene& scene, Span<const scene::EntityRef> meshes,
                                                             scene::EntityHandle owner)
        {
            for (const scene::EntityRef& r : meshes)
            {
                const scene::EntityHandle e = scene.FindEntity(r.id);
                if (scene.IsValid(e))
                {
                    return e;
                }
            }
            return owner;
        }

        /// Turns `entity` by `yaw` about the model's up (`upWorld`), seen in its own frame.
        inline void Turn(scene::Scene& scene, scene::EntityHandle entity, f32 yaw, Float3 upWorld)
        {
            if (yaw == 0.0f)
            {
                return;
            }
            Float3 axis = TransformDirection(upWorld, Inverse(scene.ComposeWorldMatrix(entity)));
            axis = LengthSquared(axis) > 1.0e-12f ? Normalized(axis) : Float3{0.0f, 1.0f, 0.0f};
            Transform t = scene.GetLocalTransform(entity);
            t.rotation = Normalized(t.rotation * Quaternion::FromAxisAngle(axis, yaw));
            scene.SetLocalTransform(entity, t);
        }

        /// An animator's tick of root motion, by its mode. The delta is in the skeleton's model
        /// space (the first mesh entity's world), so it is carried into the world through that,
        /// whatever lies between the animator and its mesh (an armature at rest).
        inline void Apply(scene::Scene& scene, scene::EntityHandle owner, Span<const scene::EntityRef> meshes,
                          const scene::EntityRef& target, RootMotionMode mode, RootMotionRuntime& rt,
                          const animation::RootMotionDelta& delta, f32 deltaTime)
        {
            const Float4x4 model = scene.ComposeWorldMatrix(ModelEntity(scene, meshes, owner));
            const Float3 upWorld = TransformDirection(Float3{0.0f, 1.0f, 0.0f}, model);
            rt.worldTranslation = TransformDirection(delta.translation, model);
            rt.yaw = delta.yaw;
            if (mode != RootMotionMode::Character)
            {
                Release(scene, rt);
            }
            if (mode == RootMotionMode::Entity)
            {
                // The named entity (a gameplay root holding the model), else the animator's own:
                // moved in its parent's space, turned about the model's up.
                scene::EntityHandle moved = owner;
                if (!target.IsNil())
                {
                    const scene::EntityHandle named = scene.FindEntity(target.id);
                    if (!scene.IsValid(named))
                    {
                        return;
                    }
                    moved = named;
                }
                const scene::EntityHandle parent = scene.GetParent(moved);
                const Float3 local = scene.IsValid(parent)
                                         ? TransformDirection(rt.worldTranslation, Inverse(scene.ComposeWorldMatrix(parent)))
                                         : rt.worldTranslation;
                Transform t = scene.GetLocalTransform(moved);
                t.position = t.position + local;
                scene.SetLocalTransform(moved, t);
                Turn(scene, moved, delta.yaw, upWorld);
                return;
            }
            if (mode != RootMotionMode::Character)
            {
                return;
            }
            scene::ISceneCharacterMotion* mover = Mover(scene);
            scene::EntityHandle e = owner;
            for (u32 depth = 0; mover != nullptr && scene.IsValid(e) && depth < 1024u; ++depth)
            {
                if (mover->HasCharacter(e))
                {
                    break;
                }
                e = scene.GetParent(e);
            }
            if (mover == nullptr || !scene.IsValid(e) || !mover->HasCharacter(e))
            {
                Release(scene, rt);
                return;
            }
            if (rt.drivingCharacter && rt.character != e)
            {
                Release(scene, rt);
            }
            // A velocity for the next fixed step. The controller collides and slides, one step late.
            Float3 velocity = deltaTime > 0.0f ? rt.worldTranslation * (1.0f / deltaTime) : Float3{};
            velocity.y = 0.0f;
            mover->MoveCharacter(e, velocity);
            rt.drivingCharacter = true;
            rt.character = e;
            Turn(scene, e, delta.yaw, upWorld);
        }
    }

    // Skeletal animation on an entity: a player over a borrowed, shared skeleton plays a clip and
    // produces per-bone skinning matrices each frame. The manager owns the player's lifetime and
    // tick. `meshEntities` are the entities whose MeshComponent receives the matrices (a character's
    // skinned mesh nodes); empty feeds the component's own entity. All borrowed resources must
    // outlive the component (the resource manager or model keeps the skeleton and clip alive).
    // A one-shot a script asked for (SceneAnimation.playOneShot), on either kind of animated
    // entity: the manager starts it over the entity's pose once its clip has loaded, and reports
    // its end. Runtime only.
    struct OneShotRuntime
    {
        foundation::resource::Ref<animation::AnimationClip> clip; // asked for, then playing (kept alive)
        animation::OneShotSettings settings;
        bool pending = false; // asked for, its clip not yet started
        bool playing = false; // started over the pose, not yet reported ended
        Guid playingId;       // its clip's id (nil for a clip made in memory)
    };

    // A one-shot that ended: its entity and its clip, for the animation subsystem's listeners.
    struct OneShotEnded
    {
        scene::EntityHandle entity;
        Guid clip;
    };

    // What the two managers do with a one-shot each tick. Static members rather than free inline
    // functions: GCC cannot read back a partition whose namespace-scope inline function
    // instantiates a template.
    struct OneShots
    {
        // Starts a requested one-shot over `overlay` once its clip has loaded.
        static void Start(OneShotRuntime& o, animation::PoseOverlay& overlay)
        {
            if (!o.pending)
            {
                return;
            }
            const animation::AnimationClip* clip = o.clip.Get();
            if (clip == nullptr)
            {
                return; // still loading
            }
            overlay.PlayOneShot(*clip, o.settings);
            o.playing = true;
            o.playingId = o.clip.id;
            o.pending = false;
        }
        // After the player's tick: a one-shot that ended joins `ended` (a new one asked for since
        // has not started yet, so the end is the old one's).
        static void Ended(OneShotRuntime& o, bool endedNow, scene::EntityHandle owner, Array<OneShotEnded>& ended)
        {
            if (!endedNow || !o.playing)
            {
                return;
            }
            ended.PushBack(OneShotEnded{owner, o.playingId});
            o.playing = false;
        }
    };

    struct SkeletalAnimationComponent
    {
        // Resource refs: serialized by Guid and resolved through the proxy (editor pickers, scene
        // round-trip), or direct runtime objects (samples, spawn code). The manager rebuilds the
        // player when the skeleton object changes (a pick or a hot reload).
        foundation::resource::Ref<animation::Skeleton> skeleton;
        foundation::resource::Ref<animation::AnimationClip> clip;
        UniquePtr<animation::AnimationPlayer> player;   // created lazily by the manager
        animation::Skeleton* playerSkeleton = nullptr;  // the skeleton the player was built for
        animation::AnimationClip* playerClip = nullptr; // the clip last handed to the player
        // Feed targets by stable guid (empty means the own entity): serializable and remapped by
        // prefabs, so one animator can drive a multi-part character's skinned mesh nodes.
        Array<scene::EntityRef> meshEntities;
        f32 speed = 1.0f;
        f32 startTime = 0.0f; // initial clock (desync a herd); applied on first tick
        bool autoPlay = true; // Play(clip) on first tick
        RootMotionMode rootMotion = RootMotionMode::Ignore; // v2
        scene::EntityRef rootMotionTarget; // v2: what Entity mode moves (empty: this entity)
        RootMotionRuntime rootMotionState; // runtime
        OneShotRuntime oneShot;            // runtime: a one-shot over the clip
        f32 clipFade = 0.0f;               // runtime: the next clip change eases over this (s)
    };

    // Persist the refs and tunables; the player and per-frame feed state are runtime-only.
    inline void Serialize(ISerializer& ar, SkeletalAnimationComponent& c)
    {
        foundation::core::Serialize(ar, "skeleton", c.skeleton);
        foundation::core::Serialize(ar, "clip", c.clip);
        foundation::core::Serialize(ar, "speed", c.speed);
        foundation::core::Serialize(ar, "startTime", c.startTime);
        foundation::core::Serialize(ar, "autoPlay", c.autoPlay);
        foundation::core::Serialize(ar, "meshEntities", c.meshEntities);
        if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2) // v1 records read as Ignore
        {
            foundation::core::Serialize(ar, "rootMotion", c.rootMotion);
            foundation::core::Serialize(ar, "rootMotionTarget", c.rootMotionTarget);
        }
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 SkeletalAnimationComponent& c)
    {
        c.skeleton.Bind(manager);
        c.clip.Bind(manager);
    }

    // Ticks every SkeletalAnimationComponent in ScenePhase::PostUpdate (the animation phase, before
    // render extraction): advances each player, then writes its current and previous skinning
    // matrices into the target MeshComponents. They are borrowed for the frame; the player, owned by
    // the component, keeps the matrix storage alive. Creates each component's player on first tick.
    // Which animator posed each mesh at its last update: what an animator manager answers
    // ISkinningPaletteSource from, so a mesh holds no pointer into the animator's player.
    class PosedMeshes
    {
    public:
        void Clear() { m_byMesh.Clear(); }
        void Record(scene::EntityHandle mesh, scene::EntityHandle animator) { m_byMesh.InsertOrAssign(mesh.index, Entry{mesh, animator}); }
        [[nodiscard]] scene::EntityHandle AnimatorOf(scene::EntityHandle mesh) const
        {
            const Entry* entry = m_byMesh.Find(mesh.index);
            return entry != nullptr && entry->mesh == mesh ? entry->animator : scene::EntityHandle::Invalid();
        }

    private:
        struct Entry
        {
            scene::EntityHandle mesh;
            scene::EntityHandle animator;
        };
        HashMap<u32, Entry> m_byMesh;
    };

    // Records the meshes animator `a` (on `owner`) poses: its own entity's, or those it names.
    template <typename Animator>
    void RecordPosedMeshes(PosedMeshes& posed, scene::Scene& scene, engine::render::MeshComponentManager& meshes, const Animator& a,
                           scene::EntityHandle owner)
    {
        const auto record = [&](scene::EntityHandle e)
        {
            if (e.IsAssigned() && meshes.Get(e) != nullptr)
                posed.Record(e, owner);
        };
        if (a.meshEntities.IsEmpty())
        {
            record(owner);
            return;
        }
        for (const scene::EntityRef& r : a.meshEntities)
        {
            record(scene.FindEntity(r.id)); // guid -> live handle each frame
        }
    }

    class SkeletalAnimationComponentManager final
        : public scene::SerializableComponentManager<SkeletalAnimationComponent>, public scene::ISkinningPaletteSource
    {
    public:
        SkeletalAnimationComponentManager()
            : scene::SerializableComponentManager<SkeletalAnimationComponent>(
                  u8"skeletal_animation")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

    protected:
        // A character this animator was walking stops with it.
        void OnComponentDestroyed(SkeletalAnimationComponent& c, scene::EntityHandle) override
        {
            if (m_scene != nullptr)
            {
                root_motion_apply::Release(*m_scene, c.rootMotionState);
            }
        }

    public:
        // Time is simulation-gated: animation is gameplay state and does not advance in a
        // non-simulating scene, such as the editor's edit mode. Consumers that want live animation
        // in a paused-looking context (the preview pages) enable simulation on their private
        // preview scene (PreviewViewport::SetSimulationEnabled). Scenes default to simulating, so
        // players and headless tests are unaffected. The pose is not gated: in a non-simulating
        // scene the manager still feeds its meshes the player's standing pose (the rig's rest pose
        // before any clip), since a skinned mesh fed nothing draws its raw vertices, which an FBX
        // file keeps lying in its authoring tool's axes.

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            PROFILE_SCOPE("Animation.Skeletal");
            m_posed.Clear(); // recorded again below: an animator gone, or a mesh it stopped naming, drops out
            ForEach(
                [&](SkeletalAnimationComponent& a, scene::EntityHandle owner)
                {
                    // Recorded first, so a frozen animator's meshes keep its last pose.
                    RecordPosedMeshes(m_posed, *m_scene, *meshes, a, owner);
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return; // frozen: time does not advance
                    }
                    animation::Skeleton* skeleton = a.skeleton.Get();
                    if (skeleton == nullptr)
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return;
                    }
                    // Rebuild the player when the skeleton object changed: the first tick, an editor
                    // pick, or a hot reload swapping the product behind the ref.
                    if (a.player.Get() == nullptr || a.playerSkeleton != skeleton)
                    {
                        a.player =
                            MakeUnique<animation::AnimationPlayer>(DefaultAllocator(), *skeleton);
                        a.playerSkeleton = skeleton;
                        a.playerClip = nullptr; // replay below: the new player has no clip yet
                    }
                    // React to the clip changing independently of the skeleton (editor picks land one
                    // at a time; a hot reload swaps the product behind the ref mid-play). autoPlay
                    // starts the new clip; manual users drive a.player->Play themselves. Not while the
                    // scene stands still: the clip starts when the simulation does.
                    animation::AnimationClip* clip = a.clip.Get();
                    if (!m_scene->SimulationEnabled())
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                    }
                    else if (clip != a.playerClip)
                    {
                        a.playerClip = clip;
                        if (a.autoPlay && clip != nullptr)
                        {
                            // A clip changed mid-play eases in when asked (SceneAnimation.setClip's
                            // blend); the first clip, and the start time, start at once.
                            a.player->CrossFadeTo(clip, a.clipFade);
                            if (a.startTime != 0.0f && a.clipFade <= 0.0f)
                            {
                                a.player->SetCurrentTime(a.startTime);
                            }
                        }
                        a.clipFade = 0.0f;
                    }
                    a.player->speed = a.speed;
                    if (m_scene->SimulationEnabled())
                    {
                        OneShots::Start(a.oneShot, a.player->Overlay());
                        a.player->Update(deltaTime);
                        OneShots::Ended(a.oneShot, a.player->ConsumeOneShotEnded(), owner, m_ended);
                        root_motion_apply::Apply(
                            *m_scene, owner, Span<const scene::EntityRef>{a.meshEntities.Data(), a.meshEntities.Size()},
                            a.rootMotionTarget, a.rootMotion, a.rootMotionState, a.player->ConsumeRootMotion(), deltaTime);
                    }
                    // The pose is evaluated here, once a frame, whether or not anything draws it: its
                    // modifiers (foot IK's ground probes) step with the frame, not with a reader.
                    (void)a.player->GetSkinningMatrices();
                });
        }

        // The one-shots that ended since the last call (the animation subsystem tells its listeners).
        void DrainEnded(Array<OneShotEnded>& out)
        {
            for (const OneShotEnded& e : m_ended)
            {
                out.PushBack(e);
            }
            m_ended.Clear();
        }

        [[nodiscard]] scene::ISkinningPaletteSource* AsSkinningPaletteSource() noexcept override { return this; }
        // The palette of the animator that posed `mesh` at its last update, read from its player now.
        [[nodiscard]] bool SkinningPaletteOf(scene::EntityHandle mesh, scene::SkinningPalette& out) override
        {
            const scene::EntityHandle animator = m_posed.AnimatorOf(mesh);
            const auto* a = animator.IsAssigned() ? Get(animator) : nullptr;
            if (a == nullptr || a->player.Get() == nullptr)
            {
                return false; // the animator went (its player with it): the mesh draws static
            }
            const Span<const Float4x4> mats = a->player->GetSkinningMatrices();
            const Span<const Float4x4> prev = a->player->GetPrevSkinningMatrices();
            out.current = mats.Data();
            out.previous = prev.Data();
            out.boneCount = static_cast<u32>(mats.Size());
            return true;
        }

    private:
        scene::Scene* m_scene = nullptr;
        Array<OneShotEnded> m_ended;
        PosedMeshes m_posed;
    };

    // State-machine-driven skeletal animation: a graph player over a borrowed, shared skeleton and
    // AnimationGraph evaluates the graph each frame (state transitions, blend trees, layer blending)
    // and produces per-bone skinning matrices. The richer counterpart to SkeletalAnimationComponent
    // (single clip); drive transitions via the player's parameters (SetFloat/SetBool/SetTrigger).
    // Same feed contract: `meshEntities` are the MeshComponents that receive the matrices (empty
    // feeds the own entity). All borrowed resources must outlive the component.
    struct AnimationGraphComponent
    {
        foundation::resource::Ref<animation::Skeleton> skeleton;
        foundation::resource::Ref<animation::AnimationGraph> graph;
        UniquePtr<animation::AnimationGraphPlayer> player; // created lazily by the manager
        animation::Skeleton* playerSkeleton = nullptr;     // what the player was built for
        animation::AnimationGraph* playerGraph = nullptr;
        Array<scene::EntityRef> meshEntities; // feed targets by stable guid (empty: own entity)
        bool active = true;                   // evaluate this frame?
        RootMotionMode rootMotion = RootMotionMode::Ignore; // v2
        scene::EntityRef rootMotionTarget; // v2: what Entity mode moves (empty: this entity)
        RootMotionRuntime rootMotionState; // runtime
        OneShotRuntime oneShot;            // runtime: a one-shot over the graph's pose
    };

    inline void Serialize(ISerializer& ar, AnimationGraphComponent& c)
    {
        foundation::core::Serialize(ar, "skeleton", c.skeleton);
        foundation::core::Serialize(ar, "graph", c.graph);
        foundation::core::Serialize(ar, "active", c.active);
        foundation::core::Serialize(ar, "meshEntities", c.meshEntities);
        if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2) // v1 records read as Ignore
        {
            foundation::core::Serialize(ar, "rootMotion", c.rootMotion);
            foundation::core::Serialize(ar, "rootMotionTarget", c.rootMotionTarget);
        }
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 AnimationGraphComponent& c)
    {
        c.skeleton.Bind(manager);
        c.graph.Bind(manager);
    }

    // Ticks every AnimationGraphComponent in ScenePhase::PostUpdate, as the skeletal manager does
    // but evaluating an AnimationGraphPlayer. Runs at a lower UpdateOrder than
    // SkeletalAnimationComponentManager, so the graph runs before the clip; an entity is expected to
    // use one or the other (mixing both pushes to the same MeshComponent, and the later writer wins).
    class AnimationGraphComponentManager final
        : public scene::SerializableComponentManager<AnimationGraphComponent>, public scene::ISkinningPaletteSource
    {
    public:
        AnimationGraphComponentManager()
            : scene::SerializableComponentManager<AnimationGraphComponent>(u8"animation_graph")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

    protected:
        void OnComponentDestroyed(AnimationGraphComponent& c, scene::EntityHandle) override
        {
            if (m_scene != nullptr)
            {
                root_motion_apply::Release(*m_scene, c.rootMotionState);
            }
        }

    public:
        // Time is simulation-gated and the pose is not, as in the clip manager (see its comment).

        // Run before the simple-clip manager (UpdateOrder 0) so the graph drives graph-backed entities.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -1; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            PROFILE_SCOPE("Animation.Graph");
            m_posed.Clear(); // recorded again below: an animator gone, or a mesh it stopped naming, drops out
            ForEach(
                [&](AnimationGraphComponent& a, scene::EntityHandle owner)
                {
                    // Recorded first, so a frozen animator's meshes keep its last pose.
                    RecordPosedMeshes(m_posed, *m_scene, *meshes, a, owner);
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return; // frozen
                    }
                    animation::Skeleton* skeleton = a.skeleton.Get();
                    animation::AnimationGraph* graph = a.graph.Get();
                    if (skeleton == nullptr || graph == nullptr)
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return;
                    }
                    // Rebuild the player when either object changed: the first tick, a pick, a reload.
                    if (a.player.Get() == nullptr || a.playerSkeleton != skeleton ||
                        a.playerGraph != graph)
                    {
                        a.player = MakeUnique<animation::AnimationGraphPlayer>(DefaultAllocator(),
                                                                               *graph, *skeleton);
                        a.playerSkeleton = skeleton;
                        a.playerGraph = graph;
                    }
                    if (!a.active)
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return;
                    }
                    if (m_scene->SimulationEnabled())
                    {
                        OneShots::Start(a.oneShot, a.player->Overlay());
                        a.player->Update(deltaTime);
                        OneShots::Ended(a.oneShot, a.player->ConsumeOneShotEnded(), owner, m_ended);
                        root_motion_apply::Apply(
                            *m_scene, owner, Span<const scene::EntityRef>{a.meshEntities.Data(), a.meshEntities.Size()},
                            a.rootMotionTarget, a.rootMotion, a.rootMotionState, a.player->ConsumeRootMotion(), deltaTime);
                    }
                    else
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                    }
                    // The pose is evaluated here, once a frame, whether or not anything draws it: its
                    // modifiers (foot IK's ground probes) step with the frame, not with a reader.
                    (void)a.player->GetSkinningMatrices();
                });
        }

        // The one-shots that ended since the last call (the animation subsystem tells its listeners).
        void DrainEnded(Array<OneShotEnded>& out)
        {
            for (const OneShotEnded& e : m_ended)
            {
                out.PushBack(e);
            }
            m_ended.Clear();
        }

        [[nodiscard]] scene::ISkinningPaletteSource* AsSkinningPaletteSource() noexcept override { return this; }
        // The palette of the animator that posed `mesh` at its last update, read from its player now.
        [[nodiscard]] bool SkinningPaletteOf(scene::EntityHandle mesh, scene::SkinningPalette& out) override
        {
            const scene::EntityHandle animator = m_posed.AnimatorOf(mesh);
            const auto* a = animator.IsAssigned() ? Get(animator) : nullptr;
            if (a == nullptr || a->player.Get() == nullptr)
            {
                return false; // the animator went (its player with it): the mesh draws static
            }
            const Span<const Float4x4> mats = a->player->GetSkinningMatrices();
            const Span<const Float4x4> prev = a->player->GetPrevSkinningMatrices();
            out.current = mats.Data();
            out.previous = prev.Data();
            out.boneCount = static_cast<u32>(mats.Size());
            return true;
        }

    private:
        scene::Scene* m_scene = nullptr;
        Array<OneShotEnded> m_ended;
        PosedMeshes m_posed;
    };

    // Instanced skinning for crowds: the companion to an engine::render::InstancedMeshComponent that
    // makes its N instances animate from a few shared poses. Each frame the manager samples each of
    // its clips at `poseCount` evenly spaced phases (each clip on its own clock) into one pose pool,
    // a block of poseCount palettes per clip in list order, and feeds the pool to the target
    // instanced meshes: an instance's clip index picks the block and the mesh's pose policy the
    // phase (render::SelectPose). So a crowd of thousands costs clips x poseCount palette computes.
    // Authored like SkeletalAnimationComponent: the skeleton and clips are asset references (or
    // direct objects from native code), the targets entity references (empty: its own entity).
    struct InstancedSkinningComponent
    {
        foundation::resource::Ref<animation::Skeleton> skeleton;
        Array<foundation::resource::Ref<animation::AnimationClip>> clips; // an instance's clip index is its slot here
        u32 poseCount = 32; // phases per clip (more = smoother spread, more compute)
        f32 speed = 1.0f;
        Array<scene::EntityRef> targets; // instanced meshes to feed (a multi-part character has one
                                         // per skinned mesh); empty: its own entity
        // Manager-owned per-frame state (not authored).
        Array<Float4x4> posePool; // clips x poseCount x boneCount skinning matrices, recomputed each frame
        Array<Float4x4>
            prevPosePool; // last frame's palettes (per-bone motion vectors); ping-ponged, not recomputed
        Array<BoneTransform> scratch; // boneCount scratch for SampleClip
        Array<f32> times;             // each clip's clock (wrapped to its duration)
        u32 boneCount = 0;
    };

    inline void Serialize(ISerializer& ar, InstancedSkinningComponent& c)
    {
        foundation::core::Serialize(ar, "skeleton", c.skeleton);
        foundation::core::Serialize(ar, "clips", c.clips);
        foundation::core::Serialize(ar, "poseCount", c.poseCount);
        foundation::core::Serialize(ar, "speed", c.speed);
        foundation::core::Serialize(ar, "targets", c.targets);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 InstancedSkinningComponent& c)
    {
        c.skeleton.Bind(manager);
        for (foundation::resource::Ref<animation::AnimationClip>& clip : c.clips)
        {
            clip.Bind(manager);
        }
    }

    // Ticks every InstancedSkinningComponent in PostUpdate (before render extraction): advances each clip's clock,
    // samples each clip at poseCount phases into the pose pool, and hands the pool to the target InstancedMeshComponents.
    class InstancedSkinningComponentManager final : public scene::SerializableComponentManager<InstancedSkinningComponent>
    {
    public:
        InstancedSkinningComponentManager()
            : scene::SerializableComponentManager<InstancedSkinningComponent>(u8"instanced_skinning")
        {
        }
        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        // Simulation-gated like the clip manager (see its comment).
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* imm = m_scene->GetSystem<engine::render::InstancedMeshComponentManager>();
            if (imm == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Animation.InstancedSkinning");

            ForEach(
                [&](InstancedSkinningComponent& s, scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return; // frozen
                    }
                    animation::Skeleton* skeleton = s.skeleton.Get();
                    const u32 clipCount = static_cast<u32>(s.clips.Size());
                    if (skeleton == nullptr || clipCount == 0 || s.poseCount == 0)
                    {
                        return;
                    }
                    const u32 boneCount = static_cast<u32>(skeleton->BoneCount());
                    if (boneCount == 0)
                    {
                        return;
                    }
                    s.boneCount = boneCount;
                    const u32 poses = clipCount * s.poseCount;
                    const usize poolSize = static_cast<usize>(poses) * boneCount;

                    // Ping-pong: last frame's pool becomes this frame's previous one (per-bone motion vectors), with no re-sampling.
                    {
                        Array<Float4x4> tmp = static_cast<Array<Float4x4>&&>(s.posePool);
                        s.posePool = static_cast<Array<Float4x4>&&>(s.prevPosePool);
                        s.prevPosePool = static_cast<Array<Float4x4>&&>(tmp);
                    }
                    s.posePool.Resize(poolSize);
                    s.scratch.Resize(boneCount);
                    s.times.Resize(clipCount, 0.0f);

                    // Each clip on its own clock, sampled at poseCount evenly spaced phases into its block.
                    for (u32 k = 0; k < clipCount; ++k)
                    {
                        animation::AnimationClip* clip = s.clips[k].Get();
                        const f32 duration = (clip != nullptr && clip->duration > 0.0f) ? clip->duration : 1.0f;
                        f32& time = s.times[k];
                        time += deltaTime * s.speed;
                        while (time >= duration)
                        {
                            time -= duration;
                        }
                        while (time < 0.0f)
                        {
                            time += duration;
                        }
                        for (u32 m = 0; m < s.poseCount; ++m)
                        {
                            Float4x4* palette = s.posePool.Data() + static_cast<usize>(k * s.poseCount + m) * boneCount;
                            if (clip == nullptr)
                            {
                                // A clip not loaded (yet): its block holds the bind pose.
                                for (u32 b = 0; b < boneCount; ++b)
                                {
                                    palette[b] = Float4x4::Identity();
                                }
                                continue;
                            }
                            f32 p = time + (static_cast<f32>(m) / static_cast<f32>(s.poseCount)) * duration;
                            while (p >= duration)
                            {
                                p -= duration;
                            }
                            SampleClip(*clip, *skeleton, p, Span<BoneTransform>{s.scratch.Data(), s.scratch.Size()});
                            skeleton->ComputeSkinningMatrices(
                                Span<const BoneTransform>{s.scratch.Data(), s.scratch.Size()},
                                Span<Float4x4>{palette, boneCount});
                        }
                    }
                    // First frame or a pool size change: no previous pool yet, so it copies the current one (zero motion).
                    if (s.prevPosePool.Size() != poolSize)
                    {
                        s.prevPosePool.Resize(poolSize);
                        if (poolSize > 0)
                        {
                            MemCopy(s.prevPosePool.Data(), s.posePool.Data(),
                                    poolSize * sizeof(Float4x4));
                        }
                    }

                    const auto feed = [&](scene::EntityHandle e)
                    {
                        if (engine::render::InstancedMeshComponent* c = imm->Get(e))
                        {
                            c->posePool =
                                s.posePool
                                    .Data(); // borrowed for the frame (the component keeps the storage alive)
                            c->prevPosePool =
                                s.prevPosePool
                                    .Data(); // last frame's palettes (per-bone motion vectors)
                            c->poseCount = poses;
                            c->posesPerClip = s.poseCount; // a block per clip: the instance's clip picks it
                            c->boneCount = boneCount;
                        }
                    };
                    if (s.targets.IsEmpty())
                    {
                        feed(owner);
                    }
                    else
                    {
                        for (const scene::EntityRef& target : s.targets)
                        {
                            feed(m_scene->FindEntity(target.id)); // guid -> live handle each frame
                        }
                    }
                });
        }

    private:
        scene::Scene* m_scene = nullptr;
    };

    // A scene-bound animation handle (scene.animation): runtime world ops on the animation
    // components that need the manager-owned runtime player, which the component data cannot reach.
    // It plays, stops and pauses a single-clip player, drives a graph's parameters and swaps the clip
    // by resource id. Keyed by entity, like ScenePhysics, SceneRender and SceneAudio: the component
    // is auto-reflected data (speed, autoPlay, active) and the scene handle carries the world ops.
    // The managers build the players in PostUpdate, so control from a behavior's onUpdate sees them;
    // a call before the first animation tick (e.g. onStart) is a safe no-op.
    struct SceneAnimation
    {
        scene::WeakSceneRef scene; // read through the scene's life: null once it is gone

        // --- single-clip playback (SkeletalAnimationComponent) ---
        // Play the entity's currently-bound clip from the start (manual re-trigger; autoPlay covers
        // the first start). No-op if the player is not built yet or the entity has no skeletal anim.
        void play(foundation::script::Entity entity) const
        {
            SkeletalAnimationComponent* c = Skeletal(entity);
            if (c != nullptr && c->player.Get() != nullptr)
            {
                c->player->Play(c->clip.Get());
            }
        }
        void stop(foundation::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Stop();
            }
        }
        void pause(foundation::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Pause();
            }
        }
        void resume(foundation::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Resume();
            }
        }
        [[nodiscard]] bool isPlaying(foundation::script::Entity entity) const
        {
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr && p->State() == animation::PlaybackState::Playing;
        }
        [[nodiscard]] f32 time(foundation::script::Entity entity) const
        {
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr ? p->CurrentTime() : 0.0f;
        }
        void setTime(foundation::script::Entity entity, f32 seconds) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->SetCurrentTime(seconds);
            }
        }
        // Swap the entity's clip as setClip does, easing from the pose shown now into the new clip
        // over `blendSeconds` instead of snapping.
        void setClip(foundation::script::Entity entity, Guid id, f32 blendSeconds) const
        {
            if (SkeletalAnimationComponent* c = Skeletal(entity))
            {
                c->clipFade = Max(0.0f, blendSeconds);
            }
            setClip(entity, id);
        }

        // --- one-shots: a clip played once over whatever the entity is animating (its clip or its
        // graph), blended in and out, after which it carries on with its own animation; its end
        // reaches the entity's behaviours as onAnimationFinished(clip) ---
        // A clip's length in seconds (0 when it is not a clip, or cannot load).
        [[nodiscard]] f32 clipLength(Guid clip) const
        {
            foundation::resource::ResourceManager* resources = foundation::script::CurrentRunResources();
            if (resources == nullptr || clip.IsNil())
            {
                return 0.0f;
            }
            foundation::resource::Ref<animation::AnimationClip> ref;
            ref.SetId(clip);
            ref.Bind(*resources);
            return ref.Get() != nullptr ? ref.Get()->duration : 0.0f;
        }
        // Plays `clip` once over the entity's animation, blending in and out over 0.2 s.
        void playOneShot(foundation::script::Entity entity, Guid clip) const
        {
            playOneShot(entity, clip, 1.0f, 0.2f, 0.2f, 0);
        }
        // The same at `speed`, blending in over `blendIn` and out over `blendOut` seconds, playing
        // `loops` more times after the first (-1: until stopped). One already playing gives way.
        void playOneShot(foundation::script::Entity entity, Guid clip, f32 speed, f32 blendIn, f32 blendOut,
                         i32 loops) const
        {
            OneShotRuntime* o = OneShotOf(entity);
            foundation::resource::ResourceManager* resources = foundation::script::CurrentRunResources();
            if (o == nullptr || clip.IsNil() || resources == nullptr)
            {
                return;
            }
            o->clip.SetId(clip);
            o->clip.Bind(*resources);
            o->settings = animation::OneShotSettings{speed, Max(0.0f, blendIn), Max(0.0f, blendOut), loops};
            o->pending = true;
        }
        // Blends the entity's one-shot out over 0.2 s (it then reports its end).
        void stopOneShot(foundation::script::Entity entity) const { stopOneShot(entity, 0.2f); }
        void stopOneShot(foundation::script::Entity entity, f32 blendOut) const
        {
            OneShotRuntime* o = OneShotOf(entity);
            if (o == nullptr)
            {
                return;
            }
            o->pending = false;
            if (animation::PoseOverlay* overlay = OverlayOf(entity))
            {
                overlay->StopOneShot(Max(0.0f, blendOut));
            }
        }
        // From the call until the one-shot has blended out.
        [[nodiscard]] bool isPlayingOneShot(foundation::script::Entity entity) const
        {
            const OneShotRuntime* o = OneShotOf(entity);
            if (o == nullptr)
            {
                return false;
            }
            const animation::PoseOverlay* overlay = OverlayOf(entity);
            return o->pending || (overlay != nullptr && overlay->IsPlayingOneShot());
        }

        // Swap the entity's animation clip to resource `id`, binding it through the run's resource
        // manager; the manager picks up the change next tick (autoPlay replays it).
        void setClip(foundation::script::Entity entity, Guid id) const
        {
            if (SkeletalAnimationComponent* c = Skeletal(entity))
            {
                c->clip.SetId(id);
                if (auto* resources = foundation::script::CurrentRunResources())
                {
                    c->clip.Bind(*resources);
                }
            }
        }

        // --- state-machine parameters (AnimationGraphComponent) ---
        void setFloat(foundation::script::Entity entity, String name, f32 value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetFloat(name.AsView(), value);
            }
        }
        void setInt(foundation::script::Entity entity, String name, i32 value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetInt(name.AsView(), value);
            }
        }
        void setBool(foundation::script::Entity entity, String name, bool value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetBool(name.AsView(), value);
            }
        }
        void setTrigger(foundation::script::Entity entity, String name) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetTrigger(name.AsView());
            }
        }

        // --- inverse kinematics (TwoBoneIkComponent / AimIkComponent on the entity) ---
        // The world point the entity's IK components reach for while they name no target entity.
        // Defined in the implementation unit (this partition cannot see :ik).
        void setIkTarget(foundation::script::Entity entity, Float3 worldPosition) const;
        // Whether every IK component on the entity reached its target on the last solve, and the
        // largest miss (metres for a two-bone chain, radians for an aim).
        [[nodiscard]] bool ikReached(foundation::script::Entity entity) const;
        [[nodiscard]] f32 ikError(foundation::script::Entity entity) const;

        // --- root motion (Manual mode) ---
        // The entity's animator's travel over its last tick, in the world, and its turn (radians
        // about up): a script steering by the clip's own speed (an animator in Manual mode moves
        // nothing itself). Zero for an entity with no animator.
        [[nodiscard]] Float3 rootMotionTranslation(foundation::script::Entity entity) const
        {
            const RootMotionRuntime* rt = RootMotionOf(entity);
            return rt != nullptr ? rt->worldTranslation : Float3{};
        }
        [[nodiscard]] f32 rootMotionYaw(foundation::script::Entity entity) const
        {
            const RootMotionRuntime* rt = RootMotionOf(entity);
            return rt != nullptr ? rt->yaw : 0.0f;
        }

        // The scene's own, read by scripts as a property of the scene (ExtendScene).
        [[nodiscard]] static SceneAnimation ForScene(const foundation::script::Scene& sceneHandle)
        {
            return SceneAnimation{sceneHandle.scene};
        }

    private:
        [[nodiscard]] const RootMotionRuntime* RootMotionOf(foundation::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            if (auto* graphs = scene->GetSystem<AnimationGraphComponentManager>())
            {
                if (const AnimationGraphComponent* g = graphs->Get(entity.Handle()))
                {
                    return &g->rootMotionState;
                }
            }
            const SkeletalAnimationComponent* c = Skeletal(entity);
            return c != nullptr ? &c->rootMotionState : nullptr;
        }
        [[nodiscard]] SkeletalAnimationComponent* Skeletal(foundation::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            auto* manager = scene->GetSystem<SkeletalAnimationComponentManager>();
            return (manager != nullptr) ? manager->Get(entity.Handle()) : nullptr;
        }
        [[nodiscard]] animation::AnimationPlayer* SkeletalPlayer(foundation::script::Entity e) const
        {
            SkeletalAnimationComponent* c = Skeletal(e);
            return (c != nullptr) ? c->player.Get() : nullptr;
        }
        [[nodiscard]] OneShotRuntime* OneShotOf(foundation::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            if (auto* graphs = scene->GetSystem<AnimationGraphComponentManager>())
            {
                if (AnimationGraphComponent* g = graphs->Get(entity.Handle()))
                {
                    return &g->oneShot;
                }
            }
            SkeletalAnimationComponent* c = Skeletal(entity);
            return c != nullptr ? &c->oneShot : nullptr;
        }
        [[nodiscard]] animation::PoseOverlay* OverlayOf(foundation::script::Entity entity) const
        {
            if (animation::AnimationGraphPlayer* g = GraphPlayer(entity))
            {
                return &g->Overlay();
            }
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr ? &p->Overlay() : nullptr;
        }
        [[nodiscard]] animation::AnimationGraphPlayer* GraphPlayer(foundation::script::Entity e) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            auto* manager = scene->GetSystem<AnimationGraphComponentManager>();
            AnimationGraphComponent* c = (manager != nullptr) ? manager->Get(e.Handle()) : nullptr;
            return (c != nullptr) ? c->player.Get() : nullptr;
        }
    };

} // exported namespace

// Reflection (tooling: the editor inspector). The REFLECT_VALUE bodies and
// RegisterAnimationComponentReflection() live in AnimationSubsystemImpl.cpp, kept out of this
// interface partition because GCC cannot read the module interface otherwise.
export namespace engine::animation
{
    void RegisterAnimationComponentReflection();

    // Surfaces the animation components to scripts: SkeletalAnimationComponent.of(entity) and
    // AnimationGraphComponent.of(entity) for the data (speed, autoPlay, active), plus
    // scene.animation for the world ops (play/stop, graph params, clip swap). Registers,
    // seeds and names them so both backends bind. Called by the composition root (like
    // RegisterRenderScriptFacade).
    void RegisterAnimationScriptFacade();
}
