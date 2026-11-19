# Run, facade and behavior naming

> APPROVED 2026-11-19. Nothing here is built. Read on master at `7fefb9087`, and on `dotnet` at
> `6df9380b9` for C#.
>
> The names decided here are the ones
> [script-backends-as-plugins.md](script-backends-as-plugins.md) builds on: its gameplay layer
> and native behaviors use them.

## Goal

Game logic has one shape in every language, AngelScript, Luau, C# and native C++, and the names
say what each thing is:

- the run that plays the project is an **ApplicationRun**, beside the editor's SimulationRun;
- a class scripts call into the engine through is a **facade**, and its C++ name says so;
- a class the engine runs is a **behavior** of one of three kinds, **RunBehavior**,
  **SceneBehavior** and **EntityBehavior**, and a class declares its kind by deriving from it.

## Decisions

1. `GameInstance` becomes `ApplicationRun`; the module `Engine.GameInstance` becomes
   `Engine.Run`. A bare `Run` was considered: it reads as the general run beside `IRun` and
   `RunBase`, and says nothing of what sets it apart from `SimulationRun`. `ApplicationRun` is
   named for what it runs, as `SimulationRun` is, and follows the rule that engine-level names say
   application for what the engine runs.
2. Every facade class takes the suffix `Facade`. What scripts see does not change: each facade
   names its script name where it is registered.
3. The behavior kinds are base types, in every language. A class is a RunBehavior, a
   SceneBehavior or an EntityBehavior because it derives from one; the reserved class names `Game`
   and `Level` go away.
4. The spelling is *behavior* everywhere: code, data, docs and specs.
5. The reserved names `Game` and `Level` are not accepted without a base, in any release.

## 1. Runs

`foundation.run` defines a run: "one running game: a player's game, each editor Game tab's game,
an in-editor dedicated server" (`Foundation/Run/Run.cppm`). `IRun` reaches one and `RunBase`
implements it. Two classes derive from `RunBase` today: `GameInstance` (the player, a Game tab, a
dedicated server) and `SimulationRun` (a scene page's Simulate, which runs the scene's behaviors
and no RunBehavior).

| Today | Becomes |
|---|---|
| module `Engine.GameInstance` (`engine.gameinstance`) | `Engine.Run` (`engine.run`) |
| `GameInstance` | `ApplicationRun` |
| `GameInstance::StartScript`, `StopScript`, `ScriptRunning` | `StartRunBehavior`, `StopRunBehavior`, `RunBehaviorRunning` |
| `DefaultApplication::StartGameScript`, `StopGameScript`, `GameScriptRunning` | `StartRunBehavior`, `StopRunBehavior`, `RunBehaviorRunning` |
| `DefaultApplication::TickGameScript`, `SetGameScriptErrorHandler` | `TickRunBehavior`, `SetRunBehaviorErrorHandler` |
| `PlayerApplication::LoadAndStartGameScript` | `LoadAndStartRunBehavior` |
| `RunScriptBinding` (what the run facade reads) | `RunBinding` |
| `RunScriptErrorSink` | `RunErrorSink` |
| `Engine.GameInstance.Tests` | `Engine.Run.Tests` |

The accessors that hand out game instances (`DefaultApplication`'s, the editor's list of Game tab
runs) and every comment and document that says "game instance" follow. `RunSave` and
`NetworkController`, partitions of the module, keep their names.

`Engine.Run` and `Foundation.Run` sit as other engine modules sit over their foundation ones: the
foundation module is the concept, the engine module the run the application plays.

## 2. Facades

A facade is a reflected C++ class whose static methods scripts call (`Run`, `Save`, `Input`,
`Physics` and the rest, registered by about fifteen `Register...ScriptFacade` functions). Its
script name is its C++ name unless the type carries the `scriptName` attribute
(`Script/RegisterTypes.cppm:112`); only the run facade does today (`run`).

- Each facade class gets the suffix: `Run` becomes `RunFacade`, `Save` becomes `SaveFacade`, and
  so on.
