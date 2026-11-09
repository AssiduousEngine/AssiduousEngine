// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - ContentWatcher's sweep, settling and refresh (ContentWatcher.cppm).

module;
#include "Core/Prelude.h"

module editor.project;

import foundation.core;
import foundation.vfs;
import foundation.content;
import :project;

using namespace foundation::core;

namespace editor
{
    namespace
    {
        // Every entry under the folder `path`.
        template <typename T>
        void RemoveUnder(HashMap<String, T>& map, StringView path)
        {
            String prefix(path);
            prefix.PushBack(utf8char('/'));
            Array<String> under;
            for (auto& [key, value] : map)
            {
                if (key.AsView().StartsWith(prefix.AsView()))
                {
                    under.PushBack(String(key.AsView()));
                }
            }
            for (const String& key : under)
            {
                map.Remove(key);
            }
        }

        [[nodiscard]] bool Listed(const Array<Guid>& list, const Guid& id)
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
    }

    ContentWatcher::ContentWatcher(EditorProject& project)
        : m_project(&project)
    {
        // Before the sweep: a worker's write landing during it is taken in at the first poll.
        m_project->RecordContentWrites(true);
        Sweep(StringView{}, m_snapshot);
    }

    ContentWatcher::~ContentWatcher()
    {
        m_project->RecordContentWrites(false);
    }

    bool ContentWatcher::Poll(foundation::content::ContentChanges& out)
    {
        out.Clear();
        m_removed.Clear();
        TakeInWrites();

        HashMap<String, Look> now;
        Sweep(StringView{}, now);
        for (auto& [path, look] : now)
        {
            const Look* known = m_snapshot.Find(path);
            if (known != nullptr && known->Same(look))
            {
                m_pending.Remove(path); // back to what was taken in
            }
            else
            {
                Observe(path.AsView(), look);
            }
        }
        for (auto& [path, look] : m_snapshot)
        {
            if (!now.Contains(path))
            {
                Observe(path.AsView(), Look{}); // gone
            }
        }
        // A file that came and went again between looks: nothing to tell.
        Array<String> vanished;
        for (auto& [path, pending] : m_pending)
        {
            if (!now.Contains(path) && !m_snapshot.Contains(path))
            {
                vanished.PushBack(String(path.AsView()));
            }
        }
        for (const String& path : vanished)
        {
            m_pending.Remove(path);
        }

        // The settled paths go together once nothing beside them is still changing, or once they
        // have waited long enough for it.
        Array<String> settled;
        bool changing = false;
        i32 waited = 0;
        for (auto& [path, pending] : m_pending)
        {
            if (!pending.settled)
            {
                changing = true;
                continue;
            }
            settled.PushBack(String(path.AsView()));
            waited = Max(waited, pending.waited);
        }
        if (settled.IsEmpty() || (changing && waited < kMaxHoldPolls))
        {
            return false;
        }

        // Envelopes first: a data stream that changed with its envelope then names the record the
        // envelope's refresh left, not one it replaced.
        foundation::content::ContentDatabase& db = m_project->SourceDb();
        const StringView extension = db.Extension();
        const auto isEnvelope = [extension](const String& p) { return p.AsView().EndsWith(extension); };
        settled.Sort([&](const String& a, const String& b) { return isEnvelope(a) && !isEnvelope(b); });

        // Where each record a changed envelope names lives, before the refresh can drop it.
        Array<RemovedRecord> places;
        for (const String& path : settled)
        {
            if (!isEnvelope(path))
            {
                continue;
            }
            const StringView recordPath = path.AsView().SubStr(0, path.Size() - extension.Size());
            if (const foundation::content::Instance* instance = db.GetInstance(recordPath))
            {
                RemovedRecord place;
                place.id = instance->Id();
                place.former = instance;
                place.path = String(recordPath);
                place.typeNamespace = String(instance->TypeNamespace());
                place.typeName = String(instance->TypeName());
                places.PushBack(Move(place));
            }
        }
        db.Refresh(settled.AsSpan(), out);
        for (RemovedRecord& place : places)
        {
            if (Listed(out.removed, place.id))
            {
                m_removed.PushBack(Move(place));
            }
        }

        // Taken in, but for a path the database could not read: it stays settled and is tried
        // again at the next poll.
        for (const String& path : settled)
        {
            bool unreadable = false;
            for (const String& u : out.unreadable)
            {
                unreadable = unreadable || u.AsView() == path.AsView();
            }
            if (unreadable)
            {
                continue;
            }
            const Pending* pending = m_pending.Find(path);
            if (pending != nullptr && pending->last.exists)
            {
                m_snapshot.InsertOrAssign(path, pending->last);
            }
            else
            {
                m_snapshot.Remove(path);
            }
            m_pending.Remove(path);
        }
        return !out.added.IsEmpty() || !out.changed.IsEmpty() || !out.removed.IsEmpty();
    }

