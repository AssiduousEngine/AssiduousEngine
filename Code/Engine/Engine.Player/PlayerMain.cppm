// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.player.main: the desktop player's boot as a library entry point. Engine.Player and the
// project's Engine.GamePlayer are each a thin stub calling it; what tells them apart is the plugins
// each links.

module;
#include "Core/Prelude.h"

export module engine.player.main;

export import foundation.core;
export import foundation.vfs;
export import foundation.vfs.pak;

export namespace engine::player
{
    // Where a game and the engine's data are, when they are not folders the player finds by path
    // (an Android app's, read in place from its APK). Each one given replaces the lookup: `game`
    // holds player.xml, `content` is its Content.pak, and `data` is the engine's data root.
    struct PlayerMounts
    {
        foundation::core::UniquePtr<foundation::vfs::IFileSystem> game;
        foundation::core::UniquePtr<foundation::vfs::PakFileSystem> content;
        foundation::core::UniquePtr<foundation::vfs::IFileSystem> data;
    };

    // Runs the player.
    int PlayerMain(int argc, char** argv);
    // Runs the player over the mounts the host made.
    int PlayerMain(int argc, char** argv, PlayerMounts mounts);
}
