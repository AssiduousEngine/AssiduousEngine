// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue's assets, their builders, the importer and File > New's creators.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

module dialogue.pipeline;

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.importer;
import input.pipeline;
import foundation.input; // ActionSet, Action
import foundation.scene;
import foundation.scene.resource;
import engine.composition; // the scene managers a level loads with, and its stream's name
import dialogue.engine;

using namespace foundation::core;

namespace dialogue
{
    REFLECT_MEMBERS(DialogueCastAsset, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Dialogue Cast"));
        builder.Property<&DialogueCastAsset::speakers>("speakers").PropAttribute("displayName", String(u8"Speakers"));
    }

    REFLECT_MEMBERS(DialogueAssetListAsset, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Dialogue Assets"));
        builder.Property<&DialogueAssetListAsset::entries>("entries").PropAttribute("displayName", String(u8"Assets"));
    }

    REFLECT_VALUE(DialogueAssetEntry, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Dialogue Asset"));
        builder.Property<&DialogueAssetEntry::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .PropAttribute("description", String(u8"What a conversation calls the asset by: one word"))
            .Property<&DialogueAssetEntry::asset>("asset")
            .PropAttribute("displayName", String(u8"Asset"));
    }

    REFLECT_MEMBERS(DialogueAsset, "rtti::dialogue")
    {
        builder.Attribute("displayName", String(u8"Dialogue"));
        builder.Property<&DialogueAsset::cast>("cast")
            .PropAttribute("displayName", String(u8"Cast"))
            .PropAttribute("referenceType", String(u8"DialogueCastAsset"))
            .Property<&DialogueAsset::assets>("assets")
            .PropAttribute("displayName", String(u8"Dialogue Assets"))
            .PropAttribute("referenceType", String(u8"DialogueAssetListAsset"));
    }

    void DialogueAsset::Serialize(ISerializer& ar)
    {
        pipeline::Asset::Serialize(ar);
        foundation::core::Serialize(ar, "cast", cast);
        foundation::core::Serialize(ar, "assets", assets);
    }

    void DialogueCastAsset::Serialize(ISerializer& ar)
    {
        pipeline::Asset::Serialize(ar);
        foundation::core::Serialize(ar, "speakers", speakers);
    }

    void DialogueAssetListAsset::Serialize(ISerializer& ar)
    {
        pipeline::Asset::Serialize(ar);
        foundation::core::Serialize(ar, "entries", entries);
    }

    void RegisterDialoguePipelineTypes(TypeRegistry& types, SerializableRegistry& serializables)
    {
        RttiRegisterValue_DialogueAssetEntry();
        RegisterArrayType<DialogueAssetEntry>();
        types.Register(TypeOf<DialogueAssetEntry>());
        types.Register(DialogueAsset::StaticType());
        types.Register(DialogueCastAsset::StaticType());
        types.Register(DialogueAssetListAsset::StaticType());
        RegisterSerializable<DialogueAsset>(serializables);
        RegisterSerializable<DialogueCastAsset>(serializables);
        RegisterSerializable<DialogueAssetListAsset>(serializables);
    }

    namespace
    {
        // The source database the cook reads asset envelopes from.
        [[nodiscard]] content::IContentDatabase* SourceDatabase(const pipeline::AssetBuildContext& ctx) noexcept
        {
            return ctx.sourceDb != nullptr ? ctx.sourceDb : ctx.db;
        }

        template <typename Visit>
        void ForEachInstance(content::Group& group, StringView typeName, const Visit& visit)
        {
            for (content::Instance* instance : group.Instances())
            {
                if (instance->TypeName() == typeName)
                {
                    visit(*instance);
                }
            }
            for (content::Group* child : group.Groups())
            {
                ForEachInstance(*child, typeName, visit);
            }
        }

        // The project's assets by guid, as the checks ask for them.
        class DatabaseAssets final : public IDialogueAssets
        {
        public:
            explicit DatabaseAssets(content::IContentDatabase& db) : m_db(&db) {}

            [[nodiscard]] String NameOf(const Guid& id) const override
            {
                content::Instance* instance = m_db->GetInstance(id);
                return instance != nullptr ? String(instance->Name()) : String();
            }

