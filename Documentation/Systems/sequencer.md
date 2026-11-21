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
| `Code/Engine/Engine.Render` | `engine.render` (`:sequence`) | `CameraCutSequenceTrack`, `FadeSequenceTrack` |
| `Code/Engine/Engine.Animation` | `engine.animation` (`:sequence`) | `ClipSequenceTrack` |
| `Code/Engine/Engine.Audio` | `engine.audio` (`:sequence`) | `SoundSequenceTrack` |
| `Code/Editor/Editor.Sequence` | `editor.sequence` | `SequenceDocument`, `SequencePreview`, `SequenceSource`, `SequencePanel` |

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
`SequenceTrackRunner` for one play: `Evaluate(step)`, `Release(restore)` and `SetPaused(paused)`
(told when the play pauses or resumes, not at a hold; a sound's voice is what needs it).

Kinds here: `PropertySequenceTrack` (property animation's `PropertyTrack`, through its track targets:
any reflected property, or `Transform`), `EventSequenceTrack` (a name and a text value at a time) and
`NestedSequenceTrack` (another sequence over a section, on the same cast, clamped or looped; a
sequence inside itself is refused).

`CameraCutSequenceTrack` (the render's) makes its bound camera the scene's view while one of its
sections runs (a section's end included, so a play that ends on a cut and holds keeps it). It is a
claim on `CameraComponentManager` (`ViewClaims()`, a `SceneClaims`), which the camera pick reads before
the primary camera; claims are runtime state, never saved. Of the claims held, the play begun
last wins (`SequencePlay::StartOrder`; a nested play answers its outermost play's), and within
one play the shot cut to; letting go passes the view to the claim under it, then back to the
primary camera. The camera knows nothing of cuts: the render starts a view's history fresh (TAA,
SSR, SSGI, exposure, motion vectors) whenever the camera it is drawn from changes
(`ViewSettings::cameraKey`), so a game switching cameras gets the same clean cut.

`ClipSequenceTrack` (the animation's) plays skeletal clips on the bound entity, a section at a time
(of two overlapping, the later): a clip, where in it to start, a speed, a length (0: the clip once),
blend in and out, and whether it loops over the section. The track drives the entity's
`PoseOverlay` (`DriveOneShot`: the clip time and weight each step, which the player's own tick
leaves alone), so a seek lands on the frame, while the graph or clip underneath keeps running and
shows again as a section blends out. Between sections the overlay lets go; a play stopped
mid-section eases back over the section's blend out. `EntityOverlay(scene, entity)` finds the
overlay (the graph's, else the skeletal animation's) each step, since a player is rebuilt when its
skeleton changes.

`SoundSequenceTrack` (the audio's) plays audio clips in the scene's audio group, a section at a time:
a clip, where in it to start, a length (0: the clip once), a volume, a bus and whether it loops.
Bound to an entity, the voice is spatial at it and follows it; bound to no one, it is flat. A
section entered part way (a seek) starts its voice at that point (`AudioPlayParams::startSeconds`);
a voice more than `kSoundDriftSeconds` (0.1 s) from the playhead is moved back to it
(`AudioEngine::SeekVoice`), and a jump moves it at once. The voice pauses with the play, and plays
on at a hold, so a line of dialogue finishes while the game waits for the press. The editor's pose
plays no sound; the play letting go stops the voice.

`FadeSequenceTrack` (the render's, bound to no one) mixes a colour over the scene's whole view by
an amount its keys give, from the play's first step until it lets go (the last key's amount holds
after it). It is a claim on `PostProcessSystem` (`FadeClaims()`), won as camera claims are; the
view's post config carries the winner (`ViewPostConfig::fade`) and the tonemap mixes it over the
graded image, so the HUD and world-space UI draw over it (subtitles stay on a fade to black).

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

## The editor

The sequence panel (`SequencePanel`, markup `Data/Editor/SequencePanel.xml`) sits under the scene
page's viewport beside the property animation panel. Its toggle on the scene bar, Scene > Sequence
Panel (`scene.view.sequencePanel`) or a sequence component's Edit Sequence shows it; selecting an
entity never does. Edit Sequence opens the component's sequence with the panel's cast filled from
the component's (`ISceneEditorPage::EditSequence(sequence, owner)`), and a sequence asset opened
while the page is on screen opens there too.

- **The document** (`SequenceDocument`) is the asset's, with its own history and unsaved mark, as a
  clip's is. Each step keeps the sequence as serialized bytes (its tracks are shared objects a copy
  would alias); an edit that changes nothing leaves no step; `RecordEdit` records an edit made in
  place (the inspector's) from the sequence as it last stood. Every change rebuilds the tracks and
  moves `Version()` on.
- **The source** (`SequenceSource`) shows the document in the dopesheet, the curve view and the
  ruler through each kind's own editing calls (`SequenceTrack::KeyChannelCount`, `GetSection` and
  the rest), so a kind a domain or a plugin adds edits with no list to extend. Rows go by part,
  then track (its name, its property, or its kind's name), then channel; a track with no key
  channels is one row of section bars; tracks bound to no one sit under "Sequence". Moves, trims
  and curve edits are gestures that preview from where they began and commit as one step.
- **The cast** is the panel's own: Cast fills a part with the scene's selected entity, or adds a
  part named for it.
- **Track** offers every registered kind (made through the serializable registry, as a sequence
  being read makes them), and a property track for each of the chosen part's animatable properties
  without one, which starts with what the entity shows at the playhead. Section adds a section at
  the playhead to the chosen section track; Marker and Hold add a marker or a hold there.
- **The inspector** beside the timeline edits the chosen track's reflected fields (a section's
  clip or sound, an event's name, a fade's colour) in place, each edit one step.
- **The preview** (`SequencePreview`) poses the scene through the game's own play
  (`SequencePlay::Pose`: no events, no sounds) with a restoring finish, so stopping it puts the
  scene back. It stops before the page saves the scene or simulates (and poses again after a
  save), when the panel hides, and while the scene simulates. The editor's viewport keeps its own
  camera, so a camera cut changes nothing there; a fade shows.
