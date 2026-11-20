// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input - :player_bindings partition
//
// A player's own bindings over a game's controls: the game's map stays as authored, the player's
// overrides lie over it (InputBindingOverrides, kept wherever the host saves them: a run's save
// file), and a change reaches the action runtime at once without resetting the game's sets. A
// binding comes by name (ParseControl) or as the player's next press (a capture), and replaces only
// the action's binding for that kind of device: rebinding the keyboard keeps the pad's.

module;
#include "Core/Prelude.h"

export module foundation.input:player_bindings;

import foundation.core;
import foundation.shell;
import :input_map;
import :action_runtime;
import :input_kind;
import :scripted_input; // the control names' parsers

using namespace foundation::core;

export namespace foundation::input
{
    /// A control by name, as a binding: "Key.Space", "Mouse.Left", "Pad.South",
    /// "PadAxis.RightTrigger", each name as the shell's enum spells it, in any case. Empty when the
    /// device or the name is unknown.
    [[nodiscard]] inline Optional<Binding> ParseControl(StringView text)
    {
        usize dot = text.Size();
        for (usize i = 0; i < text.Size(); ++i)
            if (text[i] == u8'.')
            {
                dot = i;
                break;
            }
        if (dot == text.Size())
            return {};
        const StringView device = text.SubStr(0, dot);
        const StringView name = text.SubStr(dot + 1, text.Size() - dot - 1);
        const auto same = [](StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
            {
                utf8char x = a[i], y = b[i];
                if (x >= u8'A' && x <= u8'Z')
                    x = static_cast<utf8char>(x + 32);
                if (y >= u8'A' && y <= u8'Z')
                    y = static_cast<utf8char>(y + 32);
                if (x != y)
                    return false;
            }
            return true;
        };
        Binding binding;
        if (same(device, u8"Key"))
        {
            shell::KeyCode key{};
            if (!ScriptedInputSource::ParseKey(name, key))
                return {};
            binding.source = BindingSource::Key;
            binding.code = static_cast<u32>(key);
        }
        else if (same(device, u8"Mouse"))
        {
            shell::MouseButton button{};
            if (!ScriptedInputSource::ParseMouseButton(name, button))
                return {};
            binding.source = BindingSource::MouseButton;
            binding.code = static_cast<u32>(button);
        }
        else if (same(device, u8"Pad"))
        {
            shell::GamepadButton button{};
            if (!ScriptedInputSource::ParsePadButton(name, button))
                return {};
            binding.source = BindingSource::GamepadButton;
            binding.code = static_cast<u32>(button);
        }
        else if (same(device, u8"PadAxis"))
        {
            shell::GamepadAxis axis{};
            if (!ScriptedInputSource::ParsePadAxis(name, axis))
                return {};
            binding.source = BindingSource::GamepadAxis;
            binding.code = static_cast<u32>(axis);
        }
        else
            return {};
        return binding;
    }

    /// `current` with `replacement` in place of its first binding of the same kind (the keyboard
    /// and the mouse are one kind, the pad another, touch a third), the rest of that kind dropped;
    /// after the others when it has none of that kind.
    [[nodiscard]] inline Array<Binding> WithBindingOfKind(Span<const Binding> current, const Binding& replacement)
    {
        const InputKind kind = ShownBinding::KindOf(replacement);
        Array<Binding> out;
        bool placed = false;
        for (const Binding& binding : current)
        {
            if (ShownBinding::KindOf(binding) != kind)
            {
                out.PushBack(binding);
                continue;
            }
            if (!placed)
            {
                out.PushBack(replacement);
                placed = true;
            }
        }
        if (!placed)
            out.PushBack(replacement);
        return out;
    }

    /// A game's controls as the player has them: the game's map with the player's overrides over
    /// it, kept on an action runtime. The overrides live in a store the host provides (a section
    /// of a save file) and are reported changed through `changed`; without a store they live here,
    /// for the run only.
    class PlayerBindings
    {
    public:
        /// Where the overrides are kept, and what to call when they change. Null: kept here.
        void SetStore(InputBindingOverrides* store, Function<void()> changed)
        {
            m_store = store;
            m_changed = Move(changed);
        }

