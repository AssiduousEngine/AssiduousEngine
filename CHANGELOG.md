# Changelog

What changed in AssiduousEngine between releases, newest first. A change that someone using the
engine, the editor or a game made with it would notice gets a line under **Unreleased** in the
commit that makes it; a release turns Unreleased into its version and date. The steps are in
[Documentation/Process/CONVENTIONS.md](Documentation/Process/CONVENTIONS.md), under Releases.

## Unreleased

### Added

- Rendering: quality levels. A Render Quality asset is one level a game offers: its label, the
  scene pass's MSAA count and antialiasing, a tier (Off to High) for shadows, AO, SSR, SSGI and
  bloom, a cap on the shadow reach and the environment's resolution. The project lists its levels
  and the one it starts at, and an export preset picks its own, so a phone's export can ask less
  than a desktop's. The player starts at its preset's level and play-in-editor at the project's; an
  Off tier turns its effect off whatever a scene asks, and no tier turns one on. High is each
  effect's full cost, as before the levels; Medium and Low cost less: fewer AO samples, SSR steps,
  SSGI rays and bloom levels, smaller shadow maps, and at Low a smaller environment.
- Editor: quality levels. A Render Quality asset's page draws its preview at the level as it is
  edited. The Game tab has a quality dropdown (the preset's level, or any of the project's), a
  scene viewport's Shading chip a Quality Level menu, and the Export dialog a Quality level row
  for the preset's own level. MCP: `pie_quality` reads and sets a Game tab's level.
- Scripting: the `RenderQuality` facade, an options menu's graphics setting. `levels()` lists the
  project's quality levels by label, `current()` reads the one the game plays at, and `set(label)`
  switches at once. The player keeps the choice in the user's settings and starts the next run at
  it; in the Game tab it picks the level in the quality dropdown.
- MCP: `asset_rename` renames an asset within its group as the Assets browser does, keeping its
  guid; the default scene's path follows.
- Player: `--quality <label>` starts a run at one of the game's quality levels, ahead of the saved
  choice, without keeping it.
- Samples: each sample game offers Low, Medium and High quality levels; High is how it drew
  before the levels. Their Steam Deck and Web exports start at Medium, and Sky Hopper's and
  PaperKid's settings choose the graphics level.

### Changed

- Rendering: the MSAA count and the antialiasing are the quality level's. The project's MSAA
  setting and a scene's or post profile's antialiasing mode are gone; a project saved before opens
  with one level, Default, at its MSAA count. The antialiasing is not carried over: an older scene
  or profile that ran TAA or FXAA says so in the log when it loads, and its level's `antialiasing`
  takes it. A scene's post settings keep how TAA and FXAA look.
- Building: Clang builds on Linux link with LLD, which they now require (see
  Documentation/BUILDING.md), and Clang's module interfaces are reduced
  (`-fmodules-reduced-bmi`). A clean Debug build of the engine and editor compiles in about a third
  of the time, and links about five times as fast.

### Fixed

- Content: an asset's files are its own exactly. Deleting, renaming, moving or cloning `Rock`
  took the files of a `Rock.2` beside it (the name the editor gives a second `Rock`), and an export
  that pruned or stripped `Rock` dropped or packed `Rock.2`'s as well. A data stream's name may no
  longer contain a dot.
- Files: every save replaces its file whole or not at all. Assets, scenes, settings, sources and
  exports were written over in place, so a crash, a power loss or a full disk mid-save could leave
  a file cut short; each write now goes to a temporary beside it, flushed to the disk, then renamed
  over. The editor's MCP token file is readable by its user alone (it was readable by every account
  on the machine).
- Files: an XML file that is not well-formed, or could not be read whole, fails to load and says
  why. It was read as empty, so whatever loaded it got its defaults and could save them over it.
- Files: a game's save, the editor's settings, a project's export presets and the model viewer's
  library that do not read are set aside as `<file>.unreadable` before anything is written, and the
  log says where. They were replaced by an empty store or the defaults, which the next save wrote
  over the file.
- Editor: names and paths read from files stay where they belong. Removing an export template
  deletes the folder whose `template.xml` names it, never a path built from that id, and a template
  whose id is a path is not imported. A script, GUI document or shader made with the name of one in
  another group takes its own source file instead of writing over the first one's, and
  `asset_create` refuses a name the New dialog refuses. A glTF's sidecar whose uri is absolute or
  leaves the model's folder is not copied.
- Editor: Discard Changes goes back to the page's last save. It undid every step since the page
  opened, past a save, and marked the page saved; during Simulate it froze the editor. It stops
  Simulate first now, and a page whose history no longer reaches the save reads its file again.
  Undoing back to the save marks the page saved.
- Editor: Save during Simulate writes the scene as it was when Simulate started. It wrote what the
  run had done to it.
