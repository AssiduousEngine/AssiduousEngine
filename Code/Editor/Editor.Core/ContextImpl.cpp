// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :context partition.
//
// EditorContext: the central service object handed to every page, panel and plugin. It holds the
// open project, the registries, the open pages and the active page, the global asset selection,
// and the status sink. Per-subsystem editor modules register their factories here from
// RegisterEditor(EditorContext&), and the statically assembled editor executable calls those entry
// points.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.core;

import foundation.core;
import foundation.gui;
import foundation.resource;
import pipeline.importer;
import foundation.content;
import :command;
import :selection;
import :page;
import editor.project;

using namespace foundation::core;


namespace editor
{
    void EditorContext::RequestCook(bool rebuild)
    {
        if (OnCookRequested)
        {
            OnCookRequested(rebuild);
        }
    }

    void EditorContext::RegisterAssetEdit(const EditorCommandStack& owner, const Guid& assetId, AssetEditPersist persist)
    {
        if (assetId.IsNil() || !persist)
        {
            return; // in-memory or unresolved assets have no source to persist to (the edit stays live only)
        }
        for (PendingAssetEdit& e : m_pendingAssetEdits)
        {
            if (e.owner == &owner && e.id == assetId)
            {
                e.persist = Move(persist); // last edit for this asset wins
                return;
            }
        }
        m_pendingAssetEdits.PushBack(PendingAssetEdit{&owner, assetId, Move(persist)});
    }

    bool EditorContext::HasPendingAssetEdits(const EditorCommandStack& owner) const noexcept
    {
        for (const PendingAssetEdit& e : m_pendingAssetEdits)
        {
            if (e.owner == &owner)
            {
                return true;
            }
        }
        return false;
    }

    Status EditorContext::DrainAssetEdits(foundation::content::ContentDatabase& db, const EditorCommandStack& owner)
    {
        // Taken out first: a persist may register again (it should not, but the list must not
        // move under the loop).
        Array<PendingAssetEdit> mine;
        for (usize i = 0; i < m_pendingAssetEdits.Size();)
        {
            if (m_pendingAssetEdits[i].owner == &owner)
            {
                mine.PushBack(Move(m_pendingAssetEdits[i]));
                m_pendingAssetEdits.RemoveAt(i);
            }
            else
            {
                ++i;
            }
        }
        Status result{};
        bool anyOk = false;
        for (PendingAssetEdit& e : mine)
        {
            const Status s = e.persist(db);
            if (s.IsOk())
            {
                anyOk = true;
            }
            else if (result.IsOk())
            {
                result = s; // report the first failure (the others are still attempted)
            }
        }
        if (anyOk)
        {
            RequestCook(false); // hot-swap the re-persisted products
        }
        return result;
    }

    void EditorContext::DropAssetEdits(const EditorCommandStack& owner)
    {
        Array<Guid> dropped;
        for (usize i = 0; i < m_pendingAssetEdits.Size();)
        {
            if (m_pendingAssetEdits[i].owner == &owner)
            {
                dropped.PushBack(m_pendingAssetEdits[i].id);
                m_pendingAssetEdits.RemoveAt(i);
            }
            else
            {
                ++i;
            }
        }
        // The edit is in the live product, which every page showing it shares: build it again from
        // what was cooked, which is what the file held at its last save.
        if (m_resources != nullptr)
        {
            for (const Guid& id : dropped)
            {
                (void)m_resources->Reload(id);
            }
        }
    }

    StringView EditorContext::AssetNameFor(const Guid& id) const
    {
        if (id.IsNil())
        {
            return u8"(none)";
        }
        if (m_project != nullptr)
        {
            if (foundation::content::Instance* instance = m_project->SourceDb().GetInstance(id))
            {
                return instance->Name();
            }
        }
        return u8"(missing)";
    }

    void EditorContext::Notify(NoticeKind kind, StringView message)
    {
        // Errors and warnings always reach the console log too: a toast is transient, and a
        // failure a user reports from memory ("an error flashed") must be reconstructable from
        // the log.
        if (kind == NoticeKind::Error)
        {
            LOG_ERROR(u8"Editor", u8"{}", message);
        }
        else if (kind == NoticeKind::Warning)
        {
            LOG_WARNING(u8"Editor", u8"{}", message);
        }
        if (OnNotice)
        {
            OnNotice(kind, message);
        }
        else
        {
            SetStatus(message);
        }
    }

