// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// EditorProject tests: the create and open round-trip of the fixed project layout (manifest,
// subdirectories, and the source (XML) and cooked (binary) content databases).

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.settings; // the settings attributes (label, assetType, ...)
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;
import foundation.runtime; // the plugins' entries
import engine.project;
import engine.render;  // a quality level's values
import render.pipeline; // RenderQualityAsset: an older project's upgrade
import pipeline.core;   // Asset: what a source file names
import script.pipeline; // ScriptClassAsset: code, which an older project kept under Sources/
import pipeline.importer; // ImporterRegistry (new code found under Code/)
#ifdef OPTION_HAS_ANGELSCRIPT
import angelscript.foundation; // a code language's backend for the adoption test
#endif
#ifdef OPTION_HAS_LUAU
import luau.foundation; // a language whose declarations file has a script's extension
#endif
import editor.core;

using namespace foundation::core;
using namespace editor;

namespace
{
    class TestMaterial final : public ISerializable
    {
        RTTI_OBJECT(TestMaterial, ISerializable)
    public:
        i32 shininess = 0;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "shininess", shininess);
        }
    };

    void RemoveProjectTree(StringView root)
    {
        FileDelete(PathJoin(root, u8"Project.xml"));
        FileDelete(PathJoin(root, u8"Content/materials/steel.xasset"));
        FileDelete(PathJoin(root, u8"Cooked/materials/steel.rasset"));
        RemoveDirectory(PathJoin(root, u8"Content/materials"));
        RemoveDirectory(PathJoin(root, u8"Cooked/materials"));
        RemoveDirectory(PathJoin(root, u8"Content"));
        RemoveDirectory(PathJoin(root, u8"Sources"));
        RemoveDirectory(PathJoin(root, u8"Cooked"));
        RemoveDirectory(PathJoin(root, u8"Editor"));
        RemoveDirectory(PathJoin(root, u8".cache"));
        RemoveDirectory(root);
    }
}

namespace
{
    // A model-like asset: its file lists, one per line, the files beside it that belong with it.
    class TestModelAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(TestModelAsset, pipeline::Asset)
    public:
        [[nodiscard]] Array<String> CompanionFiles(StringView fileOnDisk) const override
        {
            Array<String> companions;
            Result<Array<byte>> bytes = ReadFile(fileOnDisk);
            if (!bytes.HasValue())
                return companions;
            const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
                if (i == text.Size() || text[i] == utf8char('\n'))
                {
                    if (i > start)
                        companions.PushBack(String(text.SubStr(start, i - start)));
                    start = i + 1;
                }
            return companions;
        }
    };
}

RTTI_DEFINE_OBJECT(TestMaterial, "rtti::editor::editor::test")
RTTI_DEFINE_OBJECT(TestModelAsset, "rtti::editor::editor::test")

TEST_CASE("editor-project: create scaffolds the layout and open round-trips the manifest")
{
    const StringView dir = u8"scratch_editor_test_project";
    RemoveProjectTree(dir);

    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Test Project").IsOk());
    CHECK(FileExists(PathJoin(dir, u8"Project.xml")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Content")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Sources")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Cooked")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Editor")));
    CHECK(DirectoryExists(PathJoin(dir, u8".cache")));

    // Creating again fails: the manifest already exists.
    CHECK(EditorProject::Create(DefaultAllocator(), dir, u8"Test Project").Code() == ErrorCode::AlreadyExists);

    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    CHECK(project->Name() == u8"Test Project");
    REQUIRE(project->Settings().plugins.Size() == 1u); // the default language, and nothing else
    CHECK(project->Settings().plugins[0].id == StringView(u8"angelscript"));
    // (The manifest's data version is in the versioned-payload envelope, not a field.)
    CHECK(project->Settings().defaultScene.IsEmpty());
    // A new project lists no quality levels: it runs at the engine's own highest (4x MSAA).
    CHECK(project->Settings().qualityLevelIds.IsEmpty());
    CHECK(project->Settings().msaaSamplesBeforeLevels == 0u); // nothing to upgrade
    CHECK(project->SourceDb().RootGroup() != nullptr);
    CHECK(project->CookedDb().RootGroup() != nullptr);
    CHECK(project->SourcesRoot() == PathJoin(dir, u8"Sources"));
    CHECK(project->EditorStateRoot() == PathJoin(dir, u8"Editor"));
    // The saves folder's name, which play in editor keeps under Editor/ and the player in the user
    // data directory: the same name in both, so saves copied between them are found.
    CHECK(engine::project::SavesFolder(project->Settings().name.AsView(), DefaultAllocator()) ==
          u8"Test Project.saves");
    CHECK(engine::project::SavesFolder(u8"", DefaultAllocator()) == u8"project.saves");

    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: open fails without a manifest")
{
    const StringView dir = u8"scratch_editor_test_project_missing";
    RemoveProjectTree(dir);
    CHECK(!static_cast<bool>(EditorProject::Open(DefaultAllocator(), dir)));
    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: an unreadable manifest logs an error (missing one stays silent)")
{
    struct CaptureSink final : ILogSink
    {
        usize errors = 0;
        String lastMessage;
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (level == LogLevel::Error && category == u8"Project")
            {
                ++errors;
                lastMessage = String(message);
            }
        }
    };
    CaptureSink sink;
    GlobalLogger().AddSink(&sink);

    // Absent manifest: the scaffold path, with no error noise.
    const StringView missingDir = u8"scratch_editor_test_project_silent";
    RemoveProjectTree(missingDir);
    CHECK(!static_cast<bool>(EditorProject::Open(DefaultAllocator(), missingDir)));
    CHECK(sink.errors == 0);

    // A present but unparseable manifest (for example a pre-versioning format) fails loudly:
    // the app shell only surfaces this in the status bar, so the console line is the signal.
    const StringView dir = u8"scratch_editor_test_project_corrupt";
    RemoveProjectTree(dir);
    REQUIRE(CreateDirectory(dir));
    {
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        const StringView garbage = u8"<root><string name=\"name\">P</string></root>";
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml",
                           Span<const byte>(reinterpret_cast<const byte*>(garbage.Data()),
                                            garbage.Size()))
                    .IsOk());
    }
    CHECK(!static_cast<bool>(EditorProject::Open(DefaultAllocator(), dir)));
    CHECK(sink.errors == 1);
    const auto contains = [](StringView haystack, StringView needle)
    {
        if (needle.Size() > haystack.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= haystack.Size(); ++i)
        {
            if (haystack.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(contains(sink.lastMessage.AsView(), u8"Project.xml"));

    GlobalLogger().RemoveSink(&sink);
    RemoveProjectTree(dir);
    RemoveProjectTree(missingDir);
}

TEST_CASE("editor-project: settings changes persist through SaveSettings")
{
    const StringView dir = u8"scratch_editor_test_project_save";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    Guid savedId;

    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        REQUIRE(Guid::TryParse(u8"6ba7b810-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultSceneId));
        REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().guiLoadingDocumentId));
        REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultInputMapId));
        project->Settings().defaultScene = String(u8"scenes/main");
        savedId = project->Settings().defaultSceneId;
        Guid fontId;
        REQUIRE(Guid::TryParse(u8"6ba7b813-9dad-11d1-80b4-00c04fd430c8", fontId));
        project->Settings().defaultUiFontId = fontId; // the Default UI font settings row
        CHECK(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().defaultScene == u8"scenes/main");
        CHECK(project->Settings().defaultSceneId == savedId); // guid is authoritative
        Guid loadingId;
        REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8", loadingId));
        CHECK(project->Settings().guiLoadingDocumentId == loadingId); // an appended field round-trips
        Guid mapId;
        REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8", mapId));
        // Open's per-field settings move must not drop defaultInputMapId (silent data loss).
        CHECK(project->Settings().defaultInputMapId == mapId);
        Guid fontId;
        REQUIRE(Guid::TryParse(u8"6ba7b813-9dad-11d1-80b4-00c04fd430c8", fontId));
        CHECK(project->Settings().defaultUiFontId == fontId); // Default UI font round-trips
    }

    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: source db is XML, cooked db is binary, both round-trip")
{
    GlobalTypeRegistry().Register(TestMaterial::StaticType());
    RegisterSerializable<TestMaterial>();

    const StringView dir = u8"scratch_editor_test_project_dbs";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());

    Guid sourceId;
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));

        // Author a source instance and a cooked instance.
        auto* srcGroup = project->SourceDb().RootGroup()->CreateGroup(u8"materials");
        REQUIRE(srcGroup != nullptr);
        auto* src = srcGroup->CreateInstance(u8"steel", TestMaterial::StaticType());
        REQUIRE(src != nullptr);
        sourceId = src->Id();
        TestMaterial mat;
        mat.shininess = 7;
        REQUIRE(src->WriteObject(mat).IsOk());

        auto* cookedGroup = project->CookedDb().RootGroup()->CreateGroup(u8"materials");
        auto* cooked = cookedGroup->CreateInstance(u8"steel", TestMaterial::StaticType());
        REQUIRE(cooked != nullptr);
        REQUIRE(cooked->WriteObject(mat).IsOk());
    }

    // Envelope extensions match the source/cooked split: source readable XML, cooked binary.
    CHECK(FileExists(PathJoin(dir, u8"Content/materials/steel.xasset")));
    CHECK(FileExists(PathJoin(dir, u8"Cooked/materials/steel.rasset")));

    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        RefPtr<ISerializable> obj = project->SourceDb().ReadObject(sourceId);
        REQUIRE(obj.Get() != nullptr);
        TestMaterial* mat = Cast<TestMaterial>(obj.Get());
        REQUIRE(mat != nullptr);
        CHECK(mat->shininess == 7);
    }

    RemoveProjectTree(dir);
}

