// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Content/ watcher: a file written outside the editor taken in only once it has settled, a
// burst one refresh with each record told once, settled paths held back only so long by one that
// keeps changing, the editor's own writes never reported, a deleted record reported with where it
// lived, an unreadable envelope tried again until it reads; the cook service polling it, held off
// while a reader has the databases and at once when the window comes back; and an open page of a
// deleted asset kept, told, and written back by its save.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;
import pipeline.core;
import engine.project;
import editor.core;

using namespace foundation::core;
using namespace editor;
namespace content = foundation::content;

namespace
{
    // A record whose shininess decides its envelope's size, so each write can be told from the last
    // (the stat's time is in whole seconds).
    class WatchedRecord final : public ISerializable
    {
        RTTI_OBJECT(WatchedRecord, ISerializable)
    public:
        i32 shininess = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "shininess", shininess); }
    };

}

RTTI_DEFINE_OBJECT(WatchedRecord, "rtti::editor::watcher::test")

namespace
{
    // A project, and an outside writer over its Content/: a database of its own on a mount of its
    // own, whose writes the project never hears of.
    struct Bed
    {
        String dir;
        UniquePtr<EditorProject> project;
        UniquePtr<foundation::vfs::NativeFileSystem> outsideMount;
        UniquePtr<content::ContentDatabase> outside;

