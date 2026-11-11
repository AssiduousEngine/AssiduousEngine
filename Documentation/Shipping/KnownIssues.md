# Known issues

The CURATED, distribution-facing known-issues register: limitations and behaviors an engine
user (or an agent working on a game project) can actually hit. This file ships with the
engine tooling and feeds the MCP `known_issues` tool.

Curation rule: entries describe USER-VISIBLE symptoms with their impact and workaround -
never internal build/porting/triage state (that lives in the engine repo's development
tracker, which is not distributed). When a development issue gains a user-visible symptom,
add the user-facing half here; remove entries when the fix ships.

---

## AngelScript: harmless `$func` warning at shutdown

If a project used AngelScript coroutines, the engine may print an AngelScript
garbage-collector warning mentioning `$func` while shutting down. It is cosmetic: the
process is ending and nothing leaks at runtime. No action needed.

## Older projects: legacy `draconic::` type names load via a fallback

Projects saved by older engine versions embed legacy `draconic::`-prefixed type names in
their data (assets, settings, scenes). Current engines load them through a compatibility
fallback, and data converges to the current names as it is re-saved. If an asset fails to
resolve its type, open and re-save it (or re-import the source) to migrate it.

## Android: a game with its own native packages does not export yet

An Android preset exports a game whose code is its scripts and the engine's plugins. A project
with its own native packages (C++ under `Plugins/`) is refused with that reason, since the
player with its packages is not yet built for Android. Desktop and web exports of it work.

## Android: the first export downloads Gradle

An Android export builds the app with the template's Gradle project. The first one on a machine
downloads Gradle and the Android build plugin, so it needs the network and takes minutes; later
exports reuse them, and each preset's build is kept in `<project>/.cache/android/` so the next
export builds only what changed.

## Web (WebGPU) targets: scene MSAA is 1x or 4x only

WebGPU supports sample counts 1 and 4 for the scene pass - there is no 2x. A project
whose render settings ask for 2x MSAA is clamped to the nearest supported count at
runtime on web targets. Pick 1x or 4x directly for identical results across platforms.

## Editor text rendering covers Latin-1 only

The editor's bundled UI font rasterizes codepoints up to 255 (ASCII + Latin-1). Names
using characters outside that range (CJK, emoji, extended scripts) render as blank glyphs
in editor panels. The data itself is unaffected - it round-trips correctly; only the
editor display is limited. Prefer ASCII/Latin-1 names for now.
