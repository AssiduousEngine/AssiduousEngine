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

## The three kinds of behavior

A class the engine runs is a **behavior**, and declares its kind by deriving from it. A script
holds one behavior class, of any name; other classes beside it are its helpers.

- **EntityBehavior** - attached to an entity through a ScriptComponent behavior slot; the engine
  constructs it with its entity. This is where most gameplay lives.
- **SceneBehavior** - one per scene, set in the scene's Scene Behavior settings, constructed with
  its scene. Use it for scene-wide orchestration: spawning, win conditions, sequencing.
- **RunBehavior** - the project's startup script, set in project settings. Runs for the whole
  game run, across scene loads.

```angelscript
class Coin : EntityBehavior
{
    Coin(Entity e) {}
}
```

```lua
Coin = EntityBehavior.class()
function Coin.new(entity) return setmetatable({ entity = entity }, Coin) end
```

In AngelScript a behavior can also derive from a base class of its own (`class Mover : Base,
EntityBehavior`). In Luau the class is a global made from its kind (`local` is not found). A
script whose class derives from no kind is not a behavior, and is refused where a behavior is set;
one with two behavior classes does not cook.

## Lifecycle and events

Handlers dispatch **by presence** - implement only what you need. Every kind shares the names; each
has the ones its scope needs:

| Handler | RunBehavior | SceneBehavior | EntityBehavior |
|---|---|---|---|
| `onStart()` | yes | yes | yes |
| `onUpdate(dt)` | yes | yes | yes |
| `onFixedUpdate(dt)` | | yes | yes |
| `onStop()` | yes | yes | |
| `onEnable()`, `onDisable()`, `onDestroy()` | | | yes |
| `on<Event>(...)` | yes | yes | yes |

