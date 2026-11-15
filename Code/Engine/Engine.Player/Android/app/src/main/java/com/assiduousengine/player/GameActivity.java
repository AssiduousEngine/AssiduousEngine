// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

package com.assiduousengine.player;

import android.content.Intent;
import android.content.pm.ApplicationInfo;

import java.util.ArrayList;
import java.util.Arrays;

import org.libsdl.app.SDLActivity;

/**
 * The engine's activity: SDL's, running the engine from libmain.so (SDL is linked into it, so it is
 * the one library). Its SDL_main reads the game and the engine's data in place from this app's APK
 * (assets/Game, assets/Data; the packs stored uncompressed) and keeps the user's data and temporary
 * files in the app's own storage, so it is told where each is. A debuggable app (the development app
 * scripts/android-app.sh builds) also passes on the flags it was launched with, as `am start ... --es
 * args "--profile-capture 20"`; an exported game, a release build, takes none from outside.
 */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        ArrayList<String> arguments = new ArrayList<>(Arrays.asList(
            "--apk", getApplicationInfo().sourceDir,
            "--files", getFilesDir().getAbsolutePath(),
            "--cache", getCacheDir().getAbsolutePath()));
        Intent intent = getIntent();
        String launched = intent != null ? intent.getStringExtra("args") : null;
        if (launched != null && (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            for (String argument : launched.trim().split("\\s+")) {
                if (!argument.isEmpty()) {
                    arguments.add(argument);
                }
            }
        }
        return arguments.toArray(new String[0]);
    }
}