            [[nodiscard]] String TypeOf(const Guid& id) const override
            {
                content::Instance* instance = m_db->GetInstance(id);
                return instance != nullptr ? String(instance->TypeName()) : String();
            }

        private:
            content::IContentDatabase* m_db;
        };

        // The levels conversations name, each loaded once into a scratch scene (its prefabs placed, as
        // a load places them) for its entities' names.
        class LoadedLevels final : public IDialogueLevels
        {
        public:
            LoadedLevels(content::IContentDatabase& db, IAllocator& allocator) : m_db(&db), m_allocator(&allocator) {}

            [[nodiscard]] const Array<String>* EntityNames(const Guid& level) const override
            {
                for (const Loaded& loaded : m_loaded)
                {
                    if (loaded.level == level)
                    {
                        return loaded.ok ? &loaded.names : nullptr;
                    }
                }
                Loaded& loaded = m_loaded.EmplaceBack();
                loaded.level = level;
                content::Instance* instance = m_db->GetInstance(level);
                if (instance == nullptr)
                {
                    return nullptr;
                }
                foundation::scene::Scene scratch(*m_allocator, u8"__dialogue_level");
                engine::AddAllSceneManagers(scratch);
                if (!foundation::scene::LoadScene(*instance, scratch).IsOk())
                {
                    return nullptr;
                }
                content::IContentDatabase* db = m_db;
                const foundation::scene::PrefabPayloadResolver resolver = [db](const Guid& prefabId) -> UniquePtr<IStream>
                {
                    content::Instance* prefab = db->GetInstance(prefabId);
                    return prefab != nullptr ? prefab->ReadData(engine::kSceneStream) : UniquePtr<IStream>{};
                };
                foundation::scene::ResolveScenePrefabs(scratch, resolver);
                scratch.ForEachEntity([&scratch, &loaded](foundation::scene::EntityHandle entity)
                                      { loaded.names.PushBack(String(scratch.GetEntityName(entity))); });
                loaded.ok = true;
                return &loaded.names;
            }

        private:
            struct Loaded
            {
                Guid level;
                Array<String> names;
                bool ok = false;
            };

            content::IContentDatabase* m_db;
            IAllocator* m_allocator;
            mutable Array<Loaded> m_loaded;
        };

        [[nodiscard]] Span<const DialogueCommandForm> FormsOf(const DialogueProject& project)
        {
            return Span<const DialogueCommandForm>(project.commands.Data(), project.commands.Size());
        }
    }

    void DialogueProject::Gather(const DialogueSettings& settings, const DialogueAsset& file, content::IContentDatabase& db,
                                 const DialogueSourceReader& readSource, const Guid& fileId)
    {
        self = fileId;
        for (const String& text : settings.commands)
        {
            DialogueCommandForm form;
            String error;
            if (!ParseCommandForm(text.AsView(), form, error))
            {
                settingsProblems.PushBack(
                    DialogueProblem{0, false, Format(u8"the dialogue settings' command `{}`: {}", text, error)});
                continue;
            }
            for (const DialogueCommandForm& other : commands)
            {
                if (other.name == form.name)
                {
                    settingsProblems.PushBack(DialogueProblem{
                        0, false, Format(u8"the dialogue settings declare two commands named `{}`", form.name)});
                }
            }
            commands.PushBack(Move(form));
        }
        if (file.cast != Guid{})
        {
            RefPtr<ISerializable> read = db.ReadObject(file.cast);
            if (const auto* castAsset = Cast<DialogueCastAsset>(read.Get()))
            {
                cast = castAsset->speakers;
                hasCast = true;
            }
        }
        if (file.assets != Guid{})
        {
            RefPtr<ISerializable> read = db.ReadObject(file.assets);
            if (const auto* list = Cast<DialogueAssetListAsset>(read.Get()))
            {
                dialogueAssets = list->entries;
                hasDialogueAssets = true;
            }
        }
        if (content::Group* root = db.RootGroup())
        {
            ForEachInstance(*root, u8"InputMapAsset",
                            [this, &db](content::Instance& instance)
                            {
                                RefPtr<ISerializable> read = db.ReadObject(instance.Id());
                                const auto* map = Cast<pipeline::InputMapAsset>(read.Get());
                                if (map == nullptr)
                                {
                                    return;
                                }
                                hasInputMap = true;
                                inputMaps.PushBack(instance.Id());
                                for (const foundation::input::ActionSet& set : map->Map().sets)
                                {
                                    for (const foundation::input::Action& action : set.actions)
                                    {
                                        actions.PushBack(action.name);
                                    }
                                }
                            });
            ForEachInstance(*root, u8"DialogueAsset",
                            [this, &db, &readSource, &fileId](content::Instance& instance)
                            {
                                if (instance.Id() == fileId)
                                {
                                    return;
                                }
                                files.PushBack(instance.Id());
                                RefPtr<ISerializable> read = db.ReadObject(instance.Id());
                                const auto* other = Cast<DialogueAsset>(read.Get());
                                String text;
                                if (other == nullptr || !readSource || !readSource(other->fileName.View(), text))
                                {
                                    return; // its own cook says so
                                }
                                const DialogueParse parse = ParseDialogue(text.AsView(), FormsOf(*this));
                                DialogueFileConversations& facts = otherFiles.EmplaceBack();
                                facts.file = instance.Id();
                                Array<String> flagsRead; // what a file reads is no other file's business
                                CollectDialogueFacts(parse, facts.conversations, flagsSet, flagsRead);
                            });
        }
    }

