// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.project - the starter content a new project is seeded with. The editor's New Project and
// the MCP project_create both run this one function, so every new project ships a font (the
// engine's built-in font is not in a dist). And the content an older project gains on its upgrade:
// its first quality level.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.project;

import foundation.core;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import pipeline.importer;
import fonts.pipeline;
import texture.pipeline;
import render.pipeline; // a quality level (RenderQualityAsset)

using namespace foundation::core;

namespace editor
{
    namespace
    {
        // The group `name` under the project's root group, made if absent.
        foundation::content::Group* GroupNamed(foundation::content::Group* root, StringView name)
        {
            foundation::content::Group* group = root->GetGroup(name);
            return (group != nullptr) ? group : root->CreateGroup(name);
        }
    }

    namespace
    {
        // Every instance under `group`, its subgroups' included.
        void CollectInstances(foundation::content::Group& group, Array<foundation::content::Instance*>& out)
        {
            for (foundation::content::Instance* instance : group.Instances())
                out.PushBack(instance);
            for (foundation::content::Group* child : group.Groups())
                CollectInstances(*child, out);
        }

        // Points `instance`'s asset at `fileName`, keeping everything else it holds.
        bool Repoint(foundation::content::Instance& instance, RefPtr<ISerializable>& object, pipeline::Asset& asset, StringView fileName)
        {
            asset.fileName = foundation::vfs::SourcePath(fileName);
            return instance.WriteObject(*object).IsOk();
        }
    }

    namespace
    {
        // The folder an asset's file belongs in: its group's, under Code/ for code.
        String GroupFolderOf(const foundation::content::Instance& instance, const pipeline::Asset& asset)
        {
            const String group = instance.OwningGroup().Path();
            if (!asset.IsCode())
                return group;
            return group.IsEmpty() ? String(foundation::vfs::kSourcesCodeFolder) : PathJoin(foundation::vfs::kSourcesCodeFolder, group.AsView());
        }

        // `file` in `folder` ("" is the sources' root).
        String InFolder(StringView folder, StringView file) { return folder.IsEmpty() ? String(file) : PathJoin(folder, file); }

        // Removes `folder` (a file's old folder, as a source path) and its parents while they are
        // empty, `diskOf` naming each on disk.
        template <typename DiskOf>
        void RemoveEmptyFolders(StringView folder, DiskOf&& diskOf)
        {
            String at(folder);
            while (!at.IsEmpty() && at.AsView() != foundation::vfs::kSourcesCodeFolder)
            {
                const String full = diskOf(at.AsView());
                bool empty = true;
                (void)ListDirectory(full.AsView(), [](void* flag, StringView, bool) { *static_cast<bool*>(flag) = false; }, &empty);
                if (!empty || !DirectoryExists(full.AsView()) || !RemoveDirectory(full.AsView()))
                    return;
                at = String(PathParent(at.AsView()));
            }
        }
    }

