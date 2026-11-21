# Dialogue as an official plugin

> APPROVED 2026-11-21. D0 and D1 2026-11-21. Read on master at `247608c79`; line numbers will drift. Revisits the
> choice, made on 2026-11-06 for the first game built on the engine, to keep its dialogue in its
> own script ("A now, with plans to move to C"), now that the sequencer exists, and makes dialogue
> an official engine plugin (that note's option B).

## Goal

Conversations and the cutscenes around them, for any game on the engine: lines with a speaker and
a mood, choices, the story's flags and the branches they pick, and the staging between lines,
written as text, checked by the cook, read and previewed in the editor, validated by agents
through MCP, and played from script. The staging is the sequencer's: a dialogue plays sequences,
and a sequence can hold for a line. Everything a particular game means by a step (a fight, a
reward, a camera that follows its own rules) is a command the game declares and handles. It ships
as an official plugin (`Code/Plugins/Dialogue/`), compiled into the export templates and dormant
until a project enables it, so a game using it stays scripts and data on every platform.

## Why a plugin, and why now

The first game's note of 2026-11-06 weighed three homes: the game's script (A), an official
engine plugin (B), the game's own native plugin (C). It chose A now and C later, B only if
another game wanted dialogue. Two things have changed since:

- **The staging is the engine's now.** Half of what the note asks of a conversation (a camera move,
  a clip, a sound, a fade, a wait, a character walking somewhere) is what a sequence does. A runner
  in script would drive the sequencer from outside and pass control back and forth; a plugin plays
  sequences from the inside and lets a sequence hold for a line, through the sequencer's own
  extension point (a track kind a plugin registers, edited in the panel with no list to extend).
- **C's cost has not gone away; B avoids it.** A game with native code ships a player built for
  it on every platform (the plugins spec, Decision 6). An official plugin is compiled into the
  templates, so a game using it keeps the stock templates and its export stays data.

B's cost stands: an engine API to keep stable, both compilers through the full lanes, docs, and
a port for the Beef mirror. The design below keeps the surface small: the format, a runner, a
facade, a track kind, and a declared list of the game's commands.

## What the first game has today (read 2026-11-21)

- **The format**: one statement per line. `scene <id>`; `@ <speaker> [mood]: <text>` with
  `{Action}` in the text naming a control (`Input.describe`); `choice` / `> <answer>` / `end`;
  `set` / `clear` / `if [not] <flag>` / `else` / `end`; staging (`camera <target> [for s]`,
  `play <who> <clip> [and wait]`, `move <who> to <target> [running] [and wait]`, `sound`,
  `fade in|out`, `wait <s>`, `shake <how> [for s]`, `cue <event>`); and hand-offs to the game's
  own systems. A dozen scenes are written in it.
- **The cast**: one speaker a row, `id | shown name | portrait colour | moods | clip group`.
- **The tool**: a script that parses the scenes, checks every speaker, mood and flag, and writes
  them out as script data.
- **The runner**: a game script that plays a scene's statements, waits on presses, moves and
  clips, keeps flags in the save store (`flag.<name>`), and resumes a scene after a load
  (`dialogue.resume`). It talks to the rest of the game through a small host interface, and the box
  is the game's GUI screen.
- **The seams it kept for a move** (that note, "Decided"): the format parsed in one place, the
  generated table data only, the runner behind a small interface. The plugin keeps all three.

## What the reference engines and the middleware do

None of the engines cloned here (Lumina, Flax, Traktor, Godot, ez, Lumix, Zero, Hyperion) has a
dialogue system; a search for one found only file and message dialogs. The prior art is middleware,
read from its published documentation, not its code:

| | Yarn Spinner | Ink | Dialogic (Godot) | Unreal Conversation plugin |
|---|---|---|---|---|
| Authoring | text (`.yarn` nodes), an editor extension | text (`.ink` knots, stitches), Inky | a timeline editor of events | graph assets |
| Branching | options, `<<if>>`, jumps between nodes | choices, diverts, gathers, weave | choice and condition events | graph edges |
| State | variables (bool, number, string), a variable storage the game may replace | variables, read counts, the whole story state saved as JSON | variables in a singleton | the game's |
| Game hooks | commands `<<name args>>` the game registers, functions for expressions | external functions bound by name, tags on lines | signals, custom events | tasks the game implements |
| Lines | line ids (`#line:`) for localization, line providers | tags; string tables by convention | per-event text, translation CSV | a `FText` per node |
| Presentation | the game's line and option views | the game's | the plugin's styles, replaceable | the game's |