TEST_CASE("project: manifest round-trips the startup-script asset guid under a versioned payload")
{
    const StringView dir = u8"scratch_project_v2_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);

    const Guid scriptId = Guid{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().runBehaviorId.IsNil());
        project->Settings().runBehaviorId = scriptId;
        REQUIRE(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().runBehaviorId == scriptId);
    }

    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

TEST_CASE("project: this engine's release downloads come from its own version's release")
{
    // The repository CMake stamps (ENGINE_RELEASE_REPOSITORY), at v<the version with its label>,
    // the release's tag.
    CHECK(engine::project::kEngineReleaseRepository.ContainsIgnoreCase(u8"/")); // owner/name
    const String url = engine::project::EngineReleaseDownloadUrl(u8"samples.json");
    const String expected = Format(u8"https://github.com/{}/releases/download/v{}/samples.json", engine::project::kEngineReleaseRepository,
                                   engine::project::kEngineVersionDisplay);
    CHECK(url == expected);
    CHECK(url.AsView().StartsWith(u8"https://github.com/"));
}

// The version a person reads is the compared version with the release's label, if any; a build
// names itself by it, its build number (or as a local build) and its stamp.
TEST_CASE("project: the display version carries the label, and a build's identity its number or local")
{
    const StringView version = engine::project::kEngineVersionString;
    const StringView label = engine::project::kEngineVersionLabel;
    const String display = label.IsEmpty() ? String(version) : Format(u8"{}-{}", version, label);
    CHECK(engine::project::kEngineVersionDisplay == display.AsView());
    CHECK(engine::project::BuildIdentity(u8"abc1234 @ 2026-11-04 15:03", 412) ==
          Format(u8"{} (build 412, abc1234 @ 2026-11-04 15:03)", display));
    CHECK(engine::project::BuildIdentity(u8"abc1234-dirty.0f0f0f0f @ 2026-11-04 15:03", 0) ==
          Format(u8"{} (local build, abc1234-dirty.0f0f0f0f @ 2026-11-04 15:03)", display));
    // The compared version stays plain: a plugin manifest's check reads it.
    CHECK(foundation::runtime::ParsePluginVersion(version).HasValue());
}

TEST_CASE("project: manifests carry the engine version stamp; a stale-version manifest is refused")
{
    const StringView dir = u8"scratch_project_engine_ver_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);

    // Every save stamps the current engine version (the launcher's routing signal).
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().engineVersion == engine::project::kEngineVersionString);
        // Comparing settings against the constant cannot catch the ENGINE_VERSION_* defines
        // silently not reaching the target; the fallback value can. A build stamped 0.0.0-dev
        // means project(VERSION) did not reach it.
        CHECK(engine::project::kEngineVersionString != StringView(u8"0.0.0-dev"));
    }

    // A manifest stamped with another ProjectSettings data version (here 1, which had no
    // engineVersion key) is refused: Open fails instead of guessing a layout. The project
    // has to be re-saved by the build that wrote it (or recreated).
    {
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        engine::project::ProjectSettings stale;
        stale.name = String(u8"P");
        stale.defaultScene = String(u8"Scenes/S");
        MemoryStream buffer;
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        const SerializedDataVersion chain[] = {
            {engine::project::ProjectSettings::StaticType().id, 1u}};
        ctx->serializer->Key("dataVersions");
        u32 count = 1;
        ctx->serializer->BeginArray(count);
        u64 typeId = chain[0].typeId;
        u32 version = chain[0].version;
        ctx->serializer->Key("type");
        ctx->serializer->Scalar(&typeId, ScalarKind::UInt64);
        ctx->serializer->Key("version");
        ctx->serializer->Scalar(&version, ScalarKind::UInt32);
        ctx->serializer->EndArray();
        ctx->serializer->PushVersionScope(chain, 1);
        stale.Serialize(*ctx->serializer);
        ctx->serializer->PopVersionScope();
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(buffer);
        REQUIRE(root.AsWritable()->Save(u8"Project.xml", buffer.Bytes()).IsOk());

        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        CHECK_FALSE(static_cast<bool>(project));
    }
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
}

// A version 9 manifest still opens: the theme and loading document of the removed UI are read and
// dropped, and the fields after them land where they were.
TEST_CASE("project: a version 9 manifest opens without the old UI's two settings")
{
    const StringView dir = u8"scratch_project_v9_manifest";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    const String text = Format(
        u8"<root>\n"
        u8"<array name=\"dataVersions\" count=\"1\"><u64 name=\"type\">{}</u64><u32 name=\"version\">9</u32></array>\n"
        u8"<string name=\"name\">Old</string>\n"
        u8"<string name=\"engineVersion\">0.1.0</string>\n"
        u8"<string name=\"defaultSceneId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultScene\"/>\n"
        u8"<string name=\"startupScript\"/>\n"
        u8"<string name=\"nativeModule\"/>\n"
        u8"<string name=\"defaultInputMapId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultBusLayoutId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultUiThemeId\">6ba7b811-9dad-11d1-80b4-00c04fd430c8</string>\n"
        u8"<string name=\"startupScriptId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultUiFontId\">6ba7b813-9dad-11d1-80b4-00c04fd430c8</string>\n"
        u8"<string name=\"loadingDocumentId\">6ba7b812-9dad-11d1-80b4-00c04fd430c8</string>\n"
        u8"<u32 name=\"renderMsaaSamples\">4</u32>\n"
        u8"</root>\n",
        engine::project::ProjectSettings::StaticType().id);
    {
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(text.CStr()), text.Size())).IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().name == u8"Old");
        Guid fontId;
        REQUIRE(Guid::TryParse(u8"6ba7b813-9dad-11d1-80b4-00c04fd430c8", fontId));
        CHECK(project->Settings().defaultUiFontId == fontId); // read past the dropped theme
        CHECK(project->Settings().guiLoadingDocumentId.IsNil());
        // Its MSAA count (read past the dropped document) became the project's first quality level,
        // Quality/Default, listed and the default, the manifest saved with it.
        REQUIRE(project->Settings().qualityLevelIds.Size() == 1u);
        CHECK(project->Settings().defaultQualityId == project->Settings().qualityLevelIds[0]);
        CHECK(project->Settings().msaaSamplesBeforeLevels == 0u);
        foundation::content::Instance* level = project->SourceDb().GetInstance(u8"Quality/Default");
        REQUIRE(level != nullptr);
        CHECK(level->Id() == project->Settings().defaultQualityId);
        CHECK(level->TypeName() == StringView(u8"RenderQualityAsset"));
        pipeline::RegisterRenderProfileAssets();
        RefPtr<ISerializable> written = level->ReadObject();
        const auto* asset = Cast<pipeline::RenderQualityAsset>(written.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->values.msaaSamples == 4u); // the manifest's count
        CHECK(asset->values.ambientOcclusion == engine::render::QualityTier::High); // every effect as before
        REQUIRE(project->SaveSettings().IsOk()); // saved at the current version, without them
    }
    {
        const String manifest = PathJoin(dir, u8"Project.xml");
        const Result<Array<byte>> saved = ReadFile(manifest.AsView());
        REQUIRE(saved.HasValue());
        const StringView savedText(reinterpret_cast<const utf8char*>(saved.Value().Data()), saved.Value().Size());
        CHECK_FALSE(savedText.ContainsIgnoreCase(u8"\"defaultUiThemeId\""));
        CHECK_FALSE(savedText.ContainsIgnoreCase(u8"\"loadingDocumentId\"")); // guiLoadingDocumentId stays
        CHECK_FALSE(savedText.Contains(u8"\"renderMsaaSamples\"")); // the level holds it now
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().qualityLevelIds.Size() == 1u); // upgraded once, not again
        // The menus' list: each level by its asset's path, one whose asset is gone by its id.
        const Guid gone{0x51, 0x52};
        project->Settings().qualityLevelIds.PushBack(gone);
        const Array<ProjectQualityLevel> levels = project->QualityLevels();
        REQUIRE(levels.Size() == 2u);
        CHECK(levels[0].id == project->Settings().defaultQualityId);
        CHECK(levels[0].name == StringView(u8"Quality/Default"));
        CHECK(levels[1].id == gone);
        CHECK(levels[1].name == Format(u8"{}", gone));
    }
    (void)RemoveDirectoryRecursive(dir);
}

