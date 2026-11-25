// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A run's saves: the global store and the slots in one folder. Slots apart from the global store,
// opened, read, listed by the time saved inside each, copied, deleted; names folded; the two write
// rules; unreadable files. Every time comes from a clock the test sets, so nothing sleeps.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import engine.run;

using namespace foundation::core;
using engine::runtime::RunSaves;
using engine::runtime::SaveFile;
using engine::runtime::SaveStoreHandle;
using engine::runtime::SlotWrites;

namespace
{
    i64 g_now = 1000;
    i64 TestClock() { return g_now; }

    String FreshFolder(StringView name)
    {
        String folder = PathJoin(u8"scratch_saves", name);
        (void)RemoveDirectoryRecursive(folder.AsView());
        (void)CreateDirectories(folder.AsView());
        return folder;
    }

    String File(const String& folder, StringView name)
    {
        return PathJoin(folder.AsView(), name);
    }

    // Sets a value in a store through its handle and marks it changed, as a facade does.
    void SetInt(RunSaves& saves, SaveStoreHandle handle, StringView key, i32 value)
    {
        SaveFile* file = saves.Resolve(handle);
        REQUIRE(file != nullptr);
        if (file->Values().SetInt(key, value))
            file->MarkChanged();
    }

    i32 GetInt(RunSaves& saves, SaveStoreHandle handle, StringView key)
    {
        SaveFile* file = saves.Resolve(handle);
        return file != nullptr ? file->Values().GetInt(key, -1) : -1;
    }
}

TEST_CASE("saves: a slot's values are apart from the global store's, and both are written where they belong")
{
    const String folder = FreshFolder(u8"apart");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    g_now = 1000;
    saves.Open(folder.AsView());
    const SaveStoreHandle global = saves.GlobalHandle();
    const SaveStoreHandle slot = saves.OpenSlot(u8"one");
    REQUIRE(slot.IsSet());
    CHECK(FileExists(File(folder, u8"slot-one.save.xml").AsView())); // made at once
    CHECK(saves.SlotTime(u8"one") == 0);                            // never saved yet
    SetInt(saves, global, u8"fairs", 2);
    SetInt(saves, slot, u8"errands", 3);
    CHECK(GetInt(saves, global, u8"errands") == -1);
    CHECK(GetInt(saves, slot, u8"fairs") == -1);
    REQUIRE(saves.Flush());

    SaveFile global2(DefaultAllocator());
    global2.Open(File(folder, u8"global.save.xml").AsView());
    CHECK(global2.Values().GetInt(u8"fairs", 0) == 2);
    CHECK_FALSE(global2.Values().Has(u8"errands"));
    CHECK(global2.SavedAt() == 1000);
    CHECK(saves.SlotTime(u8"one") == 1000);
}

TEST_CASE("saves: a binding change is written to the global store with a slot open")
{
    const String folder = FreshFolder(u8"bindings");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    saves.Open(folder.AsView());
    (void)saves.OpenSlot(u8"one");
    (void)saves.Global().Bindings(); // the section the bindings write
    saves.Global().MarkChanged();    // as a rebinding marks it
    REQUIRE(saves.Flush());
    CHECK(FileExists(File(folder, u8"global.save.xml").AsView()));
    CHECK(saves.SlotTime(u8"one") == 0); // the slot did not change, so it was not saved
}

TEST_CASE("saves: opening another slot writes the first and stales its handles; names fold; bad names are refused")
{
    const String folder = FreshFolder(u8"switch");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    g_now = 2000;
    saves.Open(folder.AsView());
    const SaveStoreHandle a = saves.OpenSlot(u8"Slot1");
    SetInt(saves, a, u8"place", 4);
    const SaveStoreHandle b = saves.OpenSlot(u8"slot2");
    CHECK(saves.Resolve(a) == nullptr); // let go
    CHECK(GetInt(saves, a, u8"place") == -1);
    CHECK(saves.SlotTime(u8"slot1") == 2000); // written as the next opened
    CHECK(saves.OpenedSlot() == b);

    // Slot1 and slot1 are one slot.
    const SaveStoreHandle again = saves.OpenSlot(u8"SLOT1");
    CHECK(GetInt(saves, again, u8"place") == 4);
    CHECK(saves.OpenedSlotName() == StringView(u8"slot1"));
    // One file, named in lower case: read from the folder's listing, since a file system that
    // ignores case (Windows, macOS) finds slot-Slot1 as slot-slot1.
    Array<String> files;
    (void)ListDirectory(
        folder.AsView(), [](void* out, StringView entry, bool) { static_cast<Array<String>*>(out)->PushBack(String(entry)); }, &files);
    usize slot1 = 0;
    for (const String& f : files)
        if (f.AsView().EndsWith(u8"lot1.save.xml"))
        {
            ++slot1;
            CHECK(f == StringView(u8"slot-slot1.save.xml"));
        }
    CHECK(slot1 == 1u);

    CHECK_FALSE(saves.OpenSlot(u8"").IsSet());
    CHECK_FALSE(saves.OpenSlot(u8"no spaces").IsSet());
    CHECK_FALSE(saves.OpenSlot(u8"a/b").IsSet());
    CHECK_FALSE(saves.OpenSlot(u8"abcdefghijklmnopqrstuvwxyz0123456").IsSet()); // 33
    CHECK(saves.OpenedSlotName() == StringView(u8"slot1")); // a refused open leaves the open slot
}

