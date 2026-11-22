// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A language whose classes build together into their project's assembly, through the neutral
// builders, with a stand-in language (so these are the pipeline's rules, not a compiler's): the
// project builds from every source of its language under its folder (sorted, an IDE's bin and obj
// passed over, a file outside the folder left out), and its sources are the build's inputs; a class
// finds its project (the nearest when they nest), reads its cooked assembly, takes its description
// from it and names it; a class in no project, or whose project has not built, fails its cook; a
// project file imports as the project of its language.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import foundation.script;
import foundation.script.resource;
import script.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::script;
namespace content = foundation::content;

namespace
{
    constexpr StringView kLanguage = u8"stand-in";

    // The project "builds" by listing what it was given: the image is the source paths, and each
    // source's "class X" line is a class with an onUpdate handler.
    class StandInAssemblyCook final : public IScriptAssemblyCook
    {
    public:
        [[nodiscard]] StringView ProjectExtension() const override { return u8"siproj"; }
        [[nodiscard]] StringView SourceExtension() const override { return u8"si"; }
        [[nodiscard]] bool Build(StringView, StringView project, Span<const ScriptAssemblyFile> sources, CookScriptErrorSink& sink,
                                 ScriptAssemblySource& out) override
        {
            if (project.Contains(u8"broken"))
            {
                sink.OnError(ScriptError{ScriptErrorKind::Compile, u8"Broken.si", 3, 1, u8"does not build"});
                return false;
            }
            for (const ScriptAssemblyFile& file : sources)
            {
                for (const utf8char c : file.path)
                    out.image.PushBack(static_cast<byte>(c));
                out.image.PushBack(static_cast<byte>(u8';'));
                const usize at = file.text.AsView().Find(StringView(u8"class "));
                if (at == StringView::kNotFound)
                    continue;
                ScriptAssemblyClass c;
                c.className = String(file.text.AsView().SubStr(at + 6, file.text.Size() - at - 6));
                c.handlers.PushBack(String(u8"onUpdate"));
                out.classes.PushBack(Move(c));
            }
            return true;
        }
        [[nodiscard]] String NewProjectTemplate(StringView) const override { return String(u8"project"); }
        [[nodiscard]] Array<ScriptAssemblyFile> LocalFiles() const override
        {
            ScriptAssemblyFile file;
            file.path = String(u8"obj/Local.txt");
            file.text = String(u8"this machine");
            Array<ScriptAssemblyFile> files;
            files.PushBack(Move(file));
            return files;
        }
    };

    class StandInClassCook final : public IScriptLanguageCook
    {
    public:
        [[nodiscard]] String NewAssetTemplate(foundation::script::BehaviorKind, StringView) const override { return String{}; }
        [[nodiscard]] bool Cook(StringView, StringView, CookScriptErrorSink&, BehaviorClassSource&,
                                IScriptIncludeReader*) override
        {
            return false;
        }
        [[nodiscard]] bool Check(StringView, StringView, CookScriptErrorSink&, String&, Span<const TypeId>,
                                 IScriptIncludeReader*) override
        {
            return true;
        }
        void ListIncludes(StringView, Array<String>&) const override {}
        void PrepareCheck() override {}
        [[nodiscard]] Array<ScriptApiType> DescribeApi() override { return {}; }
        [[nodiscard]] u32 CheckManagerBuildCount() const override { return 0; }
        [[nodiscard]] bool BuildsInAssembly() const override { return true; }
        [[nodiscard]] bool CookFromAssembly(StringView source, StringView assetName, const ScriptAssemblySource& assembly,
                                            CookScriptErrorSink& sink, BehaviorClassSource& out) override
        {
            const StringView name = source.SubStr(6, source.Size() - 6);
            const ScriptAssemblyClass* c = assembly.FindClass(name);
            if (c == nullptr)
            {
                sink.OnError(ScriptError{ScriptErrorKind::Compile, assetName, -1, -1, u8"not in the assembly"});
                return false;
            }
            out.language = String(kLanguage);
            out.className = c->className;
            out.handlers = c->handlers;
            return true;
        }
    };

    struct Bed
    {
        String root = String(u8"scratch_scriptassembly");
        UniquePtr<foundation::vfs::NativeFileSystem> sources;
        UniquePtr<foundation::vfs::NativeFileSystem> sourceMount;
        UniquePtr<foundation::vfs::NativeFileSystem> outputMount;
        UniquePtr<content::ContentDatabase> sourceDb;
        UniquePtr<content::ContentDatabase> outputDb;

