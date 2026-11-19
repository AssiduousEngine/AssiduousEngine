// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A project's MCP folders: the folders outside it its tools work in, each reached only once the
// user approved it for that project. The answers are the user's, in their data, never the project's.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.settings;
import engine.project;
import editor.core;

using namespace foundation::core;
using namespace editor;

TEST_CASE("mcp folders: a listed folder is unanswered until the user answers; only a yes approves it")
{
    // A project file could name any folder; opened, it widened what an agent might touch with no
    // one asked.
    RegisterMcpFolderTypes();
    const String answersDir = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_mcp_folders_answers");
    (void)RemoveDirectoryRecursive(answersDir.AsView());
    REQUIRE(CreateDirectories(answersDir.AsView()));
    const String project = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_mcp_folders_project");
    engine::project::ProjectSettings settings;
    settings.mcpFolders.PushBack(String(u8"../Game"));
    settings.mcpFolders.PushBack(String(u8"../Art"));

    // Each relative to the project, made absolute.
    const Array<String> listed = ResolveMcpFolders(project.AsView(), settings);
    REQUIRE(listed.Size() == 2u);
    CHECK(listed[0] == PathJoin(GetCurrentDirectory().AsView(), u8"Game"));
    CHECK(listed[1] == PathJoin(GetCurrentDirectory().AsView(), u8"Art"));

    // No answers yet: both unanswered, none approved.
    McpFolderStanding standing = StandMcpFolders(answersDir.AsView(), project.AsView(), settings);
    CHECK(standing.approved.IsEmpty());
    CHECK(standing.unanswered.Size() == 2u);

    // Yes to one, no to the other, kept in the user's file.
    {
        foundation::settings::Settings store(DefaultAllocator());
        McpFolderAnswers& answers = store.Section<McpFolderAnswers>();
        const String projectPath = PathNormalizeLexically(PathAbsolute(project.AsView()).AsView());
        answers.Record(projectPath.AsView(), listed[0].AsView(), true);
        answers.Record(projectPath.AsView(), listed[1].AsView(), false);
        foundation::vfs::NativeFileSystem root(answersDir.AsView(), DefaultAllocator());
        REQUIRE(SaveMcpFolderAnswers(root, store).IsOk());
    }
    standing = StandMcpFolders(answersDir.AsView(), project.AsView(), settings);
    REQUIRE(standing.approved.Size() == 1u);
    CHECK(standing.approved[0] == listed[0]);
    CHECK(standing.unanswered.IsEmpty()); // the refused one is asked no more, and not reached

    // The answers are this project's: another project listing the same folder is asked afresh.
    const String other = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_mcp_folders_other");
    engine::project::ProjectSettings otherSettings;
    otherSettings.mcpFolders.PushBack(listed[0]); // absolute, the same folder
    standing = StandMcpFolders(answersDir.AsView(), other.AsView(), otherSettings);
    CHECK(standing.approved.IsEmpty());
    CHECK(standing.unanswered.Size() == 1u);

    (void)RemoveDirectoryRecursive(answersDir.AsView());
}