    UniquePtr<IDialogueLevels> MakeDialogueLevels(content::IContentDatabase& db, IAllocator& allocator)
    {
        return MakeUnique<LoadedLevels>(allocator, db, allocator);
    }

    DialogueParse CookDialogue(StringView text, const DialogueSettings& settings, const DialogueProject& project,
                               content::IContentDatabase* db, IAllocator& allocator)
    {
        DialogueParse parse = ParseDialogue(text, FormsOf(project));
        for (const DialogueProblem& problem : project.settingsProblems)
        {
            parse.problems.PushBack(problem);
        }
        Optional<DatabaseAssets> assets;
        if (db != nullptr)
        {
            assets.Emplace(*db);
        }
        DialogueCheckInputs inputs;
        inputs.cast = project.hasCast ? &project.cast : nullptr;
        inputs.dialogueAssets = project.hasDialogueAssets ? &project.dialogueAssets : nullptr;
        inputs.commands = FormsOf(project);
        inputs.providedFlags = Span<const String>(settings.providedFlags.Data(), settings.providedFlags.Size());
        inputs.self = project.self;
        inputs.otherFiles = Span<const DialogueFileConversations>(project.otherFiles.Data(), project.otherFiles.Size());
        inputs.otherFlagsSet = Span<const String>(project.flagsSet.Data(), project.flagsSet.Size());
        inputs.assets = db != nullptr ? &assets.Value() : nullptr;
        inputs.actions = project.hasInputMap ? &project.actions : nullptr;
        Optional<LoadedLevels> levels;
        if (db != nullptr)
        {
            levels.Emplace(*db, allocator);
            inputs.levels = &levels.Value();
        }
        CheckDialogue(parse, inputs);
        return parse;
    }

    DialogueParse CheckDialogueText(StringView text, const DialogueSettings& settings, const Guid& self,
                                    content::IContentDatabase& db, const DialogueSourceReader& read, IAllocator& allocator)
    {
        DialogueProject project;
        RefPtr<ISerializable> object = self.IsNil() ? RefPtr<ISerializable>{} : db.ReadObject(self);
        const DialogueAsset* file = Cast<DialogueAsset>(object.Get());
        const DialogueAsset none;
        project.Gather(settings, file != nullptr ? *file : none, db, read, self);
        return CookDialogue(text, settings, project, &db, allocator);
    }

    // ---- the dialogue file's builder ----------------------------------------------------------

    u64 DialogueBuilder::EnvironmentHash() const
    {
        // What the checks read from the settings: a change recooks every dialogue file.
        u64 hash = kFnv1a64OffsetBasis;
        for (const String& form : m_settings->commands)
        {
            hash = HashBytes(form.Data(), form.Size(), hash);
            hash = HashBytes("\n", 1, hash);
        }
        for (const String& flag : m_settings->providedFlags)
        {
            hash = HashBytes(flag.Data(), flag.Size(), hash);
            hash = HashBytes("\n", 1, hash);
        }
        return hash;
    }