TEST_CASE("saves: slots are listed by the time saved inside each; a copy keeps its time; a never-saved slot is last")
{
    const String folder = FreshFolder(u8"order");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    saves.Open(folder.AsView());
    // Each slot saved at its own time (a slot is written when flushed, or when the next opens).
    g_now = 100;
    SetInt(saves, saves.OpenSlot(u8"b"), u8"x", 1);
    REQUIRE(saves.Flush());
    g_now = 300;
    SetInt(saves, saves.OpenSlot(u8"a"), u8"x", 1);
    REQUIRE(saves.Flush());
    g_now = 200;
    SetInt(saves, saves.OpenSlot(u8"c"), u8"x", 1);
    saves.CloseSlot(); // c written at 200
    (void)saves.OpenSlot(u8"fresh");
    saves.CloseSlot(); // never saved

    Array<String> order = saves.Slots();
    REQUIRE(order.Size() == 4u);
    CHECK(order[0] == StringView(u8"a")); // 300
    CHECK(order[1] == StringView(u8"c")); // 200
    CHECK(order[2] == StringView(u8"b")); // 100
    CHECK(order[3] == StringView(u8"fresh"));

    // A backup made later than everything is not the slot saved last.
    g_now = 900;
    REQUIRE(saves.CopySlot(u8"b", u8"backup"));
    CHECK(saves.SlotTime(u8"backup") == 100);
    order = saves.Slots();
    CHECK(order[0] == StringView(u8"a"));
    CHECK(order[2] == StringView(u8"b")); // equal times by name: b before backup
    CHECK(order[3] == StringView(u8"backup"));
}

TEST_CASE("saves: copying onto the open slot is refused; deleting the open slot lets it go unwritten")
{
    const String folder = FreshFolder(u8"copy");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    g_now = 500;
    saves.Open(folder.AsView());
    SetInt(saves, saves.OpenSlot(u8"one"), u8"x", 1);
    saves.CloseSlot();
    const SaveStoreHandle two = saves.OpenSlot(u8"two");
    CHECK_FALSE(saves.CopySlot(u8"one", u8"two"));
    CHECK(GetInt(saves, two, u8"x") == -1);

    SetInt(saves, two, u8"y", 2);
    REQUIRE(saves.DeleteSlot(u8"two"));
    CHECK_FALSE(saves.OpenedSlot().IsSet());
    CHECK(saves.Resolve(two) == nullptr);
    CHECK_FALSE(saves.HasSlot(u8"two"));
    saves.Flush(); // nothing brings it back
    CHECK_FALSE(FileExists(File(folder, u8"slot-two.save.xml").AsView()));
    CHECK_FALSE(saves.DeleteSlot(u8"two"));
}

TEST_CASE("saves: a slot read is a snapshot, read only, and a read of a missing slot is invalid")
{
    const String folder = FreshFolder(u8"read");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    g_now = 700;
    saves.Open(folder.AsView());
    const SaveStoreHandle open = saves.OpenSlot(u8"one");
    SetInt(saves, open, u8"x", 1);
    REQUIRE(saves.Flush());
    const SaveStoreHandle read = saves.ReadSlot(u8"one");
    REQUIRE(read.IsSet());
    CHECK(saves.IsReadOnly(read));
    CHECK_FALSE(saves.IsReadOnly(open));
    CHECK(GetInt(saves, read, u8"x") == 1);
    SetInt(saves, open, u8"x", 2);
    REQUIRE(saves.Flush());
    CHECK(GetInt(saves, read, u8"x") == 1); // it does not follow the later write
    CHECK_FALSE(saves.Flush(read));          // nor is it written

    const SaveStoreHandle reread = saves.ReadSlot(u8"one");
    CHECK(GetInt(saves, reread, u8"x") == 2);
    CHECK(saves.Resolve(read) == nullptr); // a read again replaces the earlier read
    CHECK_FALSE(saves.ReadSlot(u8"none").IsSet());
}