    void EditorContext::SetProject(EditorProject* project)
    {
        m_project = project;
        m_assetSelection.Clear();
    }

    void EditorContext::SetResources(foundation::resource::ResourceManager* resources) noexcept
    {
        m_resources = resources;
    }

    foundation::resource::ResourceManager* EditorContext::Resources() const noexcept
    {
        return m_resources;
    }

    bool EditorContext::IsFavorite(const Guid& id) const
    {
        for (const Guid& f : m_favorites)
        {
            if (f == id)
            {
                return true;
            }
        }
        return false;
    }

    void EditorContext::ToggleFavorite(const Guid& id)
    {
        for (usize i = 0; i < m_favorites.Size(); ++i)
        {
            if (m_favorites[i] == id)
            {
                m_favorites.RemoveAt(i);
                if (OnFavoritesChanged)
                {
                    OnFavoritesChanged();
                }
                return;
            }
        }
        m_favorites.PushBack(id);
        if (OnFavoritesChanged)
        {
            OnFavoritesChanged();
        }
    }

    Span<const Guid> EditorContext::Favorites() const noexcept
    {
        return Span<const Guid>{m_favorites.Data(), m_favorites.Size()};
    }

    void EditorContext::SetClipboard(StringView kind, Array<byte> data)
    {
        m_clipboardKind = String(kind);
        m_clipboard = Move(data);
    }

    void EditorContext::CopyToEditorClipboard(StringView kind, Array<byte> data, StringView what)
    {
        SetClipboard(kind, Move(data));
        Notify(NoticeKind::Success, Format(u8"Copied {}", what).AsView());
    }

    bool EditorContext::CopyText(foundation::gui::IClipboard* clipboard, StringView text,
                                 StringView what)
    {
        if (clipboard == nullptr)
        {
            Notify(NoticeKind::Warning, Format(u8"Could not copy {} to the clipboard", what).AsView());
            return false;
        }
        clipboard->SetText(text);
        Notify(NoticeKind::Success, Format(u8"Copied {}", what).AsView());
        return true;
    }

    Span<const byte> EditorContext::ClipboardData(StringView kind) const noexcept
    {
        return (m_clipboardKind == kind) ? Span<const byte>{m_clipboard.Data(), m_clipboard.Size()}
                                         : Span<const byte>{};
    }

    EditorPage* EditorContext::OpenPage(foundation::content::Instance& instance)
    {
        for (const UniquePtr<EditorPage>& page : m_pages)
        {
            if (page->InstanceId() == instance.Id())
            {
                SetActivePage(page.Get());
                return page.Get();
            }
        }

        const TypeInfo* type = GlobalTypeRegistry().FindByName(
            reinterpret_cast<const char*>(String(instance.TypeNamespace()).CStr()),
            reinterpret_cast<const char*>(String(instance.TypeName()).CStr()));
        if (type == nullptr)
        {
            return nullptr;
        }

        IEditorPageFactory* factory = m_pageRegistry.FindFactory(*type);
        if (factory == nullptr)
        {
            return nullptr;
        }

        const Stopwatch openClock = Stopwatch::StartNew();
        UniquePtr<EditorPage> page = factory->CreatePage(*this, instance);
        if (!page)
        {
            return nullptr;
        }
        // This number must stay small: a page's constructor runs on the UI thread. For a large
        // prefab that time is the document parse; its resources bind asynchronously and appear
        // later.
        LOG_INFO(u8"Editor", u8"page '{}' ({}) opened in {} ms", instance.Name(),
                 instance.TypeName(), static_cast<i64>(openClock.Elapsed().AsMilliseconds()));
        page->SetInstanceId(instance.Id());

        EditorPage* raw = page.Get();
        m_pages.PushBack(Move(page));
        m_activePage = raw;
        NotifyPagesChanged();
        return raw;
    }

    EditorPage* EditorContext::AdoptPage(UniquePtr<EditorPage> page)
    {
        if (!page)
        {
            return nullptr;
        }
        EditorPage* raw = page.Get();
        m_pages.PushBack(Move(page));
        m_activePage = raw;
        NotifyPagesChanged();
        return raw;
    }

