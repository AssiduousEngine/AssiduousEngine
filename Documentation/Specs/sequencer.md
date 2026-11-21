# A sequencer for cutscenes

> APPROVED 2026-11-20. Q0 to Q2 built 2026-11-20 (to `18d154549`), Q3 2026-11-21 (to `3f78d54e2`), Q4 2026-11-21 (to `74f0019f6`); Q5 next. Read on master at `595794aee`; line numbers will drift. Replaces the draft
> in `../Plans/backlog-0.1.md` ("A sequencer for cutscenes", 2026-11-08), which this answers.

## Goal

A timeline that drives several entities at once and is played from script: transforms and any
reflected property, skeletal clips, sounds, camera cuts, fades, events and nested sequences. It is
authored beside the scene it plays in, previewed there, cooked like any asset, and hands control
back to gameplay when it ends. The sequencer depends on no domain: animation, audio and render
each add the track kinds they own, and a plugin or a game adds its own the same way.

## What the first game needs

The first game built on the engine runs its cutscenes today as a script dialogue runner, its
scenes compiled from text by a tool of its own. It has about a dozen scenes. Two staged ones hold
nearly all the staging; the rest are dialogue in place with a camera move or a clip. Its story
plans a few dozen scenes, a handful of staged cutscenes among them.

What its steps do, and what each asks of the sequencer:

| Runner step | What it does today | Sequencer |
|---|---|---|
| `line` | types a line, blocks until a press, plays the speaker's Talk clip | a **hold**: the timeline stops at a marker until the game says go on; the line itself stays the game's (a game track or an event) |
| `camera <entity> [for s]` | eases the follow camera to an entity for a while, then back | a **camera cut** to a framing camera, or a **transform** track on a camera |
| `play <who> <clip> [and wait]` | `scene.animation.playOneShot`, blends back after | a **clip** track section |
| `move <who> to <target>` | straight-line walk at a set speed, idle/walk/run cross-faded | a **transform** track plus **clip** sections |
| `fade in/out` | a GUI panel's opacity over 0.5 s, blocking | a **fade** track |
| `wait <s>` | blocks | time on the timeline |
| `sound <cue>` | logged only, never used | a **sound** track |
| `cue <event>` | the level's escape hatch: lighting tweens, spawns, flags, teleports, a title card | an **event** track |
| hand-offs, `if`/`set` | hands off to gameplay, writes the save | stays the game's: an event, then the game decides |

Actors are entities found by name, several of them spawned during the scene; there are no slots
today. Nothing is restored afterwards on purpose: world
changes persist and are rebuilt from flags on reload, and only the camera returns to the player.
There is no skip, letterbox, time scaling or look-at step; facing is automatic.

So the first version needs: property and transform tracks over several entities, clip sections,
camera cuts, fades, events, holds that wait for the game, bindings filled at runtime, and a finish
that gives the camera back without undoing the world. Sound is cheap to add once audio can start
mid-clip. Restore-on-stop matters for the editor's preview more than for the game.

## What the reference engines do

Surveyed 2026-11-20 (Lumina, Flax, Traktor, Godot; the draft of 2026-11-08 had found none in
ezEngine, Lumix or Zero).

| | Lumina `CSequence` | Flax Scene Animation | Traktor Theater | Godot AnimationPlayer |
|---|---|---|---|---|
| Time | float seconds, frame rate for display | integer frames | float seconds | double seconds |
| Nesting | no | nested track, clamp or loop | no (acts are alternatives) | animation track |
| Binding | by name, spawned prefab or camera | Guid, `MapObject` remap, prefab ids | Guid through a resolver, owner-relative | node path |
| Restore | Hold or Restore per play | `RestoreStateOnStop`, off by default | none | RESET track, backup |
| Events | named, polled queue | invoke a method | entity events | method tracks, signal |
| Audio | starts at the offset | re-seeks past 0.3 s drift | none | offset on seek |
| Camera | cut with a blend time | hard cut, flags a camera cut | none | none |
| Fade, time dilation | both | fade | none | none |
| New track kinds | a subclass, but the editor's menu is a fixed list | closed enum | closed | closed |