// The settings that the Project Settings dialog and project_settings_set edit come from the
// type's reflection, not a list: every asset setting is a Guid naming the asset type it takes,
// every setting is labelled, and the path mirrors follow their guids.
TEST_CASE("editor-project: the settings describe themselves, and the path mirrors follow the guids")
{
    const TypeInfo& type = engine::project::ProjectSettings::StaticType();
    CHECK(type.dataVersion == 18u); // 14 to 16 enable particles, terrain and navigation, which left the engine; 17 and 18 a language its scripts use
    CHECK(type.minReadDataVersion == 9u); // a version 9 manifest still reads
    u32 assets = 0;
    u32 lists = 0;
    for (const PropertyInfo& property : Properties(type))
    {
        CAPTURE(property.name);
        REQUIRE(engine::project::SettingAttribute(property, foundation::settings::kSettingLabelAttribute) != nullptr);
        if (const String* assetType =
                engine::project::SettingAttribute(property, foundation::settings::kSettingAssetTypeAttribute))
        {
            // One asset, or a list of them (the other UI fonts, the GUI's sheets).
            CHECK((engine::project::IsAssetSetting(property) || engine::project::IsAssetListSetting(property)));
            assets += engine::project::IsAssetSetting(property) ? 1u : 0u;
            lists += engine::project::IsAssetListSetting(property) ? 1u : 0u;
            CHECK_FALSE(assetType->IsEmpty());
            CHECK(engine::project::SettingAttribute(property, foundation::settings::kSettingEmptyTextAttribute) != nullptr);
        }
    }
    CHECK(assets == 7u); // scene, script, input map, bus layout, UI font, GUI loading screen, default quality
    CHECK(lists == 3u); // uiFontIds, guiStyleSheetIds, qualityLevelIds
    const PropertyInfo* scene = FindProperty(type, "defaultSceneId");
    REQUIRE(scene != nullptr);
    CHECK(*engine::project::SettingAttribute(*scene, foundation::settings::kSettingAssetTypeAttribute) ==
          u8"SceneDocument");

    engine::project::ProjectSettings settings;
    Random rng(5);
    settings.defaultSceneId = Guid::Generate(rng);
    settings.RefreshPathMirrors([](const Guid&) { return String(u8"Scenes/Arena"); });
    CHECK(settings.defaultScene == u8"Scenes/Arena");
    CHECK(settings.runBehavior.IsEmpty()); // no script: no mirror
}