        Bed()
        {
            RegisterScriptResource();
            RegisterScriptAssets();
            ScriptAssemblyCookRegistry::Get().Register(String(kLanguage), MakeUnique<StandInAssemblyCook>(DefaultAllocator()));
            ScriptLanguageCookRegistry::Get().Register(String(kLanguage), MakeUnique<StandInClassCook>(DefaultAllocator()));
            (void)RemoveDirectoryRecursive(root.AsView());
            for (const StringView folder : {StringView(u8"src/Game/Enemies"), StringView(u8"src/Game/bin"), StringView(u8"src/Tools/Inner"),
                                            StringView(u8"db"), StringView(u8"out")})
                REQUIRE(CreateDirectories(PathJoin(root.AsView(), folder).AsView()));
            sources = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), PathJoin(root.AsView(), u8"src").AsView(),
                                                                    DefaultAllocator());
            sourceMount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), PathJoin(root.AsView(), u8"db").AsView(),
                                                                        DefaultAllocator());
            outputMount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), PathJoin(root.AsView(), u8"out").AsView(),
                                                                        DefaultAllocator());
            sourceDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *sourceMount, BinarySerializerFactory(),
                                                            u8".asset");
            outputDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *outputMount, BinarySerializerFactory(),
                                                            u8".rasset");
        }
        ~Bed()
        {
            outputDb = nullptr;
            sourceDb = nullptr;
            sources = nullptr;
            sourceMount = nullptr;
            outputMount = nullptr;
            (void)RemoveDirectoryRecursive(root.AsView());
        }

        void Write(StringView path, StringView text)
        {
            REQUIRE(WriteFile(PathJoin(PathJoin(root.AsView(), u8"src").AsView(), path).AsView(),
                              Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()))
                        .IsOk());
        }

        content::Instance* AddProject(StringView name, StringView file)
        {
            ScriptAssemblyAsset asset;
            asset.fileName = foundation::vfs::SourcePath(file);
            asset.language = String(kLanguage);
            content::Instance* instance = sourceDb->RootGroup()->CreateInstance(name, ScriptAssemblyAsset::StaticType());
            REQUIRE(instance != nullptr);
            REQUIRE(instance->WriteObject(asset).IsOk());
            return instance;
        }

        AssetBuildContext Context(content::Instance* output)
        {
            AssetBuildContext ctx{DefaultAllocator()};
            ctx.sources = sources.Get();
            ctx.sourceDb = sourceDb.Get();
            ctx.db = outputDb.Get();
            ctx.output = output;
            return ctx;
        }

        // The project's product, written where a class's cook looks for it (its asset's id).
        Status BuildProject(content::Instance* project)
        {
            content::Instance* product = outputDb->GetInstance(project->Id());
            if (product == nullptr)
                product = outputDb->RootGroup()->CreateInstanceWithId(project->Id(), project->Name(), ScriptAssemblySource::StaticType());
            RefPtr<ISerializable> object = project->ReadObject();
            AssetBuildContext ctx = Context(product);
            ScriptAssemblyAssetBuilder builder;
            return builder.Build(*Cast<ScriptAssemblyAsset>(object.Get()), ctx);
        }

        Status CookClass(StringView file, BehaviorClassSource& out, AssetDependencies* deps = nullptr)
        {
            ScriptClassAsset asset;
            asset.fileName = foundation::vfs::SourcePath(file);
            asset.language = String(kLanguage);
            content::Instance* product = outputDb->RootGroup()->CreateInstance(PathFilename(file), BehaviorClassSource::StaticType());
            AssetBuildContext ctx = Context(product);
            ScriptClassAssetBuilder builder;
            if (deps != nullptr)
            {
                // As the cook driver scans: the source database as the context's db.
                AssetBuildContext scan{DefaultAllocator()};
                scan.sources = sources.Get();
                scan.db = sourceDb.Get();
                builder.ScanDependencies(asset, scan, *deps);
            }
            const Status built = builder.Build(asset, ctx);
            if (built.IsOk())
            {
                RefPtr<ISerializable> record = product->ReadObject();
                const BehaviorClassSource& cooked = *Cast<BehaviorClassSource>(record.Get());
                out.className = cooked.className;
                out.handlers = cooked.handlers;
                out.bytecode = cooked.bytecode;
                out.assembly = cooked.assembly;
            }
            return built;
        }
    };

    String ImageText(const ScriptAssemblySource& assembly)
    {
        return String(StringView(reinterpret_cast<const utf8char*>(assembly.image.Data()), assembly.image.Size()));
    }
}

