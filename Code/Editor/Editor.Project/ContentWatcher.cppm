// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - the `:content_watcher` partition.
//
// ContentWatcher: watches a project's Content/ for records changed, added or deleted by something
// other than the editor (a git pull, a text editor, another tool) and brings the source database
// in line.
//
// A stat sweep: each Poll walks the folder and compares every file's size and modification time
// with what it last took in. A path is acted on only once it has settled, the same on two looks
// in a row, so a file still being written is never read half done; a path the database cannot read
// yet (cut off, broken) is looked at again on later polls until it reads or changes. One poll's
// settled paths go to one ContentDatabase::Refresh, so a record is reported once however many of
// its files changed, and while other paths are still changing the settled ones wait for them (up
// to kMaxHoldPolls polls), so a pull spread over several polls is one refresh.
//
// The editor's own writes are not outside changes: the project keeps the paths written through
// its Content/ mount (EditorProject::RecordContentWrites), and each poll first takes those paths in
// as they are now, reporting nothing. Every write the editor makes into Content/ goes through that
// mount: the source database's (a record written, a data stream, a rename, a move, a delete, a
// group's folder) and the code that writes beside it.
//
// A sweep is O(files), so the caller throttles it (EditorCookService polls about once a second and
// when the window regains focus) and holds it off while a background reader has the database,
// since a refresh changes its index. Main thread only.

module;
#include "Core/Prelude.h"

export module editor.project:content_watcher;

import foundation.core;
import foundation.vfs;
import foundation.content;
import :project;

using namespace foundation::core;

export namespace editor
{
    class ContentWatcher
    {
    public:
        /// How many polls settled paths wait for others still changing beside them before they go
        /// anyway: a file someone keeps writing must not hold the rest back for ever.
        static constexpr i32 kMaxHoldPolls = 5;

        /// Where a record a poll removed lived, for a page still editing it to write it back.
        struct RemovedRecord
        {
            Guid id;
            /// The record the database dropped, FREED: a holder of it (the asset selection)
            /// compares it by identity and never reads it.
            const foundation::content::Instance* former = nullptr;
            String path; // its record path, "group/name", without the extension
            String typeNamespace;
            String typeName;
        };

        /// Takes in what Content/ holds now, reporting none of it, and has the project keep the
        /// editor's writes until it goes. The project is BORROWED and outlives it.
        explicit ContentWatcher(EditorProject& project);
        ~ContentWatcher();
        ContentWatcher(const ContentWatcher&) = delete;
        ContentWatcher& operator=(const ContentWatcher&) = delete;

        /// Where each record the last Poll removed lived, valid until the next Poll.
        [[nodiscard]] Span<const RemovedRecord> RemovedRecords() const noexcept { return m_removed.AsSpan(); }

        /// Paths seen changing (or settled and waiting) that have not been acted on yet.
        [[nodiscard]] usize PendingCount() const noexcept { return m_pending.Size(); }

        /// One look at Content/: what the editor wrote since is taken in without a word, then
        /// every path that changed and has settled goes to the source database in one refresh, its
        /// changes into `out` (cleared first) and where each removed record lived into
        /// RemovedRecords. True when the refresh added, changed or removed a record.
        bool Poll(foundation::content::ContentChanges& out);

    private:
        struct Look
        {
            bool exists = false;
            u64 size = 0;
            i64 modified = 0;
            [[nodiscard]] bool Same(const Look& other) const noexcept
            {
                return exists == other.exists && size == other.size && modified == other.modified;
            }
        };
        // A path seen to differ from what was taken in, not acted on yet.
        struct Pending
        {
            Look last;
            bool settled = false; // the same on the last two looks
            i32 waited = 0;       // polls it has waited, settled, for paths still changing beside it
        };

        void Observe(StringView path, const Look& look);
        void TakeInWrites();
        void TakeIn(StringView path);
        void Sweep(StringView folder, HashMap<String, Look>& into);
        [[nodiscard]] bool LookAt(StringView path, Look& out);

        EditorProject* m_project; // borrowed
        HashMap<String, Look> m_snapshot; // every file under Content/ as last taken in
        HashMap<String, Pending> m_pending;
        Array<RemovedRecord> m_removed;
    };
}
