// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// nativesample.engine - the reference game's runtime types: a component the game puts on an entity,
// which scripts reach as SpinComponent, and the scene manager that owns it and turns its entities,
// which the Engine part adds to every scene; and the game's logic written in C++ as behaviors, one of each kind, with no script: the
// game (NativeGame), the scene's (Showcase) and an entity's (Bobber). Each is a Native Behavior
// asset in the project, named like its class.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module nativesample.engine;

import foundation.core;
import foundation.scene;
import foundation.script.facades; // Entity, Scene, Time: what a behavior reads
import engine.behavior;           // the kinds' native bases

export namespace nativesample
{
    // Turns its entity about an axis of the entity's own (its local up by default) while the scene
    // simulates.
    struct SpinComponent
    {
        foundation::core::f32 turnsPerSecond = 0.25f;
        foundation::core::Float3 axis{0.0f, 1.0f, 0.0f};
    };

    inline void Serialize(foundation::core::ISerializer& ar, SpinComponent& component)
    {
        foundation::core::Serialize(ar, "turnsPerSecond", component.turnsPerSecond);
        foundation::core::Serialize(ar, "axis", component.axis);
    }

    // The spin components of a scene, saved with it; each update turns their entities.
    class SpinManager final : public foundation::scene::SerializableComponentManager<SpinComponent>
    {
    public:
        SpinManager() : SerializableComponentManager<SpinComponent>(u8"nativesample.Spin") {}

        void OnSceneCreate(foundation::scene::Scene& scene) override { m_scene = &scene; }

        // The editor's edit mode does not turn them; a simulating page, play and the player do.
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnUpdate(foundation::scene::ScenePhase phase, foundation::core::f32 deltaTime) override
        {
            if (phase != foundation::scene::ScenePhase::Update || m_scene == nullptr)
            {
                return;
            }
            ForEach(
                [&](const SpinComponent& spin, foundation::scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return;
                    }
                    const foundation::core::Quaternion turn = foundation::core::Quaternion::FromAxisAngle(
                        spin.axis, spin.turnsPerSecond * foundation::core::kTwoPi * deltaTime);
                    // The turn applies in the entity's own frame; normalizing keeps the rotation a
                    // unit one over many frames of accumulated products.
                    foundation::core::Transform local = m_scene->GetLocalTransform(owner);
                    local.rotation = foundation::core::Normalized(local.rotation * turn);
                    m_scene->SetLocalTransform(owner, local);
                });
        }

    private:
        foundation::scene::Scene* m_scene = nullptr;
    };

    // Bobs its entity up and down about where it stood when it started, a full bob `period` seconds
    // long, `height` metres from the middle to either end.
    class Bobber final : public engine::behavior::EntityBehavior
    {
        RTTI_OBJECT(Bobber, engine::behavior::EntityBehavior)
    public:
        foundation::core::f32 height = 0.25f;
        foundation::core::f32 period = 2.0f;

        void OnStart()
        {
            m_base = Self().position();
            m_time = 0.0f;
        }
        void OnUpdate(foundation::core::f32 deltaTime)
        {
            if (period <= 0.0f)
            {
                return;
            }
            m_time += deltaTime;
            const foundation::core::f32 rise = height * foundation::core::Sin(foundation::core::kTwoPi * m_time / period);
            Self().setPosition(m_base.x, m_base.y + rise, m_base.z);
        }

    private:
        foundation::core::Float3 m_base{};
        foundation::core::f32 m_time = 0.0f;
    };

    // The scene's own behavior: it says what the scene shows as it starts, and keeps how long it has
    // run.
    class Showcase final : public engine::behavior::SceneBehavior
    {
        RTTI_OBJECT(Showcase, engine::behavior::SceneBehavior)
    public:
        foundation::core::String title = foundation::core::String(u8"Spinners");
        foundation::core::f32 seconds = 0.0f;

        void OnStart();
        void OnUpdate(foundation::core::f32 deltaTime) { seconds += deltaTime; }
    };

    // The game's own object, for the whole run: it counts the frames it has seen.
    class NativeGame final : public engine::behavior::RunBehavior
    {
        RTTI_OBJECT(NativeGame, engine::behavior::RunBehavior)
    public:
        foundation::core::i32 frames = 0;

        void OnStart();
        void OnUpdate(foundation::core::f32) { ++frames; }
    };

    // The id the Engine part contributes the manager under.
    inline constexpr foundation::core::StringView kSpinContribution = u8"nativesample.spin";

    // What scripts see of the package: SpinComponent, with of, addTo and removeFrom as the engine's
    // components have. Its domain registers it as the part loads; the unload takes it out again.
    inline void RegisterSpinScriptFacade()
    {
        foundation::script::RegisterExtraScriptRootType(&foundation::core::TypeOf<SpinComponent>());
        foundation::script::RegisterExtraFacadeName(u8"SpinComponent");
    }
    inline void RemoveSpinScriptFacade()
    {
        foundation::script::RemoveExtraScriptRootType(&foundation::core::TypeOf<SpinComponent>());
        foundation::script::RemoveExtraFacadeName(u8"SpinComponent");
    }
}