// The other UI fonts are a list appended to the manifest. A manifest without it (no uiFontIds
// key, the same data version) still opens with none, one saved with it round-trips the list, the
// dist manifest's copy carries it, and the walk over the asset settings (export roots,
// asset_uses, project_health) visits each entry.
TEST_CASE("project: the other UI fonts are an appended list every asset walk reaches")
{
    const StringView dir = u8"scratch_project_ui_fonts_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());

    // Saved before the list: strip its key from the manifest the create wrote.
    {
        {
            UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
            REQUIRE(static_cast<bool>(project));
            project->Settings().defaultUiFontId = Guid{1, 2};
            REQUIRE(project->SaveSettings().IsOk());
        }
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String older(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const usize start = older.AsView().Find(u8"<array name=\"uiFontIds\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        CHECK(older.AsView().SubStr(start, close - start).Find(u8'<', 1) == StringView::kNotFound); // empty: <array .../>
        older.Remove(start, close + 2 - start);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml",
                           Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    const Guid title{0x51, 0x52};
    const Guid caption{0x53, 0x54};
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        CHECK(project->Settings().defaultUiFontId == Guid{1, 2});
        CHECK(project->Settings().uiFontIds.IsEmpty());
        project->Settings().uiFontIds.PushBack(title);
        project->Settings().uiFontIds.PushBack(caption);
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->Settings().uiFontIds.Size() == 2u);
    CHECK(project->Settings().uiFontIds[0] == title);
    CHECK(project->Settings().uiFontIds[1] == caption);

    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(project->Settings(), dist).IsOk());
    CHECK(dist.uiFontIds.Size() == 2u);

    Array<Guid> visited;
    engine::project::ForEachSettingAsset(project->Settings(),
                                         [&visited](const PropertyInfo&, const Guid& id) { visited.PushBack(id); });
    CHECK(visited.Size() == 3u); // the default font, then the list's two
    CHECK(visited[1] == title);
    CHECK(visited[2] == caption);

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The engine GUI's sheets are an appended, ordered list: a manifest without it opens with none,
// one saved with it round-trips the order, the dist copy carries it, and every asset walk visits
// each sheet.
TEST_CASE("project: the GUI's sheets are an appended list, in order, every asset walk reaches")
{
    const StringView dir = u8"scratch_project_gui_sheets_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        // Saved before the list: no key for it.
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String older(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const usize start = older.AsView().Find(u8"<array name=\"guiStyleSheetIds\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        older.Remove(start, close + 2 - start);
        // And the GUI loading screen, appended after it.
        const usize splash = older.AsView().Find(u8"guiLoadingDocumentId");
        REQUIRE(splash != StringView::kNotFound);
        const usize open = older.AsView().FindLast(u8'<', splash);
        const usize end = older.AsView().Find(u8'>', splash);
        REQUIRE(open != StringView::kNotFound);
        REQUIRE(end != StringView::kNotFound);
        usize stop = end + 1;
        if (older.AsView()[end - 1] != u8'/') // <guid ...>...</guid>: through its closing tag
            stop = older.AsView().Find(u8'>', older.AsView().Find(u8"</", end)) + 1;
        older.Remove(open, stop - open);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    const Guid shared{0x61, 0x62};
    const Guid game{0x63, 0x64};
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        CHECK(project->Settings().guiStyleSheetIds.IsEmpty());
        CHECK(project->Settings().guiLoadingDocumentId.IsNil());
        project->Settings().guiLoadingDocumentId = Guid{0x65, 0x66};
        project->Settings().guiStyleSheetIds.PushBack(shared);
        project->Settings().guiStyleSheetIds.PushBack(game);
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->Settings().guiStyleSheetIds.Size() == 2u);
    CHECK(project->Settings().guiStyleSheetIds[0] == shared); // order kept: the game's sheet applies over the shared one
    CHECK(project->Settings().guiStyleSheetIds[1] == game);

    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(project->Settings(), dist).IsOk());
    CHECK(dist.guiStyleSheetIds.Size() == 2u);
    CHECK(project->Settings().guiLoadingDocumentId == Guid{0x65, 0x66});
    CHECK(dist.guiLoadingDocumentId == Guid{0x65, 0x66});

    usize visited = 0;
    engine::project::ForEachSettingAsset(project->Settings(),
                                         [&](const PropertyInfo&, const Guid& id) { visited += (id == shared || id == game) ? 1u : 0u; });
    CHECK(visited == 2u);

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The quality levels and the default among them are appended: a manifest saved before them opens
// with none (the engine's own level). Once set they round-trip in order, the dist copy carries
// them, every asset walk reaches them (so a pruned export keeps them), and only a listed level is
// one the game may start at.
TEST_CASE("project: the quality levels are an appended list with a default among them")
{
    const StringView dir = u8"scratch_project_quality_levels_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        // Saved before them: no key for either.
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String older(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const usize start = older.AsView().Find(u8"<array name=\"qualityLevelIds\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        older.Remove(start, close + 2 - start);
        const usize key = older.AsView().Find(u8"defaultQualityId");
        REQUIRE(key != StringView::kNotFound);
        const usize open = older.AsView().FindLast(u8'<', key);
        const usize end = older.AsView().Find(u8'>', key);
        REQUIRE(open != StringView::kNotFound);
        REQUIRE(end != StringView::kNotFound);
        usize stop = end + 1;
        if (older.AsView()[end - 1] != u8'/') // <guid ...>...</guid>: through its closing tag
            stop = older.AsView().Find(u8'>', older.AsView().Find(u8"</", end)) + 1;
        older.Remove(open, stop - open);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    const Guid low{0x71, 0x72};
    const Guid high{0x73, 0x74};
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        CHECK(project->Settings().qualityLevelIds.IsEmpty());
        CHECK(project->Settings().defaultQualityId.IsNil());
        project->Settings().qualityLevelIds.PushBack(low);
        project->Settings().qualityLevelIds.PushBack(high);
        project->Settings().defaultQualityId = high;
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    const engine::project::ProjectSettings& settings = project->Settings();
    REQUIRE(settings.qualityLevelIds.Size() == 2u);
    CHECK(settings.qualityLevelIds[0] == low); // an options menu's order
    CHECK(settings.qualityLevelIds[1] == high);
    CHECK(settings.defaultQualityId == high);
    CHECK(settings.IsQualityLevel(low));
    CHECK_FALSE(settings.IsQualityLevel(Guid{0x75, 0x76}));

    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(settings, dist).IsOk());
    CHECK(dist.qualityLevelIds.Size() == 2u);
    CHECK(dist.defaultQualityId == high);

    usize visited = 0;
    engine::project::ForEachSettingAsset(settings,
                                         [&](const PropertyInfo&, const Guid& id) { visited += (id == low || id == high) ? 1u : 0u; });
    CHECK(visited == 3u); // both levels, and the default again

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// A project names the folders outside it that its MCP tools work in (a game's sources beside it).
// Appended, so a manifest saved before it opens with none; set, it round-trips.
TEST_CASE("project: the MCP folders are an appended list an older manifest reads as none")
{
    const StringView dir = u8"scratch_project_mcp_folders_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        // Saved before it: no key.
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String older(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const usize start = older.AsView().Find(u8"<array name=\"mcpFolders\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        older.Remove(start, close + 2 - start);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        CHECK(project->Settings().mcpFolders.IsEmpty());
        project->Settings().mcpFolders.PushBack(String(u8"../Game"));
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->Settings().mcpFolders.Size() == 1u);
    CHECK(project->Settings().mcpFolders[0] == u8"../Game");
    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The display settings (render resolution and fit, the player's window) are appended, so a
// manifest without them opens with their defaults; once set, they round-trip and the dist copy
// carries them.
TEST_CASE("project: the display settings are appended, with defaults an older manifest reads")
{
    const StringView dir = u8"scratch_project_display_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());

    // Saved before them: strip every display element from the manifest the create wrote.
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String text(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const StringView keys[] = {u8"renderWidth", u8"renderHeight", u8"renderFit", u8"windowWidth",
                                   u8"windowHeight", u8"windowMode", u8"windowResizable", u8"orientation"};
        for (StringView key : keys)
        {
            const usize name = text.AsView().Find(Format(u8"name=\"{}\"", key).AsView());
            REQUIRE(name != StringView::kNotFound);
            const usize start = text.AsView().FindLast(u8'<', name);
            const usize close = text.AsView().Find(u8"</", name);
            const usize end = text.AsView().Find(u8'>', close);
            text.Remove(start, end + 1 - start);
        }
        CHECK_FALSE(text.AsView().Contains(u8"renderWidth"));
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()))
                    .IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        const engine::project::ProjectSettings& settings = project->Settings();
        CHECK(settings.renderWidth == 0u);
        CHECK(settings.renderHeight == 0u);
        CHECK_FALSE(settings.HasRenderResolution()); // draws at its output's size
        CHECK(settings.renderFit == FitMode::Letterbox);
        CHECK(settings.windowWidth == 1280u);
        CHECK(settings.windowHeight == 720u);
        CHECK(settings.windowMode == engine::project::WindowMode::Windowed);
        CHECK(settings.windowResizable);
        CHECK(settings.orientation == ScreenOrientation::Any); // takes the window's shape
        project->Settings().renderWidth = 640;
        project->Settings().renderHeight = 360;
        project->Settings().renderFit = FitMode::IntegerScale;
        project->Settings().windowMode = engine::project::WindowMode::Borderless;
        project->Settings().windowResizable = false;
        project->Settings().orientation = ScreenOrientation::Landscape;
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(project->Settings(), dist).IsOk());
    CHECK(dist.HasRenderResolution());
    CHECK(dist.renderWidth == 640u);
    CHECK(dist.renderHeight == 360u);
    CHECK(dist.renderFit == FitMode::IntegerScale);
    CHECK(dist.windowMode == engine::project::WindowMode::Borderless);
    CHECK_FALSE(dist.windowResizable);
    CHECK(dist.orientation == ScreenOrientation::Landscape);

    // Reflected for the dialog and the tools: the enums name their choices.
    const PropertyInfo* fit = FindProperty(engine::project::ProjectSettings::StaticType(), "renderFit");
    REQUIRE(fit != nullptr);
    CHECK(EnumeratorCount(*fit->type) == 4u);
    const PropertyInfo* mode = FindProperty(engine::project::ProjectSettings::StaticType(), "windowMode");
    REQUIRE(mode != nullptr);
    CHECK(EnumeratorCount(*mode->type) == 3u);
    const PropertyInfo* orientation = FindProperty(engine::project::ProjectSettings::StaticType(), "orientation");
    REQUIRE(orientation != nullptr);
    CHECK(EnumeratorCount(*orientation->type) == 3u);

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The player reads its manifest before its window exists, so it can open the window the project
// asks for. It reads a dist's player.xml first, else a dev tree's Project.xml.
TEST_CASE("project: the player's manifest is the dist's, else the project's")
{
    const StringView dir = u8"scratch_player_manifest_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)FileDelete(PathJoin(dir, u8"player.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(CreateDirectory(dir));
    foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
    {
        engine::project::ProjectSettings none;
        CHECK(engine::project::LoadPlayerManifest(root, none).Code() == ErrorCode::NotFound);
    }
    engine::project::ProjectSettings project;
    project.name = String(u8"Dev");
    project.windowWidth = 800;
    REQUIRE(engine::project::SaveProjectSettings(*root.AsWritable(), project).IsOk());
    {
        engine::project::ProjectSettings read;
        REQUIRE(engine::project::LoadPlayerManifest(root, read).IsOk());
        CHECK(read.name == StringView(u8"Dev"));
        CHECK(read.windowWidth == 800u);
    }
    engine::project::ProjectSettings dist;
    dist.name = String(u8"Shipped");
    dist.windowMode = engine::project::WindowMode::Fullscreen;
    REQUIRE(engine::project::SaveProjectSettings(*root.AsWritable(), dist, engine::project::kDistManifestFile).IsOk());
    {
        engine::project::ProjectSettings read;
        REQUIRE(engine::project::LoadPlayerManifest(root, read).IsOk());
        CHECK(read.name == StringView(u8"Shipped")); // the dist's wins
        CHECK(read.windowMode == engine::project::WindowMode::Fullscreen);
    }
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)FileDelete(PathJoin(dir, u8"player.xml"));
    (void)RemoveDirectory(dir);
}

TEST_CASE("project settings: the window in a few words, and the settings that sum a project up")
{
    engine::project::ProjectSettings settings;
    CHECK(settings.WindowSummary() == StringView(u8"1280 × 720, windowed"));
    settings.windowWidth = 1920;
    settings.windowHeight = 1080;
    settings.windowMode = engine::project::WindowMode::Fullscreen;
    CHECK(settings.WindowSummary() == StringView(u8"1920 × 1080, fullscreen"));
    settings.windowMode = engine::project::WindowMode::Borderless;
    CHECK(settings.WindowSummary() == StringView(u8"borderless, the display's size")); // the display's size, not the setting's

    // The summary's settings, by their short labels, in reflection order.
    Array<String> summary;
    for (const PropertyInfo& property : Properties(engine::project::ProjectSettings::StaticType()))
        if (const String* label = engine::project::SettingAttribute(property, foundation::settings::kSettingSummaryAttribute))
            summary.PushBack(*label);
    REQUIRE(summary.Size() == 4u);
    CHECK(summary[0] == StringView(u8"Default scene"));
    CHECK(summary[1] == StringView(u8"Run behavior"));
    CHECK(summary[2] == StringView(u8"Input map"));
    CHECK(summary[3] == StringView(u8"UI font"));
}

// The plugins list is appended: a manifest without it opens with none. An entry keeps its
// platforms and its settings as written, whether or not any plugin here reads them, through a
// save, a reopen and the dist copy.
TEST_CASE("project: the plugins list is appended, and an entry no plugin here reads is kept as written")
{
    const StringView dir = u8"scratch_project_plugins_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P", {}).IsOk()); // an empty list, to strip below
    const String manifestPath = PathJoin(dir, u8"Project.xml");
    const auto manifestText = [&manifestPath]()
    {
        Result<Array<byte>> bytes = ReadFile(manifestPath.AsView());
        REQUIRE(bytes.HasValue());
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    };

    // Saved before the list: strip its key from the manifest the create wrote.
    {
        String older = manifestText();
        const usize start = older.AsView().Find(u8"<array name=\"plugins\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        older.Remove(start, close + 2 - start);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    const StringView unknown = u8"<object name=\"mystery\"><i32 name=\"appId\">480</i32></object>";
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().plugins.IsEmpty());

        foundation::runtime::EnabledPlugin steam;
        steam.id = String(u8"steam");
        steam.platforms.PushBack(String(u8"Linux64"));
        steam.platforms.PushBack(String(u8"Win64"));
        steam.settings.Resize(unknown.Size());
        MemCopy(steam.settings.Data(), unknown.Data(), unknown.Size());
        project->Settings().plugins.PushBack(Move(steam));
        foundation::runtime::EnabledPlugin game;
        game.id = String(u8"game");
        game.path = String(u8"Native");
        project->Settings().plugins.PushBack(Move(game));
        REQUIRE(project->SaveSettings().IsOk());
    }
    const String saved = manifestText();
    CHECK(saved.AsView().Contains(u8"<i32 name=\"appId\">480</i32>"));
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        engine::project::ProjectSettings& settings = project->Settings();
        REQUIRE(settings.plugins.Size() == 2u);
        REQUIRE(settings.FindPlugin(u8"steam") != nullptr);
        CHECK(settings.FindPlugin(u8"steam")->platforms.Size() == 2u);
        CHECK(settings.FindPlugin(u8"game")->path == String(u8"Native"));
        CHECK(settings.FindPlugin(u8"absent") == nullptr);
        REQUIRE(project->SaveSettings().IsOk());
        CHECK(manifestText() == saved); // read and written back unchanged

        engine::project::ProjectSettings dist;
        REQUIRE(engine::project::CopyProjectSettings(settings, dist).IsOk());
        REQUIRE(dist.FindPlugin(u8"steam") != nullptr);
        const Array<u8>& copied = dist.FindPlugin(u8"steam")->settings;
        CHECK(StringView(reinterpret_cast<const utf8char*>(copied.Data()), copied.Size()).Contains(u8"\"appId\">480<"));
    }
    (void)FileDelete(manifestPath.AsView());
    (void)RemoveDirectory(dir);
}

namespace
{
    // A plugin with all four parts, linked into this test as the generated registry links one: each
    // part records its kind as it loads and unloads.
    constexpr char8_t kFourParts[] = u8"four_parts";
    Array<String>& FourPartsLog()
    {
        static Array<String> log;
        return log;
    }
    template <foundation::runtime::PluginPartKind Kind>
    void FourPartsLoad(foundation::runtime::PluginContext&)
    {
        FourPartsLog().PushBack(Format(u8"load:{}", foundation::runtime::PluginPartKindName(Kind)));
    }
    template <foundation::runtime::PluginPartKind Kind>
    void FourPartsUnload(foundation::runtime::PluginContext&)
    {
        FourPartsLog().PushBack(Format(u8"unload:{}", foundation::runtime::PluginPartKindName(Kind)));
    }
    template <foundation::runtime::PluginPartKind Kind>
    const foundation::runtime::PluginPart* FourPartsPart()
    {
        static constexpr foundation::runtime::PluginPart part{kFourParts, Kind, &FourPartsLoad<Kind>, &FourPartsUnload<Kind>};
        return &part;
    }

    struct FourPartsLinked
    {
        Array<byte> manifest;
        foundation::runtime::PluginPartEntryFn parts[4] = {
            &FourPartsPart<foundation::runtime::PluginPartKind::Engine>, &FourPartsPart<foundation::runtime::PluginPartKind::Pipeline>,
            &FourPartsPart<foundation::runtime::PluginPartKind::Tooling>, &FourPartsPart<foundation::runtime::PluginPartKind::Editor>};
        foundation::runtime::LinkedPlugin linked[1]{};

        FourPartsLinked()
        {
            foundation::runtime::PluginManifest written;
            written.id = String(kFourParts);
            written.name = String(u8"Four parts");
            written.version = String(u8"1.0");
            written.engineVersion = String(engine::project::kEngineVersionString);
            for (foundation::runtime::PluginPartKind kind : foundation::runtime::kPluginPartKinds)
                written.parts.PushBack(foundation::runtime::PluginManifestPart{String(foundation::runtime::PluginPartKindName(kind))});
            written.platforms.PushBack(foundation::runtime::PluginPlatform{String(GetHostPlatformName())});
            MemoryStream xml;
            REQUIRE(foundation::runtime::WritePluginManifest(written, xml).IsOk());
            for (byte b : xml.Bytes())
                manifest.PushBack(b);
            manifest.PushBack(byte{0});
            linked[0] = foundation::runtime::LinkedPlugin{reinterpret_cast<const char8_t*>(manifest.Data()), parts, 4};
        }
        [[nodiscard]] Span<const foundation::runtime::LinkedPlugin> List() const { return Span<const foundation::runtime::LinkedPlugin>(linked, 1); }
    };
}

// A headless tool has no application: its plugins run over a subsystem manager of its own, and each
// tool loads exactly the parts its role takes of the plugins the project enables, unloading them
// when it goes.
TEST_CASE("tool plugins: the cook, the exporter and the MCP host each load the parts their role takes")
{
    const StringView dir = u8"scratch_project_tool_plugins";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Tools").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    foundation::runtime::EnabledPlugin entry;
    entry.id = String(kFourParts);
    project->Settings().plugins.PushBack(Move(entry));
    const FourPartsLinked linked;

    const auto loadsFor = [&](foundation::runtime::HostRole role)
    {
        FourPartsLog().Clear();
        {
            ToolPlugins plugins(DefaultAllocator(), role, *project, linked.List());
            CHECK(plugins.Report().loaded.Size() == 1u);
            CHECK(plugins.Host().IsLoaded(kFourParts));
            CHECK(plugins.Host().Role() == role);
        }
        Array<String> loads;
        bool unloaded = false;
        for (const String& line : FourPartsLog())
        {
            if (line.AsView().StartsWith(u8"load:"))
                loads.PushBack(String(line.AsView().SubStr(5, line.Size() - 5)));
            unloaded = unloaded || line.AsView().StartsWith(u8"unload:");
        }
        CHECK(unloaded); // the tool's plugins went with it
        String joined;
        for (const String& kind : loads)
        {
            joined += joined.IsEmpty() ? u8"" : u8",";
            joined += kind.AsView();
        }
        return joined;
    };
    CHECK(loadsFor(foundation::runtime::HostRole::Cook) == String(u8"Engine,Pipeline"));
    CHECK(loadsFor(foundation::runtime::HostRole::Export) == String(u8"Engine,Pipeline,Tooling"));
    CHECK(loadsFor(foundation::runtime::HostRole::Mcp) == String(u8"Engine,Pipeline,Tooling"));

    // A plugin this tool does not link stays missing; the project keeps its entry.
    {
        ToolPlugins plugins(DefaultAllocator(), foundation::runtime::HostRole::Cook, *project,
                            Span<const foundation::runtime::LinkedPlugin>{});
        REQUIRE(plugins.Report().FindRefusal(kFourParts) != nullptr);
        CHECK(project->Settings().FindPlugin(kFourParts) != nullptr);
    }
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

// Before version 11 a project named its native game module by its library path (nativeModule).
// Such a manifest still opens: the path becomes the entry of the game's package, Plugins/<Game>, its
// id the name in lower case, and a save writes version 11 without it. An empty path adds nothing.
TEST_CASE("project: a version 10 manifest's native module becomes the entry of the game's package")
{
    const auto open = [](StringView dir, StringView nativeModule)
    {
        (void)RemoveDirectoryRecursive(dir);
        const String text = Format(
            u8"<root>\n"
            u8"<array name=\"dataVersions\" count=\"1\"><u64 name=\"type\">{}</u64><u32 name=\"version\">10</u32></array>\n"
            u8"<string name=\"name\">Old</string>\n"
            u8"<string name=\"engineVersion\">0.1.0</string>\n"
            u8"<string name=\"defaultSceneId\">00000000-0000-0000-0000-000000000000</string>\n"
            u8"<string name=\"defaultScene\"/>\n"
            u8"<string name=\"startupScript\"/>\n"
            u8"<string name=\"nativeModule\">{}</string>\n"
            u8"<string name=\"defaultInputMapId\">00000000-0000-0000-0000-000000000000</string>\n"
            u8"<string name=\"defaultBusLayoutId\">00000000-0000-0000-0000-000000000000</string>\n"
            u8"<string name=\"startupScriptId\">00000000-0000-0000-0000-000000000000</string>\n"
            u8"<string name=\"defaultUiFontId\">00000000-0000-0000-0000-000000000000</string>\n"
            u8"<u32 name=\"renderMsaaSamples\">2</u32>\n"
            u8"</root>\n",
            engine::project::ProjectSettings::StaticType().id, nativeModule);
        REQUIRE(CreateDirectories(dir));
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(text.CStr()), text.Size())).IsOk());
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        // Read past the dropped path: its MSAA count is the project's first quality level now.
        CHECK(project->Settings().qualityLevelIds.Size() == 1u);
        return project;
    };

    {
        const StringView dir = u8"scratch_project_v10_native";
        UniquePtr<EditorProject> project = open(dir, u8"Native/libOldGame.so");
        REQUIRE(project->Settings().plugins.Size() == 4u); // the game's package, particles, terrain and navigation (below)
        const foundation::runtime::EnabledPlugin& entry = project->Settings().plugins[0];
        CHECK(entry.id == String(u8"oldgame"));
        CHECK(entry.path == String(u8"Plugins/OldGame"));
        CHECK(entry.platforms.IsEmpty());
        REQUIRE(project->SaveSettings().IsOk());
        const Result<Array<byte>> saved = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(saved.HasValue());
        const StringView savedText(reinterpret_cast<const utf8char*>(saved.Value().Data()), saved.Value().Size());
        CHECK_FALSE(savedText.Contains(u8"\"nativeModule\""));
        CHECK(savedText.Contains(Format(u8"<u32 name=\"version\">{}</u32>", ProjectSettings::StaticType().dataVersion).AsView())); // written at the current one
    CHECK(savedText.Contains(u8"name=\"runBehaviorId\"")); // the old key reads, the new one is written
    CHECK_FALSE(savedText.Contains(u8"startupScript"));
        project.Reset();
        UniquePtr<EditorProject> again = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(again));
        CHECK(again->Settings().plugins.Size() == 4u); // migrated once, not again
        again.Reset();
        (void)RemoveDirectoryRecursive(dir);
    }
    {
        const StringView dir = u8"scratch_project_v10_native_dll";
        UniquePtr<EditorProject> project = open(dir, u8"Native/2 Fast.dll");
        REQUIRE(project->Settings().plugins.Size() == 4u);
        CHECK(project->Settings().plugins[0].id == String(u8"game_2_fast"));
        CHECK(project->Settings().plugins[0].path == String(u8"Plugins/2 Fast"));
        project.Reset();
        (void)RemoveDirectoryRecursive(dir);
    }
    {
        const StringView dir = u8"scratch_project_v10_scripts";
        UniquePtr<EditorProject> project = open(dir, u8"");
        // Particles, terrain and navigation were the engine's when this project was written: it
        // keeps them, as plugins.
        REQUIRE(project->Settings().plugins.Size() == 3u);
        CHECK(project->Settings().plugins[0].id == String(u8"particles"));
        CHECK(project->Settings().plugins[0].path.IsEmpty());
        CHECK(project->Settings().plugins[1].id == String(u8"terrain"));
        CHECK(project->Settings().plugins[2].id == String(u8"navigation"));
        project.Reset();
        (void)RemoveDirectoryRecursive(dir);
    }
}

