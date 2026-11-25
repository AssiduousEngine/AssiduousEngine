// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :page partition.
//
// The document model: each open asset is an EditorPage, a dock tab with its own command stack
// (undo is per page), dirty tracking, and Save. This is the headless half: concrete pages live in
// UI-side modules and add their widget tree on top.
//
// Pages are created by an IEditorPageFactory, chosen by EditorPageRegistry from the instance's
// primary-object type, with the nearest type along the base chain winning.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module editor.core:page;

import foundation.core;
import foundation.content;
import :command;

using namespace foundation::core;

export namespace editor
{
    class EditorContext; // defined in :context (pages receive it on creation)

    /// An interface a page implements and publishes for other parts of the editor to act
    /// through, such as a scene page's ISceneEditorPage (its edit context, its simulation
    /// control). The base keeps a page's service table typed: every service derives from
    /// IPageService, and the TypeId it is published under says which one, so pages need no
    /// erased pointers and no RTTI. Pages have no runtime type of their own; this is how another
    /// module learns what a page can do.
    class IPageService
    {
    protected:
        ~IPageService() = default;
    };

    /// A page that plays something back: a clip, a cue, an effect, a graph preview. The playback
    /// actions (playback.play, playback.stop, playback.restart) drive any page that publishes
    /// this, so every page's transport is the same three toolbar buttons with the same meaning:
    /// - Play is a toggle, checked while playing: it starts, pauses and resumes;
    /// - Stop stops and rewinds to the start;
    /// - Restart plays from the start.
    class IPlaybackPage : public IPageService
    {
    public:
        /// Whether there is anything to play: a clip loaded, an effect built.
        [[nodiscard]] virtual bool CanPlay() const = 0;
        [[nodiscard]] virtual bool IsPlaying() const = 0;
        /// Starts from the start when stopped, or resumes where it paused.
        virtual void Play() = 0;
        virtual void Pause() = 0;
        /// Stops and rewinds to the start.
        virtual void Stop() = 0;
        /// Plays from the start.
        virtual void Restart() = 0;

    protected:
        ~IPlaybackPage() = default;
    };

    /// A page that runs a game (a Game tab). The editor stops every one before it unloads code a
    /// run executes (a plugin reload); a run's own exit request stops only its own tab.
    class IGameRunPage : public IPageService
    {
    public:
        /// Stops the run, if one is going; the tab stays open.
        virtual void StopRun() = 0;
        /// Whether a run is going (or starting): it holds play in editor's saves until it stops.
        [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

    protected:
        ~IGameRunPage() = default;
    };

    /// A page that shows code (a script's, a shader's, a text file's): where a log line, a problem
    /// or an outside request points, it shows.
    class ICodePage : public IPageService
    {
    public:
        /// Moves the cursor to `line` and `column` (1-based; 0 is the start) and brings it into view.
        virtual void ShowLine(u32 line, u32 column) = 0;

    protected:
        ~ICodePage() = default;
    };

    // One open document. Owns its command stack; the context routes Edit > Undo and Redo to the
    // active page's stack. The page is unsaved when its history has left the point last saved, or
    // when it changed without a step (MarkDirty).
    class EditorPage
    {
    public:
        // The allocator (required; the page factory forwards the EditorContext's tagged "Editor"
        // root) backs everything this page creates.
        explicit EditorPage(IAllocator& allocator) : m_allocator(&allocator) {}

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }
        virtual ~EditorPage() = default;
        EditorPage(const EditorPage&) = delete;
        EditorPage& operator=(const EditorPage&) = delete;

        /// Tab title (typically the instance name).
        [[nodiscard]] virtual StringView Title() const = 0;

        /// Persist the edited object(s) back to the source database. Clears dirty on success.
        [[nodiscard]] virtual Status Save() = 0;

        [[nodiscard]] bool IsDirty() const noexcept { return m_markedDirty || !m_commands.IsAtSaved(); }
        /// The page changed without a step in its history.
        void MarkDirty() noexcept { m_markedDirty = true; }
        /// What the page shows is what its file holds (after a save or a load).
        void ClearDirty() noexcept
        {
            m_markedDirty = false;
            m_commands.MarkSaved();
        }

