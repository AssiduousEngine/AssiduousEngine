# Changelog

What changed in AssiduousEngine between releases, newest first. A change that someone using the
engine, the editor or a game made with it would notice gets a line under **Unreleased** in the
commit that makes it; a release turns Unreleased into its version and date. The steps are in
[Documentation/Process/CONVENTIONS.md](Documentation/Process/CONVENTIONS.md), under Releases.

## Unreleased

### Added

- Export: `Tools.Export --content-only` stages the game's content, settings and shader pack with no
  player and no template, and `--platform <name>` exports a preset for another platform.
- Runtime: an app the platform takes out of view (a phone's home button, another app in front)
  stops updating, drawing and sounding, writes what each run's game saved, and lets go of its
  window's surface, which a phone takes away; it comes back where it was. Applications and
  subsystems hear it through `OnEnterBackground` and `OnEnterForeground`.
- Input: a Back key (a phone's back button, a keyboard's or a browser's Back) that a game can bind;
  in a menu it backs out as Escape does.

### Changed

- Projects: a new project starts with 4x MSAA (it was off), and the sample games have it on.
- Downloads: the editor's archive is named `Assiduous-Editor-<version>-<platform>` (it was
  `Editor-...`), so a download says what it is.
- Animation: the root motion mode that moves nothing and holds the travel for the game to apply
  is called `Manual` (it was `Script`), since game code reads it as well as a script; a script
  names it `RootMotionMode::Manual`. Saved scenes read unchanged.

### Fixed

- Rendering (Vulkan): the screen UI waits for the scene's last pass before drawing over it, where on
  a phone's GPU it could drop out of a frame and flicker. A render target reused every frame waits
  for the frame before it to finish with it, where before the two could overlap, which on a phone's
  GPU can lose the device (the picture froze while the game ran on).
- Touch: the on-screen controls appear after the first tap however quick it is, where a tap that
  landed and lifted within one slow frame went unseen.
- Editor: a project opened before it was cooked (a downloaded sample) shows its scenes' and
  prefabs' thumbnails whole: they wait for the cook to make what they show, where before they were
  drawn with those meshes missing and kept so, even after the cook. A thumbnail is drawn again
  when a cook remakes what it shows. Opening such a project no longer logs a warning for each
  reference still waiting for the cook; the page says once how many it waits for.

## 0.1.0-beta.2 - 2026-11-10

### Added

- Scenes: a native component field naming a scene (a level's exit) is a `SceneRef`: the inspector
  picks a scene for it, an agent sets it by guid, and a pruned export keeps the scene it names.
  Naming a scene never loads it.

- Editor: a scene, prefab or asset changed, added or deleted in the project's `Content/` outside
  the editor (a git pull, a text editor, another tool) is picked up: an open page reloads, or warns
  when it has unsaved edits, and a page whose file was deleted stays open and writes it back when
  saved; before, the editor kept the old copy and a later save overwrote the change.

- Scripts: `scene.animation.playOneShot` plays a clip once over whatever an entity is animating
  (its clip or its graph), blending in and out, after which it carries on with its own animation;
  `stopOneShot` and `isPlayingOneShot`, and `onAnimationFinished(clip)` on the entity's behaviours
  when it ends. `clipLength(clip)` gives a clip's length, and `setClip(entity, clip, blendSeconds)`
  crossfades instead of snapping.

- Scripts: `Input.describe(action)` names the control an action takes on the device in hand
  ("Space", or "Pad South" once a pad is picked up), as a GUI prompt shows it, for a sentence that
  tells the player what to press.

- Scripts: `Component.addTo(entity)` and `Component.removeFrom(entity)` beside `Component.of`, on
  every component a script reaches, in AngelScript and Luau.

- Editor: a scene page's Simulate shows what its scripts put on screen over the page's viewport,
  as a Game tab shows its game's.

### Changed

- Scripts: what acts on a whole scene is a property of the scene: `scene.physics`,
  `scene.animation`, `scene.audio`, `scene.render`, `scene.particles`, `scene.splines`,
  `scene.scripts`, `scene.prefabs`, `scene.debugDraw`, `scene.environment` and
  `scene.postProcess`, in place of `ScenePhysics::of(scene)` and the rest, which are gone. A
  script written against `of` changes `ScenePhysics::of(scene)` to `scene.physics` (Luau
  `ScenePhysics.of(scene)` to `scene.physics`), and so on for each.
- Samples: Lamplight's, Snowline's and Sky Hopper's hints and controls lists name the control on
  the device in hand (and follow a rebinding) instead of an action's name or a fixed key and pad
  button.
- Editor: each scene page's Simulate runs its scripts apart from other pages', with their own
  script context and clock.
- Editor: a Simulate's sound is heard while its page has the focus, as a Game tab's is: starting
  it, or clicking into its viewport, gives it the focus.

### Fixed

- Prefabs: spawning a prefab no longer reads it from disk each time, and a prefab saved in the
  editor reaches the next spawn of a running game. A level that places a prefab many times reads
  it once, and places it as a spawn would.
- Export: a pruned export (a preset's `pruneToReachable`) keeps the assets a script property names:
  a behaviour's or a Level's asset value (a prefab it spawns, a clip it plays). Before, an asset
  named only there was left out and the exported game could not find it. The same for an asset
  named only by one placed prefab's override (a mesh swapped, a script property set on that
  placement).
- Player: a game run from its project folder spawns prefabs (a script's spawn, a replicated
  spawn) and loads scenes from a script (`run.loadScene`, `run.loadSceneAsync`); both read the
  cooked database, which holds no scene or prefab there, and found nothing.
- GUI: a tween of a property code set on a node (a script's `node.opacity = 0`, then
  `node.tween("opacity", "1", ...)`) moves it; it did nothing before, as the property was code's
  and the style the tween went through left it alone. A game's fade of a panel it made did not show.
- Scenes: entities added to one scene in two places (two branches, two people) no longer get the
  same id, which a merge would have put on two entities in one file: each scene's ids now come from
  a generator seeded from the system's entropy, as assets' already did.
- MCP: `scene_write` and `prefab_write` given a `name` already taken in its group refuse instead of
  writing over the asset that holds it (a GUI document's record once became a prefab's this way).
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
