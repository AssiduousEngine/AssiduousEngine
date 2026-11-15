# Code Editor

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[code-editor-track]]

`CodeEditor` - the code-editing widget of `foundation.gui.code` (`Code/Foundation/GUI.Code`), a
library beside the GUI, drawn and themed like every GUI widget. It backs the editor's script,
shader and GUI document/sheet pages and the GUISandbox's Code page, with syntax colouring,
diagnostics, find/replace, folding, several carets, completion and hover, and gutter markers for a
debugger. The library knows no language: the owner of a language supplies its definition and its
service.

## The widget (`foundation::gui::CodeEditor`)

- **Document** (`CodeDocument`): an array of lines, each with its text and a hash the tokenizer
  caches by. Positions are (line, byte column). Anchors move through each edit (biased left or
  right), so selections, diagnostics, markers and search hits keep their places. Undo works in
  transactions that record the selections before and after, so a multi-cursor undo puts every caret
  back; typing (or deleting) within 300 ms merges into one step. A host may share its own document
  with the editor (`SetDocument`).
- **View**: each line in view is shaped with the GUI's text and kept until it changes; tabs go to
  tab stops and the caret follows the shaped glyphs, so any font works. Only the rows in view are
  drawn. Folding (from the language's brackets, indentation or regions, or from the host) and soft
  wrap share one map from lines to rows. Whitespace can be drawn (none, boundary, selection,
  trailing, all).
- **Editing**: several selections (Alt+click adds a caret, Ctrl+D the next occurrence, Ctrl+Alt+Up
  and Down a caret above or below); the keymap's text commands run at every caret. Return keeps the
  indent, Tab indents, line moves, duplication and deletion, comment toggle (Ctrl+/), auto-closed
  pairs. The keys are VS Code's, bound by `BindCodeEditorKeys` in the editor's own `codeeditor`
  key context, and can be rebound like any others.
- **Gutter + markers**: line numbers and a marker column. `AddMarker(line, kind)` marks a line
  with a kind (`breakpoint`, `execution`, `bookmark`, or the host's own) drawn by the `marker`
  part; the mark stays with its line through edits. `SetOnGutterClick` and `SetOnContextMenu` let a
  host toggle breakpoints. `SetDiagnostics` takes the host's whole set (`CodeDiagnostic`: range,
  severity, message); each draws a wave in its severity's colour and adds its message to the
  tooltip.
- **Find bar** (`CodeFindBar`): find (match case, whole word, a count of the matches), replace and
  replace all, and go to line (`line` or `line:column`); Ctrl+F, Ctrl+H, Ctrl+G, F3 / Shift+F3.
- **Theme**: each token draws in its part's colour (`codeeditor::keyword`), set through the
  `--code-*` tokens in the code sheet (`CodeSheetCSS`), so a theme recolours code without forking
  the sheet (`editor-theme.md`).

## Languages

A language is a `CodeLanguage` value: an id, a name, file extensions, word lists (keywords, types,
constants), and contexts of rules (a literal, an identifier looked up in the words, a number, a set
or run of characters, the rest of the line, a long bracket's open or close as in Luau's `[==[`),
plus editing settings (line and block comments, brackets, auto-close pairs, indent and outdent
words) and folding settings. The tokenizer reads one line at a time from the state the previous
line ended in, and the view re-tokenizes a line only when it changed or the line before it now
ends in a different state, and only up to the last line in view.

The library ships no language. The editor reads them from data files, `Data/Editor/Code/<id>.xml`
(AngelScript, Luau, HLSL, XML and CSS today), through the XML serialization, the first time a page
asks (`CodeLanguages`, `editor.app`); adding a language means adding a file. The GUISandbox reads
the same files.

## Completion and hover

`ICodeLanguageService` is the host's: `CompletionTriggers()` (such as `.` and `:`), `Complete(document,
request, out)` (every item that may apply; the editor filters by the typed word with a fuzzy match,
`CodeFuzzyMatch`) and `Hover(document, at, out)`. Typing a name, a trigger or Ctrl+Space opens the
list (`CodeCompletionList`), a popup that leaves the focus in the editor; while it shows, the
`completing` key context gives its keys priority.

`ScriptLanguageService` (`editor.script`) is the script pages' service, backend-neutral over the
page's `ScriptApiSurface`: what the runtime binds for the language
(`IScriptManager::DescribeBoundApi`), built once per page and shared by completion, hover and the
page's API browser. After `x.` or `x::` it offers the members of a type or namespace named x, or
of the type a variable x is declared with; elsewhere the language's keywords, the bound types and
namespaces and the script's own names. Hover says what a name is, or its value while a run is
paused in that script.

## The editor's code pages

`CodeAssetPage` (`editor.app`) is the base of the pages over assets written as code. It reads the
asset's source file into the markup's `page-code` editor, coloured by the page's language, checks it
0.6 s after the last edit (the derived page supplies the check), and lists problems in the page's
problems list and on their lines. Each step of the code's document is a command on the page's
stack, so the bar's Undo and Redo, the Edit menu and the code's own Ctrl+Z walk one history. Save
writes the source, then the asset, and requests a cook. The pages on it:

- **`ScriptEditorPage`** (`editor.script`, AngelScript and Luau): the check compiles as the cook does
  without running anything; completion, hover and the API browser come from `ScriptLanguageService`.
- **`ShaderEditorPage`** (`editor.shaders`, HLSL): the check compiles each stage's base variant;
  beside the code is a preview sphere drawn with the shader being edited, keeping the last version
  that compiled.
- **`GuiAssetEditorPage`** (`editor.gui`, XML documents and CSS sheets): the check reports what the
  cook would; beside the code, the runtime's `GuiSubsystem` draws a live preview with the game's
  fonts, theme and sheets.

## Debugger

Breakpoints are editor state on `EditorContext` (`ToggleBreakpoint`, `Breakpoints()`,
`OnBreakpointsChanged`); the script page's gutter toggles them and a Game run applies them to its
script debugger. While a run is paused, the Game page's debugger listener sets the
`ScriptExecutionPoint` (versioned, polled each frame), which the script page editing that file shows
as its `execution` marker and scrolls to, and installs `ScriptValueProbe`, which answers hover with a
name's value. The Game page's `DebuggerPanel` holds Continue, Step Into, Step Over and Break, the call
stack and the locals. AngelScript and Luau both have debuggers. See `script-debugger.md`.

---

Design rationale (the purpose-built-widget-vs-vendored decision, the completion-from-the-start
call, the layering decision) is in `Documentation/Archive/code-editor-design-history.md`; the
followups it once listed are in `Documentation/Archive/code-editor-followups-history.md`.