        explicit Bed(StringView name)
            : dir(name)
        {
            GlobalTypeRegistry().Register(WatchedRecord::StaticType());
            RegisterSerializable<WatchedRecord>();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Watched").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(project);
            outsideMount = MakeUnique<foundation::vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(dir.AsView(), engine::project::kProjectContentDir).AsView(), DefaultAllocator());
            outside = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *outsideMount,
                                                           foundation::xml::XmlSerializerFactory(),
                                                           engine::project::kSourceAssetExtension);
        }
        ~Bed()
        {
            outside.Reset();
            outsideMount.Reset();
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }

        // Writes "group/name" from outside, a record of its own or one it wrote before.
        Guid WriteOutside(StringView group, StringView name, i32 shininess)
        {
            content::Instance* instance = outside->RootGroup()->CreateGroup(group)->CreateInstance(name, WatchedRecord::StaticType());
            REQUIRE(instance != nullptr);
            WatchedRecord record;
            record.shininess = shininess;
            REQUIRE(instance->WriteObject(record).IsOk());
            return instance->Id();
        }
        [[nodiscard]] String ContentPath(StringView relative) const
        {
            return PathJoin(PathJoin(dir.AsView(), engine::project::kProjectContentDir).AsView(), relative);
        }
        void DeleteOutside(StringView relative) { REQUIRE(FileDelete(ContentPath(relative).AsView())); }
    };

    class CountingPage : public EditorPage
    {
    public:
        i32 modified = 0;
        i32 deleted = 0;
        explicit CountingPage(const Guid& asset) : EditorPage(DefaultAllocator()) { SetInstanceId(asset); }
        [[nodiscard]] StringView Title() const override { return u8"counting"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        void OnAssetExternallyModified() override { ++modified; }
        void OnAssetDeleted() override { ++deleted; }
    };

    // Saves its asset as a page does: into the record the source database has by its id.
    class SavingPage final : public CountingPage
    {
    public:
        SavingPage(EditorContext& context, const Guid& asset) : CountingPage(asset), m_context(&context) {}
        [[nodiscard]] Status Save() override
        {
            content::Instance* instance = m_context->Project()->SourceDb().GetInstance(InstanceId());
            if (instance == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            WatchedRecord record;
            record.shininess = 8;
            const Status written = instance->WriteObject(record);
            if (written.IsOk())
            {
                ClearDirty();
            }
            return written;
        }

    private:
        EditorContext* m_context;
    };

    template <typename P, typename... Args>
    P* Adopt(EditorContext& context, Args&&... args)
    {
        P* page = DefaultAllocator().New<P>(Forward<Args>(args)...);
        (void)context.AdoptPage(UniquePtr<EditorPage>(page, DefaultAllocator()));
        return page;
    }

    bool Listed(const Array<Guid>& list, const Guid& id)
    {
        for (const Guid& listed : list)
        {
            if (listed == id)
            {
                return true;
            }
        }
        return false;
    }

    // Two looks that find nothing to do and nothing waiting.
    void Quiet(ContentWatcher& watcher, content::ContentChanges& changes, const char* why)
    {
        INFO(why);
        CHECK_FALSE(watcher.Poll(changes));
        CHECK(changes.IsEmpty());
        CHECK_FALSE(watcher.Poll(changes));
        CHECK(changes.IsEmpty());
        CHECK(watcher.PendingCount() == 0u);
    }
}

TEST_CASE("content-watcher: a file written outside is taken in only once it has settled")
{
    Bed bed(u8"scratch_content_watch_settle");
    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;
    Quiet(watcher, changes, "nothing changed since it took Content/ in");

    const Guid id = bed.WriteOutside(u8"mats", u8"steel", 7);
    CHECK_FALSE(watcher.Poll(changes)); // seen once: it may still be being written
    CHECK(watcher.PendingCount() == 1u);
    CHECK(bed.project->SourceDb().GetInstance(id) == nullptr); // not read yet
    // Written again before it settled: it waits again.
    (void)bed.WriteOutside(u8"mats", u8"steel", 123456);
    CHECK_FALSE(watcher.Poll(changes));
    CHECK(bed.project->SourceDb().GetInstance(id) == nullptr);
    REQUIRE(watcher.Poll(changes)); // the same on two looks: settled
    REQUIRE(changes.added.Size() == 1u);
    CHECK(changes.added[0] == id);
    CHECK(changes.changed.IsEmpty());
    CHECK(changes.removed.IsEmpty());
    content::Instance* instance = bed.project->SourceDb().GetInstance(StringView(u8"mats/steel"));
    REQUIRE(instance != nullptr);
    CHECK(instance->Id() == id);
    Quiet(watcher, changes, "taken in: told once");
}

TEST_CASE("content-watcher: a burst over several polls is one refresh, and each record is told once")
{
    Bed bed(u8"scratch_content_watch_burst");
    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;
    EditorContext context{DefaultAllocator()};

    const Guid a = bed.WriteOutside(u8"burst", u8"a", 1);
    const Guid b = bed.WriteOutside(u8"burst", u8"b", 2);
    CHECK_FALSE(watcher.Poll(changes));
    // More of a, its envelope again and a data stream, while b has settled.
    const StringView words = u8"pulled";
    REQUIRE(bed.outside->GetInstance(a)
                ->WriteData(u8"scene", Span<const byte>(reinterpret_cast<const byte*>(words.Data()), words.Size()),
                            content::StreamEncoding::Text)
                .IsOk());
    (void)bed.WriteOutside(u8"burst", u8"a", 22222);
    CHECK_FALSE(watcher.Poll(changes)); // b settled, held for a, still changing
    CHECK(bed.project->SourceDb().GetInstance(b) == nullptr);
    REQUIRE(watcher.Poll(changes)); // everything settled: one refresh
    CHECK(changes.added.Size() == 2u);
    CHECK(Listed(changes.added, a));
    CHECK(Listed(changes.added, b));
    CHECK(changes.changed.IsEmpty()); // a's two files are one record, told once

    CountingPage* pageA = Adopt<CountingPage>(context, a);
    CountingPage* pageB = Adopt<CountingPage>(context, b);
    context.ApplyContentChanges(changes, watcher.RemovedRecords());
    CHECK(pageA->modified == 1);
    CHECK(pageB->modified == 1);
    Quiet(watcher, changes, "nothing more");

    // A data stream alone: its record changed.
    const StringView more = u8"pulled again";
    REQUIRE(bed.outside->GetInstance(a)
                ->WriteData(u8"scene", Span<const byte>(reinterpret_cast<const byte*>(more.Data()), more.Size()),
                            content::StreamEncoding::Text)
                .IsOk());
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    REQUIRE(changes.changed.Size() == 1u);
    CHECK(changes.changed[0] == a);
    CHECK(changes.added.IsEmpty());
    context.ApplyContentChanges(changes, watcher.RemovedRecords());
    CHECK(pageA->modified == 2);
    CHECK(pageB->modified == 1);
}

TEST_CASE("content-watcher: a file that keeps changing holds the settled back only so long")
{
    Bed bed(u8"scratch_content_watch_hold");
    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;

    const Guid busy = bed.WriteOutside(u8"hold", u8"busy", 1);
    const Guid quiet = bed.WriteOutside(u8"hold", u8"quiet", 2);
    CHECK_FALSE(watcher.Poll(changes));
    // Each poll finds busy grown again (a digit more each time), quiet as it was.
    i32 shininess = 10;
    for (i32 i = 0; i < ContentWatcher::kMaxHoldPolls; ++i)
    {
        (void)bed.WriteOutside(u8"hold", u8"busy", shininess);
        shininess *= 10;
        CHECK_FALSE(watcher.Poll(changes)); // quiet held while busy changes
    }
    (void)bed.WriteOutside(u8"hold", u8"busy", shininess);
    REQUIRE(watcher.Poll(changes)); // held long enough: quiet goes
    REQUIRE(changes.added.Size() == 1u);
    CHECK(changes.added[0] == quiet);
    CHECK(bed.project->SourceDb().GetInstance(busy) == nullptr); // busy still changing
    CHECK(watcher.PendingCount() == 1u);
    REQUIRE(watcher.Poll(changes)); // busy stopped: settled
    REQUIRE(changes.added.Size() == 1u);
    CHECK(changes.added[0] == busy);
}

TEST_CASE("content-watcher: the editor's own writes are never reported")
{
    Bed bed(u8"scratch_content_watch_own");
    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;
    content::ContentDatabase& db = bed.project->SourceDb();

    WatchedRecord record;
    content::Instance* mine = db.RootGroup()->CreateGroup(u8"mine")->CreateInstance(u8"brass", WatchedRecord::StaticType());
    REQUIRE(mine != nullptr);
    const Guid id = mine->Id();
    REQUIRE(mine->WriteObject(record).IsOk());
    const StringView words = u8"notes";
    REQUIRE(mine->WriteData(u8"notes", Span<const byte>(reinterpret_cast<const byte*>(words.Data()), words.Size()),
                            content::StreamEncoding::Text)
                .IsOk());
    Quiet(watcher, changes, "written");
    record.shininess = 4096;
    REQUIRE(mine->WriteObject(record).IsOk());
    REQUIRE(mine->DeleteData(u8"notes").IsOk());
    Quiet(watcher, changes, "written again, a stream deleted");
    REQUIRE(db.RenameInstance(id, u8"bronze").IsOk());
    Quiet(watcher, changes, "renamed");
    REQUIRE(db.MoveInstance(id, *db.RootGroup()->CreateGroup(u8"moved")).IsOk());
    Quiet(watcher, changes, "moved to a new folder");
    REQUIRE(db.RenameGroup(*db.RootGroup()->GetGroup(u8"moved"), u8"renamed").IsOk());
    Quiet(watcher, changes, "its group renamed");
    REQUIRE(db.GetInstance(StringView(u8"renamed/bronze")) != nullptr);
    CHECK(db.GetInstance(StringView(u8"renamed/bronze"))->Id() == id);

    // An outside write beside one of the editor's: only the outside one is told.
    const Guid theirs = bed.WriteOutside(u8"theirs", u8"tin", 3);
    record.shininess = 77;
    REQUIRE(db.GetInstance(id)->WriteObject(record).IsOk());
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    REQUIRE(changes.added.Size() == 1u);
    CHECK(changes.added[0] == theirs);
    CHECK(changes.changed.IsEmpty());

    REQUIRE(db.DeleteInstance(id).IsOk());
    Quiet(watcher, changes, "deleted");
}

TEST_CASE("content-watcher: a record deleted outside is reported removed, with where it lived")
{
    Bed bed(u8"scratch_content_watch_delete");
    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;
    content::ContentDatabase& db = bed.project->SourceDb();
    const Guid id = bed.WriteOutside(u8"mats", u8"steel", 7);
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    CHECK(changes.added.Size() == 1u);

    bed.DeleteOutside(u8"mats/steel.xasset");
    CHECK_FALSE(watcher.Poll(changes));
    CHECK(db.GetInstance(id) != nullptr); // not yet: it may be a rewrite under way
    REQUIRE(watcher.Poll(changes));
    REQUIRE(changes.removed.Size() == 1u);
    CHECK(changes.removed[0] == id);
    CHECK(changes.added.IsEmpty());
    CHECK(changes.changed.IsEmpty());
    REQUIRE(watcher.RemovedRecords().Size() == 1u);
    const ContentWatcher::RemovedRecord& place = watcher.RemovedRecords()[0];
    CHECK(place.id == id);
    CHECK(place.former != nullptr);
    CHECK(place.path.AsView() == StringView(u8"mats/steel"));
    CHECK(place.typeName.AsView() == StringView(reinterpret_cast<const utf8char*>(WatchedRecord::StaticType().name)));
    CHECK(db.GetInstance(id) == nullptr);
    CHECK(db.GetInstance(StringView(u8"mats/steel")) == nullptr);
    Quiet(watcher, changes, "told once");
    CHECK(watcher.RemovedRecords().IsEmpty()); // the last poll's only
}

TEST_CASE("content-watcher: an unreadable envelope is tried again until it reads")
{
    Bed bed(u8"scratch_content_watch_unreadable");
    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;
    content::ContentDatabase& db = bed.project->SourceDb();
    const Guid id = bed.WriteOutside(u8"mats", u8"steel", 7);
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));

    // Cut off inside its header, as a file still being written is.
    Result<Array<byte>> whole = ReadFile(bed.ContentPath(u8"mats/steel.xasset").AsView());
    REQUIRE(whole.HasValue());
    const StringView text(reinterpret_cast<const utf8char*>(whole.Value().Data()), whole.Value().Size());
    const usize typeAt = text.Find(u8"typeName");
    REQUIRE(typeAt != StringView::kNotFound);
    REQUIRE(bed.outsideMount->Save(u8"mats/steel.xasset", Span<const byte>(whole.Value().Data(), typeAt)).IsOk());
    CHECK_FALSE(watcher.Poll(changes));
    CHECK_FALSE(watcher.Poll(changes)); // settled, but it does not read: nothing told
    CHECK(changes.unreadable.Size() == 1u);
    CHECK(watcher.PendingCount() == 1u); // kept to try again
    CHECK(db.GetInstance(id) != nullptr); // the record kept as it was
    CHECK_FALSE(watcher.Poll(changes));
    CHECK(changes.unreadable.Size() == 1u); // tried again

    // Written whole again: read once it settles.
    (void)bed.WriteOutside(u8"mats", u8"steel", 123456);
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    REQUIRE(changes.changed.Size() == 1u);
    CHECK(changes.changed[0] == id);
    CHECK(changes.unreadable.IsEmpty());
    Quiet(watcher, changes, "read");
}

