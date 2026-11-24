// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Run - :save partition.
//
// A run's saves: the values a game keeps between runs, in stores. Every game's saves are a folder
// (`<project>.saves/`): the global store (`global.save.xml`, the values every playthrough shares and
// the player's bindings) and a game's slots (`slot-<name>.save.xml`, one playthrough's values each).
// The host names the folder (the player in the user data directory, play in editor in the
// project's Editor/ folder). A game holds a store by a handle and hands it to whatever keeps state
// for it; nothing picks a store for a game except the bindings, which are the global store's. Each
// file is an ordinary settings file holding a SaveValues section and a SaveInfo section (when it was
// last saved), written whole and atomically.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the Save facade (RTTI_OBJECT)

export module engine.run:save;

import foundation.core;
import foundation.settings;
import foundation.script;
import foundation.input; // InputBindingOverrides: the player's rebinding, kept in the global store

using namespace foundation::core;
using namespace foundation;
namespace core = foundation::core;

export namespace engine::runtime
{
    namespace script = foundation::script;

    // One store's file and its values: the global store, a slot open for writing, or a slot read.
    class SaveFile
    {
    public:
        explicit SaveFile(IAllocator& allocator)
            : m_allocator(&allocator), m_path(allocator),
              m_store(MakeUnique<settings::Settings>(allocator, allocator))
        {
        }
        SaveFile(const SaveFile&) = delete;
        SaveFile& operator=(const SaveFile&) = delete;

        // Names the store's file and reads it. Absent is an empty store. Unreadable is an empty
        // store and a warning, and the file is set aside first ("<path>.unreadable",
        // SetAsideUnreadable) so the next write never replaces the player's progress; when it cannot
        // be moved, the store is closed instead and writes nowhere. An empty path closes the store:
        // values stay readable but are written nowhere. False when it was closed that way.
        bool Open(StringView path);
        // Reads the file at `path` as it is, without naming it for writing: a slot read. False, and
        // an empty store, when it is absent or cannot be read; the file is left where it is.
        bool Read(StringView path);

        [[nodiscard]] bool IsOpen() const noexcept { return !m_path.IsEmpty(); }
        [[nodiscard]] StringView Path() const noexcept { return m_path.AsView(); }

        [[nodiscard]] settings::SaveValues& Values() { return m_store->Section<settings::SaveValues>(); }
        // The player's own bindings of the game's controls, a section of their own beside the
        // values (so clearing the values keeps them); the global store's alone. Open replaces the
        // store: ask again after it.
        [[nodiscard]] foundation::input::InputBindingOverrides& Bindings()
        {
            return m_store->Section<foundation::input::InputBindingOverrides>();
        }
        // When this store was last saved (Unix seconds), 0 for one never saved.
        [[nodiscard]] i64 SavedAt() const noexcept;

        // A change the next Flush writes. The facade calls it when a setter reports a change.
        void MarkChanged() noexcept { m_changed = true; }
        [[nodiscard]] bool HasChanges() const noexcept { return m_changed; }

        // Writes the store if it changed since the last write, its saved time set to `now`. True when
        // the file holds it afterwards (nothing to write counts); false when the write failed or there
        // is no file.
        bool Flush(i64 now);
        // Writes the store as it is with its saved time unchanged: a slot made empty, never saved.
        bool WriteAsIs();

        // Which store this is, for a handle: a number no other store of the run has had.
        [[nodiscard]] u64 Serial() const noexcept { return m_serial; }
        void SetSerial(u64 serial) noexcept { m_serial = serial; }

    private:
        bool Write();

        IAllocator* m_allocator;
        String m_path;
        // The file's whole settings store: its sections, and any section a newer build wrote, kept
        // so this build's write does not drop it.
        UniquePtr<settings::Settings> m_store;
        bool m_changed = false;
        u64 m_serial = 0;
    };

    // How a slot open for writing is written: as the game plays (when the run stops, when the
    // application goes to the background, on a flush), or only at its save points (its own flush).
    enum class SlotWrites : u8
    {
        AsItPlays,
        AtSavePoints,
    };

    // A store a game holds: the serial of a store of the run's saves. Invalid (0), or stale once its
    // store is gone (a slot closed, deleted, replaced, or read again), it reaches nothing.
    struct SaveStoreHandle
    {
        u64 serial = 0;
        [[nodiscard]] bool IsSet() const noexcept { return serial != 0; }
        [[nodiscard]] bool operator==(const SaveStoreHandle&) const noexcept = default;
    };

    // The clock a save's time comes from: Unix seconds. Tests set one.
    using SaveClock = i64 (*)();

    // A run's saves: the global store, at most one slot open for writing, and the slots read for a
    // menu. One per run; the run registers it, and its scripts reach it through their facades.
    class RunSaves
    {
    public:
        explicit RunSaves(IAllocator& allocator);
        RunSaves(const RunSaves&) = delete;
        RunSaves& operator=(const RunSaves&) = delete;

        // Names the saves folder and reads the global store from it; any slot open or read before is
        // let go, unwritten. An empty folder keeps the global store for the run only, and has no
        // slots.
        void Open(StringView folder);
        [[nodiscard]] StringView Folder() const noexcept { return m_folder.AsView(); }

        [[nodiscard]] SaveFile& Global() noexcept { return m_global; }
        [[nodiscard]] SaveStoreHandle GlobalHandle() const noexcept { return {m_global.Serial()}; }