namespace
{
    // A package folder under the project's Plugins/ with a manifest of `id` (and `engineVersion`).
    void WriteProjectPackage(StringView projectDir, StringView folder, StringView id,
                             StringView engineVersion = engine::project::kEngineVersionString)
    {
        foundation::runtime::PluginManifest manifest;
        manifest.id = String(id);
        manifest.name = Format(u8"{} package", id);
        manifest.version = String(u8"0.3.0");
        manifest.engineVersion = String(engineVersion);
        manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(u8"Engine")});
        manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(u8"Linux64")});
        const String dir = PathJoin(PathJoin(projectDir, u8"Plugins").AsView(), folder);
        REQUIRE(CreateDirectories(dir.AsView()));
        MemoryStream xml;
        REQUIRE(foundation::runtime::WritePluginManifest(manifest, xml).IsOk());
        REQUIRE(WriteFile(PathJoin(dir.AsView(), u8"Plugin.xml").AsView(), xml.Bytes()).IsOk());
    }

    [[nodiscard]] const engine::project::AvailablePlugin* FindAvailable(const Array<engine::project::AvailablePlugin>& found,
                                                                        StringView id, StringView path)
    {
        for (const engine::project::AvailablePlugin& plugin : found)
        {
            if (plugin.id == id && plugin.path == path)
            {
                return &plugin;
            }
        }
        return nullptr;
    }
}