TEST_CASE("content-watcher: the cook service looks at Content/, held off while a reader runs, at once on focus")
{
    Bed bed(u8"scratch_content_watch_cook");
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    EditorCookService cook;
    cook.Initialize(*bed.project, builders);
    i32 heard = 0;
    Guid added{};
    cook.OnContentChanged = [&](const content::ContentChanges& changes, Span<const ContentWatcher::RemovedRecord>)
    {
        ++heard;
        if (changes.added.Size() == 1)
        {
            added = changes.added[0];
        }
    };
    bool busy = true;
    cook.ExternalMutationLock = [&busy]() { return busy; };

    const Guid id = bed.WriteOutside(u8"mats", u8"steel", 7);
    CHECK_FALSE(cook.PollContent()); // held off while a reader has the databases
    CHECK_FALSE(cook.PollContent());
    CHECK(bed.project->SourceDb().GetInstance(id) == nullptr);
    busy = false;
    // The window coming back has the next Update look, whatever its clock says.
    cook.PollContentSoon();
    cook.Update({});
    CHECK(heard == 0); // seen once
    cook.PollContentSoon();
    cook.Update({});
    CHECK(heard == 1);
    CHECK(added == id);
    CHECK(bed.project->SourceDb().GetInstance(id) != nullptr);
    cook.Shutdown();
}

