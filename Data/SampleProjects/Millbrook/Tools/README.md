# Millbrook's tools

The scripts that build Millbrook through the editor's MCP tools. They talk to a running editor with
the project open and its MCP server on (`Tools.Editor Data/SampleProjects/Millbrook --mcp-port
7405`; `mcp.py` reads the token the editor writes, and `MCP_PORT` picks another port). Each
script's own header says what it makes, what it needs first and how to call it. Generated files land
in `generated/` (gitignored); the imported copies live in the project's `Sources/`.

## In order

1. **Models**: `kenney.py <packs-folder>` copies the models Millbrook uses out of the downloaded
   Kenney packs (Mini Characters, Fantasy Town Kit, Nature Kit; each zip unpacked into a folder of
   its own name) into `generated/`, repairing the Nature Kit's scenes, and each pack's licence into
   `../Licenses`. `importmodels.py` imports them: the characters into `Models/Characters`, the
   village's pieces into `Models/Town` and what grows round it into `Models/Nature`, a group per
   model.
2. **Project setup**: `controls.py` (the input map, made the project's default) and `look.py` (the
   dusk environment and post profile).
3. **The village**: `village.py` writes the scene: the square, the houses, the mill and the
   windmill, the stream, the fence and the gate, the trees, the villagers and the player, the
   places the conversations name (`SignSpot`, `WheelSpot`, `MillerAtWheel`, `WindmillSpot`), the
   cart, the lanterns' lights and the ending's camera. It is the project's default scene.
4. **Sounds and sequences**: `sounds.py` generates the sounds and the fair's tune into `Audio/`;
   `sequences.py` writes `CartRolls` (the cart into the square) and `FairOpens` (the ending) through
   `sequence_edit`.
5. **Scripts and screens**: `importscripts.py` imports the screens (`ui/*.xml`, styled by
   `ui/MillbrookSheet.css`, the project's GUI sheet) and `scripts/*.as`, filling each
   `{{Type:Name}}` with that asset's id, and makes `Millbrook.as` (the game: the title, talking,
   the commands, the pause menu, the ending, the credits, saves) the startup script. The rest: the player
   (`Player`) and the quarter-turn camera (`CameraRig`).
6. **The conversations**: `dialogue.py` imports `dialogue/*.dialogue` (Square, Bakery, Mill), writes
   the cast and the dialogue assets, imports the stock box's sheet (`box/MillbrookBox.css`), sets
   the plugin's settings (the game's commands and flags, the stock box) and checks every file with
   `dialogue_validate`.

A change to a conversation is an edit to its file under `dialogue/` and a re-run of `dialogue.py`;
a change to a script, of `importscripts.py <Name>`.

`scenegen.py` (the scene document writer) and `mcp.py` (the MCP client) are the helpers the others
share.
