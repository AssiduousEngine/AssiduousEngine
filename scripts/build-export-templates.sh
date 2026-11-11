#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# build-export-templates.sh - build export templates (prebuilt runtime bundles) for the
# supported platforms, so `Tools.Export` and the editor can stamp shippable dists against them.
#
# A template is the Engine.Player runtime for one (platform, config), its runtime sidecars and a
# template.xml manifest. It is neither game content nor a toolchain: it is the prebuilt runtime
# you ship against.
#
# Per platform this builds Engine.Player (and the native Tools.Export once), then runs
#   Tools.Export --template create <Bin/<Config>/<Platform>-<Compiler>>
# which reads the build's runtime-libs manifest, copies the player and sidecars, and writes
# template.xml.
#
# The Linux template is built RelWithDebInfo (the clang-reldbg preset: static, since a player links
# its plugins statically; the editor distribution's tree is the shared one): the player is optimised, and its
# debug info is split off before packaging: the template's player is stripped (an export copies it
# as it is, so every game would carry it otherwise) and Engine.Player.debug goes to
# <OUT>/linux64-release-symbols, to keep with the build. The template's config, and so its id, is
# RelWithDebInfo (assiduousengine-linux64-relwithdebinfo-<version>).
#
# The Android template is built RelWithDebInfo too (the android-arm64-reldbg preset, through the
# NDK at ANDROID_NDK_HOME): the player is the library the app loads (libEngine.Player.so, stripped,
# its debug info in <OUT>/android64-release-symbols), and the app's Gradle project rides with it
# (the build stages it beside the library). An export copies the project, fills in the game and
# runs its Gradle wrapper, so exporting for Android needs the Android SDK and a JDK; making the
# template does not.
#
# Windows is not built here (no toolchain on Linux); use scripts/build-export-templates.ps1 on a
# Windows agent for the Win64 template.
#
# The web templates are two: web-release (the wasm-shipping preset: smallest, no profiler), which
# the sample games' Web presets and the published demos use, and web-profile (the wasm-release
# preset: Release with the profiler, its capture and its overlay compiled in, the overlay off by
# default), to profile a game in a browser. Both land in Bin/Release/..., so the profiling one
# is given its own id.
#
#   Usage:  scripts/build-export-templates.sh [linux|web|android|all]      (default: all)
#   Env:    JOBS=N     build parallelism (default 4; higher OOMs the engine build)
#           OUT=<dir>  write self-contained bundles under <dir> (for zip/distribution);
#                      unset = --install into the local templates root (usable immediately)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-4}"
OUT="${OUT:-}"
WHICH="${1:-all}"

LINUX_PRESET="clang-reldbg"            # RelWithDebInfo, static (the player links its plugins)
LINUX_BUILD="build/$LINUX_PRESET"
LINUX_BIN="Bin/RelWithDebInfo/Linux64-Clang"
ANDROID_PRESET="android-arm64-reldbg"  # RelWithDebInfo through the NDK
ANDROID_BUILD="build/$ANDROID_PRESET"
ANDROID_BIN="Bin/RelWithDebInfo/Android64-Clang"
WEB_BUILD="build/wasm-shipping"        # the wasm-shipping preset: emscripten, Release, shipping
WEB_BIN="Bin/Release/Emscripten-Clang-Shipping" # the preset's output suffix (-Shipping)
WEB_PROFILE_BUILD="build/wasm-release"  # the wasm-release preset: emscripten, Release, profiler in
WEB_PROFILE_BIN="Bin/Release/Emscripten-Clang"
VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1)"
PREFIX="$(sed -n 's/^set(ENGINE_ID[[:space:]]*"\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"

# The native exporter packages every platform's bundle (it runs on the host, pointing at the
# target's Bin dir, including the wasm one, which it synthesizes into a "Web" template).
EXPORTER="$LINUX_BIN/Tools.Export"

log() { printf '\n== %s ==\n' "$*"; }

create_template() { # <config-dir> <out-subdir-tag> [Tools.Export --id/--name/... flags]
    local cfg="$1"
    local tag="$2"
    shift 2
    if [[ ! -x "$EXPORTER" ]]; then
        echo "!! $EXPORTER not built yet (the Linux step builds it)" >&2
        return 1
    fi
    if [[ -n "$OUT" ]]; then
        # --out writes the bundle flat into the given folder, so each platform needs its own
        # subfolder or a multi-platform run would clobber one template.xml with the next.
        local dest="$OUT/$tag"
        mkdir -p "$dest"
        "$EXPORTER" --template create "$cfg" --out "$dest" "$@"
    else
        "$EXPORTER" --template create "$cfg" --install "$@"   # --install lands each under its <id>
    fi
}

