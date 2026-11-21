# Sequencer

Cutscene timelines: a sequence drives several entities at once and is played from script. The
design and what is still to come are in [Specs/sequencer.md](../Specs/sequencer.md).

## Layers

| Library | Module | Holds |
|---|---|---|
| `Code/Foundation/Sequence` | `foundation.sequence` | `Sequence` (bindings, markers, tracks), `SequencePlay`, the track base, property and event tracks |
| `Code/Foundation/Sequence.Resource` | `foundation.sequence.resource` | `SequenceSource` (cooked), `SequenceResource`, `SequenceFactory`, `NestedSequenceTrack` |
| `Code/Pipeline/Sequence.Pipeline` | `sequence.pipeline` | `SequenceAsset`, its builder, File > New's creator |
| `Code/Engine/Engine.Sequence` | `engine.sequence` | `SequenceSystem`, `scene.sequences`, `SequencePlay` for script, the domain |
| `Code/Engine/Engine.Render` | `engine.render` (`:sequence`) | `CameraCutSequenceTrack` |

None of the sequence libraries depends on render, audio, skeletal animation or physics: a domain
adds the track kinds that drive it, and registers them with its reflection.

## The model

A `Sequence` names its cast (`bindings`), holds `markers` (a hold stops the playhead until the play
is resumed) and `tracks`. A track refers to one binding by index, or to none. Time is in seconds;
`frameRate` is for the editor's snapping only.

A track kind is a subclass of `SequenceTrack`, a reflected `ISerializable` stored as its type id and
its fields (as particle modules are). `RegisterSequenceTrack<T>()` records it with the type and
serializable registries; `SequenceTrackKinds()` lists them from the type registry, so nothing keeps
a list. A kind gives its `Duration`, an `Order` within a step (properties first, events last),
whether it `NeedsBinding`, `ResolveResources` for the assets it refers to, and `Begin`, which makes a
`SequenceTrackRunner` for one play: `Evaluate(step)` and `Release(restore)`.

Kinds here: `PropertySequenceTrack` (property animation's `PropertyTrack`, through its track targets:
any reflected property, or `Transform`), `EventSequenceTrack` (a name and a text value at a time) and
`NestedSequenceTrack` (another sequence over a section, on the same cast, clamped or looped; a
sequence inside itself is refused).

`CameraCutSequenceTrack` (the render's) makes its bound camera the scene's view while one of its
sections runs (a section's end included, so a play that ends on a cut and holds keeps it). It is a
claim on `CameraComponentManager` (`ClaimView`, `ReleaseView`), which the camera pick reads before
the primary camera; claims are runtime state, never saved. Of the claims held, the play begun
last wins (`SequencePlay::StartOrder`; a nested play answers its outermost play's), and within
one play the shot cut to; letting go passes the view to the claim under it, then back to the
primary camera. The camera knows nothing of cuts: the render starts a view's history fresh (TAA,
SSR, SSGI, exposure, motion vectors) whenever the camera it is drawn from changes
(`ViewSettings::cameraKey`), so a game switching cameras gets the same clean cut.

## A play

`SequencePlay` runs one sequence on one scene; the editor's preview and the game use the same one.

- `Play()` begins the runners and applies time 0; `Advance(dt)` plays forward, firing events over
  `(previous, now]`, stopping at the first hold on the way, wrapping a loop (what is at 0 fires
  again) or finishing.
- `Seek(t)` and `Pose(t)` jump: nothing in between fires; `Pose` is the editor's (no events, no
  sounds).
- The finish: `Release` (side effects let go, moved values stay), `Hold` (everything stays until
  `Stop`), `Restore` (every value back as it was before the play's first write to it; runners
  release last first, so a value two tracks wrote goes back to what was there before both).
- `Bind(name, entity)` during a play begins that name's tracks again on the new entity.
- Destroying a play releases nothing (the scene may be going away); `Stop` releases.

## In a scene

`SequenceSystem` is the scene's player (simulation only, `UpdateOrder` -99: after the behaviors'
-110 and the sequence components' -100). `Play(resource, id, finish, owner)` starts a play; finished plays leave the list unless
they hold; the scene stopping stops them all.

A play with an owner reports to it as entity events (`sequenceEvent`, `sequenceHeld`,
`sequenceFinished`). One without, which is what `scene.sequences.play` starts, publishes a
`SequenceNotice` (the play's id, the sequence's id, a name and a value) on the run's event bus as
`SequenceEvent`, `SequenceHeld` and `SequenceFinished`, so the same `on<Name>` handlers hear them.
An event track bound to an actor posts `sequenceEvent` to that actor.

`SequenceComponent` (manager update order -100, just before the system) starts its play in the
system with its entity as owner and its inspector cast filled in before time 0 (`SequenceSystem::Play`
takes a `prepare` step). A cast row names an entity or "self"; the rows are an array of structs,
which the prefab spawn remaps. Autoplay starts it once per run; `play()` and `stop()` are applied at
the next update; destroying or disabling the entity stops it.

The script surface is in [Shipping/Scripting.md](../Shipping/Scripting.md), under Sequences.
