# Source Path type

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[path-type-plan]]

`SourcePath` is a mount-relative logical path value type for source references in asset data - normalized,
relative, forward-slash, wire-compatible with the `String` it replaced. The type, its adoption as the
single asset path reference, and its row on the reflected asset pages shipped; the check-assets lint
is not built.

## `SourcePath` (`foundation.vfs`)

`foundation.vfs`'s `SourcePath` (`VFS/SourcePath.cppm`, the `:source_path` partition; the vfs owns the
mount concept). Not a general OS path - no volumes, no macros, no absolute form. One guarantee:
whatever you construct it from, the stored form is forward-slash, relative, and dot-segment-free.
Construction from a `StringView` normalizes `\` -> `/`, collapses duplicate slashes and `./`, and
turns absolute paths, `..` escapes and volume/scheme prefixes into the EMPTY path (a missing
reference, never a wrong one); `SourcePath::Normalize` is the same contract standalone, for
importers producing paths. Accessors: `View()`, `IsEmpty()`, `Extension()` (lowercased, no dot),
`FileName()`, `Stem()`, `Directory()`. `==`/`<` are case-SENSITIVE everywhere (one rule, all
platforms). Serialization writes/reads the stored string - the SAME wire shape as `String`, so
existing `.xasset`/`.rasset` data loads unchanged with no version bump, and Windows-authored
backslash paths silently heal on load (the bug this exists to kill). It is reflected as a value
type (`REFLECT_VALUE`, `VFSReflectionImpl.cpp`) whose surface is its read accessors, since the
stored string is private.

## Adoption

`Asset::fileName` (the editor asset base, `Pipeline.Core/Asset.cppm`) IS a `SourcePath`, inherited by
every asset type (font/image/texture/mesh/animation/material/physics/audio/script/shader/UI/inputmap/
particle...) and reflected on the base as "Source File". Builders read it through the VFS sources
mount (`AssetBuildContext::sources`: mount-based resolution, not CWD-based), and a build's extra
dependency files are `SourcePath`s too.

On the reflected asset pages (`AssetObjectPage`, `Editor.App`) a `SourcePath` field is recognized by
its type hint and shown by `SourcePathEditor`: the path as the asset names it, read-only (a typo would
cook a dangling reference). A page that declares the extensions its source may have
(`SourceExtensions()`, today the font page) also gets a Browse button, which opens the path picker
(`PathPicker`, `Editor.App/PathPickerDialog.cppm`) over the project's sources root filtered by those
extensions, producing root-relative paths by construction (so the absolute-path hole a generic OS
browse would have cannot happen).

## Deliberate non-goals

No `$(MACRO)` expansion (VFS mounts/schemes already provide machine-independent roots; macros with
silent-erase semantics are a footgun); no volume handling (OS-absolute paths - font settings, project
registry, export dirs - stay `String` at the Win32/DXC edge per the UTF-8 policy); no case-insensitive
comparison anywhere.

## Deferred

- **Extensions from reflection metadata.** The allowed extensions come from the page
  (`SourceExtensions()`), not from a field attribute (e.g. `extensions=.ttf,.otf`), so a page without
  an override shows the path with no Browse.
- **Check-assets lint.** A "Check Assets" editor action / cook-time warning (every Asset-derived
  instance's `fileName` + dependency files present in the sources mount) is not built.

---

The Traktor `traktor::Path` survey (the eager string decomposition, the serializer-primitive + typed
UI-binding model, and the sharp edges NOT copied - inconsistent case, silent macro-erase, colon-as-volume,
generic-browse-stores-absolute) is in `Documentation/Archive/path-type-design-history.md`.