TEST_CASE("content-watcher: an open page of an asset deleted outside stays, is told, and its save writes it back")
{
    Bed bed(u8"scratch_content_watch_page");
    content::ContentDatabase& db = bed.project->SourceDb();
    content::Instance* record = db.RootGroup()->CreateGroup(u8"mats")->CreateInstance(u8"steel", WatchedRecord::StaticType());
    REQUIRE(record != nullptr);
    WatchedRecord material;
    material.shininess = 7;
    REQUIRE(record->WriteObject(material).IsOk());
    const Guid id = record->Id();
    const String full = bed.ContentPath(u8"mats/steel.xasset");

    ContentWatcher watcher(*bed.project);
    content::ContentChanges changes;
    EditorContext context{DefaultAllocator()};
    context.SetProject(bed.project.Get());
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    SavingPage* page = Adopt<SavingPage>(context, context, id);
    Random rng(5);
    CountingPage* other = Adopt<CountingPage>(context, Guid::Generate(rng));
    const content::Instance* selected[] = {record};
    context.AssetSelection().Set(Span<const content::Instance* const>(selected, 1));

    bed.DeleteOutside(u8"mats/steel.xasset");
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    context.ApplyContentChanges(changes, watcher.RemovedRecords());
    CHECK(page->deleted == 1);
    CHECK(other->deleted == 0);
    CHECK(context.OpenPages().Size() == 2u); // kept open, never closed under the user
    CHECK(page->IsAssetDeleted());
    CHECK(page->IsDirty()); // its content unsaved now
    CHECK(page->DeletedRecordPath() == StringView(u8"mats/steel"));
    CHECK(page->DeletedTypeName() == StringView(reinterpret_cast<const utf8char*>(WatchedRecord::StaticType().name)));
    CHECK(context.AssetSelection().IsEmpty()); // the selection let the record go
    CHECK(said.AsView().Contains(u8"deleted on disk"));

    // Its own save finds no record; the context's puts it back first.
    CHECK(page->Save().Code() == ErrorCode::NotFound);
    REQUIRE(context.SavePage(*page).IsOk());
    CHECK_FALSE(page->IsAssetDeleted());
    CHECK_FALSE(page->IsDirty());
    content::Instance* back = db.GetInstance(id);
    REQUIRE(back != nullptr);
    CHECK(back->TypeName() == StringView(reinterpret_cast<const utf8char*>(WatchedRecord::StaticType().name)));
    CHECK(back->Path().AsView() == StringView(u8"mats/steel"));
    CHECK(FileExists(full.AsView()));
    Quiet(watcher, changes, "the save is the editor's own write");

    // Gone again, and another asset in its place: the save refuses, the content kept.
    bed.DeleteOutside(u8"mats/steel.xasset");
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    context.ApplyContentChanges(changes, watcher.RemovedRecords());
    CHECK(page->IsAssetDeleted());
    CHECK(page->deleted == 2);
    const Guid usurper = bed.WriteOutside(u8"mats", u8"steel", 99);
    REQUIRE(usurper != id); // the outside writer never knew the editor's record
    CHECK_FALSE(watcher.Poll(changes));
    REQUIRE(watcher.Poll(changes));
    REQUIRE(changes.added.Size() == 1u);
    CHECK(changes.added[0] == usurper);
    context.ApplyContentChanges(changes, watcher.RemovedRecords());
    CHECK(page->IsAssetDeleted()); // another asset came, not this one
    CHECK(context.SavePage(*page).Code() == ErrorCode::AlreadyExists);
    CHECK(said.AsView().Contains(u8"another asset"));
    CHECK(page->IsAssetDeleted());
    CHECK(page->IsDirty());
    CHECK(db.GetInstance(usurper) != nullptr); // the other asset untouched
}
