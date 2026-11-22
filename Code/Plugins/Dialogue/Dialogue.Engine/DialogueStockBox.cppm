// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DialogueStockBox: the plugin's own dialogue box, which a project turns on in the plugin's
// settings (`stockBox`) so a game has dialogue on screen before it builds a box of its own. It is a
// screen on the run's screen stack that blocks nothing under it: a portrait, the speaker's name,
// the line typed out at the settings' speed, and a choice's answers as buttons that the pad, the
// keys and the mouse all reach. The advance action (the settings', Interact by default) or a press
// on the box first finishes the typing, then advances; a press on an answer chooses it, and the
// advance action chooses the focused answer. The box
// goes when the conversation ends. Its look is a sheet over the box alone: the plugin's own, or
// the project's (`stockBoxSheet`) in its place, so a game restyles it without building its own.
// The class names the sheets style: dialogue-screen, dialogue-box (portrait-right with
// `portraitOnRight`), dialogue-portrait, dialogue-words, dialogue-name, dialogue-text,
// dialogue-answers and dialogue-answer.

module;

#include "Core/Prelude.h"

export module dialogue.engine:stockbox;

import foundation.core;
import foundation.gui;
import foundation.gui.gamekit;
import engine.gui;
import :settings;
import :cast;
import :runner;

using namespace foundation::core;

export namespace dialogue
{
    /// The box's markup, and the plugin's sheet for it.
    [[nodiscard]] StringView DialogueStockBoxMarkup() noexcept;
    [[nodiscard]] StringView DialogueStockBoxSheet() noexcept;

    class DialogueStockBox
    {
    public:
        /// What the player asked for in a frame.
        struct Request
        {
            bool advance = false;
            i32 choose = -1;
        };

        explicit DialogueStockBox(const DialogueSettings& settings) : m_settings(&settings) {}
        ~DialogueStockBox();
        DialogueStockBox(const DialogueStockBox&) = delete;
        DialogueStockBox& operator=(const DialogueStockBox&) = delete;

        /// Shows `notice` in the box on `stack`, styled by `css` (`gui`'s style manager), typing
        /// it out from the start.
        void ShowLine(engine::gui::GuiSubsystem& gui, foundation::gui::ScreenStack& stack, StringView css,
                      const DialogueLineNotice& notice, const DialogueSpeaker* speaker);
        /// Shows the choice's answers under the line shown last, the first focused.
        void ShowChoice(engine::gui::GuiSubsystem& gui, foundation::gui::ScreenStack& stack, StringView css,
                        const DialogueChoiceNotice& notice);
        /// Takes the box off its stack.
        void Hide();

        /// Types on by `deltaTime`; `advancePressed` is the advance action's press this frame.
        /// Returns what the player asked for: an advance (the line typed out already), or an answer.
        [[nodiscard]] Request Update(f32 deltaTime, bool advancePressed);

        // === For tests ===
        [[nodiscard]] bool IsShown() const noexcept { return m_screen.Get() != nullptr; }
        /// Whether its sheet took (it is set once the box is in the GUI).
        [[nodiscard]] bool IsStyled() const noexcept { return m_styled; }
        [[nodiscard]] bool IsTyping() const noexcept;
        [[nodiscard]] String ShownText() const;
        [[nodiscard]] String ShownName() const;
        [[nodiscard]] usize AnswerCount() const noexcept { return m_answers.Size(); }
        /// Answer `index`'s text; empty past the last.
        [[nodiscard]] String AnswerText(usize index) const;
        /// Presses answer `index`, as a click on it does.
        void Press(usize index);
        /// Presses the box, as a click on it does.
        void PressBox() noexcept { m_boxPressed = true; }
        [[nodiscard]] foundation::gui::Screen* ScreenNode() const noexcept { return m_screen.Get(); }

    private:
        // Makes the box on `stack` when it is not there, styled by `css`.
        bool Ensure(engine::gui::GuiSubsystem& gui, foundation::gui::ScreenStack& stack, StringView css);
        void ClearAnswers();
        void ShowTyped();

        const DialogueSettings* m_settings;
        RefPtr<foundation::gui::Screen> m_screen;
        foundation::gui::Label* m_name = nullptr;
        foundation::gui::Label* m_text = nullptr;
        foundation::gui::Image* m_portrait = nullptr;
        foundation::gui::UIWidget* m_box = nullptr;
        foundation::gui::UIWidget* m_answerList = nullptr;
        Array<RefPtr<foundation::gui::Button>> m_answers;
        String m_full;          // the line's whole text
        usize m_length = 0;     // its characters (code points)
        f32 m_typed = 0.0f;     // how many show
        bool m_boxPressed = false;
        bool m_styled = false;
        i32 m_answerPressed = -1;
    };
}
