// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The player's rebinding: controls by name, a rebind replacing one device's binding only, the
// overrides kept over the game's map, a capture taking the next press while every action rests.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.shell;
import foundation.input;

using namespace foundation::core;
using namespace foundation::input;
namespace shell = foundation::shell;

namespace
{
    Binding KeyBinding(shell::KeyCode key)
    {
        Binding binding;
        binding.source = BindingSource::Key;
        binding.code = static_cast<u32>(key);
        return binding;
    }
    Binding PadBinding(shell::GamepadButton button)
    {
        Binding binding;
        binding.source = BindingSource::GamepadButton;
        binding.code = static_cast<u32>(button);
        return binding;
    }

    // Gameplay: Jump (Space, then the pad's South) and Move (WASD); Menu: Back (Escape).
    InputMap TwoSets()
    {
        InputMap map;
        ActionSet gameplay;
        gameplay.name = String(u8"Gameplay");
        Action jump;
        jump.name = String(u8"Jump");
        jump.bindings.PushBack(KeyBinding(shell::KeyCode::Space));
        jump.bindings.PushBack(PadBinding(shell::GamepadButton::South));
        gameplay.actions.PushBack(jump);
        Action move;
        move.name = String(u8"Move");
        move.kind = ActionKind::Axis2D;
        Binding wasd;
        wasd.source = BindingSource::Composite2D;
        move.bindings.PushBack(wasd);
        gameplay.actions.PushBack(move);
        map.sets.PushBack(gameplay);
        ActionSet menu;
        menu.name = String(u8"Menu");
        menu.priority = 10;
        Action back;
        back.name = String(u8"Back");
        back.bindings.PushBack(KeyBinding(shell::KeyCode::Escape));
        menu.actions.PushBack(back);
        map.sets.PushBack(menu);
        return map;
    }

    ScriptedInput KeyAt(f64 at, shell::KeyCode key, bool down)
    {
        ScriptedInput input;
        input.at = at;
        input.kind = ScriptedInputKind::Key;
        input.key = key;
        input.down = down;
        return input;
    }
    ScriptedInput PadAt(f64 at, shell::GamepadButton button, bool down)
    {
        ScriptedInput input;
        input.at = at;
        input.kind = ScriptedInputKind::PadButton;
        input.padButton = button;
        input.down = down;
        return input;
    }
}

TEST_CASE("player bindings: a control by name is a binding, any case; an unknown one is none")
{
    const Optional<Binding> space = ParseControl(u8"Key.Space");
    REQUIRE(space.HasValue());
    CHECK(space->source == BindingSource::Key);
    CHECK(space->code == static_cast<u32>(shell::KeyCode::Space));
    const Optional<Binding> south = ParseControl(u8"pad.south");
    REQUIRE(south.HasValue());
    CHECK(south->source == BindingSource::GamepadButton);
    const Optional<Binding> trigger = ParseControl(u8"PadAxis.RightTrigger");
    REQUIRE(trigger.HasValue());
    CHECK(trigger->source == BindingSource::GamepadAxis);
    CHECK(ParseControl(u8"Mouse.Left").HasValue());
    CHECK_FALSE(ParseControl(u8"Space").HasValue());
    CHECK_FALSE(ParseControl(u8"Key.Nothing").HasValue());
    CHECK_FALSE(ParseControl(u8"Wheel.Up").HasValue());
}

TEST_CASE("player bindings: a rebind replaces the device's binding and keeps the other's, the sets as they were; a reset restores")
{
    ActionRuntime runtime;
    PlayerBindings bindings;
    usize changes = 0;
    InputBindingOverrides store;
    bindings.SetStore(&store, [&changes]() { ++changes; });
    bindings.Install(runtime, TwoSets());
    runtime.DisableSet(u8"Menu");

    REQUIRE(bindings.Set(StringView{}, u8"Jump", KeyBinding(shell::KeyCode::J)));
    const Action* jump = runtime.FindAction(u8"Gameplay", u8"Jump");
    REQUIRE(jump != nullptr);
    REQUIRE(jump->bindings.Size() == 2u);
    CHECK(jump->bindings[0].code == static_cast<u32>(shell::KeyCode::J));
    CHECK(jump->bindings[1].source == BindingSource::GamepadButton); // the pad's kept
    CHECK_FALSE(runtime.IsSetEnabled(u8"Menu"));                     // the sets as the game left them
    CHECK(changes == 1u);
    REQUIRE(store.overrides.Size() == 1u);
    CHECK(store.overrides[0].setName == StringView(u8"Gameplay"));

    CHECK_FALSE(bindings.Set(StringView{}, u8"Move", KeyBinding(shell::KeyCode::J))); // two axes
    CHECK_FALSE(bindings.Set(StringView{}, u8"Fly", KeyBinding(shell::KeyCode::J)));

    REQUIRE(bindings.Reset(StringView{}, u8"Jump"));
    CHECK(runtime.FindAction(u8"Gameplay", u8"Jump")->bindings[0].code == static_cast<u32>(shell::KeyCode::Space));
    CHECK(store.overrides.IsEmpty());
}