        /// Installs `map` on `runtime` with the overrides over it; `runtime` is remembered.
        void Install(ActionRuntime& runtime, const InputMap& map)
        {
            m_runtime = &runtime;
            m_map = map;
            m_installed = true;
            InputMap effective = map;
            ApplyBindingOverrides(effective, Overrides());
            runtime.SetMap(effective);
        }
        /// Applies the overrides again over the game's bindings, action by action, keeping the
        /// runtime's sets as they are: after the store changed underneath (a save file opened).
        void Reapply()
        {
            if (m_runtime == nullptr || !m_installed)
                return;
            InputMap effective = m_map;
            ApplyBindingOverrides(effective, Overrides());
            for (const ActionSet& set : effective.sets)
                for (const Action& action : set.actions)
                    (void)m_runtime->ReplaceBindings(set.name.AsView(), action.name.AsView(),
                                                     Span<const Binding>{action.bindings.Data(), action.bindings.Size()});
        }

        /// Binds `action` (in `set`; empty: the set it is read from) to `binding`, in place of its
        /// binding of the same kind. False when there is no such action, or it is two-dimensional
        /// (one control cannot drive two axes).
        bool Set(StringView set, StringView action, const Binding& binding)
        {
            if (m_runtime == nullptr)
                return false;
            const String setName(set.IsEmpty() ? m_runtime->SetOf(action) : set);
            const Action* current = m_runtime->FindAction(setName.AsView(), action);
            if (current == nullptr || current->kind == ActionKind::Axis2D)
                return false;
            Array<Binding> bindings = WithBindingOfKind(Span<const Binding>{current->bindings.Data(), current->bindings.Size()}, binding);
            (void)m_runtime->ReplaceBindings(setName.AsView(), action, Span<const Binding>{bindings.Data(), bindings.Size()});
            Overrides().Set(setName.AsView(), action, Move(bindings));
            Changed();
            return true;
        }
        /// Gives `action` (in `set`; empty: the set it is read from) the game's own bindings again.
        /// False when there is no such action.
        bool Reset(StringView set, StringView action)
        {
            if (m_runtime == nullptr)
                return false;
            const String setName(set.IsEmpty() ? m_runtime->SetOf(action) : set);
            const Action* authored = FindIn(m_map, setName.AsView(), action);
            if (authored == nullptr)
                return false;
            (void)m_runtime->ReplaceBindings(setName.AsView(), action,
                                             Span<const Binding>{authored->bindings.Data(), authored->bindings.Size()});
            Overrides().Clear(setName.AsView(), action);
            Changed();
            return true;
        }
        /// Every action back to the game's own bindings.
        void ResetAll()
        {
            if (Overrides().overrides.IsEmpty())
                return;
            Overrides().overrides.Clear();
            Reapply();
            Changed();
        }

        /// Starts taking the player's next press on a device of `kind` as `action`'s binding (in
        /// `set`; empty: the set it is read from); every action reads released meanwhile. Escape,
        /// or the pad's Back, cancels. False, and nothing taken, when there is no such action or it
        /// is not a button.
        bool BeginCapture(StringView set, StringView action, InputKind kind)
        {
            if (m_runtime == nullptr)
                return false;
            const String setName(set.IsEmpty() ? m_runtime->SetOf(action) : set);
            const Action* current = m_runtime->FindAction(setName.AsView(), action);
            if (current == nullptr || current->kind != ActionKind::Button)
                return false;
            m_captureSet = setName;
            m_captureAction = String(action);
            m_captureKind = kind == InputKind::Pad ? InputKind::Pad : InputKind::Keyboard;
            m_capturing = true;
            m_runtime->SetListening(true);
            return true;
        }
        void CancelCapture()
        {
            if (!m_capturing && !m_resting)
                return;
            m_capturing = false;
            m_resting = false;
            if (m_runtime != nullptr)
                m_runtime->SetListening(false);
        }
        [[nodiscard]] bool IsCapturing() const noexcept { return m_capturing; }