TEST_CASE("script assembly: a project builds every source under its folder, and its classes cook from it")
{
    Bed bed;
    bed.Write(u8"Game/Game.siproj", u8"project");
    bed.Write(u8"Game/Player.si", u8"class Player");
    bed.Write(u8"Game/Enemies/Wolf.si", u8"class Wolf");
    bed.Write(u8"Game/Helpers.si", u8"helpers, no class");
    bed.Write(u8"Game/bin/Stale.si", u8"class Stale");   // an IDE's build output: not a source
    bed.Write(u8"Game/Notes.txt", u8"class Notes");      // another language's file
    bed.Write(u8"Tools/Tools.siproj", u8"project");
    bed.Write(u8"Tools/Inner/Inner.siproj", u8"project");
    bed.Write(u8"Tools/Inner/Deep.si", u8"class Deep");
    bed.Write(u8"Loose.si", u8"class Loose");            // in no project
    content::Instance* game = bed.AddProject(u8"Game", u8"Game/Game.siproj");
    content::Instance* tools = bed.AddProject(u8"Tools", u8"Tools/Tools.siproj");
    content::Instance* inner = bed.AddProject(u8"Inner", u8"Tools/Inner/Inner.siproj");

    // The sources a project builds from are its inputs, sorted.
    {
        RefPtr<ISerializable> object = game->ReadObject();
        AssetBuildContext ctx = bed.Context(nullptr);
        AssetDependencies deps;
        ScriptAssemblyAssetBuilder builder;
        builder.ScanDependencies(*Cast<ScriptAssemblyAsset>(object.Get()), ctx, deps);
        REQUIRE(deps.files.Size() == 3u);
        CHECK(deps.files[0].View() == StringView(u8"Game/Enemies/Wolf.si"));
        CHECK(deps.files[1].View() == StringView(u8"Game/Helpers.si"));
        CHECK(deps.files[2].View() == StringView(u8"Game/Player.si"));
    }
    REQUIRE(bed.BuildProject(game).IsOk());
    {
        RefPtr<ISerializable> record = bed.outputDb->GetInstance(game->Id())->ReadObject();
        const ScriptAssemblySource* built = Cast<ScriptAssemblySource>(record.Get());
        REQUIRE(built != nullptr);
        CHECK(built->language == kLanguage);
        CHECK(ImageText(*built) == StringView(u8"Game/Enemies/Wolf.si;Game/Helpers.si;Game/Player.si;"));
        CHECK(built->FindClass(u8"Wolf") != nullptr);
    }

    // A class reads its project (which cooks first) and names it; its description is the build's.
    BehaviorClassSource player;
    AssetDependencies deps;
    REQUIRE(bed.CookClass(u8"Game/Player.si", player, &deps).IsOk());
    REQUIRE(deps.reads.Size() == 1u);
    CHECK(deps.reads[0] == game->Id());
    REQUIRE(deps.references.Size() == 1u);
    CHECK(player.assembly == game->Id());
    CHECK(player.className == u8"Player");
    REQUIRE(player.handlers.Size() == 1u);
    CHECK(player.bytecode.IsEmpty());

    // Nested projects: the nearest holds the file.
    REQUIRE(bed.BuildProject(inner).IsOk());
    BehaviorClassSource deep;
    REQUIRE(bed.CookClass(u8"Tools/Inner/Deep.si", deep).IsOk());
    CHECK(deep.assembly == inner->Id());
    CHECK(deep.assembly != tools->Id());

    // In no project, or in one that has not built: the cook fails.
    BehaviorClassSource loose;
    CHECK(bed.CookClass(u8"Loose.si", loose).Code() == ErrorCode::NotFound);
    bed.Write(u8"Tools/Tool.si", u8"class Tool");
    BehaviorClassSource tool;
    CHECK(bed.CookClass(u8"Tools/Tool.si", tool).Code() == ErrorCode::NotFound);
}