    // A path that differs from what was taken in, looked at once more: settled when it looks as it
    // did last time, waiting again when not.
    void ContentWatcher::Observe(StringView path, const Look& look)
    {
        const String key(path);
        if (Pending* pending = m_pending.Find(key))
        {
            if (pending->last.Same(look))
            {
                if (pending->settled)
                {
                    ++pending->waited;
                }
                pending->settled = true;
            }
            else
            {
                pending->last = look;
                pending->settled = false;
                pending->waited = 0;
            }
            return;
        }
        Pending fresh;
        fresh.last = look;
        m_pending.InsertOrAssign(key, fresh);
    }

    // The paths the editor wrote since the last poll, each taken in as it is now.
    void ContentWatcher::TakeInWrites()
    {
        Array<String> written;
        m_project->TakeContentWrites(written);
        for (const String& path : written)
        {
            TakeIn(path.AsView());
        }
    }

    // A path the editor wrote: a file's look now, or for a directory (a group made, renamed or
    // moved) the files under it now, and nothing for one gone; whatever was known of it before
    // goes, a change seen waiting there with it.
    void ContentWatcher::TakeIn(StringView path)
    {
        if (path.IsEmpty())
        {
            return; // the root: taking it in whole would swallow every outside change
        }
        const String key(path);
        m_pending.Remove(key);
        Look look;
        if (LookAt(path, look))
        {
            m_snapshot.InsertOrAssign(key, look);
            return;
        }
        // A file deleted had its own entry; anything else may have files under it.
        if (m_snapshot.Remove(key))
        {
            return;
        }
        RemoveUnder(m_snapshot, path);
        RemoveUnder(m_pending, path);
        if (m_project->ContentMount().Exists(path))
        {
            Sweep(path, m_snapshot);
        }
    }

    // Every regular file under `folder`, recursively, into `into` with its look.
    void ContentWatcher::Sweep(StringView folder, HashMap<String, Look>& into)
    {
        foundation::vfs::NativeFileSystem& mount = m_project->ContentMount();
        Array<foundation::vfs::DirEntry> entries;
        if (!mount.Enumerate(folder, entries).IsOk())
        {
            return;
        }
        for (const foundation::vfs::DirEntry& entry : entries)
        {
            String child(folder);
            if (!child.IsEmpty())
            {
                child.PushBack(utf8char('/'));
            }
            child.Append(entry.name.AsView());
            if (entry.isDirectory)
            {
                Sweep(child.AsView(), into);
                continue;
            }
            Look look;
            if (LookAt(child.AsView(), look))
            {
                into.InsertOrAssign(child, look);
            }
        }
    }

    // False when `path` is not a regular file on the mount.
    bool ContentWatcher::LookAt(StringView path, Look& out)
    {
        foundation::vfs::FileStatInfo info;
        if (!m_project->ContentMount().Stat(path, info))
        {
            return false;
        }
        out.exists = true;
        out.size = info.size;
        out.modified = info.modifiedTime;
        return true;
    }
}