- `onStart()` - after the run, scene or entity is live.
- `onUpdate(dt)` - per frame, `dt` in seconds. Scripts update
  before the systems that act on what they write (the navigation crowd, audio, animation), so a
  destination, a sound or a graph parameter set here takes effect this frame; positions read here
  are where those systems left things last frame. So a speed is not a moved position over `dt`:
  that move was made last frame, with last frame's time, and on uneven frames the quotient swings
  (a navigation agent's speed fed to its graph jerks its walk). Read the speed the system reports,
  an agent's `velocityX()` / `velocityY()` / `velocityZ()` say. A scene's behavior updates
  before its entities' behaviors.
- `onFixedUpdate(dt)` - each fixed physics step, `dt` the step's length; for forces and other
  work that belongs to the simulation's rate. An entity's behavior gets its first step after its
  `onStart()`, and none while its entity is inactive.
- `onStop()` - when the run or scene stops; `onDestroy()` - before an entity's behavior goes.
- `on<Event>(...)` - named events: physics contacts, and any custom event another script
  sends. `entity.send("eventName", payload)` delivers to the target entity's behaviors.
- Networked entities: gate on `NetworkComponent.of(self).authority` (Server or Client) - the
  owning side drives, the rest interpolate. It reads; replication owns the identity, so
  nothing on it is assignable from a script.

## Entities

An entity is a value, as a `Float3` is: `Entity target;` holds one, `target = e` copies it, and
two name the same entity when `a == b` (the same scene, slot and generation). An entity that names
none (a lookup that found nothing, a property never set, one destroyed since) answers
`isValid()` false. Ask that in both languages (`if (!target.isValid())`, `if not
target:isValid() then`): an entity is never null, and in Luau never `nil`, so a Luau `if target
then` passes for one that names nothing. Components and GUI nodes are handles, not values: a
component that is not there is `null` in AngelScript and `nil` in Luau. The same holds for a
`Guid`: compare with `==`.

## The scene

What acts on a whole scene is a property of the scene: `scene.physics.rayCast(from, dir, 20.0f)`
(Luau `scene.physics:rayCast(from, dir, 20)`), and from a behavior `self.scene.physics`:

- `scene.prefabs` - spawn a prefab (`spawn(prefab, position)`, with a rotation, under a parent).
- `scene.events` - emit a named event on the scene's bus.
- `scene.physics` - rays, sphere casts, overlaps, gravity, velocities, forces and impulses,
  characters.
- `scene.animation` - clips, graph parameters, one-shots, inverse kinematics targets, root motion.
- `scene.audio` - play, stop and pause sources, swap their clips, ease their volume and pitch.
- `scene.render` - swap meshes and materials, a mesh's own material values, camera targets, the
  light at a point.
- `scene.particles` - play, stop and swap effects.
- `scene.splines` - a spline's length, its points, a point along it, the point nearest.
- `scene.behaviors` - the scene's script time, messages to behaviors, a behavior added to an entity.
- `scene.debugDraw` - lines, arrows, spheres, boxes and text, cleared each frame: draw them every
  update.
- `scene.environment`, `scene.postProcess` - the scene's live sky, fog and ambient, and its
  exposure, tonemapping, bloom and anti-aliasing.

Each is a value naming its scene (`ScenePhysics physics = scene.physics;` keeps one; once the scene
is gone its calls do nothing). The `script_api` tool lists every member.

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

A prefab is still the way to make something with many parts at once (`scene.prefabs.spawn`);
`addTo` is for one component on an entity that is already there.

## Animation: one-shots, lengths and crossfades

`scene.animation` drives what an entity's animator plays, whether a single clip
(`SkeletalAnimationComponent`) or a state-machine graph (`AnimationGraphComponent`):

- `playOneShot(entity, clip)` plays `clip` once over whatever the entity is animating, blending in
  and out over 0.2 s, whatever the clip's own looping; the entity then carries on with its own
  animation, by then wherever it has got to (its idle, its graph's state), so there is no clip to
  return to. `playOneShot(entity, clip, speed, blendIn, blendOut, loops)` sets the speed, the
  blend times and how many more times it plays (`-1`: until stopped). One already playing gives way
  to a new one without a snap. A one-shot drives the whole body and moves no root: a look laid over
  the character, not its locomotion; inverse kinematics still applies after it.
- `stopOneShot(entity)` (or with a blend-out time) blends it out; `isPlayingOneShot(entity)` is
  true from the call until it has blended out.
- When a one-shot ends (played out or stopped), the entity's behaviours get
  `onAnimationFinished(Guid clip)` at the next scene tick, as physics contacts arrive.
- When an entity's animation passes one of its clip's events (a marker authored on the clip's
  page: a footstep, the moment a swing lands), its behaviours get `onAnimationEvent(string name)`
  at the next scene tick, in the order passed.
- `clipLength(clip)` is a clip's length in seconds (0 for no clip).
- `setClip(entity, clip, blendSeconds)` swaps an animator's clip easing from the pose shown now into
  the new clip, instead of the snap of `setClip(entity, clip)`.

## Sequences: cutscenes on a timeline

A sequence asset is a timeline that drives several entities at once: tracks of keys on any
reflected property or the transform, events, and markers, some of which hold the playhead. It names
its cast ("Mentor", "Actor"), and whoever plays it fills each name with an entity. `scene.sequences`
plays them:

- `scene.sequences.play(sequence)` starts the sequence asset `sequence` (its id) and returns a
  `SequencePlay`, already at time 0. `play(sequence, finish)` ends it as `"release"` (the default:
  what it moved stays where it is), `"hold"` (everything stays as its last frame left it until
  `stop()`) or `"restore"` (every value it changed goes back).
- On the play: `bind(name, entity)` fills a name, during the play too (an actor spawned for the
  scene); `pause()`, `resume()` (also what lets a hold go), `seek(time)`, `setSpeed(speed)`,
  `stop()`, `time()`, `duration()`, `isPlaying()`, `isHeld()`, and `id()`. A play that has ended is
  harmless: its calls do nothing and `isPlaying()` is false.
- `scene.sequences.stopAll()` ends every play in the scene (a skip).
- A Sequence component plays a sequence from an entity: its cast is picked in the inspector (an
  entity per name, or "This Entity"), it starts by itself unless Autoplay is off, and it stops when
  its entity is destroyed or disabled. `SequenceComponent.of(entity).play()` starts it again from
  the beginning, `stop()` stops it, and `scene.sequences.of(entity)` is its running play. What its
  play reports reaches that entity's behaviours as entity events: `onSequenceEvent(string name,
  string value)`, `onSequenceHeld(string marker)` and `onSequenceFinished()`.
- What a play reports reaches behaviours at the next scene tick as `onSequenceEvent(notice)` (an
  event key passed: its `name` and `value`), `onSequenceHeld(notice)` (the playhead stopped at the
  hold marker `name`; call `resume()` when the game is ready, the way a line of dialogue waits for a
  press) and `onSequenceFinished(notice)`. Every scene in the run hears these, so `notice.play` says
  which play it is: compare it with your play's `id()`. In AngelScript the handler takes a
  `SequenceNotice@`.
- An event track bound to an actor posts to that actor instead, as an entity event:
  `onSequenceEvent(string name, string value)`.

The sequence advances right after the behaviours each frame, on the scene's clock, so a play started
or resumed in `onUpdate` moves in the same frame.

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

## Rebinding controls

A player can change what they press for an action, and the change is kept in the game's save:

- `Input.rebind("Jump")` waits for the player's next press on the device in hand (the keyboard and
  mouse, or a pad) and makes it Jump's control for that device; the other device keeps its own.
  Every action reads released while it waits. Escape, or the pad's Back, cancels;
  `Input.cancelRebind()` does from a script. `Input.isRebinding()` is true until then, and
  `Input.describe("Jump")` names the new control after. It works on button actions.
- `Input.setBinding("Jump", "Key.Space")` sets it by name: `Key.<key>`, `Mouse.<button>`,
  `Pad.<button>` or `PadAxis.<axis>`, named as the engine spells them (`Key.LeftShift`,
  `Pad.South`, `PadAxis.RightTrigger`). False for a name or action it does not know.
- `Input.resetBinding("Jump")` gives the action the game's own controls back;
  `Input.resetAllBindings()` does it for every action.
- Each takes the action's set as a last argument when the name is in more than one; without it,
  the set the action is read from now.

## Editor properties

- **Luau**: every field the constructor sets to a number, boolean, string or Float3 becomes a
  property (numbers as float). The cook builds the class with a nil entity, so keep the
  constructor to plain field initialization.
- **AngelScript**: only member fields with `[default, "description"]` metadata are properties,
  e.g. `[3.0, "Spin rate"] float spinSpeed;`. Types: float, double, the int types, bool,
  string, `Color`, `Float3`, `Entity`, and `Guid` tagged `"asset:<Type>"`, the type the
  asset loads as (`["asset:PrefabResource", "What to spawn"] Guid prefab;`,
  `"asset:AnimationClip"`, `"asset:Material"`; the full name, `"asset:rtti::scene::PrefabResource"`,
  works too): the inspector picks the assets that make it.
  Fields without metadata are not.

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

## Graphics quality

The `RenderQuality` facade is an options menu's graphics setting: the project's quality levels
(its Render Quality assets, in the order the project lists them), the one the game plays at, and a
switch between them by label:

```
array<string>@ levels = RenderQuality::levels();   // AngelScript: "Low", "Medium", "High"
string now = RenderQuality::current();
bool switched = RenderQuality::set("Medium");      // false for a label the game does not offer
RenderQuality.set(RenderQuality.levels()[1])       -- Luau
```

- `set` switches at once, and the player keeps the choice: the next run starts at it, ahead of
  the export preset's level and the project's. A level the project no longer lists is ignored
  then. A game does not need to save it.
- In the editor's Game tab, `set` picks the level in the tab's quality dropdown, for the session.
- `current()` is empty when the game plays at the engine's own level (the project lists none).

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

- A behavior constructor runs at cook time too; side effects beyond field init will
  misbehave.
- Some engine properties are read-only on purpose (a network identity's `authority`, for
  one): every backend reads them, none assigns. AngelScript refuses the assignment at
  compile time, and Luau raises at the assignment. (The Luau declaration emitter marks them
  `read`, but nothing writes a `.d.luau` for a project yet: luau-analyze support is deferred.)
- The plain types (`Float3`, `Color`, `Quaternion`, `Transform`, `Guid`, `Entity`, the scene's
  facades and the rest) are values, as a C# struct is: a property read is a copy, so
  `light.color = c` writes the light. To change one field, copy, change and write back
  (`Color c = light.color; c.r = 0.5; light.color = c;`): AngelScript refuses
  `light.color.r = 0.5` as C# does, and in Luau it changes only the copy. A value's own fields are
  plain members (`p.x += 1`). In
  AngelScript they are declared without `@` (`Color tint`; a `Color@` is refused, and the error
  says why); in Luau a copy is a new value, but two variables holding one value share it, so
  build a new one (`Color.new(...)`) rather than edit one another variable holds. A component is
  not a value: `LightComponent.of(e)` is the light itself. An element of a component's list is
  reached in place: `crate.items_at(0).size = 5` writes the element; a variable copies it.
