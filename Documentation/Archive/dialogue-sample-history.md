# The dialogue sample: Millbrook

> ARCHIVED 2026-11-27. Built 2026-11-21 to 24, S0 to S5 and save slots: Millbrook, with its README, credits and its Integration.Plugins check. Screenshots and a played Linux export are in weekly_backlog.md. Non-authoritative: the spec as approved, kept as the record of what was built and why; line numbers will have drifted.

> APPROVED 2026-11-21. The dialogue spec's D8 ([dialogue.md](dialogue-history.md)): a new sample project
> that shows the dialogue plugin and the sequencer used together, built through the MCP tools as
> the other samples are, cooked and exported. It stands for the sequencer spec's "small sequence
> in a sample game" (its Q5) too.

## The game

An evening in a small village on the day of its fair. The player arrives as the fair is being set
up and the villagers each have something not yet done. The player talks with them, helps with
three small tasks, and when all three are done the fair opens in a short cutscene: the square
lights up, the mayor speaks, and the night begins. It plays in about ten minutes, from one scene,
on the keys and mouse or a pad.

The village, and the sample, is **Millbrook** (`Data/SampleProjects/Millbrook`): a name that fits
the kit's watermill and windmill and sits apart from the other samples' names.

## What it shows

Each of the plugin's pieces, where a player meets it:

| Piece | Where |
|---|---|
| Conversations, choices | every villager; the answers change what they say and do |
| Flags | the tasks done, what was promised; a villager remembers a choice made with another |
| Provided flags | `fair_*`, set by the game's script (the tasks it tracks), read by conversations |
| `play` on a speaker | `emote-yes`, `emote-no`, `interact-right`, `pick-up`, `sit` from the cast's clips |
| `sound` at a speaker | a door, the mill's wheel, a cat |
| `fade` | before and after the ending |
| `sequence ... and wait` | the cart rolling into the square, its driver bound by name to a speaker |
| `jump` within a file and into another | the miller's talk goes on in the mill's own file |
| A declared command | `walk <who:speaker> to <where:entity> [and wait]` and `camera <target:entity>`, which the game's script carries out |
| A command that leaves, and resume | a short errand off the square (into the mill) and back, the conversation going on where it left off; and a save mid-conversation |
| `{Action}` in a line | "Press {Interact} to talk", in the player's control on the device in hand |
| The stock box, restyled | the sample's own sheet over it (`stockBoxSheet`) |
| A sequence carrying lines | the ending: camera cuts, the lanterns, a fade, and the mayor's speech on a Dialogue Lines track, holding for each line |
| The editor | the dialogue files' pages, Preview in the square, `dialogue_validate` in its tools |

## Assets

All CC0, from Kenney's packs (each `License.txt` kept in the sample's `Licenses/`, credited in its
`CREDITS.md` though CC0 asks for none):

- **Mini Characters**: the cast and the player, skinned, with the 32 clips they share (`idle`,
  `walk`, `sprint`, `sit`, `pick-up`, `emote-yes`, `emote-no`, `interact-right`, ...), and props
  (a cane, glasses, luggage).
- **Fantasy Town Kit**: the square: walls, roofs, the fountain, stalls, the watermill, the
  windmill, carts, banners, hedges, fences, roads.
- **Nature Kit** and **Mini Forest**: trees, rocks and ground around the square.
- Possibly **Cube Pets** (a cat to find), if the engine plays its part animation (its pets move
  their parts as nodes, not by a skin); otherwise one of Mini Characters' props stands in.
- Sounds and music are not in these packs: Kenney's audio packs (CC0) for the effects, and a
  licensed or CC0 piece for the fair's music, chosen with the user.

## The cast

Five villagers and the player, each a Mini Characters model: the **mayor**, the **baker** (her
stall has no sign), the **miller** (the cart wheel is missing), a **child** (her cat is lost) and
the **guard** at the gate, who knows where everything is if asked the right way. Their shown
names, moods and portraits (rendered from the models) are the cast's; their entities are named in
the square.

## Plan

1. **S0 The project**: created through MCP with the plugin enabled, the packs imported (the
   models the scene uses, not every one), the cast and the dialogue assets made, `CREDITS.md` and
   `Licenses/`.
2. **S1 The square**: the scene built from the kits (a level generator in `Tools/`, as the other
   samples' are), dusk lighting, the player walking with a camera that follows and a prompt over
   whoever can be talked to.
3. **S2 The conversations**: the five villagers' files, the tasks as flags, the declared commands
   carried out by the game's script, resume and a save.
4. **S3 The staging**: the cart's sequence, clips and sounds on the speakers, the errand off the
   square and back.
5. **S4 The ending**: the cutscene as a sequence with its lines on a Dialogue Lines track, the
   fades, music; a title and a credits screen; the box's sheet.
6. **S5 Checks and shipping**: Integration checks that it reads and cooks (as the other samples');
   README, screenshots; an export played on Linux by the user.

## Risks

- **Part-animated pets.** Cube Pets animate their nodes; whether a node-animated glb plays is to
  be checked before it is used.
- **Model scale.** The kits are made to different scales (Mini Characters are small next to the
  Fantasy Town Kit's doors); the level generator scales each kit once.
- **Sounds and music** need choosing; until then the staging's sounds are placeholders.
