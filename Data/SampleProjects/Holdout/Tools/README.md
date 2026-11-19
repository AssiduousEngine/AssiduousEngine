# Holdout's tools

The scripts that build Holdout through the engine's MCP tools. By default each starts a headless
`Tools.Mcp` (from the engine checkout's `Bin/Debug/Linux64-Clang`, or `$TOOLS_MCP`) and opens the
project in it; with `HOLDOUT_MCP=http` they talk to an editor that has the project open with its MCP
server on instead (`Tools.Editor Data/SampleProjects/Holdout --mcp-port 7405`), so the build can be
watched. Each script's header says what it makes and what it needs first. Generated files land in
`generated/` (gitignored); the imported copies live in the project's `Sources/`, its code in `Code/`.

## In order

1. `setup.py <kit folder>`: the project (when it does not exist), its C# project
   (`Code/Holdout.csproj`) and the models from Quaternius's Toon Shooter Game Kit (CC0), which
   is downloaded by hand from its page (its Google Drive refuses scripts). The characters go to
   `Models/Characters`, the weapons to `Models/Guns` and the yard's pieces to `Models/Yard`.
2. `fonts.py`: the UI font, Russo One (`fonts/`, SIL Open Font License), imported as a distance-field
   font and made the project's default UI font.
3. `controls.py`: the input map (Controls), made the project's default: move, look (the mouse or
   the right stick), fire, jump, sprint, reload, the weapons and pause.
4. `sounds.py --kenney <folder>`: the sounds (`Audio`). The three gunshots are made here; the rest
   are from Kenney's Impact Sounds, Interface Sounds and Sci-Fi Sounds (CC0), downloaded from
   kenney.nl and unzipped into the folder given. Before `importscripts.py`, whose scripts name them.
5. `importscripts.py`: the screens (`ui/*.xml` over `ui/HoldoutSheet.css`, each checked with
   `gui_validate`, the sheet made the project's GUI sheet) and the C# (`scripts/*.cs`) imported and
   cooked, `Game.cs` made the startup script. A `{{GUI:Hud}}` (or Scene, Prefab, Audio, Clip) in a
   script is filled with that asset's id first. The C#:
   - `Game.cs`: the title, the waves, the HUD, pause and the end of a run.
   - `Player.cs`: first-person look and movement on the character body, and its health.
   - `Weapon.cs` and `Arsenal.cs`: the three weapons, hitscan from the eye, reloads and the muzzle's flash.
   - `Enemy.cs` and `Projectile.cs`: the brawlers, the gunners and their slow rounds.
   - `Pickup.cs`: a weapon or a health pack that comes back after it is taken.
   - `Barrel.cs`: an exploding barrel.
   - `Target.cs`: a practice crate that flashes when shot.
   - `Combat.cs` (what can be hurt, and the registry the weapons and blasts find it in), `Aim.cs`
     (angles and directions) and `Sounds.cs` (the sounds and playing them), shared by the rest.
6. `enemies.py`: the enemy prefabs (Enemy, Gunner, Hazmat) and the gunners' round (Shot).
7. `yard.py`: the Yard, the one level, made the project's default scene, with the dusk look of
   `look.py`: the player at the south end, loose crates to practise on, the exploding barrels, the
   pickups, and the navigation baked over it. It sizes each piece's collider from
   `kit_sizes.json`, the kit's measured bounds (written by `setup.py` from the kit's files).

`scenegen.py` (the scene document writer) and `mcp.py` (the MCP client) are the helpers the others
share.
