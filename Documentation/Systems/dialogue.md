# Dialogue

Conversations and their staging, as an official engine plugin (`Code/Plugins/Dialogue/`, id
`dialogue`). The design and what is still to come are in [Archive/dialogue-history.md](../Archive/dialogue-history.md).

## The package

The plugin is one package of four parts, declared from its `Plugin.xml` (`util_add_plugin`) and
linked into every host the engine builds: the players and so the export templates, the editor,
and the command-line tools (`UTIL_HOST_PLUGINS`, `CMakeLists.txt`). Each host links the parts its
role loads; a project enables the plugin in its `plugins` list, and a host loads nothing of it
otherwise. It supports Linux64, Win64, Web and Android64.

| Part | Library | Module | Holds |
|---|---|---|---|
| (library) | `Dialogue.Foundation` | `dialogue.foundation` | `DialogueSettings`; the compiled conversations and the cast, their cooked forms and factories. Not a part: every part links it |
| Engine | `Dialogue.Engine` | `dialogue.engine` | the dialogue's domain; the runner, and the one each run holds; the `Dialogue` facade; the stock box; the sequences' line track |
| Pipeline | `Dialogue.Pipeline` | `dialogue.pipeline` | the format's parser, the commands' notation, the cook's checks; the dialogue's assets, their builders, the importer and File > New's creators |
| Tooling | `Dialogue.Tooling` | | its MCP tools, `dialogue_validate` and `dialogue_read` |
| Editor | `Dialogue.Editor` | `dialogue.editor` | the dialogue file's, the cast's and the dialogue assets' pages |

A web, Android or tools-less build declares the Engine part alone (`cmake/Plugins.cmake`): the
others link libraries such a build does not have.

## Settings

`DialogueSettings` is the plugin's settings type: the Engine part reads the project's entry into it
when it loads (`PluginContext::Settings`), the host holds it while the plugin is loaded, and Project
Settings edits it.

| Setting | Default | What it is |
|---|---|---|
| `cast` | none | the cast a new dialogue file names, a `DialogueCastAsset` (each file names its own) |
| `assets` | none | the dialogue assets a new dialogue file names, a `DialogueAssetListAsset` (each file names its own) |
| `commands` | none | the commands the game declares, each a form (`move <who:entity> to <target:entity> [and wait]`) |
| `providedFlags` | none | flags provided from outside the conversations, by the application's own code, which a conversation may read with no `set`: a name, or a prefix ending in `*` (`seen_*`) |
| `flagPrefix` | `flag.` | where flags live in the run's save store |
| `fadeSeconds` | 0.5 | how long a `fade` that names no length takes |
| `fadeColor` | black | what a `fade` that names no colour fades to |
| `stockBoxSheet` | none | a GUI style sheet that styles the stock box in place of the plugin's own |
| `stockBox` | off | the plugin's own dialogue box shows lines and choices |
| `typingSpeed` | 40 | characters a second the stock box types at; 0 shows a line whole |
| `advanceAction` | `Interact` | the input map action that advances a line in the stock box |
| `portraitOnRight` | off | the stock box's portrait side |

## The compiled conversations

A dialogue file cooks to a `DialogueSource`, which `DialogueFactory` loads into a
`DialogueResource`: the `cast` its speakers are from, its `conversations` and, apart from them, the texts of its lines and answers by
line id (`lines`, `Text(id)`), the seam a translation fills. A `DialogueConversation` is an `id`,
the `level` its entity names are checked against when it names one, and a flat list of
`DialogueStep`s with its choice and branch blocks compiled to jumps:

| Kind | What it does |
|---|---|
| `Line` | `speaker` says the line `lineId` in `mood`; waits to be advanced |
| `Choice` | offers the `count` `Answer` steps after it; waits for a choice |
| `Answer` | one answer, `lineId`; its statements start at `next` |
| `Skip` | goes on at `next` (an answer's end, an if's side's end) |
| `If` | goes on at `next` unless `flag` is set (clear, with `negate`) |
| `Set`, `Clear` | write `flag` |
| `Wait` | waits `seconds` |
| `Play` | `speaker` plays the clip `asset` (named `name`) once; `waits` holds until it ends |
| `Sound` | plays the sound `asset`, at `speaker`'s entity when named |
| `Fade` | fades in (`fadeIn`) or out over `seconds` to `color` (the settings' when unset) |
| `Sequence` | plays the sequence `asset`; `waits` holds until it ends |
| `Jump` | ends the conversation and plays `name`, in the dialogue file `asset` (nil: this one; `file` keeps the dialogue asset's name) |
| `Command` | a declared command, `name`, with its `arguments` converted to their kinds; waits or leaves as declared |
| `End` | the conversation's end |

Each step keeps its `sourceLine`, for messages.

## The cast

A cast cooks to a `DialogueCastSource`, which `DialogueCastFactory` loads into a
`DialogueCastResource`: a list of `DialogueSpeaker`s, each a reflected value (`id`, `shownName`, a
name `plate` and its `plateColor`, `moods` each with an optional `portrait`, a default `portrait`,
the `clips` a conversation may play by name, a `talk` clip, a `voice` sound and the `entity` that
plays the speaker), its assets referred to by guid. `Find(id)` and a speaker's `FindMood(name)`
look them up.

## Parsing

`ParseDialogue(text, commands)` reads a dialogue file into its conversations, the texts of their
lines and answers, and its problems, each at its line (`DialogueParse`; `Ok()` when none is an
error). It parses on after a problem, so one pass reports them all; a block left open is reported
at the line that opened it.

- One statement a line; blank lines and lines starting with `#` are nothing. Indentation means
  nothing: a `choice` or `if` block ends at its `end`.
- `conversation <id> [in <level>]` starts a conversation, its level a dialogue asset's name; a
  second of one id in the file is an error (another file may use it).
- `@ <speaker> [mood]: <text>` is a line, `choice` / `> <answer>` / `end` a choice, `if [not]
  <flag>` / `else` / `end` a branch. A line or an answer ending in `#line:<id>` takes that id;
  otherwise it is `<conversation>.<n>`, counting lines and answers from 1. An id used twice in a
  file is an error.
- `wait`, `set`, `clear`, `play`, `sound`, `fade`, `sequence` and `jump` are built in (`sound` and
  `sequence` name a dialogue asset, one word; `jump <conversation> [in <name>]` another dialogue
  file by its dialogue asset's name), each a form
  in the commands' notation (`play <speaker:speaker> <clip:text> [and wait]`), so they read their
  arguments the way declared commands do.
- Any other statement is matched against the declared commands, the first whose form reads it.

A form (`ParseCommandForm`) is literal words, `<name:kind>` arguments (`text`, `word`, `number`,
`a|b|c`, `entity`, `speaker`, `flag`, `asset(Type)` (a dialogue asset's name), `list(name)`, an
optional part's taking a `=default`) and `[optional parts]`, ending in `waits` or `leaves` for a
command that always waits or leaves. An optional `[and wait]` lets the author choose. Its name is
its leading literal words; one that starts like a built-in statement is refused. An argument takes
the fewest words that let the rest of the statement match, so it runs to the next literal word, and
the last runs to the end; an optional part is taken when it can be. A "quoted name" is one word and
never a literal, for a name holding one of the form's words. Each matched argument is kept as
written, a number also as a number; what the parser cannot know (the cast, the assets, the level,
the flags set elsewhere) is the cook's to check.

## The cook's checks

`CheckDialogue(parse, inputs)` adds to a parsed file what the parser cannot know, each problem at
its line, and resolves the assets its steps name. Its `DialogueCheckInputs` are the cast, the
dialogue assets (`DialogueAssetEntry`: a name and an asset's guid), the declared commands, the
provided flags, the file's own asset (`self`), the project's other dialogue files (their
conversations, `DialogueFileConversations`, and the flags they set; `CollectDialogueFacts` gathers
them from a parse) and the project's assets by guid (`IDialogueAssets`: an asset's name and type).
Conversation and line ids are a file's own: a script plays a conversation through its file's asset,
so two files may use the same ids. A conversation never names an asset by path:
a clip by its name among the speaker's clips, anything else by its name in the dialogue assets.

- **Errors:** a speaker not in the cast (a line's, a `play`'s, a `sound`'s, a `speaker` argument's),
  or a mood not the speaker's, each naming what there is; a cast the settings do not name, once; a
  `play` clip not among the speaker's clips by name; a `sound`, a `sequence` or an `asset(Type)`
  argument naming no dialogue asset, one that refers to no asset, or one of another type (a `sound`
  takes an `AudioClipAsset` or a `SoundCueAsset`, a `sequence` a `SequenceAsset`); dialogue assets
  the settings do not name, once; a `jump` to a conversation its file lacks (this file's, or with
  `in <name>` the file the dialogue asset `name` holds, which must be a `DialogueAsset`); a flag an
  `if` reads that no file sets and the settings do not provide.
- **Actions:** each `{Action}` in a line's or an answer's text must be an action of one of the
  project's input maps (`{{` is a brace); a project with no input map is told once.
- **The level:** a conversation that names its level (`conversation square in village`, the level a
  dialogue asset of type `SceneDocument`) has its entity names checked against that scene's, loaded
  into a scratch scene with its prefabs placed: the speakers' `entity` and every `entity` argument.
  A missing one is a warning, since an entity spawned at play is not in the scene.
- **Warnings:** a statement nothing reaches (after a `jump`, say); an entity the level lacks.
- **Resolved:** a `play`'s clip, a `sound`'s and a `sequence`'s asset, a `jump ... in`'s dialogue
  file, and an `asset(Type)` argument's, into the step's or the argument's `asset`.

## The assets and the cook

| Asset | What it is | Cooks to |
|---|---|---|
| `DialogueAsset` | a dialogue file: its text, a `.dialogue` file in the project's sources (imported, or made by File > New's Dialogue, which names the settings' default cast and dialogue assets), and the `cast` and `assets` it names, which several files may share | `DialogueSource`, by `DialogueBuilder` |
| `DialogueCastAsset` | the cast, its `speakers` edited in the inspector (File > New's Dialogue Cast) | `DialogueCastSource`, by `DialogueCastBuilder` |
| `DialogueAssetListAsset` | the dialogue assets, its `entries` (a name and an asset each) edited in the inspector (File > New's Dialogue Assets) | nothing: read when dialogue files cook |

`DialogueBuilder` reads the file's text, gathers its project (`DialogueProject::Gather`: the
settings' commands parsed, the cast and the dialogue assets the file names, the input maps'
actions, and what the project's other dialogue files hold), parses and checks it (`CookDialogue`),
logs every problem as `<file>:<line>: <message>`, and writes the compiled conversations; a file with
an error does not cook. It declares the cast, the dialogue assets, the input maps and every other
dialogue file as read, so a change to any of them recooks it, and folds the settings it reads into
its `EnvironmentHash`, so a change to them recooks every dialogue file. `DialogueCastBuilder` writes
the speakers and declares what they refer to (portraits, clips, sounds). The `.dialogue` importer
copies the file into the sources and links it.

## The MCP tools

The Tooling part adds two tools through `McpPluginServices`, which every MCP host serves while the
plugin is loaded (the engine's tool count and `McpGuide.md` do not list them):

- `dialogue_validate` runs the cook's checks over one dialogue file (`asset`, its guid) or every one
  in the open project, without cooking: each file's problems with their lines and severities, and
  whether it would cook.
- `dialogue_read` gives one file's conversations as the cook reads them: each one's lines (id,
  source line, speaker, mood, text), choices and their answers, commands with their arguments,
  jumps, and the flags it reads and writes; and the file's problems.

Writing stays the text's: `asset_data_write` or the file itself, then `dialogue_validate`. Both
check through `CheckDialogueText`, which gathers the file's project as the cook does
(`DialogueProject::Gather`, reading the other files' sources through a `DialogueSourceReader`).

## The editor

**A dialogue file** opens in `DialogueTextPage` (`DialoguePage.xml`), on the editor's code page
(`app::CodeAssetPage`): its text in the code editor, coloured as dialogue
(`Data/Editor/Code/dialogue.xml`: the statements' words, a line's speaker and text, an answer, a
comment), and checked as the cook checks it a moment after each edit (`CheckDialogueText`, the other
files read from the sources), each problem in the list and on its line. Beside it, the outline of
its conversations, which moves the cursor to the one chosen, and the file's own slots, its cast and
its dialogue assets. Save writes the text and the asset and requests a cook; the page checks
against the plugin's settings, which the Editor part reads as it loads.

**Preview** plays the chosen conversation (the first, when none is) as the text now reads, in the
first open scene page's Simulate, started when it is not running and stopped with the preview. The
page holds a runner of its own (`DialogueRunService`) over the scene page's run, stepped with the
page, its host playing the page's compiled text in place of the cooked file
(`DialogueRunHost::SetDialogue`), staging in that scene (`SetScene`) and showing the stock box
(`ShowStockBox`). A Simulate's run has no save, so the host keeps the flags and the resume record in
a store of its own, which goes with the preview. Simulate's pointer serves the scene, so the page's
pane says what the conversation waits on and takes the advance, the answers and a command's done.
Text with errors does not play.

The Editor part adds two more pages on one base, `DialogueListPage`, a page over an asset that is a list
of items (on the editor's page over a reflected asset, `app::AssetObjectPage`), built from markup in
`Data/Editor/Dialogue/`. The items are a list beside the grid, by name; the list's bar adds an item
and removes the chosen one, and its search keeps the items whose name holds the text. The grid shows
the chosen item's fields from reflection; the asset's own rows (its source file among them) are not
shown, since the items are the asset.

- **The cast's** (`DialogueCastPage`, `CastPage.xml`): the speakers, by id (and shown name). A new
  speaker is `speaker` (`speaker_2`, ...) with a `neutral` mood. Its portraits (`Texture`), clips
  and talk clip (`AnimationClip`) and voice (`AudioClip`) are asset slots that offer only assets of
  their kind (`referenceType`); its plate colour is a colour.
- **The dialogue assets'** (`DialogueAssetsPage`, `AssetsPage.xml`): the entries, by name, each an
  asset slot that takes any asset. It has no Cook button: the list has no cooked form.

Every edit, an add and a removal included, is one undo step, and Save writes the asset and requests
a cook. A dialogue file's own `cast` and `assets` are slots that offer only a cast and a dialogue
assets list. The part adds the pages where the editor offers its context and its page services,
keeps their factories' keys, and removes them as it unloads (`EditorContext::RemovePageFactory`),
which closes their open pages.

## The runner

`DialogueRunner` plays one conversation at a time. `Play(dialogue, conversation)` starts the
conversation of a dialogue file, by the file's guid (its first conversation when none is named),
and refuses while one is playing or when there is no such conversation. It walks the steps from the
first and stops at one that waits:

| Waits on | Step | Goes on with |
|---|---|---|
| `Line` | a line | `Advance()` |
| `Choice` | a choice | `Choose(i)`, at that answer's statements |
| `Time` | `wait` | its seconds, counted by `Update` |
| `Staging` | `play` or `sequence` with `and wait`, and every `fade` | the staging done |
| `Command` | a declared command that waits | `Done()` |

`Stop()` ends it at once. A command that leaves ends it with its place kept, and `Resume()` goes on
after it. Flags are booleans under the settings' `flagPrefix` (`Flag`, `SetFlag`); an `if` reads
them, `set` and `clear` write them. A `jump` goes on in its own file or, when the cook resolved one,
in another.

What it needs from the game goes through `IDialogueHost`: loading dialogue files and casts, the
save store (flags and the resume record), what an input action is bound to, the staging, and four
notices for the game:

| Notice | Event | Holds |
|---|---|---|
| `DialogueLineNotice` | `DialogueLine` | the file, the conversation, the speaker's id and shown name, the mood (the speaker's first when unnamed), the portrait (the mood's, else the speaker's), the text with each `{Action}` filled with what it is bound to (`{{` and `}}` are braces), the line id |
| `DialogueChoiceNotice` | `DialogueChoice` | the answers' texts and line ids |
| `DialogueCommandNotice` | `DialogueCommand` | the command's name, its arguments (`Find(name)`), whether the conversation waits for `Done()` or has left |
| `DialogueEndedNotice` | `DialogueEnded` | the file and the conversation, and whether it was stopped or left |

**Resume** anchors on a line, so a save survives a patch to the conversation. The record
(`dialogue.resume` in the save store) holds the dialogue file, the conversation, the step to go on
at, the last line shown and a hash of the conversation's steps (`HashSteps`). It is written as each
line shows and when a command leaves, so a game that saves mid conversation saves its place, and
cleared when the conversation ends. `Resume()` goes on at the step when the hash matches, else at
the line if the conversation still has it, else from the start (logged).

**In a run**, the plugin contributes a runner to every run (`DialogueRunContribution`, a run service
contribution): a `DialogueRunService`, `run.Get<DialogueRunService>()`, stepped with the run's frame
(`IRunService::OnRunUpdate`). Its host, `DialogueRunHost`, loads files and casts through the run's
resources, keeps flags and the record in the store the game gives it (`Dialogue.useSave`, `DialogueRunHost::UseSave`; in scratch for the run until it is given one, and once that store is gone), reads an action's binding from
the run's input on the device in hand, and publishes the notices on the run's bus.

**Staging** is in the run's current scene (`RunBinding::currentScene`). A speaker plays as an
entity: the one the game bound (`DialogueRunService::Bind`, the facade's `bind`), else the level
entity the cast names, else none (logged once a speaker).

| Step | What the run does | Waits |
|---|---|---|
| `play` | the clip once over what the entity animates, the animation's one-shot | with `and wait`, until it blends out |
| `sound` | the clip or cue through the run's audio, at the speaker's entity when named | never |
| `fade` | a claim on the scene's fade (`PostProcessSystem::FadeClaims`), over the sequences' fades; a fade out holds at full until a fade in lets it go; the length and colour default to the settings' | always, for its length |
| `sequence` | a play in the scene's `SequenceSystem`, each binding named for a speaker filled with that speaker's entity, and one named for no speaker with the level entity of its name (a prop: a cart, a door) | with `and wait`, until it ends |

While a line shows, its speaker plays the cast's `talk` clip, looping, until the line is advanced
or the conversation ends there (`IDialogueHost::OnLineEnded`).

## The stock box

With `stockBox` on, the run's host shows what the runner tells the game in the plugin's own box as
well (`DialogueStockBox`), so a game has dialogue on screen before it builds a box of its own. It is
a screen on the run's screen stack (`GuiSubsystem::ScreensFor`) that blocks nothing under it, built
from the plugin's markup (`DialogueStockBoxMarkup`): a portrait (the notice's), the speaker's shown
name (none when the cast's `plate` is off), the line typed out at `typingSpeed` characters a second,
and a choice's answers as buttons, the first focused, so the pad, the keys and the mouse all reach
them. The advance action (`advanceAction`) or a press on the box first finishes the typing, then
advances; a press on an answer chooses it, and so does the advance action on the focused one. The box goes when the conversation ends (or the line a
sequence showed). Its look is a sheet over the box alone: the plugin's own
(`DialogueStockBoxSheet`), or the project's `stockBoxSheet` in its place. The classes a sheet
styles are `dialogue-screen`, `dialogue-box` (with `portrait-right` when `portraitOnRight` is on),
`dialogue-portrait`, `dialogue-words`, `dialogue-name`, `dialogue-text`, `dialogue-answers` and
`dialogue-answer`. The host reaches the GUI as an application service (the GUI subsystem offers
itself as one).

## Lines in a sequence

`DialogueLineTrack` is a sequence track kind (Dialogue Lines in the sequence panel's Track menu),
for a linear cutscene staged on a timeline. It names a `cast`, and each of its sections
(`DialogueLineSection`) is a line: a `speaker` of that cast, a `mood`, and its `text`, or the line
`lineId` of a dialogue file (`dialogue`). When the playhead enters a section the track's runner
shows the line through the run's runner (`DialogueRunner::ShowLine`), which the game hears as it
hears a conversation's line (no file, no conversation); the runner's hold (`HoldTime`, `IsHolding`)
stops the play at the section's end until the line is advanced (`Advance()`, `Dialogue.advance()`),
and `DialogueEnded` follows with no conversation. A line is refused, logged, while a conversation
plays, and holds nothing then; a conversation that starts takes over from a line still shown. The
editor's posing shows no lines. The track is bound to no one and runs after the other kinds.

The track registers with the plugin's types on each load; the engine registers the track kinds'
base itself, and a plugin's load registers nothing of the engine's, since unloading reverses what
the load registered.

## The script surface

`DialogueFacade` (scripts call it `Dialogue`) reaches the calling script's run's runner
(`script::CurrentService<DialogueRunService>()`): `play(dialogue)`, `play(dialogue, conversation)`,
`isPlaying`, `advance`, `choose`, `done`, `stop`, `resume`, `flag`, `setFlag` and `bind(speaker,
entity)`. The four notices are reflected value types; a choice's answers are read through
`answerCount`, `answer(i)` and `answerLineId(i)`, and a command's arguments by name through `has`,
`text`, `number`, `asset` and `entity` (the entity found by name in the run's scene, which the run's
host puts on the notice). `RegisterDialogueScriptFacade` is the domain's `registerScriptFacade`, so
it runs on each load; the Engine part's unload takes back the script names and roots it added
(`RemoveDialogueScriptFacade`). The surface for game authors is in
[Shipping/Scripting.md](../Shipping/Scripting.md), under Dialogue.

## Loading

The Engine part's load registers the settings type and the plugin's types
(`RegisterDialogueTypes`), reads the settings, and, where the host offers the engine's service
(`EnginePluginServices`), adds the dialogue's domain, whose resource module holds the two factories,
and the run service contribution that gives every run its runner.
All of it is recorded and taken out when the plugin unloads.

Tests: `Dialogue.Foundation.Tests` (the settings' defaults, their round trip through the project's
entry, their labels; the compiled conversations and the cast reading back as written, their lookups
and the speaker's reflection), `Dialogue.Pipeline.Tests` (the commands' notation and what it
refuses; the format parsed: lines, choices and branches compiled to jumps, the built-in statements,
declared commands with names of several words, line ids, and every problem at its line; the cook's
checks over a table of a project's assets; the runner playing parsed conversations against a
stand-in host: lines, choices, branches, flags, waits, staging, commands that wait or leave, jumps,
resume after a load and after a patch, a second play refused, a host that answers at once, a line
shown on its own) and `Integration.Plugins` (the package linked with its
four parts, its manifest, a project that enables it loading it with its settings, its domain and a
runner in every run, its flags in the run's save, `Dialogue` offered to scripts and taken back on
unload; a run's runner fading the scene, playing a sequence bound to the cast's entity or to one
the game bound, and showing a line on the run's bus; a sequence's line track showing its lines
through the run's runner and holding until each is advanced; the stock box on a real GUI typing a
line, finishing and advancing on a press, taking an answer and going at the end; an AngelScript and
a Luau behavior playing a
cooked conversation through `Dialogue` and answering its line, choice and command; a dialogue
file's page listing its conversations and its problems, and its preview playing the text with flags
of its own;
and, through the MCP host, a dialogue file cooking to its conversations, one with an error failing
the cook, File > New's three creators, and the two tools; and, in an editor session, the cast and the dialogue
assets opening in their pages, a speaker added, undone, found and removed, and the pages leaving
with the plugin).