- Each facade's registration states its script name, so `SaveFacade` binds as `Save` and
  `RunFacade` as `run`. The name is given explicitly, not derived by stripping the suffix: a rule
  keyed on a type's spelling is invisible where the type is declared.
- Script code does not change, and neither does the cooked script surface's content; its hash is
  recomputed and every script recooks once.
- A native behavior calls the same facades by their C++ names (`RunFacade::loadScene` where a
  script writes `run.loadScene`): one API, spelled as each language spells it.
- The C# bindings are generated from these types on `dotnet`; the generator must read the script
  name, not the C++ name. To check there before the rename lands.

## 3. Behavior kinds

### Today

A script tier is decided by where the class is used and by its name (`ScriptTier` in
`Pipeline/Script.Pipeline/ScriptAsset.cppm:615`): a behavior's class is named after its asset; a
Level must be called `Level`; the Game must be called `Game` (`Shipping/Scripting.md:17-25`). The
three tiers also name their handlers differently:

| Tier | Handlers |
|---|---|
| Game | `new()`, `launch()`, `update(dt)`, `exit()` |
| Level | `new(scene)`, `onStart`, `onUpdate`, `onFixedUpdate`, `onStop` |
| Behavior | `new(entity)`, `onEnable`, `onStart`, `onUpdate`, `onDisable`, `onDestroy` |

### The kinds

| Kind | Scope | Constructed with | Set on |
|---|---|---|---|
| `RunBehavior` | the run, across scene loads | nothing | the project (its run behavior) |
| `SceneBehavior` | a scene | its scene | the scene's settings |
| `EntityBehavior` | an entity | its entity | the entity's behavior list |

A class declares its kind by deriving from it, in every language. The class may have any name; a
file holds one behavior class (other, plain classes beside it are allowed, as today). The kind
the class derives from is recorded in its `BehaviorClass` (`kind`), and each place that takes a
class takes only its kind: the project's slot a RunBehavior, a scene's a SceneBehavior, an
entity's an EntityBehavior. A class of the wrong kind is refused where it is set, saying which
kind is needed.

### How each language declares it

- **C#:** `public class Game : RunBehavior`, `public class Player : EntityBehavior, IDamageable`.
  The kinds are classes in the engine's C# assembly. One base class is all C# allows, which is
  all this needs.
- **AngelScript:** `class Game : RunBehavior`. AngelScript cannot derive a script class from a
  class the application registers, so each kind is an application-registered interface with no
  methods: a marker, since handlers stay optional. To check: that the cook can read which
  interface a class implements (`asITypeInfo::Implements`).
- **Luau:** Luau has no class syntax; a class is a table with a metatable. The kinds are tables
  the engine provides, and a class is made from one: `Game = RunBehavior.class()`, which returns
  a class table that is its instances' metatable and looks up what it lacks in its kind. The
  script assigns it, which is what makes it the global the runtime finds; a name argument would
  be a second copy of the name that could disagree. The cook reads the kind from that line, as it
  reads the class from its `function Game.new` (a check does not run the script). A `local` class
  is not found by the runtime and declares no kind.
- **Native C++:** a class derives from `engine::RunBehavior` (or the other two) and is registered
  by its plugin's Engine part. How its handlers are found (virtual methods or reflected methods
  found by name) is open in [script-backends-as-plugins.md](script-backends-as-plugins.md).

### Handlers, aligned

With one shape across kinds, the handlers share their names; each kind has the ones its scope
needs, and dispatch stays by presence.

| Handler | RunBehavior | SceneBehavior | EntityBehavior |
|---|---|---|---|
| `onEnable`, `onDisable` | | | yes |
| `onStart` | yes (was `launch`) | yes | yes |
| `onUpdate(dt)` | yes (was `update`) | yes | yes |
| `onFixedUpdate(dt)` | | yes | |
| `onStop` | yes (was `exit`) | yes | |
| `onDestroy` | | | yes |
| `on<Event>(...)` | yes | yes | yes |