What they agree on, and what this spec takes: dialogue is text an author writes and rewrites; the
game's own steps are commands it registers by name; presentation belongs to the game, with a stock
view to start from; state is a small set of variables the game can see and save; lines carry
stable ids so another language can be added later without touching the script.

## The design

### Layers

| Part | Library | Holds |
|---|---|---|
| Engine | `Code/Plugins/Dialogue/Dialogue.Engine` (`plugin.dialogue`) | the dialogue resource, the cast (a reflected resource), the runner (a per-run service), the `Dialogue` script facade, the line track kind for sequences |
| Pipeline | `Dialogue.Pipeline` | the importer for `.dialogue` text, the cook's checks, File > New creators (a dialogue, a cast) |
| Tooling | `Dialogue.Tooling` | `dialogue_validate`, `dialogue_read` |
| Editor | `Dialogue.Editor` | the dialogue page: the text in the code editor, the cook's checks as its problems, the conversations' outline, a preview |

The parts follow the plugins spec's Decision 2 (an asset type in Pipeline, never in Editor) and
load through `PluginContext` (Decision 4). The plugin is the engine's second official plugin
after Imgui and the first with all four parts; NativeSample already proves the four in a game's
package.

### The format

The first game's format, unchanged where it is general, with its game-specific statements becoming
declared commands. Its scenes read as they are written today but for their first word (`scene`
becomes `conversation`, below); what changes is which statements the plugin knows and which the
project declares. The grammar below is its tool's (read 2026-11-21), written down. The cast
leaves the text format and becomes a reflected resource (below).

#### Why text for conversations, and the engine's formats for the rest

- **Conversations are text** because a conversation is prose a writer rewrites, with staging between
  the lines. In XML or the serialization format every line would be a quoted string in a record, its
  speaker and mood fields, a choice nested lists: harder to write, harder to read, and a diff that
  shows structure instead of the sentence. Yarn Spinner and Ink chose their own text for the same
  reason, and the first game's scenes are in it already. The cooked `DialogueAsset` is an
  ordinary serialized resource; only the source the writer edits has its own grammar.
- **The cast is a reflected resource** because it is records, not prose. Reflected, it gets the
  inspector with typed asset pickers, references that follow a moved asset, data versions and
  migration, and the generic MCP data tools, all from code the engine has.
- **The plugin's settings**, command declarations among them, are serialized as every project
  setting is. Only a command's form is a short string in the notation below, because it reads as
  the statement it matches.

#### Files

- **`.dialogue`**: UTF-8 text holding any number of conversations. The source stays in the project,
  as a script does; the cook writes a `DialogueAsset` from it.
- **One statement a line.** Leading and trailing spaces are dropped. A blank line is nothing, and
  a line whose first character is `#` is a comment. A `#` later in a line is text: comments are
  whole lines only, so a line may say "#1 in the valley".
- **Indentation is for the reader.** The first game's tool strips every line; `choice` and `if`
  blocks are closed by `end`, never by indentation. The first game indents its blocks by convention
  and the page's editor indents them for you, but a block's extent is its `end`.

#### Conversations

The first game calls them scenes; the plugin calls them conversations, so the word never
meets the engine's scenes (a conversation names its level, which is a scene).

```
conversation <id> [in <level>]
```

- `<id>` is a word (letters, digits, `_`), unique across the project, not only its file: `play`,
  `jump` and `Dialogue.play(id)` name a conversation without its file. The first game's tool already
  refuses a second conversation of a name across all files.