        // The slot `name` open for writing, made at once (an empty file, never saved) when it has no
        // file. The slot open before is written first by its rule, then let go. Invalid, and the
        // open slot as it was, when the name breaks the rule, there is no folder, or the file cannot
        // be read and cannot be set aside. The open slot again returns its handle, its rule changed
        // to `writes`.
        SaveStoreHandle OpenSlot(StringView name, SlotWrites writes = SlotWrites::AsItPlays);
        // The slot `name` as it is on disk now, read only: a snapshot for a menu's summary. A slot
        // read again replaces the earlier read of it. Invalid when there is no such slot, or its file
        // cannot be read (left where it is, with a warning).
        SaveStoreHandle ReadSlot(StringView name);
        [[nodiscard]] SaveStoreHandle OpenedSlot() const noexcept;
        [[nodiscard]] StringView OpenedSlotName() const noexcept { return m_slotName.AsView(); }
        // Writes the open slot by its rule and lets it go.
        void CloseSlot();

        // Every slot's name, the one saved last first (by the time inside each, then by name; never
        // saved last). A file that cannot be read is left out, with a warning.
        [[nodiscard]] Array<String> Slots() const;
        [[nodiscard]] bool HasSlot(StringView name) const;
        // Deletes the slot's file; the open slot is let go first, unwritten. False when there was none.
        bool DeleteSlot(StringView name);
        // Copies `from` over `to` (made or replaced), keeping its saved time. The open slot as `from`
        // is written first when it is written as it plays (one written at save points is copied as it
        // was last saved). Refused onto the open slot: its next write would undo the copy.
        bool CopySlot(StringView from, StringView to);
        // When the slot was last saved (Unix seconds); 0 when it was never saved or there is none.
        [[nodiscard]] i64 SlotTime(StringView name) const;

        // The store a handle reaches, or null when it is invalid or stale.
        [[nodiscard]] SaveFile* Resolve(SaveStoreHandle handle) noexcept;
        // Whether the handle reaches a slot read, whose values are not for writing.
        [[nodiscard]] bool IsReadOnly(SaveStoreHandle handle) const noexcept;

        // Writes every store that changed, but a slot written at its save points: the end of a run,
        // the application going to the background, a general flush. True when every store written
        // holds its values afterwards.
        bool Flush();
        // A store's own flush: writes it whatever its rule (a save point). False for a slot read, a
        // stale handle, or a failed write.
        bool Flush(SaveStoreHandle handle);

        void SetClock(SaveClock clock) noexcept { m_clock = clock; }
        [[nodiscard]] i64 Now() const noexcept;

        // A slot name: 1 to 32 letters, digits, '-' and '_'. Folded to lower case for the file, so a
        // name is one slot on every platform.
        [[nodiscard]] static bool IsSlotName(StringView name) noexcept;
        [[nodiscard]] static String FoldSlotName(StringView name);

    private:
        struct SlotRead
        {
            String name;
            UniquePtr<SaveFile> file;
        };

        [[nodiscard]] String GlobalPath() const;
        [[nodiscard]] String SlotPath(StringView folded) const;
        // The folded name when it is a slot name and there is a folder; else a warning and nothing.
        [[nodiscard]] bool CheckSlotName(StringView name, StringView call, String& folded) const;
        void ForgetReads(StringView folded);
        void LetGoOfSlot(bool write);

        IAllocator* m_allocator;
        String m_folder;
        SaveFile m_global;
        UniquePtr<SaveFile> m_slot;
        String m_slotName;
        SlotWrites m_slotWrites = SlotWrites::AsItPlays;
        Array<SlotRead> m_reads;
        u64 m_nextSerial = 1;
        SaveClock m_clock = nullptr;
    };

    // The `Save` script facade: a static facade over the calling context's global store, the same
    // shape on both backends (`Save::setInt("best.level2", 4210)`, `Save.getInt(...)` in Luau). A
    // context with no run saves (an editor tool) reads every fallback and writes nowhere.
    class SaveFacade final : public Object
    {
        RTTI_OBJECT(SaveFacade, Object)
    public:
        [[nodiscard]] static SaveFile* Resolve()
        {
            RunSaves* saves = script::CurrentService<RunSaves>(); // the calling script's run's saves
            return saves != nullptr ? &saves->Global() : nullptr;
        }

        [[nodiscard]] static bool has(core::String key);
        static void remove(core::String key);
        static void clear();

        [[nodiscard]] static i32 getInt(core::String key, i32 fallback);
        static void setInt(core::String key, i32 value);
        [[nodiscard]] static f32 getFloat(core::String key, f32 fallback);
        static void setFloat(core::String key, f32 value);
        [[nodiscard]] static bool getBool(core::String key, bool fallback);
        static void setBool(core::String key, bool value);
        [[nodiscard]] static core::String getString(core::String key, core::String fallback);
        static void setString(core::String key, core::String value);
        // A list of numbers (a recorded run): set whole, read whole; empty when absent or another
        // kind. AngelScript `array<float>`, Luau a table.
        [[nodiscard]] static core::Array<f32> getFloats(core::String key);
        static void setFloats(core::String key, core::Array<f32> values);

        // Writes now if anything changed; false if the write failed or the run has no save file.
        static bool flush();
    };

    // Registers the `Save` facade (and the save sections' types, which reading a save file needs).
    // Idempotent; the run domain calls it with the run facade.
    void RegisterSaveScriptFacade();
}
