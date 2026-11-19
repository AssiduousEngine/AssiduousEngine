// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :mcp_folders partition.
//
// A project may list folders outside it that its MCP tools work in (ProjectSettings::mcpFolders, a
// game's sources beside it as "../Game"). The list grants nothing by itself: a project is a file
// anyone can hand the user, and opening it must not widen what an agent may read and write. A host
// lets its tools reach a listed folder only once the user has approved that folder for that
// project. The answers, yes or no, are the user's, kept in their data (mcp-folders.xml), never in
// the project; the editor asks for a folder not yet answered, and a headless host, with no one to
// ask, applies only the approved ones.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.project:mcp_folders;

import :allocator;
import foundation.core;
import foundation.vfs;
import foundation.settings;
import foundation.xml.serialization;
import engine.project;

using namespace foundation::core;

export namespace editor
{
    /// The file in the user's data folder that holds the answers.
    inline constexpr StringView kMcpFoldersFile = u8"mcp-folders.xml";

    /// One answer: whether the user lets MCP tools reach `folder` for the project at `project`
    /// (both absolute and normalised).
    struct McpFolderAnswer
    {
        String project;
        String folder;
        bool allowed = false;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "project", project);
            foundation::core::Serialize(ar, "folder", folder);
            foundation::core::Serialize(ar, "allowed", allowed);
        }
    };

    inline void Serialize(ISerializer& ar, McpFolderAnswer& answer)
    {
        ar.BeginObject();
        answer.Serialize(ar);
        ar.EndObject();
    }

    /// The user's answers, one per project and folder.
    class McpFolderAnswers final : public ISerializable
    {
        RTTI_OBJECT(McpFolderAnswers, ISerializable)
    public:
        Array<McpFolderAnswer> answers;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "answers", answers); }

        /// The answer for `folder` of `project`, or null when the user has given none.
        [[nodiscard]] const McpFolderAnswer* Find(StringView project, StringView folder) const
        {
            for (const McpFolderAnswer& answer : answers)
            {
                if (answer.project.AsView() == project && answer.folder.AsView() == folder)
                {
                    return &answer;
                }
            }
            return nullptr;
        }

        /// Records the user's answer for `folder` of `project`, replacing an earlier one.
        void Record(StringView project, StringView folder, bool allowed)
        {
            for (McpFolderAnswer& answer : answers)
            {
                if (answer.project.AsView() == project && answer.folder.AsView() == folder)
                {
                    answer.allowed = allowed;
                    return;
                }
            }
            answers.PushBack(McpFolderAnswer{String(project), String(folder), allowed});
        }
    };

    /// Registers the answers' section type, so a store can read it. Call once before loading.
    inline void RegisterMcpFolderTypes()
    {
        GlobalTypeRegistry().Register(McpFolderAnswers::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<McpFolderAnswers>();
    }

    /// Reads the answers from `root`'s kMcpFoldersFile into `store`; none when the file is not there.
    [[nodiscard]] inline Status LoadMcpFolderAnswers(foundation::vfs::IFileSystem& root,
                                                     foundation::settings::Settings& store)
    {
        UniquePtr<IStream> stream = root.Open(kMcpFoldersFile, FileMode::Read);
        if (!stream)
        {
            return Status{};
        }
        return store.Load(*stream, foundation::xml::XmlSerializerFactory());
    }

    /// Writes `store` to `root`'s kMcpFoldersFile.
    [[nodiscard]] inline Status SaveMcpFolderAnswers(foundation::vfs::IFileSystem& root,
                                                     const foundation::settings::Settings& store)
    {
        foundation::vfs::IWritableFileSystem* writable = root.AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        MemoryStream buffer;
        if (Status s = store.Save(buffer, foundation::xml::XmlSerializerFactory()); !s.IsOk())
        {
            return s;
        }
        return writable->Save(kMcpFoldersFile, buffer.Bytes());
    }

    /// A project's listed folders, made absolute against its folder and normalised, as the answers
    /// and the path checks name them. Empty entries are skipped.
    [[nodiscard]] inline Array<String> ResolveMcpFolders(StringView projectDirectory,
                                                         const engine::project::ProjectSettings& settings)
    {
        Array<String> out;
        const String project = PathNormalizeLexically(PathAbsolute(projectDirectory).AsView());
        for (const String& folder : settings.mcpFolders)
        {
            if (folder.IsEmpty())
            {
                continue;
            }
            // An absolute entry stands as it is (PathJoin keeps it); a relative one is the project's.
            out.PushBack(PathNormalizeLexically(PathJoin(project.AsView(), folder.AsView()).AsView()));
        }
        return out;
    }

    /// The project's listed folders split by the user's answers: those approved, and those not yet
    /// answered (a folder the user refused is in neither).
    struct McpFolderStanding
    {
        Array<String> approved;
        Array<String> unanswered;
    };

    [[nodiscard]] inline McpFolderStanding StandMcpFolders(const McpFolderAnswers& answers, StringView projectDirectory,
                                                           const engine::project::ProjectSettings& settings)
    {
        McpFolderStanding out;
        const String project = PathNormalizeLexically(PathAbsolute(projectDirectory).AsView());
        for (String& folder : ResolveMcpFolders(projectDirectory, settings))
        {
            const McpFolderAnswer* answer = answers.Find(project.AsView(), folder.AsView());
            if (answer == nullptr)
            {
                out.unanswered.PushBack(Move(folder));
            }
            else if (answer->allowed)
            {
                out.approved.PushBack(Move(folder));
            }
        }
        return out;
    }

    /// StandMcpFolders over the answers kept in `answersDirectory` (the user's data folder; a test's
    /// own). With no file there, every listed folder is unanswered.
    [[nodiscard]] inline McpFolderStanding StandMcpFolders(StringView answersDirectory, StringView projectDirectory,
                                                           const engine::project::ProjectSettings& settings)
    {
        foundation::vfs::NativeFileSystem root(answersDirectory, EditorRootAllocator());
        foundation::settings::Settings store(EditorRootAllocator());
        if (Status s = LoadMcpFolderAnswers(root, store); !s.IsOk())
        {
            // Whatever it read before it failed is not trusted: no folder is approved.
            LOG_WARNING(u8"Editor", u8"{} does not read; no folder outside the project is approved", kMcpFoldersFile);
            return StandMcpFolders(McpFolderAnswers{}, projectDirectory, settings);
        }
        return StandMcpFolders(store.Section<McpFolderAnswers>(), projectDirectory, settings);
    }

    RTTI_DEFINE_OBJECT_VERSIONED(McpFolderAnswers, "rtti::editor::editor", 1)
}
