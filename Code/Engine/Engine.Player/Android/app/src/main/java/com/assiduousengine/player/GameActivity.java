// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

package com.assiduousengine.player;

import org.libsdl.app.SDLActivity;

/**
 * The engine's activity: SDL's, running the engine from libmain.so (SDL is linked into it, so it is
 * the one library). Its SDL_main reads the game and the engine's data in place from this app's APK
 * (assets/Game, assets/Data; the packs stored uncompressed) and keeps the user's data and temporary
 * files in the app's own storage, so it is told where each is.
 */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] {
            "--apk", getApplicationInfo().sourceDir,
            "--files", getFilesDir().getAbsolutePath(),
            "--cache", getCacheDir().getAbsolutePath(),
        };
    }
}
