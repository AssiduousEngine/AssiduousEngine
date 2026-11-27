# The C# sample: a first-person shooter

> ARCHIVED 2026-11-27. Built 2026-11-17 to 19, F0 to F5, on the dotnet branch and merged: Holdout. F6's README, pictures, Samples.json entry and Integration check, and two small controls (crouch, gamepad aim slowdown), are in weekly_backlog.md; Windows and the Deck wait for the Deck export of C#. Non-authoritative: the spec as approved, kept as the record of what was built and why; line numbers will have drifted.

> STATUS: APPROVED 2026-11-17 (user). Sized L, phases F0 to F6, on the `dotnet` branch. Read
> CONVENTIONS.md first.

## Goal

A sample game written in C#, the one that finishes the C# scripting work: it exports natively and
runs on Linux, Windows, the Steam Deck and in a browser. A first-person shooter fits because it
leans on what a game's code most often touches: a character controller, aiming and ray casts,
enemies that find their way, a weapon's timing and effects, a HUD, and input from keyboard and
mouse, a gamepad and a touch screen.

Working name: **Holdout** (open to change). A walled yard on the edge of town, held against waves of
enemies. Its own design, not modelled on any existing game.

## What it shows of C#

- A project of many files sharing types: the player, the weapons, the enemies and the wave
  director each in their own file, sharing helpers (damage, timers, tuning tables).
- Behaviours with inspector fields set per entity in the scene (a weapon's rate and spread, an
  enemy's speed and health), and the Game class holding the run (waves, score, game over).
- Components through `X.of(entity)`: CharacterComponent, NavAgentComponent, MeshComponent,
  LightComponent, AudioSourceComponent, the particle and animation facades.
- Scene queries (ray and sphere casts, overlaps), input actions, rumble, saved best score.
- The engine GUI for the HUD and menus, driven from C#.

## Assets

The art is Quaternius's **Toon Shooter Game Kit** (CC0, quaternius.com/packs/toonshootergamekit.html):
three characters (a soldier, a hazmat suit and an enemy), sixteen weapons, and 55 town and yard
pieces (containers, sandbag trenches, barriers, fences, crates, debris, exploding barrels, a health
pack, street lights, trees). glTF, imported through the MCP tools. Read from the download:

- Each character shares one 43-joint rig and the same 17 clips: Idle, Walk, Run, Jump (with
  Jump_Idle and Jump_Land), Duck, Punch, HitReact, Death, Idle_Shoot, Walk_Shoot, Run_Shoot,
  Run_Gun, Wave, Yes, No. So the enemies can shoot as well as punch.
- Each character carries all fourteen hand weapons as child nodes of its hand; a character shows
  the one it is armed with and hides the rest (MeshComponent.visible from C#).
- Materials are flat colours (no textures, one small fence texture apart), which keeps the
  download and the web build small: the glTF the game uses is 12 MB, half of it the characters.
- The pack has no licence file; the CC0 notice is the pack page's, kept in `Licenses/` with the
  page's address and the download date.
- The pack's Google Drive folder refuses scripted downloads (quota), so the user downloads it; the
  authoring scripts read its folder from an argument.
- Sounds: CC0 packs (Kenney's, or CC0 entries on Freesound), each credited.
- The sample's `CREDITS.md` and `Licenses/` carry the pack's CC0 notice and each sound's source, as
  the other samples do.

## The game

- **The yard:** one level, a walled lot of containers, sandbag trenches, barriers and debris, lit at
  dusk by street lights. Cover to move between and lines of sight to hold. Enemies come in through
  three gaps in the walls.
- **The player:** first person. Walk, sprint, jump, crouch behind cover. Health, no regeneration;
  health packs appear between waves.
- **Weapons:** a pistol to start; an SMG and a shotgun found on the yard, each with its own rate,
  spread, magazine and reload. Hitscan, with a tracer, a muzzle flash (a short point light),
  impact sparks and a kick. The weapon model is held at the bottom right of the view.
- **Enemies:** the kit's enemy character, in two kinds. Brawlers run a path to the player
  (NavAgentComponent) and punch; gunners keep their distance, stop to fire slow visible shots the
  player can dodge or take cover from (Idle_Shoot, Walk_Shoot), and need a line of sight first.
  Each flinches when hit (HitReact) and plays its death. Later waves bring more of both, faster and
  tougher. The hazmat character is a heavier enemy for the last waves.
- **Exploding barrels:** shot, they blow, hurting what is near (an overlap query) and leaving the
  spilled barrel.
- **Waves:** a short lull between waves (health packs, ammo), a wave counter and a score. After the
  last wave the yard is held; dying ends the run. Best score saved.
- **Menus:** title (play, controls, quit), pause, and the end screen, through the engine GUI.

## Controls

- Keyboard and mouse: WASD, mouse look with the pointer locked, left button fires, R reloads, 1 to 3
  pick a weapon, Space jumps, Shift sprints, C crouches, Escape pauses (and frees the pointer).
- Gamepad: left stick moves, right stick looks (with some aim slowdown over an enemy), right
  trigger fires, rumble on firing and on a hit.
- Touch: a stick on the left half, a drag on the right half to look, and buttons for fire, reload,
  jump and the weapon. For phones and the web.

## Engine work it needs

- **Pointer lock for games.** Only the editor's camera locks the pointer today
  (`InputSurface::SetPointerLock`, used by the editor viewports). A game needs it for mouse look:
  a script call (`Input.lockPointer(bool)` and `Input.pointerLocked()`), the player asking its
  window for it on the desktop and the browser's Pointer Lock API on the web (granted only on a
  click there, so the call takes effect on the next press), and freeing it when the game pauses or
  loses focus. The editor's Game tab honours it too. Every backend gets it, as script API always
  does; tests in Input.Tests and Integration.
- **Whatever else the build finds.** A gap met while making the game is fixed in the engine (and
  its MCP tool, where it is one), never worked around in the game, as the other samples did.

## Phases

- **F0: pointer lock** for games, across the player, the web and the editor's Game tab, with its
  tests.
- **F1: the project and the yard.** The sample project in `Data/SampleProjects/Holdout`, its C#
  project, the kit imported, the yard laid out by a scene script in `Tools/` (as the other samples
  do), its lighting and look.
- **F2: the player.** Movement and look on every input, one weapon firing, hits shown on test
  targets.
- **F3: the enemies.** The enemy character with its animations, navigation baked over the yard,
  chasing, attacking, damage and death.
- **F4: the game.** Waves, the three weapons, pickups, barrels, score and saved best, the HUD and
  menus, sounds.
- **F5: touch** controls, and the web export played on a phone.
- **F6: shipping.** Linux and web native exports run; README, credits, pictures; Integration checks
  that the sample reads and cooks. Windows and the Deck when the user can run them.

## Acceptance

The sample cooks through Integration; its Linux and web native exports run; the user plays it with
keyboard and mouse, a gamepad, and on a phone. Windows and the Deck are the user's checks when they
are free.

## Known risks

- The weapon held in view can pass into walls as the player stands against one. Kept short and close
  first; a separate pass drawing it over the world is engine work, only if it is needed.
- Enemy count on the web: animated characters each cost skinning. The waves are sized by a web run.
- The weapons a character carries come in as fourteen meshes on its hand. Hiding thirteen per
  enemy works, but it draws through the instanced path only if hidden meshes cost nothing; read on
  import, and if they do, the import drops the unused ones per prefab.