        /// Puts the page back to what its file holds (the toolbar's "Discard Changes"): the history
        /// goes back to the point last saved, or, when it no longer holds that point or the page
        /// changed without a step, the page reads its file again (ReloadFromSource). The history
        /// is dropped after. False, with the edits kept, when neither can be done: the history is
        /// locked (a scene page stops Simulate first), or the page cannot read its file.
        virtual bool DiscardChanges()
        {
            if (m_commands.IsLocked())
            {
                return false;
            }
            if (m_markedDirty || !m_commands.ReturnToSaved())
            {
                if (!ReloadFromSource())
                {
                    return false;
                }
            }
            m_commands.Clear();
            ClearDirty();
            return true;
        }

        /// Reads the page's file again and shows it, dropping the history. False, with nothing
        /// changed, when the page has no file or it does not read.
        virtual bool ReloadFromSource() { return false; }

        [[nodiscard]] EditorCommandStack& Commands() noexcept { return m_commands; }

        /// Unsaved work the page holds besides its asset, such as a clip its animation panel edits
        /// with its own history and Save. Each item is named for the prompts ("The clip 'Bounce'");
        /// none are listed when everything is saved. The page's unsaved mark never covers this
        /// work; closing the page or leaving the editor lists each item beside the page's own
        /// changes.
        virtual void ListUnsavedBesides(Array<String>& out) const { (void)out; }
        /// Saves all of that work; returns the first failure, if any.
        virtual Status SaveBesides() { return Status{}; }
        /// Whether the page's changes or that other work are unsaved; closing and exiting ask
        /// about this.
        [[nodiscard]] bool HasUnsavedWork() const
        {
            if (IsDirty())
                return true;
            Array<String> besides;
            ListUnsavedBesides(besides);
            return !besides.IsEmpty();
        }

        /// The asset this page edits changed outside the page (apply to prefab, reimport).
        /// The default does nothing; pages that cache loaded content override it to refresh.
        virtual void OnAssetExternallyModified() {}

        /// Its asset's file went OUTSIDE the editor (a pull, another tool) and the source database
        /// let its record go (EditorContext::NotifyAssetDeleted): the page stays open with what it
        /// shows, marked unsaved, and saving it (EditorContext::SavePage) writes the file back. A
        /// page with more to say of it overrides.
        virtual void OnAssetDeleted() {}

        /// Its asset's file went outside the editor and has not been written back.
        [[nodiscard]] bool IsAssetDeleted() const noexcept { return m_assetDeleted; }
        /// Where its asset's record lived ("group/name") while IsAssetDeleted; empty otherwise.
        [[nodiscard]] StringView DeletedRecordPath() const noexcept { return m_deletedPath.AsView(); }
        /// Its asset's stored type while IsAssetDeleted; empty otherwise.
        [[nodiscard]] StringView DeletedTypeNamespace() const noexcept { return m_deletedTypeNamespace.AsView(); }
        [[nodiscard]] StringView DeletedTypeName() const noexcept { return m_deletedTypeName.AsView(); }

        /// Rebind this page to a different source instance (Save As): the caller created
        /// `instance` and calls Save() next, so the page's current content lands there. Pages
        /// that cache the asset's name override this (calling the base) to refresh it.
        virtual void OnSavedAs(foundation::content::Instance& instance)
        {
            m_instanceId = instance.Id();
            ForgetAssetDeleted(); // it edits an asset that is there now
        }

        /// The source database instance this page edits (a zero Guid for pages with no instance).
        [[nodiscard]] const Guid& InstanceId() const noexcept { return m_instanceId; }
        void SetInstanceId(const Guid& id) noexcept { m_instanceId = id; }
        /// The registry key of the factory that made this page (EditorPageRegistry::Register); 0 for
        /// a page made without one.
        [[nodiscard]] u64 FactoryKey() const noexcept { return m_factoryKey; }

        // === Services: the interfaces this page publishes (a TypeId-keyed table, as the
        // SubsystemManager and Scene use, scoped to one page) ===

        /// Publish an interface this page implements, from its constructor. The table dies
        /// with the page, so nothing is ever unpublished.
        template <typename T>
            requires std::is_base_of_v<IPageService, T>
        void Provide(T& service)
        {
            m_services.InsertOrAssign(TypeOf<T>().id, static_cast<IPageService*>(&service));
        }

