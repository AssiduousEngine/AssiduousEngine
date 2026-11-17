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

    void EditorProject::MoveCodeIntoCodeFolder(EditorProject& project)
    {
        const StringView code = foundation::vfs::kSourcesCodeFolder;
        const String sourcesRoot = project.SourcesRoot();
        const String codeRoot = project.CodeRoot();
        const String ledger = pipeline::ImportLedgerPath(project.CacheRoot().AsView());
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

        u32 moved = 0;
        for (foundation::content::Instance* instance : instances)
        {
            RefPtr<ISerializable> object = instance->ReadObject();
            auto* asset = Cast<pipeline::Asset>(object.Get());
            StringView rest;
            if (asset == nullptr || !asset->IsCode() || asset->fileName.IsEmpty() ||
                foundation::vfs::AfterFolder(asset->fileName.View(), code, rest))
                continue;
            const String name(asset->fileName.View());
            const String from = PathJoin(sourcesRoot.AsView(), name.AsView());
            const String to = PathJoin(codeRoot.AsView(), name.AsView());
            if (FileExists(to.AsView()))
            {
                LOG_ERROR(u8"Project", u8"{} is not moved into Code/: a file of that name is there already", name);
                continue;
            }
            if (FileExists(from.AsView()))
            {
                (void)CreateDirectories(PathParent(to.AsView()));
                if (!FileMove(from.AsView(), to.AsView()))
                {
                    LOG_ERROR(u8"Project", u8"{} could not be moved into Code/", name);
                    continue;
                }
            }
            const String movedName = PathJoin(code, name.AsView());
            if (!Repoint(*instance, object, *asset, movedName.AsView()))
                continue;
            const String origin = pipeline::ImportOriginOf(ledger.AsView(), name.AsView());
            if (!origin.IsEmpty())
                pipeline::RecordImportOrigin(ledger.AsView(), movedName.AsView(), origin.AsView());
            ++moved;
        }
        if (moved > 0)
            LOG_INFO(u8"Project", u8"moved {} code file(s) from Sources/ into Code/", moved);
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
                            const pipeline::AssetCreatorRegistry& creators, StringView dataRoot,
                            bool allPrimitives)
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

        // 3. Primitive meshes: the creators File > New > Primitives runs (the cube, sphere and
        //    plane; every one with allPrimitives).
        const String sourcesRoot(project.SourcesRoot().AsView());
        const String codeRoot(project.CodeRoot().AsView());
        for (const pipeline::AssetCreator& creator : creators.All())
        {
            if (creator.category.AsView() != StringView(u8"Primitives"))
            {
                continue;
            }
            const StringView label = creator.label.AsView();
            const bool starter = label == StringView(u8"Cube") || label == StringView(u8"Sphere") ||
                                 label == StringView(u8"Plane");
            if (starter || allPrimitives)
            {
                (void)creator.Create(nullptr, root, sourcesRoot.AsView(), {}, codeRoot.AsView());
            }
        }

        LOG_INFO(u8"Editor", u8"starter content seeded (font/sky/primitives{})",
                 allPrimitives ? u8", all primitives" : u8"");
    }
}
