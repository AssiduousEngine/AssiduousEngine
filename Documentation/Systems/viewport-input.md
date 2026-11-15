# Viewport Input

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[viewport-input]]

One mechanism for routing input into a content surface placed in a window through a fit transform -
covering both the editor's embedded-game panel and the runtime fit mode with a single path. Shipped
in `foundation.shell` (`Code/Foundation/Shell/InputSurface.cppm`, the `:surface` partition) over
`foundation.core`'s `ContentFit`; the GUI's `foundation.gui.viewport::Viewport` owns an
`InputSurface` (the old `foundation.ui.viewport` was deleted 2026-10-28).

## The unified problem

The editor embedded game (game renders to an offscreen target shown in a panel; input restricted to the
panel + remapped into game-content space, with focus so keyboard/gamepad reach the game only when the
panel is active) and the runtime fit mode (game/UI rendered at a design resolution, placed into the
window with a letterbox/stretch fit; pointer remapped through the fit, bars not hitting content) are the
SAME problem: a content surface at some region of a window, through a fit transform, that owns
focus/capture and remaps input into content space. Sedulous built two converging paths
(`GameMouseAdapter` + `EngineCanvasMouseAdapter`); this is one.

## The layer

```
OS events
  -> SDL pump: tag each input event with windowID
  -> per-window device state + a per-window EVENT STREAM (the source of truth)
  -> InputRouter: focus / capture / hover ownership + dispatch order
  -> InputSurface: ContentFit transform + gate; maintains a polled snapshot
  -> consumers: poll surface->Mouse()/Keyboard()/... (drop-in), or receive events (UI)
```

- **Per-window tagging + focus** (`foundation.shell`): the SDL pump routes each input event to
  per-window device state keyed by `windowID`; `IInputManager` exposes `HoverWindow()` (the mouse
  routing authority) and `FocusedWindow()` (the keyboard/gamepad target). The editor's floating
  panels in OS windows rest on it. The raw untransformed manager stays for global input; surfaces
  layer on top.
- **Event-first with a poll snapshot** (`InputTypes.cppm`): events are the source of truth (a
  capture/target/bubble dispatch pipeline + hover enter/leave, for the retained-mode GUI); the device
  interfaces are a SNAPSHOT the surface maintains FROM that stream, so poll and event views never
  disagree.
- **`InputSurface`**: a rectangular slice of a window (a `ContentFit`) that presents gated +
  transformed `IMouse` / `IKeyboard` / `IGamepad` / `ITouch` (`Mouse()` / `Keyboard()` /
  `Gamepad(i)` / `Touch()`). Window-space positions map through the `ContentFit` into content space
  (`ContentMouse()`, `ContentDelta()`); letterbox bars return "no hit." A consumer (the editor's
  `EditorCamera`, a sample's `FlyCamera`) polls `surface->Mouse()->X()` instead of the raw manager and
  is otherwise unchanged (drop-in), so there is no per-consumer viewport code. `SetFit`,
  `SetRegion`, `SetContentSize`, `SetFitMode` and `SetRotation` retarget it; `SetWindow` moves it
  to another OS window (a panel floated out). A surface may ask for the pointer locked while it
  holds a press (`SetPointerLock`, for a camera's look or orbit).
- **`InputRouter`**: the single owner of hover, focus and capture across its surfaces, consulting
  `HoverWindow()` + the event stream once a frame (`Update`) and pushing each surface's gate. One
  surface is hovered, one focused (on a click, or following hover with `SetFocusFollowsHover`), and
  one captures the pointer while a button is held; it grants and takes back the pointer lock.
  `SetExternalCapture` lets an overlay swallow the mouse or keyboard for a frame.

## In the GUI and the editor

`GuiHost` (`foundation.gui.hosting`) keeps two routers: one for its windows' panes and one for the
surfaces of hosted content. A `Viewport` is hosted content: once handed to the host it makes its
`InputSurface` on its window and the host places it over the node each frame, so a camera reads the
surface's devices while the node is hovered or focused, and a drag begun in it keeps them past its
edge. The scene page's viewport flies its `EditorCamera` this way. The Game tab adapts its
viewport's surface to the input runtime (`GameViewportInputSource`: the mouse while hovered, the
keyboard and gamepads while focused, touch transformed and gated by position); a game in a panel
takes every key while focused, and Shift+F1 gives the keys back.

## One fit function, one enum

Draw and hit-test both derive from ONE computation, `ContentFit` (`foundation.core`'s `:content_fit`:
`DstRect`/`SrcRect` place the picture, `ToContent`/`FromContent` map points), so letterbox bars are
consistently non-interactive. There is ONE `FitMode` enum (Stretch, Letterbox, Crop, IntegerScale)
+ one `ContentFit` struct - not Sedulous's triplicated `ViewportFitMode` / `RuntimeGraphics.FitMode` /
editor `FitMode` kept aligned by matching ordinals. Content may be presented turned
(`ContentRotation`, a landscape game on a portrait screen), and the point maps turn with it. All input
KINDS are gated by the surface (not Sedulous's mouse-only handler), and there is a single coordinate
pipeline (not the world-UI path polling raw `mouse.X/Y` and disagreeing).

---

The Sedulous reference read (the gem kept + the smells NOT ported - fit-enum triplication, mouse-only
gating, two `IMouse` adapters, dead fit-aware infra, the bypassing world-UI path, scattered gating) is
in `Documentation/Archive/viewport-input-design-history.md`.
