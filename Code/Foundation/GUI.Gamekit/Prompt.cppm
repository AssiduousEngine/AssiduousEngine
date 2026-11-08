// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Gamekit - :prompt partition
//
// Button prompts. A `prompt` element names an action of the game's InputMap (and optionally its
// set) and shows the control that does it for the input kind in use: a pad's button while a pad is
// in use, a key or mouse button for the keyboard or the pointer, else any binding the action has,
// else nothing. It reads the map the game runs, after the player's rebinding, through the host's
// InputPrompts (an ActionRuntime: its map and its enabled sets, highest priority first). It labels
// the binding with foundation.input's BindingNames, and follows the input kind and the bindings as
// they change.
//
// There is no glyph table in code. A prompt exposes its binding to the sheet as attributes:
// `binding` (the label's words, lower case, joined by dashes: `pad-south`, `space`, `mouse-left`)
// and `kind` (`pad`, `keyboard`, `pointer`). A game's sheet draws its own glyphs
// (`prompt[binding="pad-south"] { background-image: url(...) }`), and the text label is the
// fallback.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.gamekit:prompt;

import foundation.core;
import foundation.input;
import foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;
namespace input = foundation::input;

export namespace foundation::gui
{
    /// The key a GUI's InputPrompts is found under (GUIContext::GetService).
    inline constexpr core::StringView kInputPromptsService = u8"gamekit.input-prompts";

    /// Returns a binding as a sheet names it: its label's words, lower case, joined by dashes
    /// ("Pad South" becomes `pad-south`).
    [[nodiscard]] inline core::String BindingToken(const input::Binding& binding)
    {
        const core::String label = input::DescribeBinding(binding);
        core::String token;
        for (const char8_t c : label.AsView())
            token.Append(c == u8' ' ? u8'-' : (c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c - u8'A' + u8'a') : c));
        return token;
    }

    /// What a GUI's prompts read the game's bindings through: the host's ActionRuntime (its map
    /// after the player's rebinding, and its enabled sets). The host installs it on the GUI and tells
    /// it when the bindings change, such as when a rebinding is saved.
    class InputPrompts
    {
    public:
        InputPrompts() = default;
        InputPrompts(const InputPrompts&) = delete;
        InputPrompts& operator=(const InputPrompts&) = delete;

        void SetRuntime(const input::ActionRuntime* runtime)
        {
            m_runtime = runtime;
            m_changed.Notify();
        }
        /// Called when the runtime's map or enabled sets change: every prompt reads them again.
        void NotifyBindingsChanged() { m_changed.Notify(); }
        /// Makes this the InputPrompts that `gui`'s prompts read. It must stay alive while it is
        /// installed.
        void Install(GUIContext& gui) { gui.SetService(kInputPromptsService, this); }
        static void Uninstall(GUIContext& gui) { gui.SetService(kInputPromptsService, nullptr); }

        /// Returns the binding to show for `action` in `set`, for input of `kind`
        /// (input::ShownBinding::For: the one a script's `Input.describe` names too). The read is
        /// tracked, so a reactive binding that calls this updates when the result changes.
        [[nodiscard]] Optional<input::Binding> Find(core::StringView action, core::StringView set, InputKind kind) const
        {
            m_changed.Track();
            if (m_runtime == nullptr)
                return {};
            return input::ShownBinding::For(*m_runtime, action, set, ToInputKind(kind));
        }

        /// The GUI's input kind as foundation.input names it (the reverse of GUI.Shell's bridge,
        /// which passes the tracker's kind to the GUI).
        [[nodiscard]] static input::InputKind ToInputKind(InputKind kind) noexcept
        {
            switch (kind)
            {
            case InputKind::Keyboard: return input::InputKind::Keyboard;
            case InputKind::Pad: return input::InputKind::Pad;
            case InputKind::Pointer: break;
            }
            return input::InputKind::Pointer;
        }

    private:
        const input::ActionRuntime* m_runtime = nullptr;
        Observable m_changed;
    };

    /// Shows the control that does an action, for the input kind in use.
    class Prompt : public Label
    {
        RTTI_OBJECT(Prompt, Label)
    public:
        Prompt()
        {
            SetTag(core::StringView(u8"prompt"));
            Bind([this]() { Refresh(); }); // re-runs when its GUI, its action, the input kind or the bindings change
        }

        [[nodiscard]] core::String GetAction() const { return m_action; }
        void SetAction(const core::String& action)
        {
            m_action = action;
            m_inputs.Notify();
        }
        /// The set the prompt reads the action in (empty: the highest-priority enabled set that has
        /// it).
        [[nodiscard]] core::String GetSet() const { return m_set; }
        void SetSet(const core::String& set)
        {
            m_set = set;
            m_inputs.Notify();
        }
        /// The binding shown, as a sheet names it (`pad-south`), and its kind (`pad`, `keyboard`,
        /// `pointer`). Both are empty when no binding is shown.
        [[nodiscard]] core::String GetBinding() const { return m_binding; }
        [[nodiscard]] core::String GetBindingKind() const { return m_kind; }

    private:
        void Refresh()
        {
            m_inputs.Track();
            SceneRoot* root = ObserveSceneRoot();
            Optional<input::Binding> binding;
            if (root != nullptr)
            {
                const InputKind kind = root->GetEventDispatcher()->ObserveInputKind();
                if (const auto* prompts = static_cast<const InputPrompts*>(root->GetContext().GetService(kInputPromptsService)))
                    binding = prompts->Find(m_action.AsView(), m_set.AsView(), kind);
            }
            Untracked([&]
                      {
                          const core::String token = binding.HasValue() ? BindingToken(binding.Value()) : core::String{};
                          const core::String kind = binding.HasValue() ? core::String(InputKindName(input::ShownBinding::KindOf(binding.Value())))
                                                                       : core::String{};
                          SetText(binding.HasValue() ? input::DescribeBinding(binding.Value()).AsView() : core::StringView{});
                          if (token.AsView() != m_binding.AsView() || kind.AsView() != m_kind.AsView())
                          {
                              m_binding = token;
                              m_kind = kind;
                              MarkStyleDirty(false); // a sheet's [binding=...] and [kind=...] read it
                          }
                      });
        }

        core::String m_action;
        core::String m_set;
        core::String m_binding;
        core::String m_kind;
        Observable m_inputs; // notified when its action or set changes
    };
}