build_linux() {
    log "Linux (Release) template"
    if [[ ! -f "$LINUX_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $LINUX_BUILD (the $LINUX_PRESET preset)"
        # The preset pins clang (the Bin/ dir is compiler-derived); CXX_COMPILER overrides it
        # for a versioned clang (see build-editor-dist.sh).
        CXX_BIN="${CXX_COMPILER:-clang++}"
        C_BIN="${C_COMPILER:-clang}"
        cmake --preset "$LINUX_PRESET" -DCMAKE_C_COMPILER="$C_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
            -DCMAKE_ASM_COMPILER="$CXX_BIN"
    fi
    cmake --build "$LINUX_BUILD" --target Engine.Player Tools.Export -j"$JOBS"

    # Package a stripped copy: the player and its sidecars in a staging folder shaped like the Bin
    # one (the template reads its config and compiler from the path), the debug info split off.
    local stage="build/tmp/template-stage/$LINUX_BIN"
    local symbols="${OUT:-build/tmp/template-stage}/linux64-release-symbols"
    rm -rf "build/tmp/template-stage" "$symbols"
    mkdir -p "$stage" "$symbols"
    cp "$LINUX_BIN/Engine.Player" "$stage/"
    # The plugins the player links (util_link_plugins writes the list): the template records them
    # and carries their package files.
    [[ -f "$LINUX_BIN/Engine.Player.plugins" ]] && cp "$LINUX_BIN/Engine.Player.plugins" "$stage/"
    if [[ -f "$LINUX_BIN/Engine.Player.runtime-libs" ]]; then
        cp "$LINUX_BIN/Engine.Player.runtime-libs" "$stage/"
        while IFS= read -r lib; do
            if [[ -n "$lib" && -f "$LINUX_BIN/$lib" ]]; then
                cp "$LINUX_BIN/$lib" "$stage/"
            fi
        done < "$LINUX_BIN/Engine.Player.runtime-libs"
    fi
    objcopy --only-keep-debug "$stage/Engine.Player" "$symbols/Engine.Player.debug"
    strip --strip-debug "$stage/Engine.Player"
    objcopy --add-gnu-debuglink="$symbols/Engine.Player.debug" "$stage/Engine.Player"
    create_template "$stage" "linux64-release"
    echo "player symbols: $symbols/Engine.Player.debug (keep it with the build)"
}

build_web() {
    log "Web (Release) template"
    if ! command -v emcc >/dev/null 2>&1; then
        if [[ -f "$HOME/emsdk/emsdk_env.sh" ]]; then
            # shellcheck disable=SC1091
            source "$HOME/emsdk/emsdk_env.sh" >/dev/null
        fi
    fi
    if ! command -v emcc >/dev/null 2>&1; then
        echo "!! emcc not on PATH (and ~/emsdk/emsdk_env.sh not found) - skipping web template" >&2
        return 0
    fi
    if [[ ! -f "$WEB_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $WEB_BUILD (the wasm-shipping preset)"
        cmake --preset wasm-shipping
    fi
    cmake --build "$WEB_BUILD" --target Engine.Player -j"$JOBS"
    # Packaged into a "Web" template by the native exporter (built in the Linux step).
    create_template "$WEB_BIN" "web-release"

    log "Web (profiling) template"
    if [[ ! -f "$WEB_PROFILE_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $WEB_PROFILE_BUILD (the wasm-release preset)"
        cmake --preset wasm-release
    fi
    cmake --build "$WEB_PROFILE_BUILD" --target Engine.Player -j"$JOBS"
    [[ -n "$VERSION" && -n "$PREFIX" ]] || { echo "!! no project VERSION or ENGINE_ID (the template id prefix) in CMakeLists.txt" >&2; return 1; }
    create_template "$WEB_PROFILE_BIN" "web-profile" \
        --id "$PREFIX-web-profile-$VERSION" \
        --name "Web Profile $VERSION" \
        --notes "Web Release with the profiler compiled in: its capture and its overlay (off by default)."
}

build_android() {
    log "Android (Release) template"
    if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
        echo "!! ANDROID_NDK_HOME is not set - skipping the Android template" >&2
        return 0
    fi
    if [[ ! -f "$ANDROID_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $ANDROID_BUILD (the $ANDROID_PRESET preset)"
        cmake --preset "$ANDROID_PRESET"
    fi
    TMPDIR="$ROOT/build/tmp" cmake --build "$ANDROID_BUILD" --target Engine.Player -j"$JOBS"

    # As the Linux one: a stripped copy staged in a folder shaped like the Bin one, with what the
    # runtime-libs list (the notices, the Gradle project) in their folders, the debug info split off.
    local llvm="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    local stage="build/tmp/template-stage/$ANDROID_BIN"
    local symbols="${OUT:-build/tmp/template-stage}/android64-release-symbols"
    local player="libEngine.Player.so"
    rm -rf "build/tmp/template-stage" "$symbols"
    mkdir -p "$stage" "$symbols"
    cp "$ANDROID_BIN/$player" "$stage/"
    [[ -f "$ANDROID_BIN/Engine.Player.plugins" ]] && cp "$ANDROID_BIN/Engine.Player.plugins" "$stage/"
    cp "$ANDROID_BIN/Engine.Player.runtime-libs" "$stage/"
    while IFS= read -r file; do
        if [[ -n "$file" ]]; then
            mkdir -p "$stage/$(dirname "$file")"
            cp -p "$ANDROID_BIN/$file" "$stage/$file"
        fi
    done < "$ANDROID_BIN/Engine.Player.runtime-libs"
    "$llvm/llvm-objcopy" --only-keep-debug "$stage/$player" "$symbols/$player.debug"
    "$llvm/llvm-strip" --strip-debug "$stage/$player"
    "$llvm/llvm-objcopy" --add-gnu-debuglink="$symbols/$player.debug" "$stage/$player"
    create_template "$stage" "android64-release"
    echo "player symbols: $symbols/$player.debug (keep it with the build)"
}

case "$WHICH" in
    linux)   build_linux ;;
    web)     build_linux; build_web ;;       # web needs the native exporter, so build Linux first
    android) build_linux; build_android ;;   # as does Android
    all)     build_linux; build_web; build_android ;;
    *) echo "usage: $0 [linux|web|android|all]   (env: JOBS=N OUT=<dir>)"; exit 2 ;;
esac

log "done"
if [[ -n "$OUT" ]]; then
    echo "templates written under: $OUT/<platform>/   (zip each platform folder to distribute)"
else
    echo "templates installed into the local templates root (Tools.Export --template list to verify)"
fi
