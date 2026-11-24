# Save slots

> STATUS: APPROVED 2026-11-24. Proposed and revised three times that day: after review (the order of
> slots, names on every platform, cloud sync, copying onto the open slot, which store the dialogue
> plugin writes), then made greenfield with stores handed explicitly (user: no save needs keeping;
> "explicit piping may be better than implicit assumption about global vs slot"), then reviewed again
> (Millbrook green through every phase, the global store's time, save points and `Saves.flush()`,
> the C# side). Sized M, in phases S0 to S3, green after each.
> Origin: the first game needs a title menu (New Game, Continue, Load), and the engine keeps one
> save per game (user, 2026-11-24: "how do other engines do save slots?", then "write the save
> slots spec"). Read CONVENTIONS.md first.

## Goal

A game keeps several saves, each a slot it names, and offers them on a menu: New Game starts an empty
slot, Continue opens the one saved last, Load lists them with what each holds. What every slot shares
(the game's options, a best time over every playthrough) stays in a store of its own. Nothing decides
for a game which store it writes: the game holds a store and hands it to whatever needs one (the
dialogue plugin, its own code), so a playthrough's flags land where the game put them and nowhere
else. A game with no slots uses the global store as its one save.

## What exists today

- `RunSave` (`Engine.Run`, `:save`) is a run's save: one settings file, written whole and
  atomically, holding a `SaveValues` section (typed values by key: int, float, bool, string, a list of
  floats) and the player's control bindings (`InputBindingOverrides`) beside it. An unreadable file is
  set aside as `<path>.unreadable` and the game starts from an empty save. One flag says whether
  anything changed; the bindings mark it too (`ApplicationRun::SetSaveFile` wires them to
  `MarkChanged`).
- The host names the file through `ApplicationRun::SetSaveFile`: the player in the user data directory
  (`GetUserDataDirectory()`), play in editor in the project's ignored `Editor/` folder, both named by
  `project::SaveFileName` (`<project>.save.xml`). The run writes what changed when its script stops
  and when the application goes to the background; `Save.flush()` writes at once. A crash loses what
  changed since the last write. On the web, `PersistUserData()` pushes the user data directory to the
  page's storage after each write.
- The `Save` script facade (static, reflected, so every script language has it) reads and writes
  those values: `has`, `remove`, `clear`, `get`/`set` for each kind, `getFloats`/`setFloats`, `flush`.
  Sky Hopper, Snowline, Lamplight, Masquerade and Millbrook use it on master, and Holdout (the C#
  sample, on the dotnet branch, with the C# bindings) does too.
- The dialogue plugin finds the run's save itself: each read and write of a flag or a conversation's
  place asks `RunSave::Values()` (`DialogueRunHost`); a run with no save keeps them in a scratch store
  for the run.
- Millbrook's title offers Begin (Carry on when a save is under way), New Game (clears the save) and
  Quit; its pause menu leaves to the title.
- The approved online spec (`Specs/online.md`, Decision 6, not built) keeps the save file in a
  backend's cloud: the newer of the cloud and the local copy is read before the run's first read, and
  the file is written to the cloud when `Save` flushes.
- What other engines do (surveyed 2026-11-24): Unreal has slots built in
  (`SaveGameToSlot(object, name, userIndex)`, `LoadGameFromSlot`, `DoesSaveGameExist`,
  `DeleteGameInSlot`), each slot a named save the platform maps to its own storage; Unity and Godot
  have none, and a game writes a file per slot itself. Games keep their settings apart from their
  slots, show a small summary of each slot on a menu, and continue from the one saved last.

## The design

### Stores, held and handed

A **store** is a set of saved values a script holds, as it holds a `Scene`: a `SaveStore` value with
the value calls (`has`, `remove`, `clear`, `get`/`set` for each kind, `getFloats`/`setFloats`,
`flush`), `isValid()` and `name()` (empty for the global store). The `Saves` facade hands stores
out; nothing writes a store it was not given.

- **The global store**, `Saves.global()`: one per game, always there. A game with no slots keeps
  everything in it, as its one save (Sky Hopper's best times, Lamplight's unlocked levels); a game
  with slots keeps what every slot shares in it (options, a count over every playthrough).
- **A slot**, `Saves.openSlot(name)`: a playthrough's values. At most one slot is open for writing at
  a time; opening another writes the open one first (by its rule, below) and leaves its handles doing
  nothing, as a `Scene` handle does after its scene goes.
- **A slot read**, `Saves.readSlot(name)`: a slot's values as they are on disk when it is read, read
  only (its setters do nothing and warn), for a Load menu's summary of each slot. It is a snapshot: it
  does not follow later writes of the same slot, the open one included. Any number may be held beside
  the open slot, one per slot: reading a slot again replaces its earlier read (whose handles go).

A `SaveStore` is a value, as `Scene` and `Entity` are: a handle to a store the run's saves own (in
AngelScript declared without `@`). A copy is the same store, so a write through one is read through
the other. `isValid()` says whether the store is still there. A handle whose store is gone (a slot
closed, deleted or replaced by a copy) reads every fallback and writes nothing, with one warning; it
holds the store's generation as well as its id, so a slot opened again under the same name is never
reached through an old handle.

### Who needs a store is given one

- **The dialogue plugin** keeps its flags and a conversation's place in the store the game gives it:
  `Dialogue.useSave(store)`, before a conversation plays (a title menu does it as it opens the slot).
  Until it is given one, or after its store is gone, it keeps them in its scratch store for the run,
  with one warning: it never picks a store itself. A game with no slots gives it the global store.
- **Anything else that keeps state between runs** takes a store the same way, from script or from
  native code (a `SaveStoreHandle` in C++, what the script's `SaveStore` wraps). The engine itself
  picks a store for one thing only: the player's control bindings, which are the player's settings
  and not the game's data, always in the global store.

### The files

Every game's saves are a folder, `<project>.saves/`, in the user data directory for the player and in
the project's `Editor/` folder for play in editor:

- `global.save.xml`: the global store's values and the player's bindings.
- `slot-<name>.save.xml`: a slot's values.

Each file also holds a `SaveInfo` section: when the store was last saved, in Unix seconds, set at
each write that holds a change and kept by a copy. A slot `openSlot` has just made holds 0 (never
saved) until its first such write, so a New Game left before anything happened is not the slot
saved last. The time comes from the run's saves' clock, which tests set.

Today's `<project>.save.xml` goes; nothing reads it (no save needs keeping, user 2026-11-24).

A slot name is 1 to 32 characters of letters, digits, `-` and `_`, and case does not count: names are
kept in lower case, so `Slot1` and `slot1` are one slot on every platform. The `slot-` prefix keeps a
name such as `con` or `nul` off Windows' reserved file names. A call with any other name does nothing
and returns an invalid store or false, with a warning naming the rule. Since names are folded, they
are not for showing: a game that lets the player name a save keeps that name as a value in the slot.

Each store has its own changed flag, so writing a slot never skips a changed binding and writing the
global store never writes a slot. The bindings mark the global store changed.

### What a script calls

- `Saves.global()`: the global store.
- `Saves.openSlot(name)` and `Saves.openSlot(name, writes)`: the slot `name` open for writing, made
  at once when it has no file (an empty file, never saved, so `hasSlot` and `slots()` see a New Game's
  slot from the start). `writes` is a `SlotWrites`, `AsItPlays` (the default) or `AtSavePoints`
  (below). An invalid store when the name breaks the rule, or the file cannot be read and cannot be
  set aside.
- `Saves.readSlot(name)`: the slot read only; an invalid store when there is none, or its file cannot
  be read (left where it is, with a warning: only opening a slot sets an unreadable file aside).
- `Saves.openedSlot()`: the slot open for writing, an invalid store when none is.
- `Saves.slots()`: every slot's name, the one saved last first (by the time inside each slot, not the
  file's date, so a copy or a cloud sync does not reorder them; equal times by name; never-saved slots
  last). Continue opens `slots()[0]`. A file that cannot be read is left out, left where it is, with a
  warning.
- `Saves.hasSlot(name)`, `Saves.deleteSlot(name)` (deleting the open slot closes it first, unwritten).
- `Saves.copySlot(from, to)`: a backup, or a save-as. The copy keeps the source's time, so it does not
  become the slot saved last. When `from` is open as it plays its changes are written first (one
  open at save points is copied as its last save point left it); a copy over an existing slot
  replaces it (its handles go); a copy **onto the open slot is refused** (false and a
  warning: the open slot's next write would undo it). To restore a backup into the slot in play:
  close it, copy, open it.
- `Saves.closeSlot()`: writes (by its rule) and closes the open slot; its handles go.
- `Saves.slotTime(name)`: when the slot was last saved, in Unix seconds (0 when there is none), shown
  against `Time.unixNow()` ("2 hours ago") or with `Text.formatDate(seconds, pattern)`, the local
  date and time by a pattern (`YYYY-MM-DD hh:mm`, `DD/MM/YYYY`; the tokens are YYYY, MM, DD, hh, mm,
  ss).
- `Saves.flush()`: writes every store that changed, except a slot opened `AtSavePoints`, which only
  its own `flush()` writes: a save point is the game's call on that store, never a general flush.

The static `Save` facade goes: a script holds a store (`Saves.global()` where it wrote `Save` before).

### When a store is written

- **The global store, and a slot opened as it plays** (the default): what changed is written when the
  run's script stops, when the application goes to the background, and on `flush()`. A crash loses
  what changed since the last of those; a game that wants less at risk flushes at its checkpoints.
- **A slot opened `AtSavePoints`:** written only on its own `flush()` (a save point, a menu's Save).
  What changed since is dropped when the run stops, the slot closes or another opens: quitting
  without saving loses it, as such games intend.

Every write is whole and atomic, an unreadable file is set aside as the save's is today, and on the
web each write and each delete pushes the user data directory to the page's storage.

### Cloud saves

A backend with `cloudSaves` (online.md, Decision 6) keeps the saves folder in its cloud as files.
Each write of a store is carried there, and so is each delete, when it is made (`deleteSlot` removes
the cloud copy too). Before the run's first read the backend brings each file in by the newer of the
cloud and local copy, by the time inside the store (the global store's too) rather than a file date;
a slot only in the cloud is fetched. The slots' order is that time, so a sync does not change it. A
delete has a case this does not settle: a machine that was offline when a slot was deleted elsewhere
still has it, and would bring it back. O4 settles it (a record of deleted slots in the global store
is the likely way). S0 adds a paragraph to online.md's Decision 6 saying all this; its O4 phase
builds it.

### The hosts and the editor

- The player and play in editor name the saves folder (`project::SavesFolder`, in place of
  `SaveFileName`); nothing else about saves is a host's.
- Play in editor's saves stay in `Editor/`, apart from a player's. Testing a beat of a story needs a
  save set just before it, which today means editing the file by hand. MCP tools read and write play
  in editor's saves: `save_list` (the global store and each slot, with their times), `save_read` (one
  store's values as JSON, by slot name, or none for the global store), `save_write` (sets and removes
  values, or writes a whole store; a slot it makes gets the current time), and `save_delete`. On the
  editor host they are refused while a Game tab plays (the run would write over them); the headless
  host has no Game tab, so there they always work.
- The editor's Game tab has a **Saves** menu: open the saves folder, and clear them all (asking
  first), for a quick new game.
- The sample games' `Editor/` saves and the flagship's are test data: they start empty under the new
  layout.

### Millbrook, the demo

Millbrook's title becomes the saves demo, three slots:

- **Continue** (shown when a slot exists) opens the slot saved last; **New Game** opens the first
  empty slot, or, with all three used, asks which to replace; **Load** lists the three, each with its
  summary (read with `readSlot`: who the player has met, the errands done, "saved 2 hours ago") and a
  Delete; **Quit**.
- Opening a slot hands it to the dialogue plugin (`Dialogue.useSave`), then puts the village back as
  the slot has it. Leaving to the title closes it.
- The global store keeps what spans playthroughs: how many times the fair has opened, shown on the
  credits.
- Millbrook's README and its tools' scripts say how, as the place a game author looks for it.

## Decided by this spec (for review)

1. A store is a value a script holds; the game hands it to whatever needs one. Nothing picks a store
   for a game, except the engine's own bindings, which live in the global store.
2. The dialogue plugin keeps its state in the store it is given (`Dialogue.useSave`), and in scratch
   for the run until it is given one.
3. A game with no slots uses the global store as its one save; there is no project setting.
4. One slot is open for writing at a time; read-only slot stores for menus are as many as wanted.
5. A slot is a file (`slot-<name>.save.xml`, names in lower case), in one folder with the global
   store: what platforms and cloud storage expect, and deleting or copying one never rewrites the
   others.
6. Continue is the slot saved last, by a time kept inside each slot; a copy keeps its source's time.
7. A slot is written as the game plays, or only at its save points (its own `flush()`, never
   `Saves.flush()`), chosen when it is opened with a `SlotWrites`.
8. Copying onto the open slot is refused; a new slot exists from `openSlot`, never saved until its
   first change is written.
9. The static `Save` facade and today's single file go (greenfield: no save needs keeping).
10. Play in editor's saves get MCP tools, so a test save is written by a tool, not by hand.

## Order of work

- **S0, the stores:** the saves folder (`project::SavesFolder`, `SaveFileName` gone); the run's saves
  with the global store, the open slot and slot reads, a changed flag each, the handle type and its
  going stale; the `SaveInfo` time from a clock the tests set; open (making a new slot's file, never
  saved), close, list by the time inside, delete, copy (keeping the time, refused onto the open slot),
  the time, flush; the name rule; both write rules; setting aside on open, skipping on list and read;
  the web push; the bindings in the global store. Online.md's Decision 6 paragraph.
  Engine.Run.Tests (on the set clock, no sleeps): a slot's values apart from the global store's; a
  binding change written with a slot open; opening another slot writes the first and stales its
  handles; order by the time inside, with a copy not reordering and a never-saved slot last; `Slot1`
  and `slot1` one slot; a refused name; an unreadable slot set aside on open, skipped by `slots()`;
  a slot read not following a later write; at save points, a stop and `Saves.flush()` drop or leave
  what the slot's own flush did not write.
- **S1, the facades and the dialogue plugin:** `Saves`, `SaveStore` and `SlotWrites` (reflected),
  `Time.unixNow`, `Text.formatDate`; `Save` gone, every sample's scripts moved to `Saves.global()`
  in the same commit (Sky Hopper, Snowline, Lamplight, Masquerade, and Millbrook's smallest port:
  the global store and `Dialogue.useSave` on it, its title unchanged), so each stays playable and
  Integration.Plugins still reads and cooks Millbrook; `Dialogue.useSave` and the plugin's scratch
  until given one. Owed on the dotnet branch when it is next rebased: the C# bindings of `Saves` and
  `SaveStore`, and Holdout's script moved off `Save`. Integration tests on AngelScript and Luau: New
  Game, Continue and Load as a title menu does them; a stale handle; Integration.Plugins: the
  dialogue's flags in the store it was given and nowhere else, in scratch before it is given one.
  Scripting.md's save section, the dialogue guide, the CHANGELOG (the new layout and API, and that a
  save from before is not read); the docs site's pages on saving are owed with it (that repository,
  not this one).
- **S2, the editor:** the `save_*` MCP tools (McpGuide.md, the tool count), the Game tab's Saves
  menu. Integration.Mcp: a slot written by `save_write` is what a play in editor opens.
- **S3, Millbrook's title:** the slots title above, its scripts and tools, its README;
  Integration.Plugins' Millbrook check still reads and cooks it. Played through by the user in the
  editor and the player: three playthroughs side by side, Continue, Load, Delete, a replace.

## Risks

- A slot changed while a scene still shows the last slot's world is the game's to handle: a title
  menu opens a slot, hands it on, then loads its scene. A conversation playing across `openSlot`
  would lose its store mid-way; the guide says to stop it first.
- A game that forgets `Dialogue.useSave` plays with flags that last one run. The warning names the
  call, and Millbrook shows it.
- A slot read reads a whole slot file. Slots are a few kilobytes; a menu of a dozen reads each once.
- The web's storage has a quota per site. Slots there are small, and a write that fails is reported
  as today's are.