        /// Takes this frame's first press of the kind being captured, if any. Call before the
        /// runtime's update, every frame. The frame a capture ends in still rests, so the press
        /// that ended it is held through a rest and fires nothing until it is let go.
        void Poll(IInputSourceProvider& source)
        {
            if (m_resting)
            {
                m_resting = false;
                if (!m_capturing && m_runtime != nullptr)
                    m_runtime->SetListening(false);
            }
            if (!m_capturing)
                return;
            shell::IKeyboard* keyboard = source.Keyboard();
            shell::IMouse* mouse = source.Mouse();
            for (const shell::InputEvent& event : source.Events())
            {
                Optional<Binding> taken;
                bool cancel = false;
                if (m_captureKind == InputKind::Keyboard)
                {
                    // A key's edge only: a held key repeats its KeyDown.
                    if (event.kind == shell::InputEventKind::KeyDown && keyboard != nullptr && keyboard->IsKeyPressed(event.key))
                    {
                        if (event.key == shell::KeyCode::Escape)
                            cancel = true;
                        else
                        {
                            Binding binding;
                            binding.source = BindingSource::Key;
                            binding.code = static_cast<u32>(event.key);
                            taken = binding;
                        }
                    }
                    else if (event.kind == shell::InputEventKind::MouseButtonDown && mouse != nullptr && mouse->IsButtonPressed(event.button))
                    {
                        Binding binding;
                        binding.source = BindingSource::MouseButton;
                        binding.code = static_cast<u32>(event.button);
                        taken = binding;
                    }
                }
                else if (event.kind == shell::InputEventKind::GamepadButtonDown)
                {
                    if (event.padButton == shell::GamepadButton::Back)
                        cancel = true;
                    else
                    {
                        Binding binding;
                        binding.source = BindingSource::GamepadButton;
                        binding.code = static_cast<u32>(event.padButton);
                        taken = binding;
                    }
                }
                else if (event.kind == shell::InputEventKind::GamepadAxis && event.value >= ActionRuntime::kPressPoint &&
                         (event.padAxis == shell::GamepadAxis::LeftTrigger || event.padAxis == shell::GamepadAxis::RightTrigger))
                {
                    Binding binding;
                    binding.source = BindingSource::GamepadAxis;
                    binding.code = static_cast<u32>(event.padAxis);
                    taken = binding;
                }
                if (cancel || taken.HasValue())
                {
                    m_capturing = false;
                    m_resting = true; // listening ends at the next Poll
                    if (taken.HasValue())
                        (void)Set(m_captureSet.AsView(), m_captureAction.AsView(), taken.Value());
                    return;
                }
            }
        }

        [[nodiscard]] const InputBindingOverrides& Current() noexcept { return Overrides(); }

    private:
        [[nodiscard]] InputBindingOverrides& Overrides() noexcept { return m_store != nullptr ? *m_store : m_own; }
        void Changed()
        {
            if (m_changed)
                m_changed();
        }
        [[nodiscard]] static const Action* FindIn(const InputMap& map, StringView set, StringView action) noexcept
        {
            for (const ActionSet& candidate : map.sets)
                if (candidate.name.AsView() == set)
                    for (const Action& a : candidate.actions)
                        if (a.name.AsView() == action)
                            return &a;
            return nullptr;
        }

        ActionRuntime* m_runtime = nullptr; // borrowed
        InputMap m_map;                     // the game's own, as authored
        bool m_installed = false;
        InputBindingOverrides* m_store = nullptr; // borrowed; null: m_own
        InputBindingOverrides m_own;
        Function<void()> m_changed;
        bool m_capturing = false;
        bool m_resting = false; // a capture ended this frame: the runtime rests until the next Poll
        InputKind m_captureKind = InputKind::Keyboard;
        String m_captureSet;
        String m_captureAction;
    };
}
