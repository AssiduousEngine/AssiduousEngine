// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The levels the cook checks a conversation's entities against, read from a real saved scene (the
// check tests use a stand-in): its entities' names come back, each level is loaded once, and a level
// that is not there is null.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import dialogue.pipeline;

using namespace foundation::core;
using namespace dialogue;

TEST_CASE("dialogue levels: a saved scene's entities by name, each level loaded once, a missing one null")
{
    const StringView dir = u8"scratch_dialogue_levels";
    (void)RemoveDirectoryRecursive(dir);
    (void)CreateDirectories(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        foundation::scene::Scene village(DefaultAllocator(), u8"village");
        (void)village.CreateEntity(u8"Keeper");
        (void)village.CreateEntity(u8"Gate");
        foundation::content::Instance* level = db.RootGroup()->CreateInstance(u8"village", foundation::scene::SceneDocument::StaticType());
        REQUIRE(level != nullptr);
        REQUIRE(foundation::scene::SaveScene(village, *level).IsOk());

        UniquePtr<IDialogueLevels> levels = MakeDialogueLevels(db, DefaultAllocator());
        const Array<String>* names = levels->EntityNames(level->Id());
        REQUIRE(names != nullptr);
        REQUIRE(names->Size() == 2u);
        CHECK(((*names)[0] == StringView(u8"Keeper") || (*names)[1] == StringView(u8"Keeper")));
        CHECK(((*names)[0] == StringView(u8"Gate") || (*names)[1] == StringView(u8"Gate")));
        CHECK(levels->EntityNames(level->Id()) == names); // loaded once: the same names again
        CHECK(levels->EntityNames(Guid{0x7, 0x7}) == nullptr);
        CHECK(levels->EntityNames(Guid{0x7, 0x7}) == nullptr); // and remembered as missing
    }
    (void)RemoveDirectoryRecursive(dir);
}
