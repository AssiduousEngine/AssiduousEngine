// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input - InputSubsystem implementation unit: the Input facade reflection body.
//
// Kept out of the interface: REFLECT_* bodies in an interface unit make GCC emit an unreadable gcm
// cluster for consumers. The interface declares RegisterInputScriptFacade(); this unit defines it
// and Input::StaticType().

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.input;
import engine.domain;
import foundation.input.resource;

import foundation.core;
import foundation.script.facades; // RegisterExtraFacadeName (Input into the behavior prelude)

using namespace foundation::core;
using namespace foundation::input;

namespace engine::input
{
    REFLECT_MEMBERS(InputFacade, "rtti::engine::input")
    {
        builder.Attribute("scriptName", "Input");
        builder.Method<&InputFacade::isDown>("isDown");
        builder.Method<&InputFacade::wasPressed>("wasPressed");
        builder.Method<&InputFacade::wasReleased>("wasReleased");
        builder.Method<&InputFacade::value>("value");
        builder.Method<&InputFacade::valueX>("valueX");
        builder.Method<&InputFacade::valueY>("valueY");
        builder.Method<&InputFacade::value2D>("value2D", {"action"});
        builder.Method<&InputFacade::pushSet>("pushSet");
        builder.Method<&InputFacade::popSet>("popSet");
        builder.Method<&InputFacade::enableSet>("enableSet");
        // Rumble, by arity: gamepad 0, or a pad by index; stopRumble stops every pad.
        builder.Method<static_cast<void (*)(f32, f32, f32)>(&InputFacade::rumble)>("rumble",
                                                                            {"low", "high", "seconds"});
        builder.Method<static_cast<void (*)(i32, f32, f32, f32)>(&InputFacade::rumble)>(
            "rumble", {"gamepad", "low", "high", "seconds"});
        builder.Method<&InputFacade::stopRumble>("stopRumble");
        builder.Method<&InputFacade::lockPointer>("lockPointer", {"locked"});
        builder.Method<&InputFacade::pointerLocked>("pointerLocked");
        // The control an action takes on the device in hand, by arity: the action, or it in a set.
        builder.Method<static_cast<String (*)(String)>(&InputFacade::describe)>("describe", {"action"});
        builder.Method<static_cast<String (*)(String, String)>(&InputFacade::describe)>("describe", {"action", "set"});
        // Some backends only materialize constructible types as foreign classes.
        builder.Constructor();
    }

    void RegisterInputScriptFacade()
    {
        GlobalTypeRegistry().Register(InputFacade::StaticType());
        // So the behavior and Level prelude imports `Input` too (AngelScript binds by registry).
        // Without this only top-level `main` and Game scripts can see it. Idempotent.
        foundation::script::RegisterExtraFacadeName(u8"Input");
    }
}

namespace engine::input
{
    const engine::DomainModule& InputDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::input::kInputResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"input",
            .registerScriptFacade = &RegisterInputScriptFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