// A package extracted into the project's Plugins folder is available, not enabled: discovery lists
// each package no entry names (with what its manifest says) and each linked plugin no entry
// enables, and reports, without offering it, a package whose manifest does not read, whose engine
// is another, or whose id is taken; a folder with no Plugin.xml is no package, and an entry's own
// package is not listed.
TEST_CASE("project: discovery lists the packages in Plugins/ and the linked plugins no entry enables")
{
    const StringView dir = u8"scratch_project_discovery";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Discovery").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    WriteProjectPackage(dir, u8"Lantern", u8"lantern");
    WriteProjectPackage(dir, u8"LanternCopy", u8"lantern");
    WriteProjectPackage(dir, u8"Old", u8"old_engine", u8"0.0");
    WriteProjectPackage(dir, u8"Mine", u8"mine");
    WriteProjectPackage(dir, u8"Taken", u8"taken");
    REQUIRE(CreateDirectories(PathJoin(dir, u8"Plugins/Broken").AsView()));
    const StringView garbage = u8"not xml";
    REQUIRE(WriteFile(PathJoin(dir, u8"Plugins/Broken/Plugin.xml").AsView(),
                      Span<const byte>(reinterpret_cast<const byte*>(garbage.Data()), garbage.Size()))
                .IsOk());
    REQUIRE(CreateDirectories(PathJoin(dir, u8"Plugins/Notes").AsView())); // no Plugin.xml: no package
    foundation::runtime::EnabledPlugin mine;
    mine.id = String(u8"mine");
    mine.path = String(u8"Plugins/Mine");
    project->Settings().plugins.PushBack(Move(mine));
    foundation::runtime::EnabledPlugin taken;
    taken.id = String(u8"taken");
    taken.path = String(u8"Elsewhere/Taken");
    project->Settings().plugins.PushBack(Move(taken));
    const FourPartsLinked linked;

    const Array<engine::project::AvailablePlugin> found =
        engine::project::DiscoverPlugins(dir, project->Settings(), linked.List(), DefaultAllocator());

    const engine::project::AvailablePlugin* lantern = FindAvailable(found, u8"lantern", u8"Plugins/Lantern");
    REQUIRE(lantern != nullptr);
    CHECK(lantern->problem.IsEmpty());
    CHECK(lantern->name == String(u8"lantern package"));
    CHECK(lantern->version == String(u8"0.3.0"));
    REQUIRE(lantern->parts.Size() == 1u);
    CHECK(lantern->parts[0] == String(u8"Engine"));
    REQUIRE(lantern->platforms.Size() == 1u);
    const engine::project::AvailablePlugin* copy = FindAvailable(found, u8"lantern", u8"Plugins/LanternCopy");
    REQUIRE(copy != nullptr);
    CHECK(copy->problem.AsView().Contains(u8"is also in 'Plugins/Lantern'"));
    const engine::project::AvailablePlugin* old = FindAvailable(found, u8"old_engine", u8"Plugins/Old");
    REQUIRE(old != nullptr);
    CHECK_FALSE(old->problem.IsEmpty());
    const engine::project::AvailablePlugin* takenPackage = FindAvailable(found, u8"taken", u8"Plugins/Taken");
    REQUIRE(takenPackage != nullptr);
    CHECK(takenPackage->problem.AsView().Contains(u8"already enabled from 'Elsewhere/Taken'"));
    const engine::project::AvailablePlugin* broken = FindAvailable(found, u8"", u8"Plugins/Broken");
    REQUIRE(broken != nullptr);
    CHECK(broken->problem.AsView().Contains(u8"does not read"));
    CHECK(FindAvailable(found, u8"mine", u8"Plugins/Mine") == nullptr); // an entry's own package
    const engine::project::AvailablePlugin* fourParts = FindAvailable(found, kFourParts, u8"");
    REQUIRE(fourParts != nullptr); // linked, no entry
    CHECK(fourParts->problem.IsEmpty());
    for (const engine::project::AvailablePlugin& plugin : found)
    {
        CHECK(plugin.path != String(u8"Plugins/Notes"));
    }

    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("project: the folders the editor, a cook and an export write are its outputs")
{
    const StringView outputs[] = {u8"Cooked", u8"Cooked-android", u8"Cooked-web-bc", u8"Editor", u8".cache", u8"Dist"};
    for (const StringView name : outputs)
    {
        CAPTURE(name);
        CHECK(engine::project::IsProjectOutputFolder(name));
    }
    const StringView owned[] = {u8"Content", u8"Sources", u8"Plugins", u8"Branding", u8"Cookbook"};
    for (const StringView name : owned)
    {
        CAPTURE(name);
        CHECK_FALSE(engine::project::IsProjectOutputFolder(name));
    }
}

// A project from before its files followed their groups: everything flat under Sources/, scripts
// too. Opened, each file moves into its group's folder, code under Code/, and its asset follows
// (id kept); a folder that was already at Sources/Code, which Code/ would hide, is renamed and the
// asset naming a file in it follows; opening again moves nothing more.
TEST_CASE("editor-project: an older project's files move into their groups' folders when it opens")
{
    const StringView dir = u8"scratch_editor_code_move";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    const String sources = PathJoin(dir, u8"Sources");
    const String code = PathJoin(dir, u8"Code");
    // Both code asset types read back (the editor's pipeline registers them; a test, itself).
    pipeline::RegisterScriptAssets();
    const auto write = [&sources](StringView path, StringView text)
    {
        const String full = PathJoin(sources.AsView(), path);
        REQUIRE(CreateDirectories(PathParent(full.AsView())));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    };
    const auto fileNameOf = [](foundation::content::Instance* instance)
    {
        RefPtr<ISerializable> object = instance->ReadObject();
        const auto* asset = Cast<pipeline::Asset>(object.Get());
        return asset != nullptr ? String(asset->fileName.View()) : String{};
    };
    Guid heroId, projectId, tilesId, notesId;
    {
        // As an older editor left it: the assets naming files under Sources/, a folder Code there.
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        foundation::content::Group* root = project->SourceDb().RootGroup();
        foundation::content::Group* quality = root->CreateGroup(u8"Quality");
        REQUIRE(quality != nullptr);
        auto add = [root](StringView name, pipeline::Asset& asset, const TypeInfo& type, StringView file,
                          foundation::content::Group* in = nullptr)
        {
            foundation::content::Instance* instance = (in != nullptr ? in : root)->CreateInstance(name, type);
            REQUIRE(instance != nullptr);
            asset.fileName = foundation::vfs::SourcePath(file);
            REQUIRE(instance->WriteObject(asset).IsOk());
            return instance->Id();
        };
        pipeline::ScriptClassAsset hero;
        heroId = add(u8"Hero", hero, pipeline::ScriptClassAsset::StaticType(), u8"Hero.as");
        pipeline::ScriptAssemblyAsset game;
        projectId = add(u8"Game", game, pipeline::ScriptAssemblyAsset::StaticType(), u8"Game/Game.siproj");
        pipeline::ScriptClassAsset notes; // a script that was in the folder Code/ will hide
        notesId = add(u8"Notes", notes, pipeline::ScriptClassAsset::StaticType(), u8"Code/Notes.as");
        pipeline::RenderQualityAsset tiles; // not code
        tilesId = add(u8"Tiles", tiles, pipeline::RenderQualityAsset::StaticType(), u8"Tiles.png", quality);
        write(u8"Hero.as", u8"class Hero {}");
        write(u8"Game/Game.siproj", u8"project");
        write(u8"Code/Notes.as", u8"older");
        write(u8"Tiles.png", u8"png");
    }

    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    foundation::content::ContentDatabase& db = project->SourceDb();
    CHECK(fileNameOf(db.GetInstance(heroId)) == StringView(u8"Code/Hero.as"));
    CHECK(FileExists(PathJoin(code.AsView(), u8"Hero.as").AsView()));
    CHECK_FALSE(FileExists(PathJoin(sources.AsView(), u8"Hero.as").AsView()));
    // In the root group: Code/ itself (its folder under Sources/, Game/, left empty and gone).
    CHECK(fileNameOf(db.GetInstance(projectId)) == StringView(u8"Code/Game.siproj"));
    CHECK(FileExists(PathJoin(code.AsView(), u8"Game.siproj").AsView()));
    CHECK_FALSE(DirectoryExists(PathJoin(sources.AsView(), u8"Game").AsView()));
    // Not code, in the Quality group: Sources/Quality/.
    CHECK(fileNameOf(db.GetInstance(tilesId)) == StringView(u8"Quality/Tiles.png"));
    CHECK(FileExists(PathJoin(sources.AsView(), u8"Quality/Tiles.png").AsView()));
    // The older folder, renamed; its script followed it and then moved into Code/ as any other
    // (the renamed folder, left empty, gone).
    CHECK(fileNameOf(db.GetInstance(notesId)) == StringView(u8"Code/Notes.as"));
    CHECK(FileExists(PathJoin(code.AsView(), u8"Notes.as").AsView()));
    CHECK(project->SourceFile(u8"Code/Hero.as") == PathJoin(code.AsView(), u8"Hero.as"));
    project.Reset();

    // Again: nothing left to move.
    project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    CHECK(fileNameOf(project->SourceDb().GetInstance(heroId)) == StringView(u8"Code/Hero.as"));
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

// Files follow their groups: a move into another group moves the file into that group's folder
// (made when missing, the old one removed once empty), a file assets in two groups share stays where
// it is, and a name already taken in the folder takes the next free one.
TEST_CASE("editor-project: an asset's file follows it into its group's folder")
{
    const StringView dir = u8"scratch_editor_group_folders";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    const String sources = PathJoin(dir, u8"Sources");
    const auto write = [&sources](StringView path)
    {
        const String full = PathJoin(sources.AsView(), path);
        REQUIRE(CreateDirectories(PathParent(full.AsView())));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(path.Data()), path.Size())).IsOk());
    };
    const auto fileNameOf = [](foundation::content::Instance* instance)
    {
        RefPtr<ISerializable> object = instance->ReadObject();
        const auto* asset = Cast<pipeline::Asset>(object.Get());
        return asset != nullptr ? String(asset->fileName.View()) : String{};
    };
    foundation::content::Group* root = project->SourceDb().RootGroup();
    foundation::content::Group* a = root->CreateGroup(u8"A");
    foundation::content::Group* b = root->CreateGroup(u8"B");
    auto add = [](foundation::content::Group* in, StringView name, StringView file)
    {
        foundation::content::Instance* instance = in->CreateInstance(name, pipeline::RenderQualityAsset::StaticType());
        REQUIRE(instance != nullptr);
        pipeline::RenderQualityAsset asset;
        asset.fileName = foundation::vfs::SourcePath(file);
        REQUIRE(instance->WriteObject(asset).IsOk());
        return instance;
    };
    write(u8"A/Lone.png");
    write(u8"Shared.png");
    write(u8"B/Lone.png"); // B's folder already has a Lone.png
    foundation::content::Instance* lone = add(a, u8"Lone", u8"A/Lone.png");
    foundation::content::Instance* sharedA = add(a, u8"SharedA", u8"Shared.png");
    foundation::content::Instance* sharedB = add(b, u8"SharedB", u8"Shared.png");

    // Moved to B: the file follows, beside B's own Lone.png under the next free name; A's folder,
    // empty now, goes.
    REQUIRE(project->SourceDb().MoveInstance(lone->Id(), *b).IsOk());
    foundation::content::Instance* const moved[] = {lone};
    CHECK(project->PlaceInGroupFolders(Span<foundation::content::Instance* const>{moved, 1}) == 1u);
    CHECK(fileNameOf(lone) == StringView(u8"B/Lone.2.png"));
    CHECK(FileExists(PathJoin(sources.AsView(), u8"B/Lone.2.png").AsView()));
    CHECK(FileExists(PathJoin(sources.AsView(), u8"B/Lone.png").AsView()));
    CHECK_FALSE(DirectoryExists(PathJoin(sources.AsView(), u8"A").AsView()));

    // Shared by assets in A and B: stays.
    foundation::content::Instance* const shared[] = {sharedA, sharedB};
    CHECK(project->PlaceInGroupFolders(Span<foundation::content::Instance* const>{shared, 2}) == 0u);
    CHECK(fileNameOf(sharedA) == StringView(u8"Shared.png"));
    CHECK(FileExists(PathJoin(sources.AsView(), u8"Shared.png").AsView()));
    // Once both are in B, it follows them there.
    REQUIRE(project->SourceDb().MoveInstance(sharedA->Id(), *b).IsOk());
    CHECK(project->PlaceInGroupFolders(Span<foundation::content::Instance* const>{shared, 2}) == 1u);
    CHECK(fileNameOf(sharedA) == StringView(u8"B/Shared.png"));
    CHECK(fileNameOf(sharedB) == StringView(u8"B/Shared.png"));
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