- `in <level>` is new and optional: a dialogue asset's name (below), of type `SceneDocument`. When
  a conversation names its level, the cook checks every entity argument (a declared command's
  `entity` kind, a speaker's `entity`) against that level's entities, a missing one a warning since
  an entity spawned at play is not in the scene; without it those names are checked at play and
  logged once when missing, as the first game's runner does today.
- A conversation runs from its first statement to the next `conversation` or the end of the file.
  Reaching its end, or a `jump`, ends it.

#### Statements

| Statement | Form | Waits | What it does |
|---|---|---|---|
| line | `@ <speaker> [mood]: <text> [#line:<id>]` | for `advance()` | shows a line; the speaker plays their talk clip while it shows |
| choice | `choice`, then `> <answer> [#line:<id>]` each followed by its statements, then `end` | for `choose(i)` | offers the answers; the chosen one's statements run, then the statement after `end` |
| branch | `if [not] <flag>`, statements, optional `else` and statements, `end` | no | runs one side on a flag |
| flags | `set <flag>`, `clear <flag>` | no | writes a flag |
| wait | `wait <seconds>` | the time | pauses the conversation |
| play | `play <speaker> <clip> [and wait]` | with `and wait` | plays a clip of the speaker's once over what they are doing (the animation's one-shot), blending back out |
| sound | `sound <name> [at <speaker>]` | no | plays the dialogue asset `name`, a sound or a cue, flat or at the speaker's entity |
| fade | `fade in\|out [for <seconds>] [<colour>]` | the fade | the sequencer's fade claim; the length and colour default to the settings' |
| sequence | `sequence <name> [and wait]` | with `and wait` | plays the dialogue asset `name`, a sequence, its parts bound by name to the cast's entities; `and wait` holds until it ends |
| jump | `jump <conversation>` | no | ends this conversation and plays that one |
| command | anything else | as declared | a command the project declares (below) |

`@` lines, `choice`, `if`, `set`, `clear`, `wait` and `play` are the first game's as written. Its
`fade in|out` is kept and gains a length and colour. Its `sound <cue>` (declared, unused by any of
its conversations) becomes a dialogue asset by name. `sequence` and `jump` are new. Its `camera`,
`move`, `shake`, `cue` and its hand-offs become its declared commands.

Blocks nest: an answer may hold a `choice` or an `if`, and an `if` side may hold a `choice`. Each
answer's statements run to the next `>` at its level or the choice's `end`.

#### Names with spaces

Entity, clip and character names have spaces ("Old Well 01", "Wave Hello", "Grey Cat"), so an
argument is not one word. The rule is the first game's tool's, made general: **an argument runs to
the next literal word of its statement's form, and the last one runs to the end of the line**, after
a trailing optional part (`and wait`, `running`, `for <seconds>`) is taken off. So `move Grey Cat to
Player running and wait` reads as who `Grey Cat`, target `Player`, running, waits; `camera Old Well
01 for 2` as target `Old Well 01`, for 2 seconds. A name that contains one of its form's literal
words (an entity called "Path to Mill" in a `move`) is written in quotes: `move Player to "Path to
Mill"`.

#### Text in a line

- **`{Action}`** names an action of the project's input map (`{Interact}`, `{Move}`) and shows the
  control it takes on the device in hand (`Input.describe`), so a line never names a key the
  player may not be holding. The cook checks the name against the input map. `{{` and `}}` are
  literal braces.
- **Nothing else is markup** in the first version: a character's thoughts in parentheses are
  plain text. Emphasis and colour wait for the GUI's text runs to be wanted in a line.
- **`#line:<id>`** at the very end of a line or an answer gives it an explicit id (below). It is the
  only place a `#` after the start of a line means anything.

#### Declared commands

The project declares each command as a **form** in the plugin's settings, in the notation this
spec uses: literal words, `<name:kind>` arguments, `[optional parts]`, and how it waits. A
game's might be:

```
camera <target:entity> [for <seconds:number=2>]
move <who:entity> to <target:entity> [running] [and wait]
shake <how:gentle|strong|violent> [for <seconds:number=1>]
cue <event:text>
fight <foe:list(foes)>                                          leaves
give item <item:list(items)> [<count:number=1>]
give coins <purse:copper|silver|gold> <count:number>
```

