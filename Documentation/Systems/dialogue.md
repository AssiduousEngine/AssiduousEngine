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
| Engine | `Dialogue.Engine` | `dialogue.engine` | `DialogueSettings` |
| Pipeline | `Dialogue.Pipeline` | | its entry |
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
| `commands` | none | the commands the game declares, each a form (`move <who:entity> to <target:entity> [and wait]`) |
| `providedFlags` | none | flags provided from outside the conversations, by the application's own code, which a conversation may read with no `set`: a name, or a prefix ending in `*` (`seen_*`) |
| `flagPrefix` | `flag.` | where flags live in the run's save store |
| `stockBox` | off | the plugin's own dialogue box shows lines and choices |
| `typingSpeed` | 40 | characters a second the stock box types at; 0 shows a line whole |
| `advanceAction` | `Interact` | the input map action that advances a line in the stock box |
| `portraitOnRight` | off | the stock box's portrait side |

Tests: `Dialogue.Engine.Tests` (the settings' defaults, their round trip through the project's
entry, their labels) and `Integration.Plugins` (the package linked with its four parts, its
manifest, and a project that enables it loading it with its settings).