// A file another file names beside it (a model's texture under tex/) moves only with that file, so
// its relative path holds: placing the folders leaves it where its owner points. A file two owners
// in different groups share keeps both owners where they are.
TEST_CASE("editor-project: a companion file moves with its owner, never on its own")
{
    GlobalTypeRegistry().Register(TestModelAsset::StaticType()); // read back as itself
    RegisterSerializable<TestModelAsset>();
    const StringView dir = u8"scratch_editor_companions";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    const String sources = PathJoin(dir, u8"Sources");
    const auto write = [&sources](StringView path, StringView text)
    {
        const String full = PathJoin(sources.AsView(), path);
        REQUIRE(CreateDirectories(PathParent(full.AsView())));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    };
    const auto fileNameOf = [](foundation::content::Instance* instance)
    {
        RefPtr<ISerializable> object = instance->ReadObject();
        const auto* asset = Cast<pipeline::Asset>(object.Get());
        return asset != nullptr ? String(asset->fileName.View()) : String{};
    };
    foundation::content::Group* root = project->SourceDb().RootGroup();
    auto add = [](foundation::content::Group* in, StringView name, const TypeInfo& type, StringView file)
    {
        foundation::content::Instance* instance = in->CreateInstance(name, type);
        REQUIRE(instance != nullptr);
        RefPtr<ISerializable> object = MakeRef<TestModelAsset>(DefaultAllocator());
        if (&type == &pipeline::RenderQualityAsset::StaticType())
            object = MakeRef<pipeline::RenderQualityAsset>(DefaultAllocator());
        Cast<pipeline::Asset>(object.Get())->fileName = foundation::vfs::SourcePath(file);
        REQUIRE(instance->WriteObject(*object).IsOk());
        return instance;
    };

    // A model in its folder already, its texture under tex/ beside it: nothing moves.
    foundation::content::Group* crate = root->CreateGroup(u8"Crate");
    write(u8"Crate/Crate.model", u8"tex/wood.png");
    write(u8"Crate/tex/wood.png", u8"png");
    (void)add(crate, u8"Crate", TestModelAsset::StaticType(), u8"Crate/Crate.model");
    foundation::content::Instance* wood = add(crate, u8"wood", pipeline::RenderQualityAsset::StaticType(), u8"Crate/tex/wood.png");

    // Two flat models in different groups sharing one texture: both stay, the texture with them.
    foundation::content::Group* a = root->CreateGroup(u8"A");
    foundation::content::Group* b = root->CreateGroup(u8"B");
    write(u8"x.model", u8"shared.png");
    write(u8"y.model", u8"shared.png");
    write(u8"shared.png", u8"png");
    foundation::content::Instance* x = add(a, u8"x", TestModelAsset::StaticType(), u8"x.model");
    foundation::content::Instance* y = add(b, u8"y", TestModelAsset::StaticType(), u8"y.model");

    Array<foundation::content::Instance*> all;
    foundation::content::Group* const groups[] = {crate, a, b};
    for (foundation::content::Group* group : groups)
        for (foundation::content::Instance* instance : group->Instances())
            all.PushBack(instance);
    (void)project->PlaceInGroupFolders(Span<foundation::content::Instance* const>{all.Data(), all.Size()});
    CHECK(fileNameOf(wood) == StringView(u8"Crate/tex/wood.png"));
    CHECK(FileExists(PathJoin(sources.AsView(), u8"Crate/tex/wood.png").AsView()));
    CHECK(fileNameOf(x) == StringView(u8"x.model"));
    CHECK(fileNameOf(y) == StringView(u8"y.model"));
    CHECK(FileExists(PathJoin(sources.AsView(), u8"shared.png").AsView()));
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

#ifdef OPTION_HAS_ANGELSCRIPT
// A script made in an outside editor under Code/ becomes an asset where it lies: in the group its
// folder names (made), its file not copied; a second report adopts nothing more; a file no code
// importer takes, and one removed, are left alone.
TEST_CASE("editor-project: new code under Code becomes an asset where it lies")
{
    angelscript::RegisterAngelScriptBackend();
    pipeline::RegisterScriptAssets();
    const StringView dir = u8"scratch_editor_adopt_code";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    pipeline::ImporterRegistry importers(DefaultAllocator());
    (void)importers.Register(MakeUnique<pipeline::ScriptFileImporter>(DefaultAllocator()));
    const auto write = [&project](StringView path, StringView text)
    {
        const String full = project->SourceFile(path);
        REQUIRE(CreateDirectories(PathParent(full.AsView())));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    };
    write(u8"Code/Enemies/Boss.as", u8"class Boss { void onStart() {} }");
    write(u8"Code/Notes.txt", u8"not code");
    const String changed[] = {String(u8"Code/Enemies/Boss.as"), String(u8"Code/Notes.txt"), String(u8"Code/Gone.as"),
                              String(u8"Models/Crate.png")};
    CHECK(project->AdoptNewCode(Span<const String>{changed, 4}, importers) == 1u);
    foundation::content::Group* enemies = project->SourceDb().RootGroup()->GetGroup(u8"Enemies");
    REQUIRE(enemies != nullptr);
    foundation::content::Instance* boss = enemies->GetInstance(u8"Boss");
    REQUIRE(boss != nullptr);
    RefPtr<ISerializable> object = boss->ReadObject();
    REQUIRE(Cast<pipeline::ScriptClassAsset>(object.Get()) != nullptr);
    CHECK(Cast<pipeline::ScriptClassAsset>(object.Get())->fileName.View() == StringView(u8"Code/Enemies/Boss.as"));
    CHECK_FALSE(FileExists(project->SourceFile(u8"Code/Enemies/Boss.2.as").AsView())); // no copy beside it
    // Reported again: it is an asset's now.
    CHECK(project->AdoptNewCode(Span<const String>{changed, 4}, importers) == 0u);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
#endif

#ifdef OPTION_HAS_LUAU
// The editor writes an outside editor's files into Code/ as a project opens: each language's
// declarations (Luau's engine.d.luau, whose extension the script importer takes) and the VS Code
// ones. They are not the project's code: the scan that adopts new code passes them by, and the
// script beside them is adopted as before. The declarations used to become an asset that failed
// its cook.
TEST_CASE("editor-project: the files written for an outside editor are not adopted as code")
{
    luau::RegisterLuauScriptBackend();
    pipeline::RegisterScriptAssets();
    const StringView dir = u8"scratch_editor_adopt_tooling";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    pipeline::ImporterRegistry importers(DefaultAllocator());
    (void)importers.Register(MakeUnique<pipeline::ScriptFileImporter>(DefaultAllocator()));
    const auto write = [&project](StringView path, StringView text)
    {
        const String full = project->SourceFile(path);
        REQUIRE(CreateDirectories(PathParent(full.AsView())));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    };
    CHECK(IsCodeToolingFile(u8"engine.d.luau"));
    CHECK(IsCodeToolingFile(u8".vscode/settings.json"));
    CHECK_FALSE(IsCodeToolingFile(u8"Wolf.luau"));
    write(u8"Code/engine.d.luau", u8"declare class Entity end\n");
    write(u8"Code/Wolf.luau", u8"Wolf = EntityBehavior.class()\nfunction Wolf.new(entity) return setmetatable({}, Wolf) end\n");
    const String changed[] = {String(u8"Code/engine.d.luau"), String(u8"Code/Wolf.luau")};
    CHECK(project->AdoptNewCode(Span<const String>{changed, 2}, importers) == 1u);
    CHECK(project->SourceDb().RootGroup()->GetInstance(u8"Wolf") != nullptr);
    CHECK(project->SourceDb().RootGroup()->GetInstance(u8"engine.d") == nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
#endif

// A project written before Luau became a plugin keeps the language its scripts use, and gains no
// other: one with a Luau script enables luau as it opens, one with only AngelScript scripts (and a
// script with no language, which is AngelScript) does not, and a project at the current version is
// left as it is.
TEST_CASE("project: an older project enables the language plugins its scripts use, and no other")
{
    pipeline::RegisterScriptAssets();
    struct Enabled
    {
        bool angelscript = false;
        bool luau = false;
    };
    const auto make = [](StringView dir, StringView language, u32 version)
    {
        (void)RemoveDirectoryRecursive(dir);
        REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Languages").IsOk());
        {
            UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
            REQUIRE(project);
            foundation::content::Instance* instance =
                project->SourceDb().RootGroup()->CreateInstance(u8"Mover", pipeline::ScriptClassAsset::StaticType());
            REQUIRE(instance != nullptr);
            pipeline::ScriptClassAsset script;
            script.language = String(language);
            REQUIRE(instance->WriteObject(script).IsOk());
            project->Settings().plugins.Clear(); // no language plugin, as an older engine wrote it
            REQUIRE(project->SaveSettings().IsOk());
        }
        // The manifest as an older engine wrote it.
        const String manifest = PathJoin(dir, u8"Project.xml");
        Result<Array<byte>> bytes = ReadFile(manifest.AsView());
        REQUIRE(bytes.HasValue());
        const StringView read(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        const String current = Format(u8"<u32 name=\"version\">{}</u32>", engine::project::ProjectSettings::StaticType().dataVersion);
        const usize at = read.Find(current.AsView());
        REQUIRE(at != StringView::kNotFound);
        String text(read.SubStr(0, at));
        text.Append(Format(u8"<u32 name=\"version\">{}</u32>", version).AsView());
        text.Append(read.SubStr(at + current.Size(), read.Size() - at - current.Size()));
        REQUIRE(WriteFile(manifest.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.CStr()), text.Size())).IsOk());
        UniquePtr<EditorProject> reopened = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(reopened);
        return Enabled{reopened->Settings().FindPlugin(u8"angelscript") != nullptr,
                       reopened->Settings().FindPlugin(u8"luau") != nullptr};
    };
    const u32 current = engine::project::ProjectSettings::StaticType().dataVersion;
    const Enabled luau = make(u8"scratch_project_languages_luau", u8"luau", 16);
    CHECK(luau.luau);
    CHECK_FALSE(luau.angelscript);
    const Enabled angelscript = make(u8"scratch_project_languages_as", u8"angelscript", 17);
    CHECK(angelscript.angelscript);
    CHECK_FALSE(angelscript.luau); // read after Luau left, but its scripts are not Luau
    const Enabled unnamed = make(u8"scratch_project_languages_default", u8"", 16);
    CHECK(unnamed.angelscript); // a script with no language is AngelScript
    CHECK_FALSE(unnamed.luau);
    // At the current version a language is enabled by hand, or not.
    const Enabled currentLuau = make(u8"scratch_project_languages_current", u8"luau", current);
    CHECK_FALSE(currentLuau.luau);
    CHECK_FALSE(currentLuau.angelscript);
    const Enabled currentAs = make(u8"scratch_project_languages_current_as", u8"angelscript", current);
    CHECK_FALSE(currentAs.angelscript);
    const StringView made[] = {u8"scratch_project_languages_luau", u8"scratch_project_languages_as",
                               u8"scratch_project_languages_default", u8"scratch_project_languages_current",
                               u8"scratch_project_languages_current_as"};
    for (StringView dir : made)
    {
        (void)RemoveDirectoryRecursive(dir);
    }
}