- **Kinds:** `text` (as written), `word`, `number`, `a|b|c` (one of these words), `entity` (a level
  entity by name; checked when the conversation names its level), `speaker` (a cast id), `flag`,
  `asset(<Type>)` (a dialogue asset of that type, by its name, cooked to its guid), `list(<name>)`
  (one of a named list the settings hold, written there or read from a data asset's keys: a game's
  item list). `=<value>` gives a default.
- **Waiting:** a command does not wait unless its form says so. `[and wait]` lets the author choose;
  `waits` always holds; either holds until the game calls `Dialogue.done()`. `leaves` ends the
  conversation with its place saved, for a step that takes the player somewhere else (a fight): the
  game calls `Dialogue.resume()` when it comes back, as the first game's runner does today.
- **Matching:** a statement takes the first form whose literal words match, so `give item` and
  `give coins` are two commands. Two forms that could match the same statement are an error in
  the settings.
- **What the game gets:** `DialogueCommand` carries the command's name (its literal words joined:
  `give coins`) and its arguments by name, converted to their kinds: `cmd.text("who")`,
  `cmd.number("seconds")`, `cmd.has("running")`, `cmd.asset("clip")`. The game never reparses a
  line.

#### Flags

A flag is a word. The cook checks that every flag an `if` reads is set by some `set` or `clear` in
the project, or is one the settings list as provided from outside the conversations
(`providedFlags`: names, or a prefix such as `seen_`, which the application's scripts set). That
replaces the first game's tool's hard-coded exception for such a prefix. Flags live in the run's
save store under `flag.<name>`.

#### The cast

`DialogueCast`, a reflected resource (`REFLECT_MEMBERS`, edited in the inspector, read by the
MCP data tools). Each dialogue file names its cast (user 2026-11-21), so a project may have several
(one a chapter, say) and files may share one; the plugin's settings give the cast a new file takes.
It holds a list of `DialogueSpeaker` values:

| Field | Type | What it is |
|---|---|---|
| `id` | string, a word | what `@` lines, `play` and the `speaker` kind name |
| `shownName` | string | the name the box shows; empty shows none (a narrator) |
| `plateColor` | colour, optional | the name plate's tint; unset shows no plate |
| `moods` | list of `DialogueMood` | each a `name` and an optional `portrait` (texture); the first is the default mood |
| `portrait` | texture, optional | the portrait for a mood that has none of its own; unset falls back to the plate colour |
| `clips` | list of animation clips | what `play <speaker> <clip>` may name, by the clip asset's name |
| `talk` | animation clip, optional | played while the speaker's line shows |
| `voice` | sound, optional | played as the text types |
| `entity` | string | the level entity that plays the speaker, by name; else the game's `onDialogueResolve` |

- **Clips are references, not a group path.** The first game's cast names an asset group
  and `play` looks a clip up in it by name at import. A list of clip references is checked by the
  inspector as it is picked and follows a renamed or moved clip; `play` still names a clip by its
  asset name, so the conversation text does not change. A speaker who shares another's whole group
  gets the same clips picked; whether the inspector
  should offer "add every clip in a folder" is a question for D2.
- **The page:** the editor's generic asset page (`GenericAssetEditorPage`, `Editor.Generic`)
  builds its form from `Serialize` and shows a guid as a reference that takes any asset. If it
  cannot yet show a reflected resource's fields with their types (texture, clip, sound), D2 either
  teaches it reflected fields, which every reflected asset gains, or the plugin's Editor part gives
  the cast a page of its own; D2 reads that page first and chooses.
- **The first game's cast** is converted once at D7, through the MCP data tools, and its own
  tool retired.

#### Dialogue assets

A conversation never names an asset by its path, which a rename or a move would break. A clip is
named among its speaker's clips in the cast; every other asset a conversation names (a `sound`'s, a
`sequence`'s, a command's `asset(<Type>)` argument) is an entry in the project's **dialogue
assets**: a `DialogueAssetListAsset` the dialogue file names (as it names its cast; several files
may share one, and the settings give a new file's), edited in the inspector, each entry a name (one
word: `door_creak`) and a reference to the asset, which holds its guid. The cook finds the entry by
name and checks the asset's type fits (a `sound` takes an audio clip or a cue, a `sequence` a
sequence); the compiled step holds the guid. The list is read only when conversations cook, so it
has no cooked form of its own.

