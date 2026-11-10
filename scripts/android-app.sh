#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# android-app.sh - build an Android app (APK) around one of the engine's programs and, with
# --install, put it on the attached phone.
#
# The app is Code/Engine/Engine.Player/Android: SDL's activity running the program from
# libmain.so. This stages what it carries under build/android-app/<Target>/ (the program as
# libmain.so, its data as assets/Data) and runs the project's Gradle wrapper there. The
# programs are the RHI samples: their shaders come cooked from
# Data/Samples/RHI (the host's Tools.ShaderPack --entries), since Android has no shader compiler.
#
#   Usage:  scripts/android-app.sh <Target> [--install] [--run]
#           e.g. scripts/android-app.sh Sample001_Triangle --install --run
#   Env:    CONFIG=Debug         the Android build's configuration (default Debug)
#           HOST_BIN=<dir>       the host build with Tools.ShaderPack (default Bin/Debug/Linux64-Clang)
#           SERIAL=<id>          the phone, when more than one is attached (adb -s)
#           NO_BUILD=1           package what is already built (skip the cmake build of <Target>)
#
# Needs the Android build tree (cmake --preset android-arm64), ANDROID_HOME and a JDK 17 for
# Gradle, and network access the first time (the wrapper fetches Gradle, Gradle the Android
# plugin).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET="${1:-}"
if [[ -z "$TARGET" || "$TARGET" == -* ]]; then
    sed -n '6,24p' "$0" >&2
    exit 2
fi
shift
INSTALL=0
RUN=0
for arg in "$@"; do
    case "$arg" in
        --install) INSTALL=1 ;;
        --run) INSTALL=1; RUN=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

CONFIG="${CONFIG:-Debug}"
ANDROID_BIN="Bin/$CONFIG/Android64-Clang"
HOST_BIN="${HOST_BIN:-Bin/Debug/Linux64-Clang}"
STAGED="$ROOT/build/android-app/$TARGET"
PROJECT="$ROOT/Code/Engine/Engine.Player/Android"
ADB=(adb)
if [[ -n "${SERIAL:-}" ]]; then ADB+=(-s "$SERIAL"); fi
: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

if [[ "${NO_BUILD:-0}" != "1" ]]; then
    TMPDIR="$ROOT/build/tmp" cmake --build build/android-arm64 -j4 --target "$TARGET"
fi
LIBRARY="$ANDROID_BIN/lib$TARGET.so"
if [[ ! -f "$LIBRARY" ]]; then
    echo "no $LIBRARY: build $TARGET in build/android-arm64 (an RHI sample builds as a library there)" >&2
    exit 1
fi

echo "== staging $TARGET into $STAGED"
rm -rf "$STAGED/jniLibs" "$STAGED/assets"
mkdir -p "$STAGED/jniLibs/arm64-v8a" "$STAGED/assets/Data/Samples/RHI"
# The debug info stays in the build (addr2line reads it there); the app carries the code.
"$STRIP" --strip-debug -o "$STAGED/jniLibs/arm64-v8a/libmain.so" "$LIBRARY"
"$HOST_BIN/Tools.ShaderPack" --entries Data/Samples/RHI "$STAGED/assets/Data/Samples/RHI/shaders.dpak" spirv

echo "== building the APK"
(cd "$PROJECT" && ./gradlew --quiet assembleDebug -PengineStaged="$STAGED")
APK="$STAGED/gradle/app/outputs/apk/debug/app-debug.apk"
ls -la "$APK"

if [[ "$INSTALL" == "1" ]]; then
    "${ADB[@]}" install -r "$APK"
fi
if [[ "$RUN" == "1" ]]; then
    "${ADB[@]}" logcat -c
    "${ADB[@]}" shell am start -n com.assiduousengine.player/.GameActivity
    echo "== logcat (Ctrl+C to stop)"
    "${ADB[@]}" logcat -s Engine SDL SDL/APP AndroidRuntime DEBUG
fi
