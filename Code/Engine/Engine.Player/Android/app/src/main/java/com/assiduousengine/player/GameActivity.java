// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

package com.assiduousengine.player;

import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * The engine's activity: SDL's, running the engine from libmain.so (SDL is linked into it, so it is
 * the one library), its SDL_main given the data root.
 *
 * The data (the APK's assets/Data) is copied out to the app's files the first time this version of
 * the app runs, since the engine reads its data by path.
 * TODO(android): read the packs in place from the APK instead of copying them out.
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "Engine";
    private static final String DATA = "Data";

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        final File root = new File(getFilesDir(), DATA);
        try {
            copyDataOut(root);
        } catch (IOException e) {
            Log.e(TAG, "could not copy the app's data out of the APK", e);
        }
        return new String[] { "--data-root", root.getAbsolutePath() };
    }

    // Copies assets/Data to `root` unless this version of the app already did, then writes the data
    // root's marker (Android's packager leaves dot files out of the assets).
    private void copyDataOut(File root) throws IOException {
        final File stamp = new File(getFilesDir(), "data.stamp");
        final String version = installedVersion();
        if (root.isDirectory() && version.equals(readText(stamp))) {
            return;
        }
        deleteTree(root);
        copyAssets(getAssets(), DATA, root);
        new File(root, ".dataroot").createNewFile();
        writeText(stamp, version);
    }

    private String installedVersion() {
        try {
            final PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return Long.toString(info.lastUpdateTime);
        } catch (Exception e) {
            return "";
        }
    }

    private static void copyAssets(AssetManager assets, String path, File to) throws IOException {
        final String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            to.getParentFile().mkdirs();
            try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(to)) {
                final byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            }
            return;
        }
        to.mkdirs();
        for (String child : children) {
            copyAssets(assets, path + "/" + child, new File(to, child));
        }
    }

    private static void deleteTree(File file) {
        final File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    private static String readText(File file) {
        try (InputStream in = new FileInputStream(file)) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}