    u32 EditorProject::PlaceInGroupFolders(Span<foundation::content::Instance* const> assets)
    {
        const String ledger = pipeline::ImportLedgerPath(CacheRoot().AsView());
        // Every asset's file and the folder its group asks for, across the whole project: a file
        // moves only when every asset naming it asks for the same folder.
        struct Use
        {
            foundation::content::Instance* instance = nullptr;
            String folder;
        };
        HashMap<String, Array<Use>> uses(*m_allocator);
        Array<foundation::content::Instance*> all(*m_allocator);
        CollectInstances(*SourceDb().RootGroup(), all);
        for (foundation::content::Instance* instance : all)
        {
            RefPtr<ISerializable> object = instance->ReadObject();
            const auto* asset = Cast<pipeline::Asset>(object.Get());
            if (asset == nullptr || asset->fileName.IsEmpty())
                continue;
            const String file(asset->fileName.View());
            Array<Use>* list = uses.Find(file);
            if (list == nullptr)
            {
                uses.InsertOrAssign(file, Array<Use>(*m_allocator));
                list = uses.Find(file);
            }
            list->PushBack(Use{instance, GroupFolderOf(*instance, *asset)});
        }

        // Which files are companions (a .gltf's sidecars), and of which files: a companion moves only
        // with the file it belongs to, never on its own (it would leave that file's relative path
        // behind), and a file whose companion another file also names stays where it is (moving
        // the companion would break the other's path).
        HashMap<String, Array<String>> ownersOf(*m_allocator);
        for (auto& entry : uses)
        {
            RefPtr<ISerializable> object = entry.value[0].instance->ReadObject();
            const auto* asset = Cast<pipeline::Asset>(object.Get());
            if (asset == nullptr)
                continue;
            const StringView folder = PathParent(entry.key.AsView());
            for (const String& uri : asset->CompanionFiles(SourceFile(entry.key.AsView()).AsView()))
            {
                const String companion = InFolder(folder, uri.AsView());
                Array<String>* owners = ownersOf.Find(companion);
                if (owners == nullptr)
                {
                    ownersOf.InsertOrAssign(companion, Array<String>(*m_allocator));
                    owners = ownersOf.Find(companion);
                }
                owners->PushBack(entry.key);
            }
        }

        // Repoints every asset that names `from` to `to`, and carries the ledger's origin.
        auto repoint = [&](StringView from, StringView to)
        {
            if (const Array<Use>* list = uses.Find(String(from)))
                for (const Use& use : *list)
                {
                    RefPtr<ISerializable> object = use.instance->ReadObject();
                    if (auto* asset = Cast<pipeline::Asset>(object.Get()))
                        (void)Repoint(*use.instance, object, *asset, to);
                }
            const String origin = pipeline::ImportOriginOf(ledger.AsView(), from);
            if (!origin.IsEmpty())
                pipeline::RecordImportOrigin(ledger.AsView(), to, origin.AsView());
        };

        auto diskOf = [this](StringView sourceName) { return SourceFile(sourceName); };
        u32 moved = 0;
        Array<String> done(*m_allocator);
        for (foundation::content::Instance* instance : assets)
        {
            RefPtr<ISerializable> object = instance->ReadObject();
            const auto* asset = Cast<pipeline::Asset>(object.Get());
            if (asset == nullptr || asset->fileName.IsEmpty())
                continue;
            const String file(asset->fileName.View());
            bool already = false;
            for (const String& d : done)
                already = already || d == file;
            if (already)
                continue;
            done.PushBack(file);
            const Array<Use>* list = uses.Find(file);
            const String folder = GroupFolderOf(*instance, *asset);
            bool agreed = list != nullptr;
            if (list != nullptr)
                for (const Use& use : *list)
                    agreed = agreed && use.folder == folder;
            if (!agreed || PathParent(file.AsView()) == folder.AsView())
                continue; // shared across groups, or in its folder already
            if (ownersOf.Find(file) != nullptr)
                continue; // another file's companion: it moves with that file

            const String fromDisk = SourceFile(file.AsView());
            if (!FileExists(fromDisk.AsView()))
                continue; // a missing file: its asset says so, nothing to move
            // Its name in the folder, the next free one when another file has it.
            const StringView stem = pipeline::FileStemOf(pipeline::FileNameOf(file.AsView()));
            const StringView name = pipeline::FileNameOf(file.AsView());
            const StringView extension = name.SubStr(stem.Size(), name.Size() - stem.Size());
            String target = InFolder(folder.AsView(), name);
            for (u32 n = 2; FileExists(SourceFile(target.AsView()).AsView()); ++n)
                target = InFolder(folder.AsView(), Format(u8"{}.{}{}", stem, n, extension).AsView());

            // Its companions, which keep their places relative to it; none may land on a file.
            const StringView oldFolder = PathParent(file.AsView());
            const Array<String> companions = asset->CompanionFiles(fromDisk.AsView());
            bool clear = true;
            bool sharedCompanion = false;
            for (const String& companion : companions)
            {
                clear = clear && !FileExists(SourceFile(InFolder(folder.AsView(), companion.AsView()).AsView()).AsView());
                const Array<String>* owners = ownersOf.Find(InFolder(oldFolder, companion.AsView()));
                sharedCompanion = sharedCompanion || (owners != nullptr && owners->Size() > 1);
            }
            if (sharedCompanion)
            {
                LOG_WARNING(u8"Project", u8"{} stays: a file beside it belongs to another file too", file);
                continue;
            }
            if (!clear)
            {
                LOG_WARNING(u8"Project", u8"{} stays: a file beside it would land on another in {}", file, folder);
                continue;
            }
            const String toDisk = SourceFile(target.AsView());
            (void)CreateDirectories(PathParent(toDisk.AsView()));
            if (!FileMove(fromDisk.AsView(), toDisk.AsView()))
            {
                LOG_ERROR(u8"Project", u8"{} could not be moved to {}", file, target);
                continue;
            }
            repoint(file.AsView(), target.AsView());
            for (const String& companion : companions)
            {
                const String from = InFolder(oldFolder, companion.AsView());
                const String to = InFolder(folder.AsView(), companion.AsView());
                const String companionDisk = SourceFile(from.AsView());
                if (!FileExists(companionDisk.AsView()))
                    continue;
                const String companionTo = SourceFile(to.AsView());
                (void)CreateDirectories(PathParent(companionTo.AsView()));
                if (FileMove(companionDisk.AsView(), companionTo.AsView()))
                {
                    repoint(from.AsView(), to.AsView());
                    done.PushBack(from);
                    RemoveEmptyFolders(PathParent(from.AsView()), diskOf);
                }
            }
            RemoveEmptyFolders(oldFolder, diskOf);
            ++moved;
        }
        return moved;
    }

