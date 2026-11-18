// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :code_opener partition
//
// EditorCodeOpener: where the editor opens code, by the user's Code Editor preference
// (EditorCodeEditorSettings). An outside editor gets its command with the file, line, column and
// the project's Code/ folder put in; the built-in editor opens the asset's page at the line. A
// command that does not start falls back to the built-in page, saying so. The Code folder opens in
// the outside editor's folder command, or the system's file manager. Every way the user opens code
// comes here (the Assets browser, a log line, the menu); an agent's page_open keeps its page.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:code_opener;

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.settings;
import editor.project;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    class EditorCodeOpener
    {
    public:
        struct Services
        {
            editor::EditorContext* context = nullptr; // the project, the user's store, notices
            /// Opens (or brings forward) the page for an asset; null when it has none.
            Function<editor::EditorPage*(foundation::content::Instance&)> openPage;
            /// Starts a program and leaves it running; LaunchDetached unless a test records it.
            Function<bool(StringView program, Span<const StringView> args)> launch;
            /// Shows a folder in the system's file manager.
            Function<void(StringView folder)> showFolder;
        };

        explicit EditorCodeOpener(Services services) : m_services(Move(services))
        {
            if (!m_services.launch)
                m_services.launch = [](StringView program, Span<const StringView> args) { return LaunchDetached(program, args); };
        }

        /// Opens `file` (a path on disk) at `line` and `column` (1-based; 0 is the start) where the
        /// preference says, or in the built-in editor when `builtIn`. An empty `file` opens the
        /// project's Code/ folder. False when nothing opened.
        bool Open(StringView file, u32 line, u32 column, bool builtIn = false)
        {
            editor::EditorProject* project = m_services.context != nullptr ? m_services.context->Project() : nullptr;
            if (project == nullptr)
                return false;
            const String folder = project->CodeRoot();
            const editor::CodeEditorCommands commands = editor::CodeEditorCommandsOf(Preference());
            if (file.IsEmpty())
            {
                if (!builtIn && !commands.folderCommand.IsEmpty() &&
                    Launch(commands.folderCommand.AsView(), editor::CodeLocation{StringView{}, 0, 0, folder.AsView()}))
                    return true;
                if (m_services.showFolder)
                    m_services.showFolder(folder.AsView());
                return static_cast<bool>(m_services.showFolder);
            }
            if (!builtIn && commands.Outside())
            {
                if (Launch(commands.fileCommand.AsView(), editor::CodeLocation{file, line, column, folder.AsView()}))
                    return true;
                Notify(editor::NoticeKind::Warning, Format(u8"{} did not start; the file opens here instead.", ProgramOf(commands.fileCommand.AsView())).AsView());
            }
            return OpenBuiltIn(*project, file, line, column);
        }

        /// Opens a code asset where the preference says (the Assets browser's Open), or in the
        /// built-in editor when `builtIn`. False when it is not code, or nothing opened.
        bool OpenAsset(foundation::content::Instance& instance, bool builtIn = false)
        {
            editor::EditorProject* project = m_services.context != nullptr ? m_services.context->Project() : nullptr;
            if (project == nullptr || !IsCode(instance))
                return false;
            return Open(project->SourceFile(instance.SourceName()).AsView(), 0, 0, builtIn);
        }

        /// Whether the asset's file is code (it lives in the project's Code/ folder).
        [[nodiscard]] static bool IsCode(const foundation::content::Instance& instance)
        {
            StringView rest;
            return foundation::vfs::AfterFolder(instance.SourceName(), foundation::vfs::kSourcesCodeFolder, rest) && !rest.IsEmpty();
        }

    private:
        [[nodiscard]] const editor::EditorCodeEditorSettings* Preference() const
        {
            const foundation::settings::Settings* user = m_services.context->UserEditorSettings();
            return user != nullptr ? user->Find<editor::EditorCodeEditorSettings>() : nullptr;
        }

        bool Launch(StringView command, const editor::CodeLocation& at)
        {
            const Array<String> parts = editor::ExpandCodeEditorCommand(command, at);
            if (parts.IsEmpty())
                return false;
            Array<StringView> args;
            for (usize i = 1; i < parts.Size(); ++i)
                args.PushBack(parts[i].AsView());
            if (m_services.launch(parts[0].AsView(), Span<const StringView>(args.Data(), args.Size())))
                return true;
            LOG_WARNING(u8"Editor", u8"the code editor command did not start: {}", command);
            return false;
        }

        [[nodiscard]] static String ProgramOf(StringView command)
        {
            const Array<String> parts = editor::SplitCommandLine(command);
            return parts.IsEmpty() ? String(command) : parts[0];
        }

        // The asset whose file `file` is opens its page at the line.
        bool OpenBuiltIn(editor::EditorProject& project, StringView file, u32 line, u32 column)
        {
            const String name = SourceNameOf(project, file);
            foundation::content::Instance* instance = name.IsEmpty() ? nullptr : FindBySource(*project.SourceDb().RootGroup(), name.AsView());
            editor::EditorPage* page = instance != nullptr && m_services.openPage ? m_services.openPage(*instance) : nullptr;
            if (page == nullptr)
            {
                Notify(editor::NoticeKind::Warning, Format(u8"No page here opens {}.", PathFilename(file)).AsView());
                return false;
            }
            if (editor::ICodePage* code = page->Service<editor::ICodePage>(); code != nullptr && line > 0)
                code->ShowLine(line, column);
            return true;
        }

        // The source path a file on disk has: Code/ and its place under the code folder, or its place
        // under Sources/. Empty when it is under neither.
        [[nodiscard]] static String SourceNameOf(editor::EditorProject& project, StringView file)
        {
            String path(file);
            for (usize i = 0; i < path.Size(); ++i)
                if (path[i] == utf8char('\\'))
                    path[i] = utf8char('/');
            StringView rest;
            const String code = project.CodeRoot();
            if (foundation::vfs::AfterFolder(path.AsView(), code.AsView(), rest) && !rest.IsEmpty())
                return PathJoin(foundation::vfs::kSourcesCodeFolder, rest);
            const String sources = project.SourcesRoot();
            if (foundation::vfs::AfterFolder(path.AsView(), sources.AsView(), rest) && !rest.IsEmpty())
                return String(rest);
            return String{};
        }

        [[nodiscard]] static foundation::content::Instance* FindBySource(foundation::content::Group& group, StringView name)
        {
            for (foundation::content::Instance* instance : group.Instances())
                if (instance->SourceName() == name)
                    return instance;
            for (foundation::content::Group* child : group.Groups())
                if (foundation::content::Instance* found = FindBySource(*child, name))
                    return found;
            return nullptr;
        }

        void Notify(editor::NoticeKind kind, StringView message)
        {
            if (m_services.context != nullptr)
                m_services.context->Notify(kind, message);
        }

        Services m_services;
    };
}
