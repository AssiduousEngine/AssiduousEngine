// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue's assets and their cook:
//   * DialogueAsset: a dialogue file, its text in the project's sources (a `.dialogue` file, imported
//     or made by File > New). DialogueBuilder parses it, checks it against the project and writes a
//     DialogueSource; a file with an error does not cook, every problem logged at its line.
//   * DialogueCastAsset: the cast, its speakers edited in the inspector. DialogueCastBuilder writes
//     a DialogueCastSource.
//   * DialogueAssetListAsset: the dialogue assets, a name and an asset for each, edited in the
//     inspector. It is read when conversations cook and has no cooked form of its own.
// The settings (the cast, the dialogue assets, the commands, the provided flags) are the plugin's,
// read once at load; a change to them recooks every dialogue file (EnvironmentHash).

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module dialogue.pipeline:assets;

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.importer;
import dialogue.engine;
import :parse;
import :check;

using namespace foundation::core;

export namespace dialogue
{
    namespace content = foundation::content;

    class DialogueAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(DialogueAsset, pipeline::Asset)
    public:
        void Serialize(ISerializer& ar) override { pipeline::Asset::Serialize(ar); }
    };

    class DialogueCastAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(DialogueCastAsset, pipeline::Asset)
    public:
        Array<DialogueSpeaker> speakers;

        void Serialize(ISerializer& ar) override;
    };

    class DialogueAssetListAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(DialogueAssetListAsset, pipeline::Asset)
    public:
        Array<DialogueAssetEntry> entries;

        void Serialize(ISerializer& ar) override;
    };

    /// A dialogue file's project: what its checks read, gathered from a source database and the
    /// sources it links. Cook and tools build it the same way.
    struct DialogueProject
    {
        Array<DialogueCommandForm> commands;
        Array<DialogueProblem> settingsProblems; // a command whose form is not one (line 0)
        Array<DialogueSpeaker> cast;
        bool hasCast = false;
        Array<DialogueAssetEntry> dialogueAssets;
        bool hasDialogueAssets = false;
        Array<String> conversations; // every other file's
        Array<String> lineIds;
        Array<String> flagsSet;
        Array<Guid> files; // every other dialogue file's asset

        /// Gathers it for the file `self` (nil for none) from `db`, reading the other files'
        /// sources through `reader`'s sources.
        void Gather(const DialogueSettings& settings, content::IContentDatabase& db, const pipeline::AssetBuildContext& reader,
                    const Guid& self);
    };

    /// Parses `text` and checks it against `project`, its assets reached through `db`.
    [[nodiscard]] DialogueParse CookDialogue(StringView text, const DialogueSettings& settings,
                                             const DialogueProject& project, content::IContentDatabase* db);

    class DialogueBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        explicit DialogueBuilder(const DialogueSettings& settings) : m_settings(&settings) {}

        [[nodiscard]] const TypeInfo* AssetType() const override { return &DialogueAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &DialogueSource::StaticType(); }
        [[nodiscard]] u64 EnvironmentHash() const override;
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx,
                              pipeline::AssetDependencies& out) override;
        [[nodiscard]] Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override;

    private:
        const DialogueSettings* m_settings;
    };

    class DialogueCastBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override { return &DialogueCastAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &DialogueCastSource::StaticType(); }
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx,
                              pipeline::AssetDependencies& out) override;
        [[nodiscard]] Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override;
    };

    /// Imports a `.dialogue` file: copied into the sources, a DialogueAsset linking it.
    class DialogueFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Dialogue"; }
        [[nodiscard]] bool Accepts(StringView extension) const override;
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath, const pipeline::ImportOptions*,
                                                          Object*) override;
        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"DialogueAsset"; }
        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group, StringView sourcePath) override;
        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator&) const override { return {}; }
        [[nodiscard]] Result<content::Instance*> Import(StringView sourcePath, const pipeline::ImportContext& context,
                                                       content::Group& group, const pipeline::ImportOptions* options,
                                                       Object*, Array<pipeline::DeferredImportWrite>*) override;
    };

    /// The creators' labels, as File > New and asset_create name them.
    inline constexpr StringView kDialogueCreator = u8"Dialogue";
    inline constexpr StringView kDialogueCastCreator = u8"Dialogue Cast";
    inline constexpr StringView kDialogueAssetsCreator = u8"Dialogue Assets";

    /// File > New's three creators.
    void AddDialogueCreators(pipeline::PipelinePluginServices& services);

    /// Registers the assets' types and the reflected values their inspector edits.
    void RegisterDialoguePipelineTypes(TypeRegistry& types, SerializableRegistry& serializables);
}
