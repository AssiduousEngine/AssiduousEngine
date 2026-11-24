# Input

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[input-subsystem]]

Named-action input as data: gameplay reads actions ("Jump", "Move", "Fire"), never raw keys.
Bindings are a cooked asset, rebindable + persistable at runtime, organized into priority-ordered
action sets, with declared value kinds (Button / Axis1D / Axis2D), interactions (Hold/Tap/DoubleTap),
UI-vs-game arbitration, play-in-editor forwarding, touch controls, and a script facade. Per-player
device pairing is not built.

## Modules

- **`foundation.shell`** (`Code/Foundation/Shell`) - the RAW device layer: the
  tagged `InputEvent` stream, `IKeyboard`/`IMouse`/`IGamepad`/`ITouch` facades, and
  `InputSurface`/`InputRouter` for transformed + gated viewport input. `KeyCode::Back` is the back
  button (a phone's, or a keyboard's or browser's Back key); on Android SDL traps the system back
  button and delivers it as this key. `IWindow::SafeArea()` is the part of the window no system UI
  covers (a phone's status and navigation bars, a camera cutout), in window pixels, the whole window
  on a desktop.
- **`foundation.input`** (`Code/Foundation/Input`) - the ACTION layer: the `InputMap` data model +
  `ActionRuntime` (evaluation, sets, edges, aggregation, processors, interactions) + the consumption
  mask + rebind + user-overlay persistence. Also `InputKindTracker` (the kind of input in use:
  pointer, keyboard or pad) and `ScriptedInputSource` (a timeline played through virtual devices,
  read from JSON by the playtest tools, so a playtest goes through the project's input map).
- **`foundation.input.resource`** (`Code/Foundation/Input.Resource`) - the cooked `InputMapResource`.
- **`input.pipeline`** (`Code/Pipeline/Input.Pipeline`) - `InputMapAsset` + its builder.
- **`engine.input`** (`Code/Engine/Engine.Input`) - `InputSubsystem` (runtime tick + the
  `IInputSourceProvider` seam + the `Input` script facade, registered by the input domain) and
  `FittedInputSource` (another source's devices with the pointer and touches mapped into a game's
  fitted render space, turned back for a pre-rotated present).
- **`editor.input`** (`Code/Editor/Editor.Input`) - `InputMapPage`.

## Data model (the asset)

`InputMap` (one asset = one game's bindings) -> `ActionSet[]` (name + priority) -> `Action[]` (name +
declared `kind`: Button / Axis1D / Axis2D) -> `Binding[]` (key / mouse-button / mouse-axis /
gamepad-button / gamepad-axis / gamepad-stick / composite-2D / touch). Per-action processors
(smoothing sensitivity/gravity/snap, response-curve exponent, a time-scale flag with mouse-delta
NEVER time-scaled) and interactions (`Interaction`: `Hold` / `Tap` / `DoubleTap`, Button actions
only). Kinds are DECLARED, not inferred; binding-to-kind mismatches validate at save + cook.

## Runtime evaluation (`ActionRuntime`)

Fed per frame from the shell event stream + polled device facades:
- **Per-device aggregation** (Godot): pressed OR-folds, strength MAX-folds across devices/bindings;
  `raw` bypasses processors.
- **Edges** (`WasPressed`/`WasReleased`) against a frame counter, from 1 (an action that never
  moved has no edge). Inside a scene's fixed step (`core::CurrentFixedStep`), an edge belongs to
  the first step of a frame and covers every frame since the scene's step before it: a press is
  seen once however many steps a frame runs, none included. A key or button pressed and released
  inside one frame reads as down for that frame (the latch's Pressed), so a quick tap is a press.
- **When:** the input subsystem evaluates its runtime and every run's (`IRunInput`, which a game
  instance offers) in its `BeginFrame` at order -620: after the GUI marks what it took, before
  physics and the scenes step and update.
- **Queries** on a name-hash-resolved `ActionRef`: `IsDown` / `WasPressed` / `WasReleased` / `Value` /
  `Value2D`, plus synthesized `Axis(neg,pos)` / `Vector2(...)` with circular dead zone.
- **Set stack**: all enabled sets evaluate; queries resolve by priority. Pushing an exclusive set
  (a pause menu -> "Menu") makes other sets read released, with release-edges firing once and
  held-then-suppressed actions latched until physically released. An action two sets share answers
  from the exclusive set. `IsKeyBound(key)` says whether an action of the sets in effect listens to a
  key.

## UI-vs-game arbitration

`ActionRuntime::SetConsumptionMask` is driven each frame by `Engine.GUI`'s `GuiSubsystem`: the GUI
reports which input CLASSES (pointer / keyboard / text) it took, and matching binding sources are
masked for that frame on every runtime (`InputSubsystem::ForEachActionRuntime`: its own, then each
run's). An exclusive set handles modal states; while a text field has focus (`WantsTextInput`) ALL
key bindings are suppressed. The back button: when neither the GUI (a popup or dialog closing, a
screen's `back` handler) nor an action of the sets in effect takes it, `GuiSubsystem::BackUnhandled`
is set and the host hands it to the platform (`IShell::PassBackToPlatform`, Android's back).

## Play-in-editor

`ActionRuntime` takes its devices through the `IInputSourceProvider` seam: the game host passes the
raw shell, play-in-editor passes the viewport's gated/transformed `InputSurface` (hover = mouse,
focus = keyboard, click-to-focus) - so keyboard + gamepad forward from the editor viewport by
construction.

## Resources + rebinding

- **`InputMapResource`** (cooked, write-through: validate + serialize) - `resource::Ref`; the runtime
  binds the project's `defaultInputMapId` at startup.
- **User rebind overlay** - NOT part of the asset. `InputBindingOverrides` (a `foundation.settings`
  section) stores per-action replacement bindings. A run keeps it in its global store, beside the
  values every playthrough shares (`SaveFile::Bindings` of `RunSaves::Global`), and `PlayerBindings` lays it over the game's map when the map
  is installed or the save opens, and over one action at each rebind, without resetting the sets
  (`ActionRuntime::ReplaceBindings`). A rebind replaces the action's binding for one kind of
  device (keyboard and mouse, or pad) and keeps the rest; a capture (`BeginCapture`, polled before
  the runtime's update) holds every action released, as a disabled set does, until the press.
  Reset = clear the overlay (the asset stays pristine).

## Editor + scripting

- **`InputMapPage`** - tree of sets -> actions -> bindings (add/remove/reorder, undoable), with a
  per-row **Listen** capture (must-have / must-not-have capability filters so an axis rebind ignores
  keys) and inline validation (duplicate bindings within a set, kind mismatches, empty actions).
- **`Input` facade** (each script reaches its own run's `ActionRuntime`, a run service;
  AngelScript `Input::`, Luau `Input.`): `isDown` / `wasPressed` / `wasReleased` /
  `value` / `valueX` / `valueY` / `value2D` (a Float2) by action name, `pushSet` / `popSet` /
  `enableSet`, and `describe(action)` / `describe(action, set)`, the name of the control the action
  takes on the kind of input in use (for prompts). Rebinding: `rebind(action[, set])` takes the
  player's next press, `isRebinding()` / `cancelRebind()`, `setBinding(action, control[, set])` by
  name (`"Key.Space"`, `"Mouse.Left"`, `"Pad.South"`, `"PadAxis.RightTrigger"`),
  `resetBinding(action[, set])` and `resetAllBindings()`; each saves with the game's save.
  **Rumble**: `rumble(low, high, seconds)` runs gamepad 0's motors (`low` the heavy, low-frequency
  one, `high` the light, high-frequency one, each 0 to 1), `rumble(gamepad, low, high, seconds)` a
  pad by index, `stopRumble()` every pad. A request waits for the runtime's next update and reaches
  the pad through that frame's devices, so a runtime holds no device and each run (an editor Game
  tab, the player) rumbles only its own; a later request for the same pad replaces it. A run's end
  stops its pads (`ApplicationRun::StopRunBehavior`). Rumble is not gated on the Game tab's focus: asking
  an unfocused pad to stop has to work.
  **Pointer lock** (mouse look): `lockPointer(true)` hides the pointer and holds it in place, so
  only its movement is read (bind `MouseDelta` to a look action); `lockPointer(false)` frees it, and
  `pointerLocked()` says whether it is locked now. Like rumble, the ask waits for the runtime's next
  update and reaches that run's own mouse: the window in the player, the editor's window from a
  Game tab. A browser grants a lock only on a user action, so on the web the lock takes effect at
  the next click on the canvas; its Escape ends it, `pointerLocked()` then reads false (a game shows
  its pause) and the next click locks it again until the game frees it. A run's end frees a pointer
  its game locked.

## Touch + portability

Touch bindings shipped (TouchButton regions + a floating TouchStick), demonstrated on the Steam Deck
in the InputActions sample and played on an Android phone in Sky Hopper. A TouchDrag reads, each
frame, how far the touch it captured (one that came down in its region) moved, a fraction of the
screen times its scale: touch look, the mouse delta's counterpart; it is not drawn. Touches arrive in the
mouse's space (normalized on the web); `Engine.GUI`'s `GuiTouchGuide` draws each run's touch
controls (`ActionRuntime::ForEachTouchControl`). The player keeps a game's frame, screens and
pointer inside the window's safe area. The Android player (`PLATFORM_ANDROID`) runs the same SDL
shell. `ENV_INPUT_TRACE` logs what the input path saw, for a device no debugger reaches. SDL scancodes are physical (layout-independent matching; localized labels
via keycode translation). WASM delivers the same events; the runtime tolerates device counts changing
any frame (hotplug + Gamepad-API gesture activation).

## Deferred

Per-player device pairing (`PlayerInput`), action-triggered haptics, per-scene/split-screen input:
`Documentation/Archive/input-followups-history.md` keeps the detail.

---

Design rationale (the reference survey - ezEngine / Godot / Flax and others - and the open questions)
is in `Documentation/Archive/input-design-history.md`.
