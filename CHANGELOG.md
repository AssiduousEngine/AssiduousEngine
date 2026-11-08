# Changelog

What changed in AssiduousEngine between releases, newest first. A change that someone using the
engine, the editor or a game made with it would notice gets a line under **Unreleased** in the
commit that makes it; a release turns Unreleased into its version and date. The steps are in
[Documentation/Process/CONVENTIONS.md](Documentation/Process/CONVENTIONS.md), under Releases.

## Unreleased

### Added

- Scripts: `Component.addTo(entity)` and `Component.removeFrom(entity)` beside `Component.of`, on
  every component a script reaches, in AngelScript and Luau.

- Editor: a scene page's Simulate shows what its scripts put on screen over the page's viewport,
  as a Game tab shows its game's.

### Changed

- Editor: each scene page's Simulate runs its scripts apart from other pages', with their own
  script context and clock.
- Editor: a Simulate's sound is heard while its page has the focus, as a Game tab's is: starting
  it, or clicking into its viewport, gives it the focus.

### Fixed

- Physics: a destroyed entity, or a removed rigid body, character or joint, no longer leaves its
  body in the physics world (an invisible collider) until the scene ends.
- Audio: a destroyed entity's sound, or a removed audio source's, stops at once instead of playing
  on.
- Editor: importing an asset no longer freezes the editor for seconds in a large project: each
  import read every asset in the project to check one file name.
- Scripts: assigning a struct-valued property, such as a light's colour, or a field of one
  (`light.color.r = 0.5`) now writes it. AngelScript lost both, Luau the field write.
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
