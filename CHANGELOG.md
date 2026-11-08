# Changelog

What changed in AssiduousEngine between releases, newest first. A change that someone using the
engine, the editor or a game made with it would notice gets a line under **Unreleased** in the
commit that makes it; a release turns Unreleased into its version and date. The steps are in
[Documentation/Process/CONVENTIONS.md](Documentation/Process/CONVENTIONS.md), under Releases.

## Unreleased

### Changed

- Editor: each scene page's Simulate runs its scripts apart from other pages', with their own
  script context and clock.

### Fixed

- Editor: what a Simulate's scripts put on screen goes when it stops, and no longer shows over a
  later Play.
- Editor: context menus open at the pointer under a UI scale other than 100% (the asset browser,
  the hierarchy, the log, the pickers, the bus layout, the particle, animation graph and model
  viewer pages), and a tab's Move to opens below its tab.
- Editor: the UI scale preference shows its percentage while its slider is dragged.
- Editor: Add Component is disabled while a scene simulates, as the rest of the inspector is.
- Play: a game with no startup script keeps its save, in play in editor and in the player.

## 0.1.0-beta - 2026-11-07

The first public beta.