TEST_CASE("script assembly: a project that does not build writes nothing, and its file imports as its language's project")
{
    Bed bed;
    bed.Write(u8"Game/Game.siproj", u8"broken");
    bed.Write(u8"Game/Player.si", u8"class Player");
    content::Instance* game = bed.AddProject(u8"Game", u8"Game/Game.siproj");
    CHECK_FALSE(bed.BuildProject(game).IsOk());
    RefPtr<ISerializable> record = bed.outputDb->GetInstance(game->Id())->ReadObject();
    CHECK(Cast<ScriptAssemblySource>(record.Get()) == nullptr);

    ScriptAssemblyFileImporter importer;
    CHECK(importer.Accepts(u8"siproj"));
    CHECK_FALSE(importer.Accepts(u8"si"));
    CHECK(importer.AssetTypeName(u8"Game.siproj") == StringView(u8"ScriptAssemblyAsset"));
}

TEST_CASE("script assembly: a project's local files are written beside it when it is made and at each cook")
{
    Bed bed;
    const String src = PathAbsolute(PathJoin(bed.root.AsView(), u8"src").AsView());
    const auto read = [](StringView path)
    {
        Result<Array<byte>> bytes = ReadFile(path);
        return bytes.HasValue() ? String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()))
                                : String{};
    };

    // Made: the project file in the project's code folder (its fileName Code/Made.siproj) and,
    // beside it, the cook's local files; nothing under the sources folder.
    const String code = PathAbsolute(PathJoin(bed.root.AsView(), u8"code").AsView());
    AssetCreationContext context;
    context.root = bed.sourceDb->RootGroup();
    context.sourcesRoot = src.AsView();
    context.codeRoot = code.AsView();
    context.name = u8"Made";
    content::Instance* made = CreateScriptProjectInstance(context, kLanguage, u8".siproj");
    REQUIRE(made != nullptr);
    CHECK(FileExists(PathJoin(code.AsView(), u8"Made.siproj").AsView()));
    CHECK_FALSE(FileExists(PathJoin(src.AsView(), u8"Made.siproj").AsView()));
    CHECK(read(PathJoin(code.AsView(), u8"obj/Local.txt").AsView()) == StringView(u8"this machine"));
    RefPtr<ISerializable> madeAsset = made->ReadObject();
    REQUIRE(Cast<ScriptAssemblyAsset>(madeAsset.Get()) != nullptr);
    CHECK(Cast<ScriptAssemblyAsset>(madeAsset.Get())->fileName.View() == StringView(u8"Code/Made.siproj"));
    // Made in a group: in that group's folder under Code/, its local files beside it.
    content::Group* tools = bed.sourceDb->RootGroup()->CreateGroup(u8"Tools");
    REQUIRE(tools != nullptr);
    context.picked = tools;
    context.name = u8"Builder";
    content::Instance* grouped = CreateScriptProjectInstance(context, kLanguage, u8".siproj");
    REQUIRE(grouped != nullptr);
    RefPtr<ISerializable> groupedAsset = grouped->ReadObject();
    CHECK(Cast<ScriptAssemblyAsset>(groupedAsset.Get())->fileName.View() == StringView(u8"Code/Tools/Builder.siproj"));
    CHECK(FileExists(PathJoin(code.AsView(), u8"Tools/Builder.siproj").AsView()));
    CHECK(read(PathJoin(code.AsView(), u8"Tools/obj/Local.txt").AsView()) == StringView(u8"this machine"));
    context.picked = nullptr;

    // Cooked: written beside the project's file, and written again when it is no longer what the
    // cook would write (another machine's, or edited).
    bed.Write(u8"Game/Game.siproj", u8"project");
    bed.Write(u8"Game/Player.si", u8"class Player");
    content::Instance* game = bed.AddProject(u8"Game", u8"Game/Game.siproj");
    REQUIRE(bed.BuildProject(game).IsOk());
    const String local = PathJoin(src.AsView(), u8"Game/obj/Local.txt");
    CHECK(read(local.AsView()) == StringView(u8"this machine"));
    bed.Write(u8"Game/obj/Local.txt", u8"another machine");
    REQUIRE(bed.BuildProject(game).IsOk());
    CHECK(read(local.AsView()) == StringView(u8"this machine"));

    // Never an input: the project still builds only its sources.
    RefPtr<ISerializable> record = bed.outputDb->GetInstance(game->Id())->ReadObject();
    CHECK(ImageText(*Cast<ScriptAssemblySource>(record.Get())) == StringView(u8"Game/Player.si;"));
}