Whether an EntityBehavior's `onDestroy` and the others' `onStop` become one name is left open:
they differ today because an entity is destroyed and a run or scene stops.

### Names that follow

| Today | Becomes |
|---|---|
| `ScriptTier` (`Behavior`, `Level`, `Game`) | `BehaviorKind` (`Entity`, `Scene`, `Run`) |
| `ScriptStarterClassName`, the reserved names | gone: a starter's class is named after its asset, whatever its kind |
| the project's `startupScriptId`, `startupScript` | `runBehaviorId`, `runBehavior` |
| scene `SceneScriptSettings`, `SceneScriptSystem` | `SceneBehaviorSettings`, `SceneBehaviorSystem` |
| `ScriptSceneSystem` | `EntityBehaviorSystem` |
| editor: the "Script" component; the "Scene Script" settings and their "Level Script" field (`ScriptSubsystemImpl.cpp:53-69`) | the "Behavior" component; "Scene Behavior" settings, field "Behavior"; the project setting reads "Run Behavior" |

The rest of the layer's renames (`Engine.Behavior`, `BehaviorSubsystem`, `BehaviorComponent`,
`AttachedBehavior`, `BehaviorClass`, `BehaviorProperty*`) are in the backends spec's naming table,
which this spec's kinds replace where they differ (its Game, Level and entity kinds).

## Data and migration

- **Project file:** `startupScriptId` and `startupScript` become `runBehaviorId` and
  `runBehavior`; a version bump reads the old keys.
- **Scenes:** the component's type id `"script"` becomes `"behavior"`, and the scene's Level
  settings take their new name; a version bump each. The `"behaviors"` key stays.
- **Script sources:** a class without a kind is not a behavior. The cook refuses a file used as a
  behavior whose class derives from no kind, and says what to add (`class Game : RunBehavior`).
  It does not rewrite user sources. Every sample project, template and starter is updated in the
  same change, so the samples cook throughout (Integration.Mcp, Integration.Plugins).
- **Cooked scripts:** recook once (the surface hash changes with the facade names and the kinds).

The old reserved names are not accepted, not even for a release: a `Game` or `Level` class
without a base is refused like any other class without a kind (decided 2026-11-19). A project made
with an earlier release adds the base its classes need; the refusal names it, and `CHANGELOG.md`
says so under the release that lands it.

## Documents

`Shipping/Scripting.md` (the three kinds, how each language declares one, the handler table),
`Shipping/Scenes.md`, `Shipping/McpGuide.md` and any MCP tool that says "script" for a behavior
(with `kEngineToolCount`), the starters, `CHANGELOG.md` (renamed handlers and the base type are
breaking for a project's scripts), and the comments across the modules renamed.

## Order of work

Each step leaves the tree green, with the samples cooking.

1. **Runs.** `Engine.Run`, `ApplicationRun` and the members above. Code names only; no data.
2. **Facades.** The suffix and explicit script names; the surface hash; the C# generator checked
   on `dotnet`.
3. **Kinds as base types.** The kinds in each language, the cook reading the kind, the slots
   refusing the wrong kind, the reserved names gone; every sample's scripts updated.
4. **Handlers aligned.** `launch`, `update` and `exit` renamed; samples and starters.
5. **Layer renames and data.** The backends spec's naming table with this spec's kinds, the
   project and scene version bumps, the editor's words.
6. **Documents** land with each step that changes what they describe, not at the end.

## Risks

- **Breadth.** Every sample's scripts change in steps 3 and 4. A mistake shows as a sample that
  no longer cooks or plays; the Integration suites catch the first, a playthrough of each sample
  the second (the user's).
- **C# on its branch.** `dotnet` carries the C# half; each step lands there by rebase, with its
  C# side (the kinds as C# classes, the generator) as commits of its own.
- **Luau's declaration.** It is the one language where the kind is a call, not syntax. If the cook
  cannot read it reliably, the fallback is a declared field (`Game.kind = RunBehavior`); decide
  before step 3.