    void EditorContext::ClosePage(EditorPage* page)
    {
        for (usize i = 0; i < m_pages.Size(); ++i)
        {
            if (m_pages[i].Get() == page)
            {
                // A page closed with edits unsaved: what its tools edited live goes with them.
                DropAssetEdits(page->Commands());
                if (m_activePage == page)
                {
                    m_activePage = m_pages.Size() > 1
                                       ? m_pages[i + 1 < m_pages.Size() ? i + 1 : i - 1].Get()
                                       : nullptr;
                }
                m_pages.RemoveAt(i);
                NotifyPagesChanged();
                return;
            }
        }
    }

    Span<const UniquePtr<EditorPage>> EditorContext::OpenPages() const noexcept
    {
        return Span<const UniquePtr<EditorPage>>{m_pages.Data(), m_pages.Size()};
    }

    usize EditorContext::NotifyAssetExternallyModified(const Guid& assetId)
    {
        ReloadAuthoredResource(assetId); // a loaded prefab reads it again
        usize told = 0;
        for (const UniquePtr<EditorPage>& page : m_pages)
        {
            if (page->InstanceId() == assetId)
            {
                page->OnAssetExternallyModified();
                ++told;
            }
        }
        return told;
    }

    usize EditorContext::NotifyAssetDeleted(const Guid& assetId, StringView recordPath, StringView typeNamespace,
                                            StringView typeName, const foundation::content::Instance* former)
    {
        if (m_thumbnails != nullptr)
        {
            m_thumbnails->Invalidate(assetId);
        }
        if (former != nullptr)
        {
            m_assetSelection.Remove(former);
        }
        // The loaded resource of an authored record (a prefab) is evicted, so a spawn of it is
        // refused rather than served from memory, and a held reference reads unbound until the
        // record comes back.
        if (m_resources != nullptr && m_resources->ReadsAuthoredRecord(typeNamespace, typeName))
        {
            m_resources->Evict(assetId);
        }
        usize told = 0;
        for (const UniquePtr<EditorPage>& page : m_pages)
        {
            if (page->InstanceId() != assetId)
            {
                continue;
            }
            page->NoteAssetDeleted(recordPath, typeNamespace, typeName);
            page->MarkDirty();
            page->OnAssetDeleted();
            Notify(NoticeKind::Warning,
                   Format(u8"'{}' was deleted on disk. It stays open; saving it writes it back.", page->Title()).AsView());
            ++told;
        }
        return told;
    }

    void EditorContext::ApplyContentChanges(const foundation::content::ContentChanges& changes,
                                            Span<const ContentWatcher::RemovedRecord> removed)
    {
        const auto listed = [](const Array<Guid>& list, const Guid& id)
        {
            for (const Guid& listedId : list)
            {
                if (listedId == id)
                {
                    return true;
                }
            }
            return false;
        };
        for (const Guid& id : changes.changed)
        {
            if (listed(changes.removed, id) || listed(changes.added, id))
            {
                continue;
            }
            if (m_thumbnails != nullptr)
            {
                m_thumbnails->Invalidate(id);
            }
            (void)NotifyAssetExternallyModified(id);
        }
        for (const Guid& id : changes.added)
        {
            if (m_thumbnails != nullptr)
            {
                m_thumbnails->Invalidate(id);
            }
            (void)NotifyAssetExternallyModified(id);
        }
        for (const ContentWatcher::RemovedRecord& record : removed)
        {
            (void)NotifyAssetDeleted(record.id, record.path.AsView(), record.typeNamespace.AsView(),
                                     record.typeName.AsView(), record.former);
        }
    }

    void EditorContext::ReloadAuthoredResource(const Guid& assetId)
    {
        if (m_resources == nullptr || !m_resources->HasHandle(assetId))
        {
            return;
        }
        const foundation::content::Instance* record = m_resources->Database().GetInstance(assetId);
        if (record != nullptr && m_resources->ReadsAuthoredRecord(record->TypeNamespace(), record->TypeName()))
        {
            (void)m_resources->Reload(assetId);
        }
    }

    Status EditorContext::SavePage(EditorPage& page)
    {
        const Status saved = SavePageItself(page);
        // What the page's tools edited live in other assets (a terrain's heights) is part of its
        // save: written whether or not the page's own file was.
        if (m_project == nullptr || !HasPendingAssetEdits(page.Commands()))
        {
            return saved;
        }
        const Status persisted = DrainAssetEdits(m_project->SourceDb(), page.Commands());
        if (!persisted.IsOk())
        {
            LOG_ERROR(u8"Editor", u8"'{}': writing what its tools edited in other assets failed", page.Title());
        }
        return saved.IsOk() ? persisted : saved;
    }

