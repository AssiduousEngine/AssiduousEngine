// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_template partition.
//
// Export templates are portable, per-platform prebuilt bundles (a player binary, its runtime
// sidecars and a template.xml manifest) that presets reference by id or platform. They live in a
// machine-local templates root (not committed), can be imported or downloaded, and do not depend
// on any one machine's paths. The host's implicit template is synthesized from the running tool's
// own directory (Bin/...), so a development export for the current platform needs no setup.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.project;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import engine.project;
import :export_preset; // ExportPreset, ExportPresetSet

using namespace foundation::core;
namespace vfs = foundation::vfs;

namespace editor
{
    StringView ExportTemplate::EffectiveConfig() const noexcept
    {
        return config.IsEmpty() ? StringView(u8"Release") : config.AsView();
    }

    void ExportTemplate::Serialize(ISerializer& ar)
    {
        foundation::core::Serialize(ar, "id", id);
        foundation::core::Serialize(ar, "name", name);
        foundation::core::Serialize(ar, "platform", platform);
        foundation::core::Serialize(ar, "engineVersion", engineVersion);
        foundation::core::Serialize(ar, "playerBinary", playerBinary);
        foundation::core::Serialize(ar, "sidecars", sidecars);
        foundation::core::Serialize(ar, "notes", notes);
        // The (platform, config) axis: config, compiler metadata and a parallel symbols group.
        foundation::core::Serialize(ar, "config", config);
        foundation::core::Serialize(ar, "compiler", compiler);
        foundation::core::Serialize(ar, "symbols", symbols);
        SerializeAppended(ar, "icon", icon); // after the rest, since older manifests lack it
        SerializeAppended(ar, "plugins", plugins);
        if (ar.Mode() == SerializeMode::Read && config.IsEmpty())
        {
            config = String(u8"Release"); // an empty config resolves to Release
        }
    }
    const ExportTemplate* TemplateRegistry::FindBy(StringView platform, StringView config) const
    {
        const StringView wantConfig = config.IsEmpty() ? StringView(u8"Release") : config;

        // Pass 1: exact (platform, config).
        const ExportTemplate* exactHost = nullptr;
        for (const UniquePtr<ExportTemplate>& t : m_templates)
        {
            if (t->platform.AsView() != platform || t->EffectiveConfig() != wantConfig)
            {
                continue;
            }
            if (t->isHost)
            {
                exactHost = t.Get();
            }
            else
            {
                return t.Get();
            }
        }
        if (exactHost != nullptr)
        {
            return exactHost;
        }

        // Pass 2: platform-only fallback, preferring a Release config, imported over host.
        const ExportTemplate* bestImported = nullptr;
        bool bestImportedRelease = false;
        const ExportTemplate* bestHost = nullptr;
        bool bestHostRelease = false;
        for (const UniquePtr<ExportTemplate>& t : m_templates)
        {
            if (t->platform.AsView() != platform)
            {
                continue;
            }
            const bool isRelease = t->EffectiveConfig() == StringView(u8"Release");
            if (t->isHost)
            {
                if (bestHost == nullptr || (isRelease && !bestHostRelease))
                {
                    bestHost = t.Get();
                    bestHostRelease = isRelease;
                }
            }
            else
            {
                if (bestImported == nullptr || (isRelease && !bestImportedRelease))
                {
                    bestImported = t.Get();
                    bestImportedRelease = isRelease;
                }
            }
        }
        if (bestImported != nullptr)
        {
            return bestImported;
        }
        return bestHost;
    }

    const ExportTemplate* TemplateRegistry::Resolve(const ExportPreset& preset) const
    {
        if (!preset.templateId.IsEmpty())
        {
            return FindById(preset.templateId.AsView());
        }
        return FindBy(preset.platform.AsView(), preset.config.AsView());
    }

    // Declared in ExportTemplate.cppm.
    Status ImportTemplate(StringView srcDir, StringView templatesRoot, String* outId)
    {
        vfs::NativeFileSystem srcFs(srcDir, editor::EditorRootAllocator());
        ExportTemplate manifest;
        if (!LoadTemplateManifest(srcFs, manifest).IsOk() || manifest.id.IsEmpty())
        {
            return Status{ErrorCode::NotFound};
        }
        // The id names the folder it lands in: one plain name, never a path out of the root.
        if (!PathIsPlainComponent(manifest.id.AsView()))
        {
            return Status{ErrorCode::InvalidArgument};
        }

        (void)CreateDirectories(templatesRoot);
        const String dst = PathJoin(templatesRoot, manifest.id.AsView());
        if (!CopyDirectoryRecursive(srcDir, dst.AsView()))
        {
            return Status{ErrorCode::Internal};
        }
        if (outId != nullptr)
        {
            *outId = manifest.id;
        }
        return Status{};
    }

    Status RemoveTemplate(StringView templatesRoot, StringView templateId)
    {
        if (templateId.IsEmpty())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        // The folder whose template.xml names the id, among the root's own folders: never a path
        // built from the id (which a bundle wrote), and the right folder when one was renamed.
        vfs::NativeFileSystem rootFs(templatesRoot, editor::EditorRootAllocator());
        Array<vfs::DirEntry> entries;
        if (rootFs.AsEnumerable() == nullptr || !rootFs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            return Status{ErrorCode::NotFound};
        }
        for (const vfs::DirEntry& entry : entries)
        {
            if (!entry.isDirectory)
            {
                continue;
            }
            ExportTemplate manifest;
            const String manifestPath = PathJoin(entry.name.AsView(), kTemplateManifestFile);
            if (!LoadTemplateManifest(rootFs, manifest, manifestPath.AsView()).IsOk() ||
                manifest.id.AsView() != templateId)
            {
                continue;
            }
            const String dir = PathJoin(templatesRoot, entry.name.AsView());
            return RemoveDirectoryRecursive(dir.AsView()) ? Status{} : Status{ErrorCode::Internal};
        }
        return Status{ErrorCode::NotFound}; // nothing to delete, so it was not there
    }
}
