#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# android-tests.sh - run the engine's headless test suites on an Android phone over adb.
#
# Builds nothing: build the suites first in the android-arm64 tree, e.g.
#   cmake --preset android-arm64 && cmake --build build/android-arm64 -j4 --target Core.Tests
# Each suite is pushed to /data/local/tmp/engine-tests on the phone (where adb's shell may run a
# native executable) and run there, from that folder; its scratch writes stay beside it. The
# engine's data (Data/.dataroot, Engine, Tests, Samples, Editor: what the suites read) is pushed
# beside the suites; the Android build points them there (BUILDSYSTEM_TEST_SOURCE_DIR).
#
#   Usage:  scripts/android-tests.sh [Suite ...]      (default: every *.Tests built for Android)
#   Env:    BIN=<dir>       where the suites are (default Bin/Debug/Android64-Clang)
#           SERIAL=<id>     the phone, when more than one is attached (adb -s)
#           DATA=0          skip pushing Data/
#           ARGS="..."      passed to each suite (doctest's, e.g. -tc="pattern")
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="${BIN:-Bin/Debug/Android64-Clang}"
REMOTE="/data/local/tmp/engine-tests"
ADB=(adb)
if [[ -n "${SERIAL:-}" ]]; then ADB+=(-s "$SERIAL"); fi

if ! command -v adb > /dev/null; then
    echo "adb not found: put \$ANDROID_HOME/platform-tools on PATH" >&2
    exit 2
fi
if [[ "$("${ADB[@]}" get-state 2> /dev/null || true)" != "device" ]]; then
    echo "no phone attached (adb devices): connect it over USB with USB debugging on" >&2
    exit 2
fi

suites=("$@")
if [[ ${#suites[@]} -eq 0 ]]; then
    while IFS= read -r path; do suites+=("$(basename "$path")"); done \
        < <(find "$BIN" -maxdepth 1 -type f -name '*.Tests' -perm -u+x | sort)
fi
if [[ ${#suites[@]} -eq 0 ]]; then
    echo "no test suites in $BIN: build them in build/android-arm64 first" >&2
    exit 2
fi

"${ADB[@]}" shell mkdir -p "$REMOTE"
if [[ "${DATA:-1}" != "0" ]]; then
    # The data root walks up from the executable for Data/.dataroot: put it beside the suites, with
    # the data the suites read (not the sample projects: no suite run here cooks them).
    "${ADB[@]}" shell mkdir -p "$REMOTE/Data"
    for part in .dataroot Engine Tests Samples Editor; do
        "${ADB[@]}" push --sync "Data/$part" "$REMOTE/Data/" > /dev/null
    done
fi

# The libraries a suite loads at runtime (Core.Tests' TestPlugin) go beside it: it loads them from
# its own folder there.
while IFS= read -r library; do
    "${ADB[@]}" push --sync "$library" "$REMOTE/" > /dev/null
done < <(find "$BIN" -maxdepth 1 -type f -name '*.so')

failed=()
for suite in "${suites[@]}"; do
    if [[ ! -f "$BIN/$suite" ]]; then
        echo "== $suite: not built" >&2
        failed+=("$suite")
        continue
    fi
    "${ADB[@]}" push --sync "$BIN/$suite" "$REMOTE/" > /dev/null
    echo "== $suite"
    if "${ADB[@]}" shell "cd $REMOTE && ./$suite ${ARGS:-}"; then
        :
    else
        failed+=("$suite")
    fi
done

echo
if [[ ${#failed[@]} -eq 0 ]]; then
    echo "all ${#suites[@]} suite(s) passed"
else
    echo "${#failed[@]} of ${#suites[@]} suite(s) failed: ${failed[*]}"
    exit 1
fi