- Editor: terrain and vegetation brush strokes are saved with the page they were made on, by
  Save, Save All and the save when closing or exiting; only Save wrote them, and wrote every
  page's. Discard, or closing the page without saving, drops them and shows the terrain as last
  saved. A sculpt after a hole cut no longer loses the holes on save.

## 0.1.0-beta.3 - 2026-11-12

### Added

- Rendering: the player draws its frame turned for a display that presents turned (a phone held
  away from its natural orientation), so the display has nothing to turn (pre-rotation); its scene,
  its screens and a screenshot follow the turn, and the pointer is unchanged. A window asks for it
  (`RenderWindowDesc::preRotate`); the RHI samples and the editor do not.
- Editor: Preferences' templates folder has a Browse button, which picks the folder with the
  system's folder dialog.
- Export: `Tools.Export --content-only` stages the game's content, settings and shader pack with no
  player and no template, and `--platform <name>` exports a preset for another platform.
- Runtime: an app the platform takes out of view (a phone's home button, another app in front)
  stops updating, drawing and sounding, writes what each run's game saved, and lets go of its
  window's surface, which a phone takes away; it comes back where it was. Applications and
  subsystems hear it through `OnEnterBackground` and `OnEnterForeground`.
- Input: a Back key (a phone's back button, a keyboard's or a browser's Back) that a game can bind;
  in a menu it backs out as Escape does. A Back press nothing in the game handles (no action binds
  it, no menu or screen uses it) goes to the platform: a phone puts the app in the background.
- Rendering: the game keeps inside the window's safe area, what a phone's status bar, navigation
  bar and camera cutout leave of it: its frame, its screens and its pointer are fitted there.

### Changed

- Samples: Lamplight and Masquerade smooth their edges with FXAA in place of TAA.
- Native code: the Dear ImGui plugin draws as a screen overlay of the render subsystem, over the
  game's screens, in the shared overlay pass (blending encoded colours as the screens do), and
  turned with a pre-rotated window. An application with a render subsystem no longer calls
  `ImguiSubsystem::Render`; one without (a plain `IApplication`) still does.
- Native code: the SDL3 shell is `Shell.SDL3` (module `foundation.shell.sdl3`, target
  `Foundation::Shell.SDL3`); it was `Shell.Desktop`, but it is Android's shell too.
- GUI: a command every handler passes on (`Propagate`: not mine) is unhandled: `runCommand`
  returns false for it, and its key goes on as an unbound key does, where before any handler on
  the path took the key. A screen stack passes on a Back it pops nothing with.
- Projects: a new project starts with 4x MSAA (it was off), and the sample games have it on.
- Downloads: the editor's archive is named `Assiduous-Editor-<version>-<platform>` (it was
  `Editor-...`), so a download says what it is.
- Animation: the root motion mode that moves nothing and holds the travel for the game to apply
  is called `Manual` (it was `Script`), since game code reads it as well as a script; a script
  names it `RootMotionMode::Manual`. Saved scenes read unchanged.
- Editor: an export preset's extra files are the project's: Add Files picks from the project's own
  files (not what the editor, a cook or an export writes) and names each relative to the project, so
  the preset travels with it; From Disk takes a file of the project's through the system's dialog and
  refuses one outside it.

### Fixed

- Editor: play-in-editor draws the game at the project's scene-pass MSAA, as the player does; it
  drew at 1x.
- Rendering: a game with MSAA on keeps its scenes' post-processing in the player (auto exposure,
  AO, SSR and TAA, from the scene or its profile); the project's MSAA setting replaced them with the
  engine's defaults, so since the samples turned MSAA on the player drew them darker and plainer
  than the editor.
- Rendering: a scene's sky lighting (IBL) and its reflection probes bake once, not again in each of
  the scene's first 20 frames; on a phone those frames ran at about 4 fps, the first seconds of
  every game.
- Rendering: a Vulkan surface that offers RGBA formats and no BGRA ones (an Android phone's) gets an
  sRGB swapchain and its UNORM view; it got a UNORM one, which drew every game too dark.
- Rendering: the MSAA resolve and TAA retire a bind group they replace until the frames in flight
  that may use it have finished; they freed it at once, which Vulkan's validation reports as a
  descriptor set freed while in use (`vkFreeDescriptorSets`), at the start of a scene with MSAA.
- Networking: the reliable UDP transport resends a reliable message whose first packet was lost
  (that packet was acked by mistake), lets a client connect again from the same address (after a
  restart or a disconnect, its messages were dropped as duplicates), keeps sequenced messages in
  order (they were sent as plain unreliable ones), and finds a connection without searching all of
  them. The wire format changes: an older build does not talk to this one.
- Editor: the Export dialog shows the selected preset's settings again; since the property grid
  measured its rows, the column beside the presets had shrunk to nothing.
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