#### Line ids

Every line and every answer has an id. Without `#line:` it is positional, `<conversation>.<n>`
counting lines and answers from 1, and it changes when a line is inserted above it; positional ids
are used for nothing that outlives a build. When a project starts translating, the page's **Tag
lines** command writes an explicit `#line:<id>` onto each untagged line, as Yarn's tagger does;
explicit ids are unique across the project and never change after that. The cooked asset carries a
string table keyed by id: the seam a translation fills.

#### What the cook writes

A `DialogueAsset` holds its conversations, each a flat list of steps with its blocks compiled to
jumps (as the first game's tool compiles them today): an operation, its arguments resolved (assets
as guids, names as text, numbers as numbers), a jump target where it branches, the line id where it
has text, and the source line for messages. The texts go in the asset's string table, not in the
steps. The cast needs no cooking of its own: it is a reflected resource, serialized as one.

#### The cook's checks

Every problem is reported at its file and line, and an asset with an error does not cook (the
editor and `dialogue_validate` show the same list):

- **Errors:** a line that is no statement; an `@` line without `:`; `>` outside a choice; `else` or
  `end` with nothing open; a choice with no answers; a block left open at the conversation's end; a
  conversation id used twice; a speaker not in the cast, or a mood not theirs; `play` for a speaker
  without a clip group, or a clip the group lacks; a sound, sequence or conversation that does not
  exist; an `{Action}` not in the input map; a flag read that nothing sets or declares; a statement
  that matches no declared form (the message names the nearest form); an argument of the wrong kind;
  a `#line:` id used twice.
- **Warnings:** statements after a `jump` that nothing reaches; a speaker with neither a portrait
  for a mood nor a default portrait (the plate colour is used); a flag set that nothing reads; an
  entity missing from the conversation's named level.
- **The cast's own checks**, shown on its page and by `dialogue_validate`: a speaker id that is not
  a word or is used twice; a speaker with no moods; two moods of one speaker with one name; a
  `talk` clip not in the speaker's `clips`.

### The runner

`DialogueRunner` is a per-run service (the plugins spec: "per-run state goes on the runs
themselves"), so flags and the conversation in progress belong to the game's run, as the first
game's do.

- **Plays one conversation at a time**: `play(id)` starts it; statements run until one waits (a
  line, a choice, a waited clip, sequence or command, a `wait`). A line waits for `advance()`, a
  choice for `choose(i)`.
- **Flags** are booleans in the run's save store under a prefix the settings give (`flag.` by
  default, the first game's), read by `if`, written by `set` / `clear` and by script
  (`Dialogue.setFlag`). Numbers and strings are left out until a conversation needs one.
- **Resume after a load**: the first game saves the conversation (`dialogue.resume`) and a step
  index (`dialogue.pc`), and only when a hand-off leaves the conversation. A step index points
  somewhere else once the asset changes between the save and the load (a patch). The plugin saves
  under `dialogue.resume` the conversation, the id of the last line shown, the steps since it, and a
  hash of the conversation's steps; on a match it resumes at the step, on a mismatch at that line if
  the conversation still has it, else at the conversation's start (logged). It saves when a `leaves`
  command runs and whenever the game saves mid-conversation.
- **Speakers to entities**: the cast's `entity`, else the game's `onDialogueResolve` handler,
  else none (logged once). The speaker of a line plays its talk clip while the line shows , when the
  cast names one.
- **What it tells the game**, on the run's event bus as the sequencer's notices are (a behavior's
  `on<Name>` hears them): `DialogueLine` (speaker id, shown name, mood, portrait, text with
  `{Action}` filled, line id), `DialogueChoice` (the answers' texts), `DialogueCommand` (name and
  arguments), `DialogueEnded` (conversation id). The game's own box shows them; the plugin's stock
  box (below) does when the project turns it on.

### Dialogue in a sequence

A linear cutscene (no choices) is often easier to stage on a timeline than as text. The plugin
registers a **line track kind** (`DialogueLineTrack`, `RegisterSequenceTrack<T>()`): sections, each
a line (a cast id, a mood, the text or a line id from a dialogue asset). When the playhead enters
a section the runner shows the line; at the section's end the play holds until the line is
advanced. That needs one addition to the sequencer, kept general: **a track's runner may hold the
play** (`SequenceTrackContext` gains the play's hold and release, so a runner holds as a marker
does and releases on its own), which the sound kind could also use later.

The two directions then meet: a conversation plays sequences for its staging
(`sequence gate_open and wait`), and a sequence carries lines for a linear cutscene. Choices stay
in conversations; a sequence does not branch.

### The stock box

A GUI document in the plugin's data (a portrait, a name, the typed text, the answers; the pad and
the mouse both drive it) that a project turns on in the plugin's settings and restyles with its
own sheet, so a game starts with dialogue on screen and replaces the box when it wants its own.
Typing speed, the advance control (an input map action, `Interact` by default) and the portrait
side are settings.