    bool EditorContext::DiscardPage(EditorPage& page)
    {
        if (!page.DiscardChanges())
        {
            return false;
        }
        DropAssetEdits(page.Commands());
        return true;
    }

    Status EditorContext::SavePageItself(EditorPage& page)
    {
        if (!page.IsAssetDeleted() || m_project == nullptr)
        {
            return page.Save();
        }
        foundation::content::ContentDatabase& db = m_project->SourceDb();
        if (db.GetInstance(page.InstanceId()) != nullptr)
        {
            page.ForgetAssetDeleted(); // back on disk already
            return page.Save();
        }
        const StringView path = page.DeletedRecordPath();
        foundation::content::Group* group = db.RootGroup();
        StringView name = path;
        const usize slash = path.FindLast(utf8char('/'));
        if (slash != StringView::kNotFound)
        {
            usize start = 0;
            for (usize i = 0; i <= slash; ++i)
            {
                if (i == slash || path[i] == utf8char('/'))
                {
                    const StringView part = path.SubStr(start, i - start);
                    if (!part.IsEmpty())
                    {
                        group = group->CreateGroup(part);
                    }
                    start = i + 1;
                }
            }
            name = path.SubStr(slash + 1, path.Size() - slash - 1);
        }
        if (group->GetInstance(name) != nullptr)
        {
            Notify(NoticeKind::Warning,
                   Format(u8"'{}' was deleted on disk and another asset is at '{}' now: Save As keeps it.", page.Title(), path)
                       .AsView());
            return Status{ErrorCode::AlreadyExists};
        }
        foundation::content::Instance* restored =
            group->AddInstance(page.InstanceId(), name, page.DeletedTypeNamespace(), page.DeletedTypeName());
        const Status saved = page.Save();
        if (!saved.IsOk())
        {
            (void)db.DeleteInstance(restored->Id());
            return saved;
        }
        page.ForgetAssetDeleted();
        return saved;
    }

    void EditorContext::SetActivePage(EditorPage* page)
    {
        if (m_activePage == page)
        {
            return;
        }
        m_activePage = page;
        NotifyPagesChanged();
    }

    void EditorContext::RevealPage(EditorPage* page)
    {
        SetActivePage(page);
        if (page != nullptr && OnRevealPage)
        {
            OnRevealPage(page);
        }
    }

    Selection<const foundation::content::Instance*>& EditorContext::AssetSelection() noexcept
    {
        return m_assetSelection;
    }

    void EditorContext::AddImportListener(
        Function<void(foundation::content::Instance&, const pipeline::ImportOptions*)> listener)
    {
        m_importListeners.PushBack(Move(listener));
    }

    void EditorContext::NotifyImported(foundation::content::Instance& instance,
                                       const pipeline::ImportOptions* options)
    {
        for (const auto& listener : m_importListeners)
        {
            listener(instance, options);
        }
    }

    void EditorContext::ToggleBreakpoint(StringView file, i32 line)
    {
        for (usize i = 0; i < m_breakpoints.Size(); ++i)
        {
            if (m_breakpoints[i].line == line && m_breakpoints[i].file.AsView() == file)
            {
                m_breakpoints.RemoveAt(i);
                if (OnBreakpointsChanged)
                {
                    OnBreakpointsChanged();
                }
                return;
            }
        }
        m_breakpoints.PushBack(ScriptBreakpoint{String(file), line});
        if (OnBreakpointsChanged)
        {
            OnBreakpointsChanged();
        }
    }

    bool EditorContext::HasBreakpoint(StringView file, i32 line) const
    {
        for (const ScriptBreakpoint& breakpoint : m_breakpoints)
        {
            if (breakpoint.line == line && breakpoint.file.AsView() == file)
            {
                return true;
            }
        }
        return false;
    }

    Span<const EditorContext::ScriptBreakpoint> EditorContext::Breakpoints() const noexcept
    {
        return Span<const ScriptBreakpoint>{m_breakpoints.Data(), m_breakpoints.Size()};
    }

    void EditorContext::SetStatus(StringView text)
    {
        if (OnStatus)
        {
            OnStatus(text);
        }
    }

    void EditorContext::NotifyPagesChanged()
    {
        if (OnPagesChanged)
        {
            OnPagesChanged();
        }
    }
}