TEST_CASE("player bindings: overrides saved before lie over the game's map when it is installed, and again when the store changes")
{
    InputBindingOverrides saved;
    Array<Binding> pad;
    pad.PushBack(PadBinding(shell::GamepadButton::East));
    saved.Set(u8"Gameplay", u8"Jump", Move(pad));

    ActionRuntime runtime;
    PlayerBindings bindings;
    bindings.Install(runtime, TwoSets()); // no store yet: the game's own
    CHECK(runtime.FindAction(u8"Gameplay", u8"Jump")->bindings.Size() == 2u);
    bindings.SetStore(&saved, Function<void()>{});
    bindings.Reapply();
    const Action* jump = runtime.FindAction(u8"Gameplay", u8"Jump");
    REQUIRE(jump->bindings.Size() == 1u);
    CHECK(jump->bindings[0].code == static_cast<u32>(shell::GamepadButton::East));

    bindings.ResetAll();
    CHECK(runtime.FindAction(u8"Gameplay", u8"Jump")->bindings.Size() == 2u);
    CHECK(saved.overrides.IsEmpty());
}

TEST_CASE("player bindings: a capture rests every action, takes the next press of its device, and Escape cancels")
{
    ActionRuntime runtime;
    PlayerBindings bindings;
    bindings.Install(runtime, TwoSets());
    ScriptedInputSource source(DefaultAllocator());
    REQUIRE(source.Add(KeyAt(0.0, shell::KeyCode::Space, true)));  // held as the capture begins
    REQUIRE(source.Add(PadAt(0.1, shell::GamepadButton::West, true))); // another device: ignored
    REQUIRE(source.Add(KeyAt(0.2, shell::KeyCode::K, true)));
    REQUIRE(source.Add(KeyAt(0.3, shell::KeyCode::Escape, true)));
    const ActionRef jump = runtime.Resolve(u8"Jump");

    source.Advance(0.0);
    bindings.Poll(source);
    runtime.Update(source, 0.016f);
    CHECK(runtime.IsDown(jump));

    REQUIRE(bindings.BeginCapture(StringView{}, u8"Jump", InputKind::Keyboard));
    CHECK_FALSE(bindings.BeginCapture(StringView{}, u8"Move", InputKind::Keyboard)); // not a button
    source.Advance(0.1);
    bindings.Poll(source);
    runtime.Update(source, 0.016f);
    CHECK(bindings.IsCapturing());
    CHECK_FALSE(runtime.IsDown(jump)); // resting while it listens, Space still held

    source.Advance(0.2);
    bindings.Poll(source); // K is taken
    runtime.Update(source, 0.016f);
    CHECK_FALSE(bindings.IsCapturing());
    const Action* rebound = runtime.FindAction(u8"Gameplay", u8"Jump");
    CHECK(rebound->bindings[0].code == static_cast<u32>(shell::KeyCode::K));
    CHECK(rebound->bindings[1].source == BindingSource::GamepadButton);
    CHECK_FALSE(runtime.IsDown(jump)); // K, held through the capture, waits for its release
    source.Advance(0.25);
    bindings.Poll(source);
    runtime.Update(source, 0.016f);
    CHECK_FALSE(runtime.IsDown(jump));

    REQUIRE(bindings.BeginCapture(StringView{}, u8"Jump", InputKind::Keyboard));
    source.Advance(0.3);
    bindings.Poll(source); // Escape cancels
    CHECK_FALSE(bindings.IsCapturing());
    CHECK(runtime.FindAction(u8"Gameplay", u8"Jump")->bindings[0].code == static_cast<u32>(shell::KeyCode::K));
}