    void DialogueBuilder::ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx,
                                           pipeline::AssetDependencies& out)
    {
        const auto& file = static_cast<const DialogueAsset&>(asset);
        content::IContentDatabase* db = SourceDatabase(ctx);
        if (db == nullptr)
        {
            return;
        }
        // The cast and the dialogue assets are read, and every other dialogue file's text: a change
        // to any of them can change what this file's checks find (the flags the others set, the
        // conversations a jump into one names). The other files are their text, not reads: a read
        // folds in the read asset's own recipe, and each file reading every other is a cycle.
        if (file.cast != Guid{})
        {
            out.reads.PushBack(file.cast);
            out.references.PushBack(file.cast); // the runner loads it for the lines' speakers
        }
        if (file.assets != Guid{})
        {
            out.reads.PushBack(file.assets);
            // What the conversations play (sounds, sequences, other dialogue files, commands'
            // assets) is reached through the dialogue assets, so an export takes every one.
            RefPtr<ISerializable> read = db->ReadObject(file.assets);
            if (const auto* list = Cast<DialogueAssetListAsset>(read.Get()))
            {
                for (const DialogueAssetEntry& entry : list->entries)
                {
                    if (entry.asset != Guid{} && entry.asset != (ctx.source != nullptr ? ctx.source->Id() : Guid{}))
                    {
                        out.references.PushBack(entry.asset);
                    }
                }
            }
        }
        const Guid self = ctx.source != nullptr ? ctx.source->Id() : Guid{};
        if (content::Group* root = db->RootGroup())
        {
            ForEachInstance(*root, u8"InputMapAsset", [&out](content::Instance& instance) { out.reads.PushBack(instance.Id()); });
            ForEachInstance(*root, u8"DialogueAsset",
                            [&out, &self, db](content::Instance& instance)
                            {
                                if (instance.Id() == self)
                                {
                                    return;
                                }
                                RefPtr<ISerializable> read = db->ReadObject(instance.Id());
                                if (const auto* other = Cast<DialogueAsset>(read.Get()))
                                {
                                    out.files.PushBack(other->fileName);
                                }
                            });
        }
    }

    Status DialogueBuilder::Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx)
    {
        if (ctx.output == nullptr)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        const StringView file = asset.fileName.View();
        String text;
        const Status read = ReadSourceText(ctx, file, text);
        if (!read.IsOk())
        {
            LOG_ERROR(u8"Dialogue", u8"'{}': the dialogue file is missing", file);
            return read;
        }
        content::IContentDatabase* db = SourceDatabase(ctx);
        DialogueProject project;
        if (db != nullptr)
        {
            const DialogueSourceReader reader = [&ctx](StringView fileName, String& out)
            { return ReadSourceText(ctx, fileName, out).IsOk(); };
            project.Gather(*m_settings, static_cast<const DialogueAsset&>(asset), *db, reader,
                           ctx.source != nullptr ? ctx.source->Id() : Guid{});
        }
        DialogueParse parse = CookDialogue(text.AsView(), *m_settings, project, db, *ctx.allocator);
        for (const DialogueProblem& problem : parse.problems)
        {
            if (problem.warning)
            {
                LOG_WARNING(u8"Dialogue", u8"{}:{}: {}", file, problem.line, problem.message);
            }
            else
            {
                LOG_ERROR(u8"Dialogue", u8"{}:{}: {}", file, problem.line, problem.message);
            }
        }
        if (!parse.Ok())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        DialogueSource cooked;
        cooked.cast = static_cast<const DialogueAsset&>(asset).cast;
        cooked.conversations = Move(parse.conversations);
        cooked.lines = Move(parse.lines);
        return ctx.output->WriteObject(cooked);
    }

    // ---- the cast's builder --------------------------------------------------------------------

    void DialogueCastBuilder::ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx,
                                               pipeline::AssetDependencies& out)
    {
        (void)ctx;
        // What the cast refers to at runtime: its portraits, clips and sounds must exist.
        const auto& cast = static_cast<const DialogueCastAsset&>(asset);
        const auto refer = [&out](const Guid& id)
        {
            if (id != Guid{})
            {
                out.references.PushBack(id);
            }
        };
        for (const DialogueSpeaker& speaker : cast.speakers)
        {
            refer(speaker.portrait);
            refer(speaker.talk);
            refer(speaker.voice);
            for (const DialogueMood& mood : speaker.moods)
            {
                refer(mood.portrait);
            }
            for (const Guid& clip : speaker.clips)
            {
                refer(clip);
            }
        }
    }

    Status DialogueCastBuilder::Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx)
    {
        if (ctx.output == nullptr)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        DialogueCastSource cooked;
        cooked.speakers = static_cast<const DialogueCastAsset&>(asset).speakers;
        return ctx.output->WriteObject(cooked);
    }

    // ---- the importer ---------------------------------------------------------------------------

    bool DialogueFileImporter::Accepts(StringView extension) const { return extension == StringView(u8"dialogue"); }

    pipeline::ImportPlan DialogueFileImporter::DescribeImport(StringView sourcePath, const pipeline::ImportOptions*, Object*)
    {
        return pipeline::SingleAssetPlan(sourcePath);
    }

    pipeline::ImportPlan DialogueFileImporter::StoredSelection(content::Group& group, StringView sourcePath)
    {
        return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"DialogueAsset");
    }

    Result<content::Instance*> DialogueFileImporter::Import(StringView sourcePath, const pipeline::ImportContext& context,
                                                            content::Group& group, const pipeline::ImportOptions* options,
                                                            Object*, Array<pipeline::DeferredImportWrite>*)
    {
        String fileName;
        pipeline::ImportClashKind clash = pipeline::ImportClashKind::None;
        Result<content::Instance*> claimed =
            pipeline::ClaimSingleAsset(context, group, options, sourcePath, DialogueAsset::StaticType(), fileName, &clash);
        if (!claimed.HasValue())
        {
            return Err(claimed.Error());
        }
        content::Instance* instance = claimed.Value();
        DialogueAsset asset;
        asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
        const Status written = instance->WriteObject(asset);
        if (!written.IsOk())
        {
            return Err(written.Code());
        }
        return instance;
    }

    // ---- File > New -----------------------------------------------------------------------------

    void AddDialogueCreators(pipeline::PipelinePluginServices& services, const DialogueSettings& settings)
    {
        {
            pipeline::AssetCreator creator;
            creator.label = String(kDialogueCreator);
            creator.category = String(u8"Dialogue");
            creator.type = &DialogueAsset::StaticType();
            creator.defaultGroup = String(u8"Dialogue");
            creator.run = [&settings](const pipeline::AssetCreationContext& at)
            {
                constexpr StringView kStarter = u8"# One statement a line. A conversation starts with its id, then its lines:\n"
                                                u8"#\n"
                                                u8"# conversation greeting\n"
                                                u8"# @ speaker mood: What the speaker says.\n";
                DialogueAsset asset;
                asset.cast = settings.cast;
                asset.assets = settings.assets;
                return pipeline::CreateLinkedTextAsset(at, u8"Dialogue", u8".dialogue", kStarter,
                                                       DialogueAsset::StaticType(), asset);
            };
            (void)services.AddCreator(Move(creator));
        }
        {
            pipeline::AssetCreator creator;
            creator.label = String(kDialogueCastCreator);
            creator.category = String(u8"Dialogue");
            creator.type = &DialogueCastAsset::StaticType();
            creator.defaultGroup = String(u8"Dialogue");
            creator.run = [](const pipeline::AssetCreationContext& at)
            {
                DialogueCastAsset cast;
                return pipeline::CreateWrittenInstance(at.Target(), at.NameOr(u8"Cast"), DialogueCastAsset::StaticType(), cast);
            };
            (void)services.AddCreator(Move(creator));
        }
        {
            pipeline::AssetCreator creator;
            creator.label = String(kDialogueAssetsCreator);
            creator.category = String(u8"Dialogue");
            creator.type = &DialogueAssetListAsset::StaticType();
            creator.defaultGroup = String(u8"Dialogue");
            creator.run = [](const pipeline::AssetCreationContext& at)
            {
                DialogueAssetListAsset list;
                return pipeline::CreateWrittenInstance(at.Target(), at.NameOr(u8"Dialogue Assets"),
                                                       DialogueAssetListAsset::StaticType(), list);
            };
            (void)services.AddCreator(Move(creator));
        }
    }
}