### The editor and agents

- **The dialogue page** opens a `.dialogue` asset in the code editor (the GUI.Code editor the script
  pages use), its problems the cook's checks as you type, an outline of its conversations, and
  **Preview**: plays the chosen conversation in the scene page in front, through the runner with the
  stock box, flags in a scratch store that the preview drops. The cast opens in the inspector (The
  cast, above).
- **MCP**: `dialogue_validate` (the cook's checks over one asset or all, each problem with its line)
  and `dialogue_read` (the conversations, their lines with ids, choices, commands and the flags each
  reads and writes). Writing stays the text's: `asset_data_write` or the file itself, then validate.
  `McpGuide.md` and `kEngineToolCount` do not change (plugin tools register through
  `McpPluginServices`, counted by the plugins' own tests).

### Script surface

`Dialogue` (a facade, as `Audio` is): `play(conversationId)`, `isPlaying()`, `advance()`,
`choose(index)`, `done()` (a waiting command finished), `stop()`, `flag(name)`,
`setFlag(name, on)`, `resume()` (after a load). Handlers on the run's bus: `onDialogueLine`,
`onDialogueChoice`, `onDialogueCommand`, `onDialogueEnded`, `onDialogueResolve`. Both languages,
tested in `Code/Integration` as the sequencer's script surface is.

## Decided by this spec (for review)

1. **B, an official plugin**, over the first game's planned C (above).
2. **The first game's format is kept**; its game-specific statements become declared commands, and
   `camera`, `move`, `shake` with them (each game means something different by them). `play`,
   `sound`, `fade`, `wait` and `sequence` are built in because the engine has one obvious way to
   do each.
3. **Text is the source**, as Yarn and Ink; no graph editor. The page is a text editor with checks
   and a preview.
4. **Flags only** (booleans) in the first version, as the first game uses; numbers and strings when
   a conversation needs them.
5. **Dialogue branches, sequences do not.** A sequence may carry lines (the line track) and hold
   for them; choices belong to conversations.
6. **Line ids now, translation later.** Positional ids until a project tags its lines; explicit
   `#line:` ids after.
7. **Commands are declared as forms** (literal words, typed arguments, optional parts, how they
   wait) in the plugin's settings, and reach the game with their arguments parsed and typed.
8. **The cast is a reflected resource**, edited in the inspector; conversations stay text. The
   first game's cast is converted once.
9. **Resume anchors on a line**, not a step index, so a save survives a patch to the conversation.
10. **Conversation, not scene**, for the unit of dialogue, so it never reads as the engine's scenes.
11. **Assets by name, never by path** (user 2026-11-21): a clip through the cast, everything else
    through the dialogue assets, each holding the guid, so a renamed or moved asset is still found.
12. **A dialogue file names its cast and its dialogue assets** (user 2026-11-21), both shareable;
    the settings give a new file's. A conversation names neither, and a cast does not name an asset
    list: speakers and staging are kept apart.

## Order of work

Each step a commit with its tests, green on clang and gcc.

1. **D0 The sequencer's runner hold**: a track's runner holds and releases the play
   (`SequenceTrackContext`), with tests in `Sequence.Tests`.
   *As built:* the runner is asked, not handed the play: `HoldTime(after, upTo)` names where it
   holds and `IsHolding()` whether the hold stands, polled at the hold and each step after, so the
   context gained nothing. A hold already let go when reached is passed in the same step. `Resume`,
   `Seek` and `Stop` let go of a runner's hold as of a marker's; only a marker's is reported to the
   listener. A nested sequence's holds do not hold the outer play, as before for markers.
2. **D1 The plugin's skeleton**: `Code/Plugins/Dialogue/` with its manifest and four empty parts
   loading in every host, its settings type (`commands`, flag prefix, stock box), in the static
   registry and the templates' list. *As built:* the plugin is the first in `UTIL_HOST_PLUGINS`, so
   the players, the editor and the tools link it. `DialogueSettings` holds `commands`,
   `providedFlags` (a prefix written `seen_*`), `flagPrefix`, `stockBox`, `typingSpeed`,
   `advanceAction` and `portraitOnRight`; the cast setting comes with the cast in D2, and the fade
   defaults with the runner in D3. A tools-less desktop build now declares only a plugin's Engine
   part, as a web or Android one does. The system is described in
   `Documentation/Systems/dialogue.md`.
3. **D2 The format and the cook**: the parser (a port of the first game's tool's), the `.dialogue`
   asset and the reflected `DialogueCast` with its page, the importer, the checks (speakers, moods,
   flags set before read, commands against the settings, clips and sounds and sequences by name),
   File > New. Tests over the first game's scenes copied in as fixtures, their `scene` keyword
   renamed.
4. **D3 The runner and the facade**: the per-run runner, flags in the save store, resume,
   the events, `Dialogue` in both languages, `sequence` and the built-in staging. Integration
   tests in both languages.
5. **D4 The line track**: `DialogueLineTrack` in a sequence, holding for the line; offered by the
   sequence panel's Track menu with no change there.
6. **D5 The stock box**: the GUI document, its settings, a sample of it in use.
7. **D6 The editor and agents**: the dialogue page (code editor, checks, outline, preview), and
   `dialogue_validate` / `dialogue_read`.
8. **D7 Proof, with the user**: the first game enables the plugin, its scenes become
   conversation assets and its cast a `DialogueCast`, its host interface becomes command handlers,
   and one of its cutscenes is rebuilt with its staging as sequences (the sequencer spec's Q5,
   folded in here). Its own tool and generated script data are retired.
9. **D8 A sample** (user 2026-11-21): a new sample project that showcases the two, built through the
   MCP tools as the other samples are, whose conversations play with their staging as sequences and
   whose cutscenes carry lines: the dialogue and the sequencer used together and played from script,
   cooked and exported. Integration checks that it reads and cooks, as it does for the other
   samples. It stands for the sequencer spec's "small sequence in a sample game" (Q5) too.

## Risks

- **An engine API for one game's needs.** The commands keep the first game's specifics out; the
  review of D2's format and D3's facade is where the surface is fixed.
- **The format's grammar.** A forgotten `end` closes nothing until the conversation ends, and the
  error then points at the conversation's end, far from the mistake. The parser reports the open
  block's own line with the error, and the page's editor flags indentation that disagrees with the
  blocks as a warning, so the reader's indentation and the parser's `end`s cannot drift apart
  unseen.
- **Names with spaces.** An argument running to the next literal word is the first game's rule and
  reads well, but a name holding a literal word needs quotes; the cook's message for a statement
  that matches no form says so when quoting would make it match.
- **Two runners at once.** A conversation that plays a sequence that holds for a line of another
  conversation: one runner, one conversation at a time; a line track's line goes through the same
  runner and refuses while a conversation is playing (logged).
- **The port.** Sedulous mirrors the engine; a plugin with four parts is a sizeable port. Its
  notes go out per phase.
- **Web.** The plugin must be in the web template (static) for a web export to use it; D1 adds
  it to the templates' list on every platform.
