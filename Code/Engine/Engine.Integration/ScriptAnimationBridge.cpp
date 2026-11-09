// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Integration - implementation unit: the animation bridge's bodies (the virtual override
// and the subsystem calls stay out of the interface, as the physics bridge's do).

module;
#include "Core/Prelude.h"

module engine.integration;

import foundation.core;
import engine.animation;
import engine.script;

using namespace foundation::core;

namespace engine::integration
{
    void ScriptAnimationBridge::Listener::OnOneShotEnded(const engine::animation::AnimationOneShotEnded& ended)
    {
        if (scripts == nullptr)
        {
            return;
        }
        Array<Variant> args;
        args.PushBack(Variant::From<Guid>(ended.clip));
        scripts->DeliverEvent(ended.scene, ended.entity, u8"onAnimationFinished", Move(args));
    }

    ScriptAnimationBridge::~ScriptAnimationBridge()
    {
        Uninstall();
    }

    void ScriptAnimationBridge::Install(engine::animation::AnimationSubsystem& animation,
                                        engine::script::ScriptSubsystem& scripts)
    {
        Uninstall();
        m_listener.scripts = &scripts;
        m_animation = &animation;
        m_animation->RegisterListener(&m_listener);
    }

    void ScriptAnimationBridge::Uninstall()
    {
        if (m_animation != nullptr)
        {
            m_animation->UnregisterListener(&m_listener);
            m_animation = nullptr;
        }
        m_listener.scripts = nullptr;
    }
}
