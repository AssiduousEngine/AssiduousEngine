// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The stock box: its markup and sheet, built on the run's screen stack and typed out.

module;

#include "Core/Prelude.h"

module dialogue.engine;

import foundation.core;
import foundation.gui;
import foundation.gui.gamekit;
import foundation.gui.resources;
import engine.gui;

using namespace foundation::core;

namespace dialogue
{
    namespace gui = foundation::gui;

    namespace
    {
        [[nodiscard]] usize CodePoints(StringView text) noexcept
        {
            usize count = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                count += (static_cast<u8>(text[i]) & 0xC0u) != 0x80u ? 1u : 0u;
            }
            return count;
        }

        // The first `count` code points of `text`.
        [[nodiscard]] StringView Prefix(StringView text, usize count) noexcept
        {
            usize seen = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if ((static_cast<u8>(text[i]) & 0xC0u) != 0x80u)
                {
                    if (seen == count)
                    {
                        return text.SubStr(0, i);
                    }
                    ++seen;
                }
            }
            return text;
        }

        // The node `id` under `node` (depth first): the tree is not in a root yet, where a handle
        // lookup would find it.
        [[nodiscard]] gui::Node* FindById(gui::Node& node, StringView id)
        {
            if (const gui::UIWidget* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            {
                return &node;
            }
            for (usize i = 0; i < node.ChildCount(); ++i)
            {
                if (gui::Node* found = FindById(*node.GetChildAt(i), id))
                {
                    return found;
                }
            }
            return nullptr;
        }

        template <typename T>
        [[nodiscard]] T* Element(gui::Node& tree, StringView id)
        {
            return Cast<T>(FindById(tree, id));
        }
    }

    StringView DialogueStockBoxMarkup() noexcept
    {
        return u8"<Screen class=\"dialogue-screen\" blocks=\"false\" back=\"none\">\n"
               u8"  <Panel id=\"dialogue-box\" class=\"dialogue-box\">\n"
               u8"    <Image id=\"dialogue-portrait\" class=\"dialogue-portrait\"/>\n"
               u8"    <Panel class=\"dialogue-words\">\n"
               u8"      <Label id=\"dialogue-name\" class=\"dialogue-name\" text=\"\"/>\n"
               u8"      <Label id=\"dialogue-text\" class=\"dialogue-text\" text=\"\"/>\n"
               u8"      <Panel id=\"dialogue-answers\" class=\"dialogue-answers\"/>\n"
               u8"    </Panel>\n"
               u8"  </Panel>\n"
               u8"</Screen>\n";
    }

    StringView DialogueStockBoxSheet() noexcept
    {
        return u8".dialogue-screen { flex-grow: 1; flex-direction: column; justify-content: end; align-items: center; "
               u8"padding: 24px; }\n"
               u8".dialogue-box { flex-direction: row; align-items: start; gap: 18px; width: 920px; padding: 18px; "
               u8"background-color: rgba(14, 14, 20, 0.9); border: 2px solid #5A5468; border-radius: 14px; }\n"
               u8".dialogue-box.portrait-right { flex-direction: row-reverse; }\n"
               u8".dialogue-portrait { width: 120px; height: 120px; border-radius: 10px; }\n"
               u8".dialogue-words { flex-direction: column; flex-grow: 1; gap: 8px; }\n"
               u8".dialogue-name { font-size: 22px; color: #F2D9A6; }\n"
               u8".dialogue-text { font-size: 21px; color: #F4F0E8; white-space: normal; }\n"
               u8".dialogue-answers { flex-direction: column; gap: 6px; margin-top: 4px; }\n"
               u8"button.dialogue-answer { height: 40px; font-size: 19px; color: #F4F0E8; background-color: #24222E; "
               u8"border: 2px solid #4A4658; border-radius: 8px; justify-content: start; padding: 0px 14px; }\n"
               u8"button.dialogue-answer:hover { background-color: #302D3C; border: 2px solid #9A92B8; }\n"
               u8"button.dialogue-answer:focus-visible { border: 3px solid #E8D8A8; }\n";
    }

    DialogueStockBox::~DialogueStockBox() { Hide(); }

    bool DialogueStockBox::Ensure(engine::gui::GuiSubsystem& guiSystem, gui::ScreenStack& stack, StringView css)
    {
        if (m_screen && m_screen->GetStack() != nullptr)
        {
            return true;
        }
        gui::GuiDocument document;
        document.markup = String(DialogueStockBoxMarkup());
        RefPtr<gui::Node> tree = guiSystem.BuildDocument(document);
        gui::Screen* screen = tree ? Cast<gui::Screen>(tree.Get()) : nullptr;
        if (screen == nullptr)
        {
            return false;
        }
        m_screen = RefPtr<gui::Screen>(screen);
        m_box = Element<gui::UIWidget>(*screen, u8"dialogue-box");
        m_portrait = Element<gui::Image>(*screen, u8"dialogue-portrait");
        m_name = Element<gui::Label>(*screen, u8"dialogue-name");
        m_text = Element<gui::Label>(*screen, u8"dialogue-text");
        m_answerList = Element<gui::UIWidget>(*screen, u8"dialogue-answers");
        if (m_box == nullptr || m_portrait == nullptr || m_name == nullptr || m_text == nullptr || m_answerList == nullptr)
        {
            m_screen = {};
            return false;
        }
        if (m_settings->portraitOnRight)
        {
            m_box->AddClass(u8"portrait-right");
        }
        DialogueStockBox* self = this;
        (void)m_box->AddEventListener(gui::EventType::MouseClick, [self](const gui::Event&) { self->m_boxPressed = true; });
        (void)guiSystem.Styles().SetSubtreeSheet(*screen, gui::CSSParser::Parse(css, u8"dialogue stock box"));
        (void)stack.Push(*screen);
        return true;
    }

    void DialogueStockBox::ShowLine(engine::gui::GuiSubsystem& guiSystem, gui::ScreenStack& stack, StringView css,
                                    const DialogueLineNotice& notice, const DialogueSpeaker* speaker)
    {
        if (!Ensure(guiSystem, stack, css))
        {
            return;
        }
        ClearAnswers();
        m_name->SetText(notice.shownName.AsView());
        m_name->SetDisplayed(!notice.shownName.IsEmpty() && (speaker == nullptr || speaker->plate));
        m_portrait->SetDisplayed(!notice.portrait.IsNil());
        if (!notice.portrait.IsNil())
        {
            m_portrait->SetSource(Format(u8"{}", notice.portrait));
        }
        m_full = notice.text;
        m_length = CodePoints(m_full.AsView());
        m_typed = m_settings->typingSpeed > 0.0f ? 0.0f : static_cast<f32>(m_length);
        m_boxPressed = false;
        ShowTyped();
    }

    void DialogueStockBox::ShowChoice(engine::gui::GuiSubsystem& guiSystem, gui::ScreenStack& stack, StringView css,
                                      const DialogueChoiceNotice& notice)
    {
        if (!Ensure(guiSystem, stack, css))
        {
            return;
        }
        ClearAnswers();
        m_typed = static_cast<f32>(m_length); // the line above it, whole
        ShowTyped();
        for (usize i = 0; i < notice.answers.Size(); ++i)
        {
            gui::Button* button = Cast<gui::Button>(gui::Node::FromHandle(m_answerList->AddElement(u8"Button")));
            if (button == nullptr)
            {
                continue;
            }
            button->AddClass(u8"dialogue-answer");
            button->SetText(notice.answers[i].AsView());
            DialogueStockBox* self = this;
            const i32 index = static_cast<i32>(i);
            button->SetOnClick([self, index]() { self->m_answerPressed = index; });
            m_answers.PushBack(RefPtr<gui::Button>(button));
        }
        if (!m_answers.IsEmpty())
        {
            m_answers[0]->RequestFocus();
        }
    }

    void DialogueStockBox::Hide()
    {
        ClearAnswers();
        if (m_screen)
        {
            gui::ScreenStack* stack = m_screen->GetStack();
            if (stack != nullptr && stack->Top() == m_screen.Get())
            {
                (void)stack->Pop();
            }
            else if (stack != nullptr)
            {
                m_screen->SetDisplayed(false); // under a screen the game pushed since: out of sight until it is top
            }
        }
        m_screen = {};
        m_box = nullptr;
        m_portrait = nullptr;
        m_name = nullptr;
        m_text = nullptr;
        m_answerList = nullptr;
        m_full = {};
        m_length = 0;
        m_typed = 0.0f;
    }

    DialogueStockBox::Request DialogueStockBox::Update(f32 deltaTime, bool advancePressed)
    {
        Request request;
        const bool pressed = advancePressed || m_boxPressed;
        m_boxPressed = false;
        if (!m_screen)
        {
            m_answerPressed = -1;
            return request;
        }
        if (!m_answers.IsEmpty())
        {
            request.choose = m_answerPressed; // the answers take the presses; the advance action is theirs to confirm
            m_answerPressed = -1;
            return request;
        }
        if (IsTyping())
        {
            m_typed = pressed ? static_cast<f32>(m_length) : m_typed + m_settings->typingSpeed * deltaTime;
            ShowTyped();
            return request; // a press finishes the typing; the next advances
        }
        request.advance = pressed;
        return request;
    }

    bool DialogueStockBox::IsTyping() const noexcept { return m_screen && m_typed < static_cast<f32>(m_length); }

    String DialogueStockBox::ShownText() const { return m_text != nullptr ? String(m_text->GetText()) : String(); }

    String DialogueStockBox::ShownName() const
    {
        return m_name != nullptr && m_name->IsDisplayed() ? String(m_name->GetText()) : String();
    }

    void DialogueStockBox::Press(usize index)
    {
        if (index < m_answers.Size())
        {
            m_answerPressed = static_cast<i32>(index);
        }
    }

    void DialogueStockBox::ClearAnswers()
    {
        for (const RefPtr<gui::Button>& answer : m_answers)
        {
            answer->RemoveFromParent();
        }
        m_answers.Clear();
        m_answerPressed = -1;
    }

    void DialogueStockBox::ShowTyped()
    {
        if (m_text == nullptr)
        {
            return;
        }
        const usize shown = Min(m_length, static_cast<usize>(m_typed));
        m_text->SetText(Prefix(m_full.AsView(), shown));
    }
}