        /// The interface this page publishes as T, or null when this is not that kind of page.
        template <typename T>
            requires std::is_base_of_v<IPageService, T>
        [[nodiscard]] T* Service() const noexcept
        {
            IPageService* const* found = m_services.Find(TypeOf<T>().id);
            return found != nullptr ? static_cast<T*>(*found) : nullptr;
        }

    protected:
        IAllocator* m_allocator;
        EditorCommandStack m_commands;
        Guid m_instanceId{};

    private:
        u64 m_factoryKey = 0;
        friend class EditorContext; // tells it its asset went, and that it came back, and its factory
        void NoteAssetDeleted(StringView recordPath, StringView typeNamespace, StringView typeName)
        {
            m_assetDeleted = true;
            m_deletedPath = String(recordPath);
            m_deletedTypeNamespace = String(typeNamespace);
            m_deletedTypeName = String(typeName);
        }
        // The asset is back (written again, or returned on disk).
        void ForgetAssetDeleted()
        {
            m_assetDeleted = false;
            m_deletedPath.Clear();
            m_deletedTypeNamespace.Clear();
            m_deletedTypeName.Clear();
        }

        bool m_markedDirty = false; // changed without a step in the history
        HashMap<TypeId, IPageService*> m_services; // keyed like the SubsystemManager's subsystems
        // Where its asset's record lived when its file went outside the editor, and its stored type.
        bool m_assetDeleted = false;
        String m_deletedPath;
        String m_deletedTypeNamespace;
        String m_deletedTypeName;
    };

    // Creates pages for one primary-object type (and, through nearest-type dispatch, its
    // subclasses unless a more specific factory is registered).
    class IEditorPageFactory
    {
    public:
        virtual ~IEditorPageFactory() = default;

        /// The primary-object type this factory's pages edit.
        [[nodiscard]] virtual const TypeInfo* PrimaryType() const = 0;

        /// Create a page editing `instance`. Null on failure (an unreadable object, for one).
        [[nodiscard]] virtual UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) = 0;
    };

    // Factory registry with nearest-type dispatch: the factory whose PrimaryType() is closest
    // along the asset type's base chain wins.
    class EditorPageRegistry
    {
    public:
        EditorPageRegistry() = default;
        EditorPageRegistry(const EditorPageRegistry&) = delete;
        EditorPageRegistry& operator=(const EditorPageRegistry&) = delete;

        /// Adds a factory; its key for Unregister (0, nothing added, for a null one or one with no
        /// type). A plugin keeps the key and removes its factory when it unloads.
        u64 Register(UniquePtr<IEditorPageFactory> factory)
        {
            if (!factory || factory->PrimaryType() == nullptr)
            {
                return 0;
            }
            const u64 key = m_nextKey++;
            m_factories.PushBack(Entry{key, Move(factory)});
            return key;
        }

        /// Removes the factory `key`; false when there is none. The pages it made stay open: the
        /// editor's context closes them first (EditorContext::RemovePageFactory).
        bool Unregister(u64 key)
        {
            for (usize i = 0; i < m_factories.Size(); ++i)
            {
                if (m_factories[i].key == key)
                {
                    m_factories.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] IEditorPageFactory* FindFactory(const TypeInfo& type) const
        {
            const Entry* best = FindEntry(type);
            return best != nullptr ? best->factory.Get() : nullptr;
        }

        /// The key of the factory FindFactory answers for `type`; 0 for none.
        [[nodiscard]] u64 FindFactoryKey(const TypeInfo& type) const
        {
            const Entry* best = FindEntry(type);
            return best != nullptr ? best->key : 0;
        }

        [[nodiscard]] usize Size() const noexcept { return m_factories.Size(); }

    private:
        struct Entry
        {
            u64 key = 0;
            UniquePtr<IEditorPageFactory> factory;
        };

        [[nodiscard]] const Entry* FindEntry(const TypeInfo& type) const
        {
            const Entry* best = nullptr;
            u32 bestDistance = 0;
            for (const Entry& entry : m_factories)
            {
                u32 distance = 0;
                for (const TypeInfo* t = &type; t != nullptr; t = t->base, ++distance)
                {
                    if (t == entry.factory->PrimaryType())
                    {
                        if (best == nullptr || distance < bestDistance)
                        {
                            best = &entry;
                            bestDistance = distance;
                        }
                        break;
                    }
                }
            }
            return best;
        }

        Array<Entry> m_factories;
        u64 m_nextKey = 1;
    };
}
