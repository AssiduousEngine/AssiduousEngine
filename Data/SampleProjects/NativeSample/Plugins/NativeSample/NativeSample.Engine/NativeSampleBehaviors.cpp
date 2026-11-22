// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// nativesample.engine's implementation unit: the behaviors' reflection (the REFLECT_* bodies live
// in the module's purview) and what they log.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module nativesample.engine;

import foundation.core;

namespace nativesample
{
    // Their properties are the fields marked behaviorProperty: what the inspector shows and an
    // entity or the scene sets. The rest (a frame count, the scene's clock) are theirs alone.
    REFLECT_MEMBERS(Bobber, "rtti::nativesample")
    {
        builder.Property<&Bobber::height>("height")
            .PropAttribute("behaviorProperty", true)
            .PropAttribute("description", foundation::core::String(u8"Metres from the middle of the bob to either end"));
        builder.Property<&Bobber::period>("period")
            .PropAttribute("behaviorProperty", true)
            .PropAttribute("description", foundation::core::String(u8"Seconds for one whole bob"));
        builder.Method<&Bobber::OnStart>("onStart");
        builder.Method<&Bobber::OnUpdate>("onUpdate");
        builder.Creator();
    }

    void Showcase::OnStart() { LOG_INFO(u8"Game", u8"{}: the scene starts (a native scene behavior)", title); }

    REFLECT_MEMBERS(Showcase, "rtti::nativesample")
    {
        builder.Property<&Showcase::title>("title").PropAttribute("behaviorProperty", true);
        builder.Property<&Showcase::seconds>("seconds");
        builder.Method<&Showcase::OnStart>("onStart");
        builder.Method<&Showcase::OnUpdate>("onUpdate");
        builder.Creator();
    }

    void NativeGame::OnStart() { LOG_INFO(u8"Game", u8"NativeSample: the game starts (a native run behavior)"); }

    REFLECT_MEMBERS(NativeGame, "rtti::nativesample")
    {
        builder.Property<&NativeGame::frames>("frames");
        builder.Method<&NativeGame::OnStart>("onStart");
        builder.Method<&NativeGame::OnUpdate>("onUpdate");
        builder.Creator();
    }
}
