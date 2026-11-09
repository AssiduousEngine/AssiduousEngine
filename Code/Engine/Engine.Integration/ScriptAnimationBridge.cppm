// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Integration - the `:animation` partition.
//
// ScriptAnimationBridge forwards the animation's events to the script subsystem: a one-shot that
// ended (SceneAnimation.playOneShot) reaches the entity's behaviours as onAnimationFinished(clip).
// The animation exposes IAnimationListener and knows nothing about scripts; the script subsystem
// exposes DeliverEvent and knows nothing about animation. This is the one place that names both.

module;
#include "Core/Prelude.h"

export module engine.integration:animation;

import foundation.core;
import engine.animation; // AnimationSubsystem, IAnimationListener, AnimationOneShotEnded
import engine.script;    // ScriptSubsystem

export namespace engine::integration
{
    /// Registers an animation listener that delivers its events to a script subsystem. The
    /// composition root (the app) owns one and Install()s it once both subsystems exist; it
    /// Uninstall()s on teardown (the destructor does too).
    class ScriptAnimationBridge
    {
    public:
        ScriptAnimationBridge() = default;
        ~ScriptAnimationBridge();

        ScriptAnimationBridge(const ScriptAnimationBridge&) = delete;
        ScriptAnimationBridge& operator=(const ScriptAnimationBridge&) = delete;

        /// Point `animation`'s events at `scripts`. Calling again re-points cleanly.
        void Install(engine::animation::AnimationSubsystem& animation, engine::script::ScriptSubsystem& scripts);

        /// Unregister from the animation subsystem. Idempotent.
        void Uninstall();

        [[nodiscard]] bool Installed() const noexcept { return m_animation != nullptr; }

    private:
        struct Listener final : public engine::animation::IAnimationListener
        {
            engine::script::ScriptSubsystem* scripts = nullptr;
            void OnOneShotEnded(const engine::animation::AnimationOneShotEnded& ended) override;
        };

        Listener m_listener;
        engine::animation::AnimationSubsystem* m_animation = nullptr;
    };
}
