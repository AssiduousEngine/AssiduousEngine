// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :text_file_page partition
//
// TextFilePage: a file in the project's Code folder that is no asset (a .csproj, a .luaurc), as
// plain text in a code editor. Save writes the file; Discard Changes reads it again. A file that
// does not read shows empty and read-only, and is never saved over. The code editor keeps its own
// undo, so the page's history stays empty and an edit only marks the page unsaved.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:text_file_page;

import foundation.core;
import foundation.gui;
import foundation.gui.code;
import editor.core;
import :ui_page;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class TextFilePage final : public UIEditorPage, public editor::ICodePage
    {
    public:
        TextFilePage(IAllocator& allocator, StringView file) : UIEditorPage(allocator), m_file(file)
        {
            Provide<editor::ICodePage>(*this);
            m_content = MakeRef<gui::Panel>(allocator);
            m_content->AddClass(u8"page");
            m_content->AddClass(u8"code-page");
            m_code = MakeRef<gui::CodeEditor>(allocator);
            m_code->AddClass(u8"page-code");
            m_content->AddChild(m_code.Get());
            (void)ReloadFromSource();
            TextFilePage* self = this;
            m_listener = m_code->GetDocument().AddChangeListener([self](const gui::CodeChange&)
                                                                 {
                                                                     if (!self->m_loading)
                                                                         self->MarkDirty();
                                                                 });
        }
        ~TextFilePage() override
        {
            if (m_code && m_listener != 0)
                m_code->GetDocument().RemoveChangeListener(m_listener);
        }

        [[nodiscard]] StringView Title() const override { return PathFilename(m_file.AsView()); }
        [[nodiscard]] RefPtr<gui::Node> Content() override { return RefPtr<gui::Node>(m_content.Get()); }

        [[nodiscard]] Status Save() override
        {
            if (!m_read)
                return Status{ErrorCode::NotSupported}; // what did not read is not written over
            const String text = m_code->GetText();
            const Status written = WriteFile(m_file.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()));
            if (!written.IsOk())
            {
                LOG_ERROR(u8"Editor", u8"{} could not be written", m_file);
                return written;
            }
            ClearDirty();
            return Status{};
        }

        bool ReloadFromSource() override
        {
            Result<Array<byte>> bytes = ReadFile(m_file.AsView());
            m_read = bytes.HasValue();
            m_loading = true;
            m_code->SetText(m_read ? StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()) : StringView{});
            m_loading = false;
            m_code->SetReadOnly(!m_read);
            if (!m_read)
                LOG_WARNING(u8"Editor", u8"{} could not be read", m_file);
            return m_read;
        }

        void ShowLine(u32 line, u32 column) override
        {
            m_code->SetCaret(gui::CodePosition{line > 0 ? line - 1 : 0, column > 0 ? column - 1 : 0});
            m_code->ScrollToCaret();
            m_code->RequestFocus();
        }

        /// The file it shows, on disk.
        [[nodiscard]] StringView File() const noexcept { return m_file.AsView(); }
        // === For tests ===
        [[nodiscard]] gui::CodeEditor* Code() const noexcept { return m_code.Get(); }

    private:
        String m_file;
        RefPtr<gui::Panel> m_content;
        RefPtr<gui::CodeEditor> m_code;
        u32 m_listener = 0;
        bool m_read = false;
        bool m_loading = false;
    };
}
