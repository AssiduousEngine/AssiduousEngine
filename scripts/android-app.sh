#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# android-app.sh - build an Android app (APK) around one of the engine's programs and, with
# --install, put it on the attached phone.
#
# The app is Code/Engine/Engine.Player/Android: SDL's activity running the program from
# libmain.so, which reads its data in place from the APK. This stages what it carries under
# build/android-app/<Target>/ (the program as libmain.so, its data as assets/) and runs the
# project's Gradle wrapper there. The program is the player, with a game, or an RHI sample:
#   - Engine.Player --game <project>: the game's content exported for Android (the host's
#     Tools.Export --content-only --platform Android64: Content.pak with ASTC textures and
#     player.xml as assets/Game, the SPIR-V shader pack as assets/Data).
#   - an RHI sample: their shaders cooked from Data/Samples/RHI (the host's Tools.ShaderPack
#     --entries), since Android has no shader compiler.
#
#   Usage:  scripts/android-app.sh <Target> [--game <project>] [--install] [--run] [--args "<flags>"]
#           e.g. scripts/android-app.sh Engine.Player --game Data/SampleProjects/SkyHopper --run
#                scripts/android-app.sh Sample001_Triangle --install --run
#                CONFIG=RelWithDebInfo scripts/android-app.sh Engine.Player --game <project> \
#                    --run --args "--profile-capture 20"
#   --args passes flags to the program at launch (the development app is debuggable, so it takes
#   them; an exported game does not); it implies --run.
#   Env:    CONFIG=Debug         the Android build's configuration (default Debug): Debug builds in
#                                build/android-arm64, RelWithDebInfo in build/android-arm64-reldbg
#           HOST_BIN=<dir>       the host build with Tools.ShaderPack and Tools.Export
#                                (default Bin/Debug/Linux64-Clang)
#           SERIAL=<id>          the phone, when more than one is attached (adb -s)
#           NO_BUILD=1           package what is already built (skip the cmake build of <Target>)
#           VULKAN_LAYER=<.so>   a Vulkan layer to carry beside the program, as the Khronos
#                                validation layer's Android build: a debuggable app loads the
#                                layers in its own library folder, and the engine asks for
#                                validation in Debug
#
# Needs the Android build tree (cmake --preset android-arm64, or android-arm64-reldbg),
# ANDROID_HOME and a JDK 17 for Gradle, and network access the first time (the wrapper fetches
# Gradle, Gradle the Android plugin).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET="${1:-}"
if [[ -z "$TARGET" || "$TARGET" == -* ]]; then
    sed -n '6,39p' "$0" >&2
    exit 2
fi
shift
INSTALL=0
RUN=0
GAME=""
ARGS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install) INSTALL=1 ;;
        --run) INSTALL=1; RUN=1 ;;
        --game) GAME="$2"; shift ;;
        --args) INSTALL=1; RUN=1; ARGS="$2"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

CONFIG="${CONFIG:-Debug}"
ANDROID_BIN="Bin/$CONFIG/Android64-Clang"
case "$CONFIG" in
    Debug) BUILD_TREE="build/android-arm64" ;;
    RelWithDebInfo) BUILD_TREE="build/android-arm64-reldbg" ;;
    *) echo "CONFIG is Debug or RelWithDebInfo, not $CONFIG" >&2; exit 2 ;;
esac
HOST_BIN="${HOST_BIN:-Bin/Debug/Linux64-Clang}"
STAGED="$ROOT/build/android-app/$TARGET"
PROJECT="$ROOT/Code/Engine/Engine.Player/Android"
ADB=(adb)
if [[ -n "${SERIAL:-}" ]]; then ADB+=(-s "$SERIAL"); fi
: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

if [[ "${NO_BUILD:-0}" != "1" ]]; then
    TMPDIR="$ROOT/build/tmp" cmake --build "$BUILD_TREE" -j4 --target "$TARGET"
fi
LIBRARY="$ANDROID_BIN/lib$TARGET.so"
if [[ ! -f "$LIBRARY" ]]; then
    echo "no $LIBRARY: build $TARGET in $BUILD_TREE (an RHI sample builds as a library there)" >&2
    exit 1
fi

echo "== staging $TARGET into $STAGED"
rm -rf "$STAGED/jniLibs" "$STAGED/assets"
mkdir -p "$STAGED/jniLibs/arm64-v8a" "$STAGED/assets/Data"
# The debug info stays in the build (addr2line reads it there); the app carries the code.
"$STRIP" --strip-debug -o "$STAGED/jniLibs/arm64-v8a/libmain.so" "$LIBRARY"
if [[ -n "${VULKAN_LAYER:-}" ]]; then
    cp "$VULKAN_LAYER" "$STAGED/jniLibs/arm64-v8a/"
fi
if [[ -n "$GAME" ]]; then
    EXPORT="$STAGED/export"
    rm -rf "$EXPORT"
    "$HOST_BIN/Tools.Export" "$GAME" --content-only --platform Android64 --out "$EXPORT"
    DIST="$(find "$EXPORT" -mindepth 1 -maxdepth 1 -type d | head -1)"
    mkdir -p "$STAGED/assets/Game" "$STAGED/assets/Data/Engine/Shaders"
    cp "$DIST/Content.pak" "$DIST/player.xml" "$STAGED/assets/Game/"
    cp "$DIST/Data/Engine/Shaders/shaders.dpak" "$STAGED/assets/Data/Engine/Shaders/"
    # The rest of the engine's data (the GUI's built-in fonts) as a desktop player's dist carries
    # it; the shader sources stay behind, since the pack replaces them.
    for ENTRY in Data/Engine/*; do
        [[ "$(basename "$ENTRY")" == "Shaders" ]] || cp -r "$ENTRY" "$STAGED/assets/Data/Engine/"
    done
else
    mkdir -p "$STAGED/assets/Data/Samples/RHI"
    "$HOST_BIN/Tools.ShaderPack" --entries Data/Samples/RHI "$STAGED/assets/Data/Samples/RHI/shaders.dpak" spirv
fi

echo "== building the APK"
(cd "$PROJECT" && ./gradlew --quiet assembleDebug -PengineStaged="$STAGED")
APK="$STAGED/gradle/app/outputs/apk/debug/app-debug.apk"
ls -la "$APK"

if [[ "$INSTALL" == "1" ]]; then
    "${ADB[@]}" install -r "$APK"
fi
if [[ "$RUN" == "1" ]]; then
    "${ADB[@]}" logcat -c
    if [[ -n "$ARGS" ]]; then
        "${ADB[@]}" shell am start -n com.assiduousengine.player/.GameActivity --es args "\"$ARGS\""
    else
        "${ADB[@]}" shell am start -n com.assiduousengine.player/.GameActivity
    fi
    echo "== logcat (Ctrl+C to stop)"
    "${ADB[@]}" logcat -s Engine SDL SDL/APP AndroidRuntime DEBUG
fi
