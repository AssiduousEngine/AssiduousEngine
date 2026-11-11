// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.Player on Android: the SDL_main the app's activity calls (SDL's Java glue). The game and the
// engine's data are read in place from the app's APK (assets/Game: player.xml and Content.pak,
// stored uncompressed; assets/Data: the engine's data), and the user data and temporary files go in
// the app's own storage. The activity passes where those are:
//   --apk <the APK's path> --files <its files folder> --cache <its cache folder>

#include <cstring>

#include "Core/Prelude.h"
#include "Core/Log/Log.h"

import foundation.core;
import foundation.vfs;
import foundation.vfs.pak;
import foundation.vfs.zip;
import engine.player.main;

using namespace foundation::core;

namespace
{
    // The value after `name` in argv, or empty.
    StringView Argument(int argc, char** argv, const char* name)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::strcmp(argv[i], name) == 0)
            {
                return StringView(reinterpret_cast<const utf8char*>(argv[i + 1]));
            }
        }
        return StringView{};
    }
}

extern "C" int SDL_main(int argc, char* argv[])
{
    const StringView apk = Argument(argc, argv, "--apk");
    const StringView files = Argument(argc, argv, "--files");
    const StringView cache = Argument(argc, argv, "--cache");
    // The user data (settings, saves) and temporary files in the app's storage: Core finds them
    // through these, as on Linux.
    if (!files.IsEmpty())
    {
        (void)SetEnvironmentVariable(u8"XDG_DATA_HOME", files);
    }
    if (!cache.IsEmpty())
    {
        (void)SetEnvironmentVariable(u8"TMPDIR", cache);
    }
    if (apk.IsEmpty())
    {
        LOG_ERROR(u8"Player", u8"no --apk: the activity names the app's APK, where the game is");
        return 1;
    }

    IAllocator& allocator = DefaultAllocator();
    engine::player::PlayerMounts mounts;
    auto game = MakeUnique<foundation::vfs::ZipFileSystem>(allocator, apk, u8"assets/Game", allocator);
    if (!game->IsValid())
    {
        LOG_ERROR(u8"Player", u8"the APK at '{}' cannot be read: {}", apk, game->Error());
        return 1;
    }
    u64 offset = 0;
    u64 size = 0;
    if (game->StoredRegion(u8"Content.pak", offset, size))
    {
        mounts.content = MakeUnique<foundation::vfs::PakFileSystem>(allocator, apk, offset, size, allocator);
    }
    else if (game->Exists(u8"Content.pak"))
    {
        LOG_ERROR(u8"Player", u8"the APK's Content.pak is compressed; it must be stored to be read in place");
        return 1;
    }
    mounts.game = Move(game);
    mounts.data = MakeUnique<foundation::vfs::ZipFileSystem>(allocator, apk, u8"assets/Data", allocator);
    return engine::player::PlayerMain(argc, argv, Move(mounts));
}
