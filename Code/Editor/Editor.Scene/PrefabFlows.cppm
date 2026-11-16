// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :prefab_flows partition.
//
// The scene page's prefab flows, which write assets and ask first before they change one:
//   - making a prefab from an entity and what is under it (saved under "Prefabs/", named after the
//     entity, with the original replaced by an instance of it, as one undo group);
//   - writing an instance's state back to its prefab and rebuilding every instance in the open
//     scenes from it (one undo step; it asks first, since the asset changes);
//   - discarding an instance's overrides (one undo step; it asks first too, since entities
//     parented under the instance that are not the prefab's are removed);
//   - spawning an instance through the asset picker.
// It also holds the prefab half of a save: what refuses it (a prefab has one root and never
// contains itself), and rebuilding the other open scenes' instances once it is saved. It lives
// outside the page's class; the page's dialogs, its deferral and its picker come in as services.

module;
#include "Core/Prelude.h"

export module editor.scene:prefab_flows;

import foundation.core;
import foundation.content;
import foundation.resource; // AsyncBindScope
import foundation.scene;
import foundation.scene.resource;
import engine.scene;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    class PrefabFlows
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            SceneEditContext* edit = nullptr;
            engine::scene::SceneSubsystem* scenes = nullptr; // the open scenes (null: this one alone)
            /// The asset the page edits (it changes on Save As).
            Function<Guid()> pageAsset;
            /// Asks `text` under `title`; `yes` runs when the user takes `verb`.
            Function<void(StringView title, StringView text, StringView verb, Function<void()> yes)> confirm;
            /// Runs work after the event that asked for it (a dialog's answer).
            Function<void(Function<void()>)> later;
            /// Opens the asset picker over `types`; `chosen` gets what is picked.
            Function<void(Span<const String> types, Function<void(const Guid&)> chosen)> pickAsset;
        };

        explicit PrefabFlows(Services services) : m_services(Move(services)) {}

        /// Makes a prefab asset from `entityId` and everything under it, replacing the entity with
        /// an instance.
        void CreateFromEntity(const Guid& entityId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            if (project == nullptr)
                return;
            const foundation::scene::EntityHandle live = Edit().Resolve(entityId);
            if (!live.IsAssigned())
                return;
            MemoryStream payload;
            if (!foundation::scene::CapturePrefab(scene, live, payload).IsOk())
            {
                Context().Notify(NoticeKind::Error, u8"Prefab capture failed.");
                return;
            }
            foundation::content::Group* root = project->SourceDb().RootGroup();
            foundation::content::Group* prefabs = root->GetGroup(u8"Prefabs");
            if (prefabs == nullptr)
                prefabs = root->CreateGroup(u8"Prefabs");
            String base(scene.GetEntityName(live));
            if (base.IsEmpty())
                base = String(u8"Prefab");
            const String name = prefabs->UniqueInstanceName(base.AsView());
            foundation::content::Instance* asset = prefabs->CreateInstance(name.AsView(), foundation::scene::PrefabDocument::StaticType());
            if (asset == nullptr)
                return;
            foundation::scene::PrefabDocument doc;
            doc.name = name;
            if (!asset->WriteObject(doc).IsOk() || !asset->WriteData(u8"scene", payload.Bytes(), foundation::content::StreamEncoding::Text).IsOk())
            {
                Context().Notify(NoticeKind::Error, u8"Prefab asset write failed.");
                return;
            }
            Array<byte> bytes;
            const Span<const byte> view = payload.Bytes();
            bytes.Reserve(view.Size());
            for (byte b : view)
                bytes.PushBack(b);
            const Guid instanceRoot = Edit().ReplaceWithPrefabInstance(entityId, asset->Id(), Move(bytes));
            if (!instanceRoot.IsNil())
                Context().Notify(NoticeKind::Success, Format(u8"Created prefab '{}'.", name).AsView());
        }

        /// Asks, then writes the instance at `rootId` back to its prefab.
        void Apply(const Guid& rootId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            foundation::scene::Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootId);
            if (project == nullptr || state == nullptr || !m_services.confirm)
                return;
            foundation::content::Instance* asset = project->SourceDb().GetInstance(state->prefabId);
            if (asset == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"The instance's prefab asset no longer exists.");
                return;
            }
            const String text = Format(u8"Apply '{}' to prefab '{}'? The prefab asset is rewritten and every instance in open scenes "
                                       u8"updates to match.",
                                       scene.GetEntityName(scene.FindEntity(rootId)), asset->Name());
            PrefabFlows* self = this;
            // Deferred: rebuilding instances destroys and respawns entities (and their rows),
            // never in the middle of the dialog's event.
            m_services.confirm(u8"Apply to Prefab", text.AsView(), u8"Apply", [self, rootId]() { self->Later([self, rootId]() { self->ApplyNow(rootId); }); });
        }
        /// The instance's current state becomes the template (keyed by source id, so other
        /// instances' overrides stay valid), then every instance everywhere, this one included,
        /// rebuilds from it. One undo step: Undo writes the prefab as it was, rebuilds the other
        /// scenes' instances from it and puts this scene back.
        void ApplyNow(const Guid& rootId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            foundation::scene::Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootId);
            if (project == nullptr || state == nullptr)
                return;
            foundation::content::Instance* asset = project->SourceDb().GetInstance(state->prefabId);
            if (asset == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"The instance's prefab asset no longer exists.");
                return;
            }
            const Guid prefabId = state->prefabId;
            Array<byte> before = ReadPayload(*asset);
            Array<byte> after;
            {
                MemoryStream payload;
                if (!foundation::scene::CaptureInstanceAsTemplate(scene, *state, payload, Edit().PrefabResolver()).IsOk())
                {
                    Context().Notify(NoticeKind::Error, u8"Apply to Prefab failed.");
                    return;
                }
                for (byte b : payload.Bytes())
                    after.PushBack(b);
            }
            PrefabFlows* self = this;
            const auto write = [self, prefabId](const Array<byte>& bytes)
            {
                EditorProject* p = self->Project();
                foundation::content::Instance* a = p != nullptr ? p->SourceDb().GetInstance(prefabId) : nullptr;
                if (a == nullptr || !a->WriteData(u8"scene", Span<const byte>{bytes.Data(), bytes.Size()}, foundation::content::StreamEncoding::Text).IsOk())
                {
                    self->Context().Notify(NoticeKind::Error, u8"The prefab asset could not be written.");
                    return false;
                }
                // An open page on the prefab itself shows the template (plain entities, not an
                // instance): the rebuild cannot reach it, so it is told to refresh.
                (void)self->Context().NotifyAssetExternallyModified(prefabId);
                return true;
            };
            // Undo and Redo restore this scene from its snapshots; the other open scenes rebuild.
            const auto others = [self, prefabId, write](const Array<byte>& bytes)
            {
                if (write(bytes))
                    self->RebuildInstances(prefabId, Span<const byte>{bytes.Data(), bytes.Size()}, &self->Scene());
            };
            const bool applied = Edit().RunSnapshotCommand(
                u8"prefab_apply",
                [&]()
                {
                    if (!write(after))
                        return false;
                    RebuildInstances(prefabId, Span<const byte>{after.Data(), after.Size()}, nullptr);
                    return true;
                },
                [others, before = Move(before)]() { others(before); },
                [others, after]() { others(after); });
            if (!applied)
            {
                Context().Notify(NoticeKind::Error, u8"Apply to Prefab failed.");
                return;
            }
            Context().Notify(NoticeKind::Success, Format(u8"Applied to prefab '{}'.", asset->Name()).AsView());
        }

        /// Asks, then discards the instance's overrides.
        void Revert(const Guid& rootId)
        {
            foundation::scene::Scene& scene = Scene();
            if (Project() == nullptr || scene.FindPrefabInstanceByRoot(rootId) == nullptr || !m_services.confirm)
                return;
            const String text = Format(u8"Revert '{}' to its prefab? All overrides on this instance are discarded, and any non-prefab "
                                       u8"entities parented under it are destroyed.",
                                       scene.GetEntityName(scene.FindEntity(rootId)));
            PrefabFlows* self = this;
            m_services.confirm(u8"Revert Instance", text.AsView(), u8"Revert", [self, rootId]() { self->Later([self, rootId]() { self->RevertNow(rootId); }); });
        }
        /// Respawns the instance from the current template, keeping its placement, as one undo step.
        void RevertNow(const Guid& rootId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            foundation::scene::Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootId);
            if (project == nullptr || state == nullptr)
                return;
            foundation::content::Instance* asset = project->SourceDb().GetInstance(state->prefabId);
            const Array<byte> bytes = asset != nullptr ? ReadPayload(*asset) : Array<byte>{};
            if (bytes.IsEmpty())
            {
                Context().Notify(NoticeKind::Warning, u8"The instance's prefab asset no longer exists.");
                return;
            }
            const bool reverted = Edit().RunSnapshotCommand(u8"prefab_revert", [&]()
            {
                if (!foundation::scene::RevertPrefabInstance(scene, rootId, Span<const byte>{bytes.Data(), bytes.Size()}, Edit().PrefabResolver()))
                    return false;
                Edit().ResolveRestoredResources();
                return true;
            });
            if (reverted)
                Context().Notify(NoticeKind::Info, u8"Instance reverted to its prefab.");
        }

        /// Spawns an instance of the prefab the picker gives, under `parent` (nil: the scene's
        /// root).
        void PickAndSpawn(const Guid& parent)
        {
            if (Project() == nullptr || !m_services.pickAsset)
                return;
            const String types[] = {String(u8"PrefabDocument")};
            PrefabFlows* self = this;
            m_services.pickAsset(Span<const String>(types, 1u), [self, parent](const Guid& picked) { self->Spawn(picked, parent); });
        }
        /// Spawns an instance of prefab `prefabId` under `parent`; false, with the reason reported,
        /// when it cannot.
        bool Spawn(const Guid& prefabId, const Guid& parent)
        {
            EditorProject* project = Project();
            if (prefabId.IsNil() || project == nullptr)
                return false;
            if (m_services.pageAsset && prefabId == m_services.pageAsset())
            {
                Context().Notify(NoticeKind::Warning, u8"A prefab cannot contain an instance of itself.");
                return false;
            }
            foundation::content::Instance* prefab = project->SourceDb().GetInstance(prefabId);
            UniquePtr<IStream> payload = prefab != nullptr ? prefab->ReadData(u8"scene") : UniquePtr<IStream>{};
            if (payload.Get() == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"Prefab has no content yet (save it once first).");
                return false;
            }
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            (void)payload->Read(bytes.Data(), bytes.Size());
            return !Edit().SpawnPrefabInstance(prefabId, Move(bytes), parent).IsNil();
        }

        /// Why the scene, as the prefab `prefabId`, may not be saved; empty when it may.
        [[nodiscard]] static String SaveRefusal(foundation::scene::Scene& scene, const Guid& prefabId)
        {
            usize roots = 0;
            for (foundation::scene::EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
                ++roots;
            if (roots > 1)
                return String(u8"A prefab needs exactly one root entity - parent everything under a single root, then save.");
            bool self = false;
            scene.ForEachPrefabInstance([&](foundation::scene::Scene::PrefabInstanceState& state) { self = self || state.prefabId == prefabId; });
            if (self)
                return String(u8"A prefab cannot contain an instance of itself - remove it, then save.");
            return String{};
        }
        /// Called when the prefab `asset` was saved: its instances in every other open scene
        /// rebuild with their overrides kept (captured, respawned and applied again).
        void RebuildOtherInstances(foundation::content::Instance& asset)
        {
            UniquePtr<IStream> payload = asset.ReadData(u8"scene");
            if (payload.Get() == nullptr)
                return;
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            (void)payload->Read(bytes.Data(), bytes.Size());
            RebuildInstances(asset.Id(), Span<const byte>{bytes.Data(), bytes.Size()}, &Scene());
        }

    private:
        [[nodiscard]] EditorContext& Context() const { return *m_services.context; }
        [[nodiscard]] SceneEditContext& Edit() const { return *m_services.edit; }
        [[nodiscard]] foundation::scene::Scene& Scene() const { return m_services.edit->Scene(); }
        [[nodiscard]] EditorProject* Project() const { return m_services.context->Project(); }
        void Later(Function<void()> work)
        {
            if (m_services.later)
                m_services.later(Move(work));
            else
                work();
        }
        // Rebuilds every open scene's instances of `prefabId` from `payload`, except `skip`'s.
        static Array<byte> ReadPayload(foundation::content::Instance& asset)
        {
            Array<byte> bytes;
            UniquePtr<IStream> payload = asset.ReadData(u8"scene");
            if (payload.Get() == nullptr)
                return bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            if (payload->Read(bytes.Data(), bytes.Size()) != bytes.Size())
                bytes.Clear();
            return bytes;
        }

        void RebuildInstances(const Guid& prefabId, Span<const byte> payload, foundation::scene::Scene* skip)
        {
            EditorContext& context = Context();
            const auto rebuild = [&](foundation::scene::Scene& scene)
            {
                if (&scene == skip)
                    return;
                const u32 rebuilt = foundation::scene::RebuildPrefabInstances(scene, prefabId, payload, Edit().PrefabResolver());
                if (rebuilt > 0 && context.Resources() != nullptr)
                {
                    foundation::resource::AsyncBindScope asyncScope(*context.Resources());
                    foundation::scene::ResolveSceneResources(scene, *context.Resources());
                }
            };
            if (m_services.scenes != nullptr)
                m_services.scenes->ForEachScene(rebuild);
            else
                rebuild(Scene());
        }

        Services m_services;
    };
}
