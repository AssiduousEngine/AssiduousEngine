# Dialogue

Conversations and their staging, as an official engine plugin (`Code/Plugins/Dialogue/`, id
`dialogue`). The design and what is still to come are in [Specs/dialogue.md](../Specs/dialogue.md).

## The package

The plugin is one package of four parts, declared from its `Plugin.xml` (`util_add_plugin`) and
linked into every host the engine builds: the players and so the export templates, the editor,
and the command-line tools (`UTIL_HOST_PLUGINS`, `CMakeLists.txt`). Each host links the parts its
role loads; a project enables the plugin in its `plugins` list, and a host loads nothing of it
otherwise. It supports Linux64, Win64, Web and Android64.

| Part | Library | Module | Holds |
|---|---|---|---|
| Engine | `Dialogue.Engine` | `dialogue.engine` | `DialogueSettings`; the compiled conversations and the cast, their cooked forms and factories; the dialogue's domain |
| Pipeline | `Dialogue.Pipeline` | `dialogue.pipeline` | the format's parser, the commands' notation, the cook's checks; the dialogue's assets, their builders, the importer and File > New's creators |
| Tooling | `Dialogue.Tooling` | | its entry |
| Editor | `Dialogue.Editor` | | its entry |

A web, Android or tools-less build declares the Engine part alone (`cmake/Plugins.cmake`): the
others link libraries such a build does not have.

## Settings

`DialogueSettings` is the plugin's settings type: the Engine part reads the project's entry into it
when it loads (`PluginContext::Settings`), the host holds it while the plugin is loaded, and Project
Settings edits it.

| Setting | Default | What it is |
|---|---|---|
| `cast` | none | the project's cast, a `DialogueCastAsset`: who its conversations' lines may name |
| `assets` | none | the project's dialogue assets, a `DialogueAssetListAsset`: the sounds, sequences and other assets its conversations name, each by a name of its own |
| `commands` | none | the commands the game declares, each a form (`move <who:entity> to <target:entity> [and wait]`) |
| `providedFlags` | none | flags provided from outside the conversations, by the application's own code, which a conversation may read with no `set`: a name, or a prefix ending in `*` (`seen_*`) |
| `flagPrefix` | `flag.` | where flags live in the run's save store |
| `stockBox` | off | the plugin's own dialogue box shows lines and choices |
| `typingSpeed` | 40 | characters a second the stock box types at; 0 shows a line whole |
| `advanceAction` | `Interact` | the input map action that advances a line in the stock box |
| `portraitOnRight` | off | the stock box's portrait side |

## The compiled conversations

A dialogue file cooks to a `DialogueSource`, which `DialogueFactory` loads into a
`DialogueResource`: its `conversations` and, apart from them, the texts of its lines and answers by
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
| `Jump` | ends the conversation and plays `name` |
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
- `conversation <id> [in <level>]` starts a conversation; a second of one id is an error.
- `@ <speaker> [mood]: <text>` is a line, `choice` / `> <answer>` / `end` a choice, `if [not]
  <flag>` / `else` / `end` a branch. A line or an answer ending in `#line:<id>` takes that id;
  otherwise it is `<conversation>.<n>`, counting lines and answers from 1. An id used twice in a
  file is an error.
- `wait`, `set`, `clear`, `play`, `sound`, `fade`, `sequence` and `jump` are built in (`sound` and
  `sequence` name a dialogue asset, one word), each a form
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
provided flags, the project's other dialogue files (their conversation ids, their explicit line ids
and the flags they set; `CollectDialogueFacts` gathers them from a parse) and the project's assets
by guid (`IDialogueAssets`: an asset's name and type). A conversation never names an asset by path:
a clip by its name among the speaker's clips, anything else by its name in the dialogue assets.

- **Errors:** a speaker not in the cast (a line's, a `play`'s, a `sound`'s, a `speaker` argument's),
  or a mood not the speaker's, each naming what there is; a cast the settings do not name, once; a
  `play` clip not among the speaker's clips by name; a `sound`, a `sequence` or an `asset(Type)`
  argument naming no dialogue asset, one that refers to no asset, or one of another type (a `sound`
  takes an `AudioClipAsset` or a `SoundCueAsset`, a `sequence` a `SequenceAsset`); dialogue assets
  the settings do not name, once; a `jump` to a conversation no file has; a flag an `if` reads that
  no file sets and the settings do not provide; a conversation id or an explicit line id another
  file has too.
- **Warnings:** a statement nothing reaches (after a `jump`, say).
- **Resolved:** a `play`'s clip, a `sound`'s and a `sequence`'s asset, and an `asset(Type)`
  argument's, into the step's or the argument's `asset`.

## The assets and the cook

| Asset | What it is | Cooks to |
|---|---|---|
| `DialogueAsset` | a dialogue file, its text a `.dialogue` file in the project's sources (imported, or made by File > New's Dialogue) | `DialogueSource`, by `DialogueBuilder` |
| `DialogueCastAsset` | the cast, its `speakers` edited in the inspector (File > New's Dialogue Cast) | `DialogueCastSource`, by `DialogueCastBuilder` |
| `DialogueAssetListAsset` | the dialogue assets, its `entries` (a name and an asset each) edited in the inspector (File > New's Dialogue Assets) | nothing: read when dialogue files cook |

`DialogueBuilder` reads the file's text, gathers its project (`DialogueProject::Gather`: the
settings' commands parsed, the cast and the dialogue assets the settings name, and what the
project's other dialogue files hold), parses and checks it (`CookDialogue`), logs every problem as
`<file>:<line>: <message>`, and writes the compiled conversations; a file with an error does not
cook. It declares the cast, the dialogue assets and every other dialogue file as read, so a change
to any of them recooks it, and folds the settings it reads into its `EnvironmentHash`, so a change
to them recooks every dialogue file. `DialogueCastBuilder` writes the speakers and declares what
they refer to (portraits, clips, sounds). The `.dialogue` importer copies the file into the sources
and links it.

## Loading

The Engine part's load registers the settings type and the plugin's types
(`RegisterDialogueTypes`), reads the settings, and, where the host offers the engine's service
(`EnginePluginServices`), adds the dialogue's domain, whose resource module holds the two factories.
All of it is recorded and taken out when the plugin unloads.

Tests: `Dialogue.Engine.Tests` (the settings' defaults, their round trip through the project's
entry, their labels; the compiled conversations and the cast reading back as written, their lookups
and the speaker's reflection), `Dialogue.Pipeline.Tests` (the commands' notation and what it
refuses; the format parsed: lines, choices and branches compiled to jumps, the built-in statements,
declared commands with names of several words, line ids, and every problem at its line; the cook's
checks over a table of a project's assets) and `Integration.Plugins` (the package linked with its
four parts, its manifest, a project that enables it loading it with its settings and its domain,
and, through the MCP host, a dialogue file cooking to its conversations, one with an error failing
the cook, and File > New's three creators).