    u32 EditorProject::AdoptNewCode(Span<const String> changed, const pipeline::ImporterRegistry& importers)
    {
        const StringView code = foundation::vfs::kSourcesCodeFolder;
        u32 adopted = 0;
        Array<foundation::content::Instance*> all(*m_allocator); // read once, when a candidate needs it
        bool collected = false;
        for (const String& path : changed)
        {
            StringView rest;
            if (!foundation::vfs::AfterFolder(path.AsView(), code, rest) || rest.IsEmpty())
                continue;
            // The editor writes an outside editor's files here (the declarations, engine.d.luau
            // among them, whose extension a script importer takes): they are not the project's code.
            if (IsCodeToolingFile(rest))
                continue;
            const String onDisk = SourceFile(path.AsView());
            if (!FileExists(onDisk.AsView()))
                continue; // removed: its asset, if any, finds its file missing
            pipeline::IFileImporter* importer = importers.FindFor(pipeline::FileExtensionLower(path.AsView()).AsView());
            if (importer == nullptr || !importer->ImportsCode())
                continue;
            // One an asset names already is not new (its edit is a cook's business).
            if (!collected)
            {
                CollectInstances(*SourceDb().RootGroup(), all);
                collected = true;
            }
            bool named = false;
            for (const foundation::content::Instance* instance : all)
                named = named || instance->SourceName() == path.AsView();
            if (named)
                continue;
            // Its group: the folders between Code/ and the file, made where missing.
            foundation::content::Group* group = SourceDb().RootGroup();
            const StringView folders = PathParent(rest);
            usize start = 0;
            for (usize i = 0; i <= folders.Size() && group != nullptr; ++i)
            {
                if (i < folders.Size() && folders[i] != utf8char('/'))
                    continue;
                if (i > start)
                {
                    const StringView name = folders.SubStr(start, i - start);
                    foundation::content::Group* child = group->GetGroup(name);
                    group = child != nullptr ? child : group->CreateGroup(name);
                }
                start = i + 1;
            }
            if (group == nullptr)
                continue;
            pipeline::ImportContext context{*m_allocator, SourcesRoot()};
            context.codeRoot = CodeRoot();
            context.ledgerPath = pipeline::ImportLedgerPath(CacheRoot().AsView());
            // Its copy is itself (the import's target in its group's folder is where it lies), so
            // nothing is written but its asset.
            Result<foundation::content::Instance*> made = importer->Import(onDisk.AsView(), context, *group, nullptr, nullptr, nullptr);
            if (!made.HasValue())
            {
                LOG_WARNING(u8"Project", u8"{} is not imported: {} has an asset of its name already", path, group->Path());
                continue;
            }
            LOG_INFO(u8"Project", u8"imported new code {} into '{}'", path, group->Path());
            ++adopted;
        }
        return adopted;
    }

