# Settings

> Status: CURRENT
> Verified: 2026-11-15 @ c18ee48f5
> Track: [[settings-and-export-track]]

`foundation.settings::Settings` is the typed, versioned, backend-agnostic settings primitive: a store
of reflected `ISerializable` sections persisted through the one `ISerializer` stack. Shipped and used
for the editor's per-user and per-project stores, a game's per-user settings and its save. The applied multi-store
layering + the project manager are in `Documentation/Systems/project-and-settings.md`.

## The store (`foundation.settings::Settings`)

A settings section is a reflected `ISerializable` struct with a data version; a subsystem declares one
(`RTTI_DEFINE_OBJECT_VERSIONED(T, domain, version)`), and the store keys sections by the reflected type
name - no path strings:
- `settings.Section<T>()` - lazily created/loaded typed accessor.
- `settings.MarkChanged<T>()` - fires `OnChanged(typeName)` (the store cannot detect field writes, so
  callers announce).
- `settings.OnChanged(Function<void(StringView)>)` - subscribers react (theme changed -> re-theme).
- `settings.Find<T>()` - the section when the store holds one, else null (no lazy creation).
- `Load(IStream, SerializerFactory, TypeRegistry, SerializableRegistry)` / `Save(IStream, SerializerFactory)` - backend
  chosen by the caller: `XmlSerializerFactory` for hand-editable files, a binary factory in tests. Not
  tied to XML.

Reads take the whole section (no field-merge); a field missing from a stored section keeps the struct's
member-initializer default, so adding fields later is backward-safe.

## Registration + unknown-section passthrough

Sections must be REGISTERED (type registry + serializable registry) before a store loads: a section
whose type is not registered reads as unknown, so its owner then sees the struct's defaults (per-module
sections register in that module's editor entry point, and the player registers its own before it
reads the user's settings). Unknown-section passthrough SHIPPED: a section whose type is unknown or
unregistered in THIS build is captured (`m_unknownSections`, `UnknownSectionCount()`), logged, and
re-emitted verbatim inside its frame on the next `Save`, so a newer build's extra settings survive an
older build's open + re-save; it never fails the load. This works on a self-describing backend (XML:
framed sections a reader can skip); a positional binary backend that cannot capture the frame drops
the section with a warning (settings files are XML, so passthrough holds). A binary stream also starts
with the store's format version, and a mismatch refuses the load (NotSupported).

## Layering

The designed single-store Default/User/Project LAYER STACK did NOT ship as an in-store construct; there
is no `SettingsLayer` enum. Layering is achieved by SEPARATE `Settings` store INSTANCES, one per file
(the editor's per-user store, its per-project store, a game's per-user store, a save), each a flat
store. The project manifest is not a store: it is one versioned `ProjectSettings` payload through the
same serializer stack. The precedence and
the three concrete stores are documented in `project-and-settings.md`.

## Consumers

Reflected sections ship through this machinery: the editor's per-user prefs (`EditorExportSettings`,
`RecentProjectsSettings`, `EditorUiSettings`, `EditorShortcutSettings`, `EditorMcpSettings`, ...), its
per-project state (`EditorDockLayoutSettings`, `EditorFavoritesSettings`, `EditorOpenPagesSettings`,
`EditorAssetBrowserSettings`, the pages' preview sections such as `MaterialPreviewSettings`), and a
game's per-user settings, `<user-data>/<project>.user.settings.xml`, which the player reads once at
start: the mixer's volumes (`AudioUserSettings`) and the chosen quality level
(`RenderQualityUserSettings`), written back whole when one changes. The user-data-dir helper (for the
per-user files) lives in `foundation.core` (`System.cppm`, `GetUserDataDirectory`); on Android it is
the app's own storage.

A game's save is one more: `SaveValues` (`SaveValues.cppm`), the open-ended section. Its fields are not
known ahead, so it keeps typed values (bool, int, float, text) by key, sorted, with kinds written by
name. A save file is an ordinary settings file holding that one section; the run that owns it, the
`Save` script facade and where each host keeps the file are in `application-run.md` and
`Documentation/Archive/save-data-history.md`.

## Deferred

The dynamic **`PropertyBag` + `SettingsKey<T>`** bag (a compact tagged-`Value` map for genuinely
open-ended / plugin key-values) is NOT built. The one consumer with open-ended keys so far, a game's
save, has its own section (`SaveValues`, above), which is what this would have layered in as.

---

The Traktor `Core/Settings` survey, the original one-store-with-a-layer-stack design (superseded by the
per-file-store model), and the phased plan are in `Documentation/Archive/settings-design-history.md`.