TEST_CASE("saves: an unreadable slot is set aside when opened, and left out of the list and left where it is otherwise")
{
    const String folder = FreshFolder(u8"unreadable");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    saves.Open(folder.AsView());
    const char garbage[] = "not a save";
    const String bad = File(folder, u8"slot-bad.save.xml");
    REQUIRE(WriteFile(bad.AsView(), Span<const byte>{reinterpret_cast<const byte*>(garbage), sizeof(garbage) - 1}).IsOk());
    SetInt(saves, saves.OpenSlot(u8"good"), u8"x", 1);
    saves.CloseSlot();

    Array<String> listed = saves.Slots();
    REQUIRE(listed.Size() == 1u);
    CHECK(listed[0] == StringView(u8"good"));
    CHECK_FALSE(saves.ReadSlot(u8"bad").IsSet());
    CHECK(FileExists(bad.AsView())); // listing and reading leave it

    const SaveStoreHandle opened = saves.OpenSlot(u8"bad");
    REQUIRE(opened.IsSet()); // an empty slot in its place
    CHECK(FileExists(Format(u8"{}.unreadable", bad.AsView()).AsView()));
    CHECK(saves.HasSlot(u8"bad")); // its empty file written at once, as a new slot's is
    CHECK(saves.Slots().Size() == 2u);
    (void)FileDelete(Format(u8"{}.unreadable", bad.AsView()).AsView());
}

TEST_CASE("saves: a slot opened at save points is written only by its own flush")
{
    const String folder = FreshFolder(u8"savepoints");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    g_now = 800;
    saves.Open(folder.AsView());
    const SaveStoreHandle slot = saves.OpenSlot(u8"one", SlotWrites::AtSavePoints);
    SetInt(saves, slot, u8"x", 1);
    CHECK(saves.SlotTime(u8"one") == 0); // made, never saved
    REQUIRE(saves.Flush(slot)); // a save point
    CHECK(saves.SlotTime(u8"one") == 800);
    SetInt(saves, slot, u8"x", 2);
    REQUIRE(saves.Flush());     // a general flush (the run stopping, the background) leaves it
    saves.CloseSlot();          // and so does closing it

    const SaveStoreHandle read = saves.ReadSlot(u8"one");
    CHECK(GetInt(saves, read, u8"x") == 1);
}

TEST_CASE("saves: with no folder, the global store lasts the run and there are no slots")
{
    RunSaves saves(DefaultAllocator());
    saves.Open(StringView{});
    SetInt(saves, saves.GlobalHandle(), u8"coins", 5);
    CHECK(GetInt(saves, saves.GlobalHandle(), u8"coins") == 5);
    CHECK_FALSE(saves.OpenSlot(u8"one").IsSet());
    CHECK(saves.Slots().IsEmpty());
    CHECK(saves.Flush()); // nothing to write, and that is fine
}

TEST_CASE("saves: the files' names are spelled in one place, a slot's folded")
{
    CHECK(RunSaves::GlobalFile(u8"Game.saves", DefaultAllocator()) == PathJoin(u8"Game.saves", u8"global.save.xml"));
    CHECK(RunSaves::SlotFile(u8"Game.saves", u8"Slot1", DefaultAllocator()) == PathJoin(u8"Game.saves", u8"slot-slot1.save.xml"));
}

TEST_CASE("saves: a slot's time read once for a menu stays right through this run's writes")
{
    const String folder = FreshFolder(u8"times");
    RunSaves saves(DefaultAllocator());
    saves.SetClock(&TestClock);
    saves.Open(folder.AsView());
    g_now = 100;
    SetInt(saves, saves.OpenSlot(u8"a"), u8"x", 1);
    saves.CloseSlot();
    CHECK(saves.SlotTime(u8"a") == 100);
    REQUIRE(saves.Slots().Size() == 1u);
    g_now = 200;
    SetInt(saves, saves.OpenSlot(u8"a"), u8"x", 2); // written again through the open slot
    REQUIRE(saves.Flush());
    CHECK(saves.SlotTime(u8"a") == 200);
    saves.CloseSlot();
    CHECK(saves.SlotTime(u8"a") == 200);
    REQUIRE(saves.CopySlot(u8"a", u8"b"));
    CHECK(saves.SlotTime(u8"b") == 200); // the copy keeps the source's
    REQUIRE(saves.DeleteSlot(u8"a"));
    CHECK(saves.SlotTime(u8"a") == 0);
    CHECK(saves.Slots().Size() == 1u);
}