    void EditorProject::PlaceFilesAtOpen(EditorProject& project)
    {
        const StringView code = foundation::vfs::kSourcesCodeFolder;
        const String sourcesRoot = project.SourcesRoot();
        Array<foundation::content::Instance*> instances;
        CollectInstances(*project.SourceDb().RootGroup(), instances);

        // A folder Sources/Code from before would be hidden by Code/: renamed first, and the assets
        // naming files in it follow it.
        const String hidden = PathJoin(sourcesRoot.AsView(), code);
        if (DirectoryExists(hidden.AsView()))
        {
            String renamed = Format(u8"{}.moved", code);
            for (u32 n = 2; DirectoryExists(PathJoin(sourcesRoot.AsView(), renamed.AsView()).AsView()); ++n)
                renamed = Format(u8"{}.moved{}", code, n);
            if (!FileMove(hidden.AsView(), PathJoin(sourcesRoot.AsView(), renamed.AsView()).AsView()))
            {
                LOG_ERROR(u8"Project", u8"Sources/{} could not be renamed, so the project's code stays under Sources/ for now", code);
                return;
            }
            LOG_WARNING(u8"Project", u8"Sources/{} is now Sources/{}: Code/ is the project's code folder", code, renamed);
            for (foundation::content::Instance* instance : instances)
            {
                RefPtr<ISerializable> object = instance->ReadObject();
                auto* asset = Cast<pipeline::Asset>(object.Get());
                StringView rest;
                if (asset != nullptr && foundation::vfs::AfterFolder(asset->fileName.View(), code, rest))
                    (void)Repoint(*instance, object, *asset, PathJoin(renamed.AsView(), rest).AsView());
            }
        }

        const u32 moved = project.PlaceInGroupFolders(Span<foundation::content::Instance* const>{instances.Data(), instances.Size()});
        if (moved > 0)
            LOG_INFO(u8"Project", u8"moved {} file(s) into their groups' folders", moved);
    }

    void EditorProject::UpgradeToQualityLevels(EditorProject& project, u32 msaa)
    {
        ProjectSettings& settings = project.Settings();
        foundation::content::Group* group = GroupNamed(project.SourceDb().RootGroup(), u8"Quality");
        foundation::content::Instance* instance = group != nullptr ? group->GetInstance(u8"Default") : nullptr;
        if (instance == nullptr && group != nullptr)
        {
            instance = group->CreateInstance(u8"Default", pipeline::RenderQualityAsset::StaticType());
        }
        pipeline::RenderQualityAsset asset;
        asset.values.label = String(u8"Default");
        asset.values.msaaSamples = msaa;
        if (instance == nullptr || instance->TypeName() != StringView(u8"RenderQualityAsset") ||
            !instance->WriteObject(asset).IsOk())
        {
            LOG_ERROR(u8"Project", u8"the upgrade to quality levels could not write Quality/Default: the project "
                                   u8"runs at the engine's own highest level until it lists one");
            return;
        }
        settings.qualityLevelIds.PushBack(instance->Id());
        settings.defaultQualityId = instance->Id();
        if (!project.SaveSettings().IsOk())
        {
            LOG_ERROR(u8"Project", u8"the upgrade to quality levels wrote Quality/Default but not the manifest");
            return;
        }
        LOG_INFO(u8"Project",
                 u8"upgraded to quality levels: Quality/Default at {}x MSAA is the game's level; antialiasing "
                 u8"is the level's now, so give it the one the scenes asked for (their loads say which)",
                 msaa);
    }