Taken: one evaluation path for preview and play (Lumina's instance); events over `(previous,
now]` and never on a seek (Lumina, Traktor); an order per track kind; bindings remappable at
runtime (Flax) and resolved relative to the owner inside a prefab (Traktor); restore by snapshot
on first write (Flax), plus each kind releasing its own side effects (Lumina); audio drift
correction (Flax); dropping temporal history on a cut (Flax, though there the cut flags it; here
the render sees it for itself); editor-only data stripped by
the cook (Traktor); a manual update mode for the editor (Flax, Godot).

Avoided: binding by name with a world scan, or by path string; a track menu that is a fixed list
while the runtime claims to be open; restore off by default with no release of side effects.

## What exists today

- **Property animation** is the template for every layer and is reused, not copied.
  `foundation.propertyanimation` has `PropertyTrack` (component type, property path, kind, curve
  channels, quaternion keys; `PropertyAnimationModule.cppm:71-78`), `SampleMerged`, which keeps
  the live value for unkeyed channels (:120), and the binding `ResolveBinding` / `WriteBinding`
  through reflection, so setters run (:209-219; `PropertyAnimationImpl.cpp:21-126`). Time is f32
  seconds; frame rate lives only on the editor's `TimeAxis`. No events. Its resource is a flat
  wire struct versioned by appending (`PropertyAnimationResource.cppm:46-229`), cooked by
  `PropertyAnimation.Pipeline` (builder `Version()` 3).
- **`PropertyAnimatorComponent`** plays one clip on its own entity only, has a reserved
  `"Transform"` target (`PropertyAnimatorComponent.cppm:58`, :185-198), finds other targets by
  reflected type name (:307-320), and posts nothing when it ends (:337-347).
- **Skeletal one-shots** go through `PoseOverlay` (`Animation/PoseOverlay.cppm:34-111`), which
  blends in and out over whatever the entity plays. It reads `ClipTime()` (:210) but cannot be
  seeked. Animation already posts `animationFinished` and `animationEvent` as entity events
  (`AnimationSubsystemImpl.cpp:498-531`).
- **Audio** can read a voice's true cursor (`AudioEngine::GetVoiceStatus`, `cursorSeconds`;
  `AudioEngine.cppm:230`, :298) and glide its pitch (:303), but cannot seek or start a voice
  partway in (`AudioPlayParams` has only `startPaused`).
- **Cameras**: the render takes the first camera that is primary, active and has no target
  (`ExtractImpl.cpp:524-546`). Nothing overrides that pick. A view's TAA, SSR, SSGI and
  exposure history lives in a slot found by the view's history key (its viewport, target, scene
  and corner; `RenderSubsystemImpl.cpp:537-550`), and a slot new to a view starts fresh
  (`PipelineImpl.cpp:1983-2005`). The camera is not part of that key, so switching cameras today
  blends the old camera's history into the new one's first frames.
- **No screen fade**: `PostProcessSettings` has no fade colour.
- **Time scale** exists per scene, per context and per run (`Scene::SetTimeScale`,
  `Scene.cppm:627-632`).
- **Entity events**: a domain posts `EntityEvent{target, name, args}` to the scene
  (`Scene.cppm:100-121`, :588) and behaviors get `on<Name>`. The run's `EventBus` broadcasts.
- **Domains** contribute through `DomainModule` (`Engine.Domain/Domain.cppm:34-50`); a plugin
  through `SceneModuleContributions::Global()` (`SceneComposition.cppm:122-150`). Registries
  filled at registration are the precedent (`RegisterExtraFacadeName`, `AssetCreatorRegistry`).
- **Prefab references**: `RemapPrefabEntityRefs` remaps a component's `EntityRef` fields and
  arrays of them, but not an `EntityRef` inside a nested struct or an array of structs
  (`SceneResourceImpl.cpp:847-897`).
- **Editor**: GUI.Toolkit has `TimeAxis`, `TimeRuler` with movable markers, `Dopesheet` (keys
  only, no duration bars), `CurveView` and `ITimelineSource`. `Editor.PropertyAnimation` has the
  document with its own undo, `ClipPreview` (poses an entity at a time and restores what it
  overwrote) and `ClipTransport`, and its panel sits under the scene page's viewport.

## The design

### Layers

As property animation is split:

| Library | Holds | Depends on |
|---|---|---|
| `Foundation/Sequence` | the model, the play that evaluates it, the track base, the kinds that need no domain (property, event) | Core, Scene, PropertyAnimation |
| `Foundation/Sequence.Resource` | the cooked sequence and its factory | Sequence, Resource, Content |
| `Pipeline/Sequence.Pipeline` | the authored asset, cooked with editor-only data stripped | Sequence.Resource, Pipeline |
| `Engine.Sequence` | the scene system that plays sequences, the component, bindings, finish and restore, the script facade, the domain module | Sequence.Resource, Engine.Scene, Script.Facades, Domain |
| `Editor.Sequence` | the sequence panel on the scene page | Editor.Scene, GUI.Toolkit, Editor.PropertyAnimation (its preview pieces) |

`Foundation/Sequence` takes Scene, not only Core, because a kind applies itself to entities; its
tests stay headless. Never Render, Audio, Animation or Physics anywhere in these.

### The model

```
Sequence
  duration        f32 seconds
  frameRate       display and snapping only; evaluation is continuous
  bindings[]      { name, kind: Entity | Owner }       the cast, by name
  tracks[]        { kind, binding (index or none), muted, data (the kind's reflected struct) }
  markers[]       { time, name, hold }                  named points; a hold stops there
```

A track's data has one of two shapes, both in the Foundation library so every kind reuses them:
**keys** (curves, as `PropertyTrack` has) or **sections** (`{start, duration}` plus the kind's
fields: a clip, a sound, a camera, a nested sequence). Tracks are stored as a scene stores
components: the kind's name and its reflected body, so a new kind needs no change to the format.

### Track kinds

A kind is a subclass of `SequenceTrack` (a reflected `ISerializable`, as particle modules are),
registered with the type and serializable registries by its owner's registration
(`RegisterSequenceTrack<T>()`). A sequence stores each track as its type id and its fields, as a
particle system stores its modules. The type registry is the list of kinds: whatever needs them
asks it for `SequenceTrack`'s registered subclasses (`SequenceTrackKinds()`). A kind provides:

- its keys or sections and their serialization, and whether it needs a bound entity;
- an evaluation order (property and transform first, then animation, camera, fade, events last);
- `Begin` (resolve what it needs on the bound entity) returning a runner for that play, whose
  `Evaluate(step)` gets the time, the previous time, and whether the step is a jump (seek, scrub)
  or the editor's preview, and whose `Release(restore)`
  (stop voices and overlays, drop the camera override, clear the fade; when restoring, put back
  what it changed).

The editor's add-track menu, its icons and its inspector all come from the type registry; there is
no list of kinds anywhere.

| Kind | Owner | Shape | Notes |
|---|---|---|---|
| Property | Foundation | keys | any reflected property, through property animation's track targets (`ResolveTrackTarget`, `ApplyTrack`), which the animator and the editor's clip preview also use; `Transform` as the animator has it |
| Event | Foundation | keys | a name and a text value (typed values later); posted as the entity event `sequenceEvent` to the bound entity, or reported by the play when unbound (see the player); fired over `(previous, now]`, never on a jump |
| Sequence | Sequence.Resource | sections | a nested sequence on the same bindings, clamped or looped; recursion refused when loaded. It refers to a sequence asset, so it lives with the resource |
| Clip | Animation | sections | a skeletal clip on the bound entity through `PoseOverlay`, with blend in and out, speed and loop |
| Sound | Audio | sections | a clip or cue, on the bound entity (spatial) or not; started at its offset when entered mid-section, paused with the play, re-seeked past a drift threshold |
| Camera cut | Render | sections | while a section runs, its bound camera is the view |
| Fade | Render | keys | amount and colour over the whole view |

Left for later, each a kind on its own: time dilation (`Scene::SetTimeScale`, with the play on
unscaled time), look-at, spawning a prefab for the sequence's length (Lumina's spawnables). Camera
shake stays out of the engine, as the draft ruled.

### Bindings

The sequence names its cast; whoever starts a play fills it. A binding is one of:

- **Filled at play**: script binds a name to any entity when it starts the play or while it runs
  (`play.bind("Mentor", mentor)`). This is how the first game's actors come in, spawned ones
  included, and the usual way for a play started from script.
- **Entity**: a sequence component's table maps the name to an `EntityRef`, set in the inspector.
  Inside a prefab the refs remap like any other (once nested `EntityRef`s remap; see the order of
  work).
- **Owner**: the component's own entity. A sequence can be marked **relative to its owner**, so
  its transform keys are in the owner's space and a prefab carrying a staged moment can be placed
  many times (Traktor's entity form).

A script bind wins over the table. A track whose binding is empty is skipped with one warning per
play, as the animator does today.

### The player

The player is the scene system, `SequenceSystem`, as Traktor's world component is: it holds every
play running in its scene, ticks them, and owns what they share. A component is only another way
to start a play in it.

**A play** is one running instance of a sequence: its time, its bindings, its owner (an entity, or
none for a play started from script), `speed`, `loop`, its clock (scaled game time, or unscaled,
so a sequence can run while the game is slowed), and a **finish**:

- **Release** (default): every kind releases its side effects (the camera goes back to gameplay,
  voices stop, the fade clears), and what properties moved stays where the sequence left it. This
  is what the first game does today.
- **Hold**: everything stays as the last frame left it, the camera included, until `stop()`.
- **Restore**: everything the sequence wrote goes back to its value before the first write
  (snapshotted on first write, per entity and property), and every kind releases.

**What plays share.** The view camera and the fade are one per scene: the play started last that
has a camera cut or fade section running holds them, and when it releases they pass back to the
play under it, or to gameplay. Two plays writing the same property: the later play's write lands
last, since the system evaluates plays in the order they started.

The system ticks in Update, after behaviors and before PostUpdate's animation, so a clip section's
overlay and a transform key land in the same frame. It is simulation-only, and has a manual mode
the editor drives.

**From script**, the system is `scene.sequences` (a scene property, as `scene.animation` is):

- `scene.sequences.play(sequence)` starts a play and returns its handle, `SequencePlay`, a value
  that names one play in one scene; `play(sequence, finish)` with a finish other than Release.
- On the handle: `bind(name, entity)`, `stop()`, `pause()`, `resume()`, `seek(time)`, `time()`,
  `duration()`, `isPlaying()`, `isHeld()`, `setSpeed(speed)`. A handle whose play has ended is
  harmless: its calls do nothing and `isPlaying()` is false.
- `scene.sequences.stopAll()`, for a skip or a level unloading.

**The component**, `SequenceComponent`: a sequence, a binding table, `autoplay` and the play's
settings. It starts its play in the system with its entity as owner and its table as bindings,
and stops it when the entity is destroyed or disabled. Script reaches its play through
`SequenceComponent.of(entity).play()`, which returns the same `SequencePlay` handle. The
inspector's "Edit Sequence" is on it.

**What a play reports.** A play started from script reports on the run's event bus
(`Scene::Events()`; the scene has no bus of its own), where a scene behavior hears it as
`on<Name>`. Every scene in the run hears that bus, so each event's value is a `SequenceNotice`:
the play's handle, the sequence's guid, and the event's own name and value. A level script
compares the handle with the one `play` returned. A play a component owns posts the same events
as entity events to its owner (`Scene::PostEntityEvent`), so that entity's behaviors hear them.

| Entity event | On the run's bus | When | Carries |
|---|---|---|---|
| `sequenceEvent` | `SequenceEvent` | an event key is passed | the key's name and value |
| `sequenceHeld` | `SequenceHeld` | the playhead reaches a hold marker and stops | the marker's name |
| `sequenceFinished` | `SequenceFinished` | the end is reached (not on `stop()`) | nothing more |

The two spellings reach the same handlers: an entity event `name` calls `on<Name>` (as
`contactBegin` calls `onContactBegin`), and a handler `on<Name>` hears the bus event `Name`. The
notice holds strings, so it crosses into AngelScript as a reference (`SequenceNotice@`); the handle
is a plain value. As built (Q2): the notice is `{play: int, sequence: Guid, name, value}`, a play's
`id()` is what `play` is compared with, `play(sequence, finish)` names the finish as text
(`"release"`, `"hold"`, `"restore"`), and the system's update order is -99, after the behaviors'
-110 and the sequence components' -100 (nothing may run between the behaviors and the first
gameplay systems at -100). Destroying a play releases nothing (the scene may be going away); stopping does.

A hold is how a line of dialogue waits for the player: the game's runner hears `sequenceHeld`,
shows the line, and calls `resume()` on the handle at the press. The sequencer knows nothing of
dialogue.

### Preview in the editor

The sequence panel opens as the property animation panel does: by its toggle on the scene page's
bar and in the Scene menu (an action beside `scene.view.animationPanel`, `SceneActions.cppm:48`,
:387), or from a sequence component's inspector, whose "Edit Sequence" opens its sequence
bound to that component (as an animator's "Edit Clip" does, `BuiltinInspectorExtras.cppm:87-92`,
through a page method beside `EditAnimationClip`). Selecting an entity never opens it. It sits in
the same dock under the viewport, a tab beside the animation panel. The panel's preview poses the scene at the
playhead through the same evaluation the game uses, in manual mode: no events, no voices on a
scrub, camera cuts shown only when "preview cuts" is on (so the editor camera stays free),
everything restored when the panel closes or the scene starts simulating, as `ClipPreview` does.
Pressing play in the panel plays it with sound. Events are listed on the ruler but never sent.

The page: a track list grouped by binding, a dopesheet for key tracks, a new section lane widget
in GUI.Toolkit for section tracks (bars that move, trim and snap), markers and holds on the ruler,
and the curve view for a selected property track. One undo stack per sequence document, as the
clip document has.

### Agents

MCP tools to read and write a sequence (bindings, tracks, keys, sections, markers) and to pose a
scene page's preview at a time and screenshot it. `McpGuide.md` and `kEngineToolCount` change with
them.

## Decided by this spec (for review)

1. **Blending with gameplay: hard cut, except clips.** Property tracks write over gameplay, as
   every surveyed engine but Godot does; a clip section blends in and out through `PoseOverlay`,
   which already does it. Camera cuts are hard; a smooth move is a transform track on a camera.
   Godot's blend from the live value is a later option per track, not a first-version need: the
   first game's moves start where the actor stands, which a key at the start does.
2. **Skeletal clips and property tracks keep their own formats.** A clip section refers to a
   skeletal clip asset; a property track is property animation's. They are different data (bone
   poses against reflected curves), and nothing in the first game needs them merged.
3. **Seconds, not frames**, as property animation; the frame rate snaps the editor only.
4. **Release is the default finish**, since it is what the first game's cutscenes do; Restore is for
   preview and for games that want it.
5. **The scene system is the player, a component is optional.** A level's cutscenes are the
   level's, started from its script with no entity to carry them, and one place has to own the
   view camera and the fade. The component stays for what only an entity gives: a staged moment
   inside a prefab, played relative to its owner, and bindings picked in the inspector. Traktor
   has the same two forms over one `Performance`.

## Order of work

Each phase green on clang and gcc, with its tests, its documentation and its CHANGELOG line.

1. **Q0 Model**: first, property animation's track targets on a scene (the animator and the
   editor's clip preview each had a copy; the sequencer would have been a third), its own commit.
   Then `Foundation/Sequence` with the model, the play (evaluation over `(previous, now]`, jumps,
   holds, loops, the three finishes, rebinding mid-play, the preview pose), serialization, and the
   property and event kinds. Headless tests: event firing on play, on seek, on loop wrap; holds;
   finishes and restore; property writes through setters.
2. **Q1 Asset**: `Sequence.Resource` and `Sequence.Pipeline` (version 1, appended fields, editor
   data stripped), the File > New creator, and the nested sequence kind with its recursion guard.
   Round-trip and cook tests.
3. **Q2 Player**: nested `EntityRef` remap in prefabs (a fix for every component, its own commit
   first), then `Engine.Sequence`: the scene system and its plays, bindings, finish modes, what
   plays share, `scene.sequences` and its notices on the run's bus, and the domain module; then
   the component on top of it, with its entity events. Script tests in `Code/Integration`, in
   every language.
4. **Q3 Domain kinds**, a commit each with what it needs: the clip kind (a seek for
   `PoseOverlay`); the sound kind (a start offset and a seek on `AudioEngine`, then drift
   correction); the camera cut kind (a view camera override in the render's camera pick); the
   render starting a view's history fresh when the camera it is drawn from changes (its own
   commit, first: it fixes any game that switches cameras, with or without a sequence); the fade
   kind (a fade colour in the composite). As built: the clip kind drives the overlay (its clip
   time and weight each step, `PoseOverlay::DriveOneShot`) rather than seeking a one-shot, so the
   player's own tick never moves it; the camera cut and the fade are claims on scene state
   (`SceneClaims`), the play begun last winning; runners are told of a pause
   (`SequenceTrackRunner::SetPaused`), not of a hold, so a voice pauses with the play and plays on
   at a hold.
5. **Q4 Editor**: the section lane widget, the panel, preview, undo; then the MCP tools and the
   guide. As built: sections are rows of the toolkit's Dopesheet, not a widget of their own (one
   list on one axis keeps a part's key rows and section rows together; ruled 2026-11-21); each
   track kind says how its keys and sections are edited (`SequenceTrack::KeyChannelCount`,
   `GetSection` and the rest), which the panel and the tools both use; the panel is a pane of its
   own under the viewport beside the animation panel, not a tab; "preview cuts" is not built (the
   editor viewport keeps its own camera, so a cut shows nothing there yet); and pressing play in
   the panel plays no sound (an editor scene has no audio group until it simulates). The MCP tools
   are `sequence_read`, `sequence_edit` (a list of edits) and `sequence_preview`.
6. **Q5 Proof**: rebuild one of the first game's staged cutscenes as a sequence the runner drives
   through holds and events (in that game's repository, with the user), and a small sequence in a
   sample game so Integration covers a cooked one (the dialogue spec's D8 sample, which uses both). Documentation: `Systems/sequencer.md` and a
   section in `Shipping/Scripting.md`.

## Risks

- **Two writers.** A property track and gameplay script writing the same property fight; the
  last writer wins each frame. The documentation says a sequence owns what it binds while it
  plays; the system's phase puts it after behaviors.
- **A clip section against an animation graph.** The overlay plays over the graph; a graph that
  also reacts to its parameters during a cutscene can show through when the overlay blends out.
  Seen in Q3, not designed around ahead.
- **The camera override** must survive the scene being saved during play in the editor and never
  be serialized; it is runtime state of the render scene, not a component field.
- **Audio offset accuracy** depends on the codec; a streamed clip may only seek to a block, so
  the drift threshold has to cover it.
- **A cut within one camera.** A camera that jumps (a transform track with a step key) is still
  the same camera, so its history is kept; its motion vectors then point off screen and TAA's
  rejection has to carry it. If that ghosts, the render can also treat a jump of the view past a
  threshold as a cut. Not measured yet: it is a look, the user's to judge on a built sequence
  (Q5's rebuild has one).
- **Restore across prefab spawns**: an entity spawned by an event during the sequence is not in
  the snapshot. Restore puts back what existed; what the game spawned is the game's.
