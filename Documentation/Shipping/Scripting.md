# Scripting

How gameplay code works in a game project. This is the workflow guide; the AUTHORITATIVE
API surface (every bound type, method, and facade, per backend) comes from the MCP
`script_api` tool - always prefer it over memorized signatures.

## Backends

Two script languages are supported: **AngelScript** and **Luau**. Each script asset declares
its language, but a run has ONE gameplay context: the first behavior loaded fixes the language,
and a behavior in the other language stays disabled (logged once, category Script). Write a
game's scripts in one language. The bound engine surface is the same across backends; syntax
and idioms differ (e.g. Luau uses colon calls for instance methods, dot calls for static
facades).

## The three script tiers

- **Behavior** - attached to an entity through a ScriptComponent behavior slot. One class per
  script; the engine constructs it with the entity handle. This is where most gameplay lives.
- **Level** - the per-scene script (reserved class name `Level`), set in the scene's Scene
  Script settings. One instance per scene, constructed with the scene handle. Use it for
  scene-wide orchestration: spawning, win conditions, sequencing.
- **Game** - the project's startup script (reserved class name `Game`), set in project
  settings. Runs for the whole game run, across scene loads.

## Lifecycle and events

Handlers dispatch **by presence** - implement only what you need:

- `onStart()` - after the entity/scene is live.
- `onUpdate(dt)` - per frame, `dt` in seconds. (Levels also get `onFixedUpdate`.) Scripts update
  before the systems that act on what they write (the navigation crowd, audio, animation), so a
  destination, a sound or a graph parameter set here takes effect this frame; positions read here
  are where those systems left things last frame. So a speed is not a moved position over `dt`:
  that move was made last frame, with last frame's time, and on uneven frames the quotient swings
  (a navigation agent's speed fed to its graph jerks its walk). Read the speed the system reports,
  an agent's `velocityX()` / `velocityY()` / `velocityZ()` say. A scene's Level script updates
  before its entities' behaviors.
- `onDestroy()` - before teardown.
- `on<Event>(...)` - named events: physics contacts, and any custom event another script
  sends. `entity.send("eventName", payload)` delivers to the target entity's behaviors.
- Networked entities: gate on `NetworkComponent.of(self).authority` (Server or Client) - the
  owning side drives, the rest interpolate. It reads; replication owns the identity, so
  nothing on it is assignable from a script.

## Components

Every component a script reaches has three static methods on its type (AngelScript
`RigidBodyComponent::of(e)`, Luau `RigidBodyComponent.of(e)`):

- `of(entity)` - the entity's component, a handle that finds it again on each use; null (nil)
  when the entity has none, so `if (c !is null)` / `if c then` is the test where it may not.
- `addTo(entity)` - gives the entity the component, default-made, when it has none, and returns
  it either way, ready to set up: `RigidBodyComponent@ b = RigidBodyComponent::addTo(self);
  b.shape = ShapeKind::Capsule;`. Its systems take it up as they take up one placed in the editor
  (it initializes at the next frame's start; physics builds its body, audio plays it if it
  autoplays).
- `removeFrom(entity)` - takes it off and says whether there was one. What it held goes with it:
  a body leaves the physics world, a voice stops. A handle kept to it reads as gone, which is a
  script error where it is used.

A prefab is still the way to make something with many parts at once (`ScenePrefabs::of(scene)
.spawn`); `addTo` is for one component on an entity that is already there.

## Naming a control

Text that tells the player what to press names the control, never the action: an action's name
("Interact") is the game's word, the control ("F", or "Pad South" once a pad is picked up) is the
player's, and it changes with their rebinding. On a screen, put a `Prompt` beside the words
(`<Prompt action="Interact"/><Label text="open the door"/>`): it follows the device in hand by
itself, and a sheet can draw it as a glyph (`prompt[binding="pad-south"]`). In a sentence a script
builds, `Input.describe(action)` (or `Input.describe(action, set)`) gives the same control's
name for the device in hand, "" when the action has no binding: `"Hold " + Input.describe("Interact")
+ ": pick the lock"`. Build such a sentence when it is shown, not once, so it follows a pad
picked up.

## Editor properties

- **Luau**: every field the constructor sets to a number, boolean, string or Float3 becomes a
  property (numbers as float). The cook builds the class with a nil entity, so keep the
  constructor to plain field initialization.
- **AngelScript**: only member fields with `[default, "description"]` metadata are properties,
  e.g. `[3.0, "Spin rate"] float spinSpeed;`. Types: float, double, the int types, bool,
  string, `Color@`, `Float3@`, `Entity@`, and `Guid@` tagged `"asset:<Type>"`
  (`["asset:Prefab", "What to spawn"] Guid@ prefab;`). Fields without metadata are not.

## Saving

The `Save` facade keeps values between runs: a best time, a high score, an unlocked level, the
game's own options. Values are typed (int, float, bool, string) and keyed by any string you
choose:

```
Save::setInt("best.level2", 4210);              // AngelScript
int best = Save::getInt("best.level2", 0);      // the fallback when there is none yet
Save.setFloat("time.level2", 41.5)              -- Luau
```

- `has`, `remove` and `clear` do what they say. A value read as another kind than it was
  written answers the fallback, except that an int reads as a float.
- The run writes what changed when it ends. Call `Save::flush()` at the moment that matters (a
  level clear, leaving a settings screen) so a crash or a forced quit loses nothing.
- The player keeps the file in the user's data directory; play in editor keeps its own in the
  project's ignored `Editor/` folder, so testing never touches a player's save. Every game has one,
  whether its scripts are a game script or only its scenes' and entities' scripts.
- The audio bus volumes a settings screen sets are saved by the player on its own; a game
  does not need to save them.

## Web requests

The `Http` facade reaches web services: a leaderboard, a news feed, the game's own server. A
request answers by calling its handler on the game's thread a frame or more later, with the
status, the body text, and an error (empty when a response came):

```
void scores(int status, const string &in body, const string &in error) { ... }   // AngelScript
HttpHandler @h = scores; Http::get("https://example.com/scores", h);

Http.post("https://example.com/scores", "application/json", '{"score":9}',          -- Luau
  function(status, body, err) if status == 200 then ... end end)
```

- `Http.send(method, url, headers, body, handler)` takes any method; `headers` is
  `"Name: value"` lines. Each call returns an id for `Http.cancel(id)`, whose handler is then
  never called.
- A 404 or a 500 is a response (status set, `error` empty). Status 0 means none came, and
  `error` says why: no network, a timeout (30 s), a certificate not trusted.
- `https` checks the server's certificate against the system's trusted roots. Bodies are text.
- In a browser build the browser makes the request: a server on another origin must send
  CORS headers, or the request fails without a reason the game can see.
- Requests stop with the run: a handler is never called after its script has stopped.

## Working through the MCP tools

- `script_api` - the live bound API for a chosen backend. Read it before writing code.
- Scripts are project assets: import/list/cook them like any asset (see Assets.md).
- Coroutines are available in every backend for multi-frame sequences.
- A scene page's Simulate (`simulate_start`) runs that scene's behaviors and Level script as a
  run of its own, one per page: their own context, clock and `gui` root. What they push on screen
  draws over the page's viewport (and so in `viewport_screenshot`), and goes when the Simulate
  stops. It has no Game script, input map or save; test those with play in editor.

## Gotchas

- Class names `Game` and `Level` are reserved for their tiers - do not use them for
  behaviors.
- A behavior constructor runs at cook time too; side effects beyond field init will
  misbehave.
- Some engine properties are read-only on purpose (a network identity's `authority`, for
  one): every backend reads them, none assigns. AngelScript refuses the assignment at
  compile time, and Luau raises at the assignment. (The Luau declaration emitter marks them
  `read`, but nothing writes a `.d.luau` for a project yet: luau-analyze support is deferred.)
- A struct-valued property (a light's `color`, a `Float3`) reads as a handle over the value where
  it lives: `light.color = c` and `light.color.r = 0.5` both write the light, in both languages.
  To keep a copy in AngelScript, declare a value (`Color saved = light.color;`); a handle
  (`Color@ c = light.color;`) follows the light. In Luau every such read is a handle, so copy the
  fields (`Color.new(c.r, c.g, c.b, c.a)`) to keep a value apart from its owner.