    void SeedStarterContent(IAllocator& allocator, EditorProject& project,
                            const pipeline::AssetCreatorRegistry& creators, StringView dataRoot)
    {
        foundation::content::Group* root = project.SourceDb().RootGroup();
        pipeline::ImportContext import{allocator, project.SourcesRoot()};
        import.codeRoot = project.CodeRoot();

        // 1. The UI font: Roboto as a font asset, set as the manifest's default (the font a
        //    shipped game binds; the built-in one is not in a dist). A distance-field font, so a
        //    72 px title and a 14 px caption both draw clean from its one bake.
        {
            const String source =
                foundation::vfs::DataPath(dataRoot, u8"Engine/Fonts/roboto/Roboto-Regular.ttf");
            import.folder = String(u8"Fonts"); // in the group it is added to
            Result<String> copied = pipeline::CopyIntoSources(import, source.AsView());
            if (copied.HasValue())
            {
                if (foundation::content::Instance* instance = GroupNamed(root, u8"Fonts")->CreateInstance(
                        u8"Roboto", pipeline::FontAsset::StaticType()))
                {
                    pipeline::FontAsset asset;
                    asset.fileName = foundation::vfs::SourcePath(copied.Value().AsView());
                    asset.family = String(u8"Roboto");
                    asset.mode = pipeline::FontBakeMode::DistanceField;
                    if (instance->WriteObject(asset).IsOk())
                    {
                        project.Settings().defaultUiFontId = instance->Id();
                    }
                }
            }
            else
            {
                LOG_WARNING(u8"Editor", u8"starter font missing ({}) - new project has no default UI font",
                            source);
            }
        }

        // 2. The default sky: Poly Haven's Kloofendal 48d Partly Cloudy (Pure Sky) at 1K, CC0 (its
        //    licence beside it), an equirectangular skybox texture. It is a 1024 x 512 HDR of 1.4 MB,
        //    small enough to cook quickly, which is all a starting sky needs.
        {
            const String source = foundation::vfs::DataPath(
                dataRoot, u8"Templates/kloofendal_48d_partly_cloudy_puresky/kloofendal_48d_partly_cloudy_puresky_1k.hdr");
            import.folder = String(u8"Environment"); // in the group it is added to
            Result<String> copied = pipeline::CopyIntoSources(import, source.AsView());
            if (copied.HasValue())
            {
                if (foundation::content::Instance* instance = GroupNamed(root, u8"Environment")->CreateInstance(
                        u8"Sky", pipeline::TextureAsset::StaticType()))
                {
                    pipeline::TextureAsset asset;
                    asset.fileName = foundation::vfs::SourcePath(copied.Value().AsView());
                    asset.SetupForEquirectangularSkybox();
                    (void)instance->WriteObject(asset);
                }
            }
        }

        // 3. Primitive meshes: every creator File > New > Primitives runs.
        const String sourcesRoot(project.SourcesRoot().AsView());
        const String codeRoot(project.CodeRoot().AsView());
        for (const pipeline::AssetCreator& creator : creators.All())
        {
            if (creator.category.AsView() != StringView(u8"Primitives"))
            {
                continue;
            }
            (void)creator.Create(nullptr, root, sourcesRoot.AsView(), {}, codeRoot.AsView());
        }

        LOG_INFO(u8"Editor", u8"starter content seeded (font/sky/primitives)");
    }
}
