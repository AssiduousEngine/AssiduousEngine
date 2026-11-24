# Property tracks with open targets

> ARCHIVED 2026-11-24, fully built on master (the sources, the players, the editors); what it left
> open is in Documentation/Plans/weekly_backlog.md ("Left over from the property targets spec").
> Non-authoritative: the original spec, kept as the record of what was built and why; the
> authoritative description is Documentation/Systems/sequencer.md, under "What a property track
> keys".

## Goal

A sequence can change the light of a whole scene over time: the ambient, the image light, the sky,
the fog, the post-process. Today it cannot. A property track keys a property of an entity's
component, and a scene's environment and post-process are not components: they are scene systems
with settings of their own. The first game's staged moments all need it (a rift that drains the
light from the sky, a dusk and a night falling), and today the game does it by hand in script, a
lerp over eleven numbers each frame.

The gap is in the property track's design: its target was taken to be a component by design, so its
binding, its editing rows and its warnings all assume an entity. Adding scene settings as a second
case would repeat the mistake one level up (the next thing worth keying, a music bus's volume under a
cutscene or a material instance's parameter, would be a third case). This spec opens the target: a
**target source**, registered by whoever owns the thing keyed, resolves a track's target on a scene.
The engine registers two, an entity's components and a scene's settings; a domain or a plugin adds
its own with nothing in property animation to change. Both users of the property track get it:
sequences and property animation clips.

## What exists today

- Property animation's `PropertyTrack` names its target as `componentType` and `propertyPath`, with
  its keys. `ResolveTrackTarget` binds it on a scene (the component's manager and the property
  chain; `Transform` is the entity's own transform), and `ReadTrackTarget`, `WriteTrackTarget` and
  `ApplyTrack` read and write it on an entity. Two players use it: a property animation clip on an
  entity (`PropertyAnimatorComponent`) and a sequence's `PropertySequenceTrack`, bound to one entity
  of the cast.
- Sequence track kinds are open the same way this spec wants targets to be: a kind is a concrete
  subclass of the reflected `SequenceTrack`, registered with the type registry
  (`RegisterSequenceTrack<T>()`), listed from it (`SequenceTrackKinds()`), and gone when the domain or
  plugin that registered it unloads.
- A scene system with settings says so through `SceneSystem`: `SettingsId()` (`environment`,
  `postprocess`, `physics`, `navigation`, `sceneBehavior`), `SettingsType()`, `SettingsInstance()`;
  one that can take its values from a shared profile also answers `SettingsProfile()`,
  `UseSettingsProfile(id)` and `CopySettingsProfileIntoScene()` (the values in effect become the
  block's own, in use, the profile kept a pick away).
- The editors offer a property track per animatable leaf of a component type
  (`CollectAnimatableProperties`: f32, Float3, Color, Quaternion, nested structs by dotted path).

## The design

### A target source

`PropertyTargetSource` is an interface in property animation. A domain or a plugin registers an
instance of its own (`RegisterPropertyTargetSource`) as it loads and takes it back by id
(`UnregisterPropertyTargetSource`) as it unloads, as its track kinds come and go; the registry finds
a source by its `Id()` (`FindPropertyTargetSource`) and lists them (`PropertyTargetSources()`). It
answers:

| Call | What it does |
|---|---|
| `Id()` | the stable name a track saves (`component`, `settings`) |
| `NeedsEntity()` | whether a track of it is played on an entity |
| `Resolve(scene, target)` | finds a target name on a scene: what the source keeps to find the instance again, and the reflected type the path walks (the property chain and the kind are checked once, for every source) |
| `Read` and `Write` (scene, resolved, entity) | the value through the property chain, the instance found again every time (pools move); a source writes in its own way (the transform is read, changed and set back through the scene, so the world matrix follows) |
| `BeginWrites(scene, resolved)` | what a source does before a player's first write to a target, and an object the player keeps and tells `Restored()` once it has put back what it changed (the settings source's profile, below); nothing by default |
| `Targets(scene, entity)` | for the editors: the target names it offers on this scene (and entity, when it needs one), each with its reflected type, from which `CollectAnimatableProperties` lists the leaves |

`PropertyTrack` names its target by `source`, `target` and `propertyPath` (`componentType` becomes
`target`, under the source `component` when the source is unset). `TrackTarget` keeps the source and
what it resolved; `ReadTrackTarget`, `WriteTrackTarget` and `ApplyTrack` go through it and stay one
path for every source. A track whose source is not registered (a plugin's, not loaded) is not
resolved and says so, naming the source, like an unknown component today.

### The two sources the engine registers

| Source | `target` names | Resolves to | Needs an entity | Registered by |
|---|---|---|---|---|
| `component` | a component type, or `Transform` | the type's manager and the property chain; the transform through Get/SetLocalTransform as now | yes | property animation |
| `settings` | a settings id (`environment`) | the scene system whose `SettingsId()` it is, the chain on its `SettingsType()` | no | property animation (it needs only `SceneSystem`) |

Each is registered where the knowledge lives, with no list: a third (an audio bus's volume, a GUI
property) would be the audio's or the GUI's own registration.

### Writing a scene's settings

- A write goes to the scene's own block (`SettingsInstance()`), never to a profile: a profile is
  shared by every scene that uses it, and a track lights one scene.
- **A system on a profile.** `BeginWrites` on a system whose values come from a profile
  (`SettingsProfile()` not nil) makes the values in effect the scene's own
  (`CopySettingsProfileIntoScene()`), so the scene looks the same until the keys move it, and the
  writes show; the object it returns remembers the profile. A player calls it once per track per
  play, after keeping the value it found.
- **The finishes.** `Release` and `Hold` leave the scene on its own values as the track left them (a
  rift's light stays when its sequence ends). `Restore` puts back each value read before the play's
  first write, then tells the source's object, which gives the system its profile again
  (`UseSettingsProfile`).
  Runners release last first, so a system two tracks wrote goes back to its profile once, after
  both. A play never writes the scene file: in the editor, the preview's finish is `Restore` and the
  page stops it before a save, as for any property.
- **A property animation clip** with a settings track writes the scene's settings while its entity's
  animator plays (a lamp whose clip pulses the fog), under the same calls. The animator never puts
  back what it writes, for any target, so a scene it takes off its profile stays on its own values;
  the editor's clip preview restores, as a sequence's does.

### Bindings and the cast

A sequence's property track needs a binding when its source does: `NeedsBinding()` answers from the
source, so a settings track is bound to no one, never warns for an empty binding, and sits under
"Sequence" with the other unbound tracks.

### The editors

- The sequence panel's Track menu offers, for the chosen part, every source that needs an entity
  (the entity's components, as now), and always every source that does not (the scene's settings
  systems, labelled `Scene:`, on no part: the panel picks a part whenever the cast has one, so
  offering them only without one would rarely show them). Each target's leaves come from `Targets`
  and `CollectAnimatableProperties`, starting with the value in effect at the playhead (a source's
  `ReadShown`: a settings block on a profile shows the profile's values).
- The property animation panel offers both, the entity's and the scene's.
- A row is named `<target>.<path>` (`environment.ambientIntensity`); keys and curves edit as any
  property track's (the same editing calls).

### Agents and the saved form

- `sequence_read`, `sequence_edit` and the property animation tools read and write `source` and
  `target` by their reflected paths; `McpGuide.md` says what a source is and names the two.
- `componentType` is renamed, so the property animation clip asset and the sequence asset bump their
  data versions and read the older payload's `componentType` as a `component` target (the project's
  migration, as any field rename).

## Decided by this spec (for review)

1. A property track's target is open: a registered source resolves it. Not a kind of its own, not a
   closed list of scopes.
2. The scene's own block, never a profile's; a system on a profile is detached into the scene on
   the first write and given its profile back by `Restore`.
3. Every settings system is a target: `physics` and `navigation` resolve too (gravity would animate).

## Order of work

1. **The sources.** `PropertyTargetSource`, its registry; the `component` source
   (today's resolving, moved behind the interface, no change in what it does); the `settings`
   source with the profile's detach; `PropertyTrack`'s `source` and `target`, the data versions and
   their migration. Tests in `PropertyAnimation.Tests` on a scene with a settings system of the
   test's own (with and without a profile): a settings track resolved and written; an unknown
   target, a wrong path, a wrong kind and an unregistered source each reported; the detach on the
   first write; a source of the test's own registered, resolved, listed and unregistered; an old
   clip and an old sequence read as component targets.
2. **The players.** The sequence's property track bound to no one for a source that needs none,
   each finish (`Release`, `Hold`, `Restore`, a profile given back once after two tracks), and a clip
   on an entity keying a setting. Tests in `Sequence.Tests` and `Engine.Animation.Tests`;
   `Documentation/Systems/sequencer.md`, the property animation document and the scripting guide's
   Sequences part say what a track can key and how a domain adds a source.
3. **The editors and the guide.** The two panels' targets from the sources, the rows, the preview
   posing and restoring them; an editor test adding a settings track in each panel and posing it;
   `McpGuide.md`.

## Risks

- **A setting that is costly to change.** Some are read once into something baked: the comment on
  `skyIntensity` says it is baked into the environment cube. Step 1 checks what each environment
  field costs when it changes every frame (a re-bake each frame is a finding to report and fix,
  not a reason to refuse the key), and the systems document says which fields are cheap to animate.
- **Two plays on one target**: the later play's write lands, as for properties.
- **A detached scene saved.** Only a game's run can leave a scene detached (`Release`), and a run's
  scene is not written back; the editor restores before a save. A test pins the editor's side.
- **The component source moved behind the interface**: what every clip and sequence does today goes
  through it, so step 1 keeps the existing property animation and sequence tests passing unchanged
  as the proof that nothing moved.
