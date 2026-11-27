// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export partition.
//
// The export pipeline as a library (the Tools.Export CLI and the editor's Export menu are thin
// callers; tests drive it headlessly): cook, stage scenes as binary envelopes, pack Content.pak,
// then write the dist manifest. The caller stages the player executable, since where it lives is
// the caller's concern rather than the pipeline's.
//
//   dist layout:  <out>/Content.pak   cooked products and scenes (one binary DB)
//                 <out>/player.xml    dist manifest (ProjectSettings shape)
//
// Scenes have no builder because their cooked form is the authored form, so export re-encodes
// each XML envelope to binary and copies the "scene" stream under the same guids; resource refs
// and the manifest's defaultScene path keep working in the pak.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

module editor.project;

import foundation.core;
import foundation.settings; // the settings attributes (label, assetType, ...)
import foundation.vfs;
import foundation.vfs.pak;
import foundation.content;
import engine.project;
import foundation.scene.resource;
import pipeline.core;
import pipeline.cook;
import script.pipeline; // ScriptClassAsset and the language cooks (the per-platform script check)
import foundation.shaders;
import foundation.shaders.system; // kShaderFolder / kShaderPackPath (the dist data layout)
import foundation.shaders.resource; // ShaderSource: a project's custom shader, cooked into the pack
import :project;
import :export_preset;
import :export_roots;
import :export_template;
import foundation.runtime; // the project's plugins (their manifests, where each is enabled)

using namespace foundation::core;
using namespace foundation;
using namespace pipeline;
namespace shaders = foundation::shaders;
namespace vfs = foundation::vfs;

namespace editor
{
    REFLECT_ENUM(ExportKind, "rtti::editor::editor")
    {
        builder.Value("Template", ExportKind::Template).Value("Native", ExportKind::Native);
    }

    REFLECT_ENUM(AndroidPackage, "rtti::editor::editor")
    {
        builder.Value("Apk", AndroidPackage::Apk).Value("Bundle", AndroidPackage::Bundle).Value("Both", AndroidPackage::Both);
    }

    REFLECT_VALUE(ExportPreset, "rtti::editor::editor")
    {
        const auto label = [&builder](StringView text) { builder.PropAttribute(foundation::settings::kSettingLabelAttribute, String(text)); };
        const Float4 renderSize{0.0f, 16384.0f, 1.0f, 0.0f};
        const Float4 windowSize{1.0f, 16384.0f, 1.0f, 0.0f};
        RegisterCoreTypes(); // FitMode
        RegisterArrayType<String>(); // additionalFiles
        builder.Property<&ExportPreset::name>("name");
        label(u8"Name");
        builder.Property<&ExportPreset::platform>("platform");
        label(u8"Platform");
        RttiRegisterEnum_ExportKind();
        builder.Property<&ExportPreset::kind>("kind");
        label(u8"Export kind");
        builder.PropAttribute("description", String(u8"Template: the template's prebuilt player. Native: the game's own "
                                                    u8"player, built from the engine checkout with its plugin packages "
                                                    u8"and C# (needs the platform's toolchain)."));
        builder.Property<&ExportPreset::config>("config");
        label(u8"Config");
        builder.Property<&ExportPreset::templateId>("templateId");
        label(u8"Template");
        builder.Property<&ExportPreset::playerName>("playerName");
        label(u8"Player name");
        builder.Property<&ExportPreset::outputSubdir>("outputSubdir");
        label(u8"Output subdir");
        builder.Property<&ExportPreset::additionalFiles>("additionalFiles");
        label(u8"Extra files");
        builder.Property<&ExportPreset::stageSymbols>("stageSymbols");
        label(u8"Stage debug symbols");
        builder.Property<&ExportPreset::pruneToReachable>("pruneToReachable");
        label(u8"Prune to reachable content");
        builder.Property<&ExportPreset::includeShaderSource>("includeShaderSource");
        label(u8"Include shader source");
        builder.Property<&ExportPreset::overridesRender>("overridesRender");
        label(u8"Own render size");
        builder.Property<&ExportPreset::renderWidth>("renderWidth");
        label(u8"Render width");
        builder.PropAttribute("range", renderSize);
        builder.Property<&ExportPreset::renderHeight>("renderHeight");
        label(u8"Render height");
        builder.PropAttribute("range", renderSize);
        builder.Property<&ExportPreset::renderFit>("renderFit");
        label(u8"Render fit");
        builder.Property<&ExportPreset::overridesWindow>("overridesWindow");
        label(u8"Own window");
        builder.Property<&ExportPreset::windowWidth>("windowWidth");
        label(u8"Window width");
        builder.PropAttribute("range", windowSize);
        builder.Property<&ExportPreset::windowHeight>("windowHeight");
        label(u8"Window height");
        builder.PropAttribute("range", windowSize);
        builder.Property<&ExportPreset::windowMode>("windowMode");
        label(u8"Window mode");
        builder.Property<&ExportPreset::windowResizable>("windowResizable");
        label(u8"Window resizable");
        builder.Property<&ExportPreset::presentMode>("presentMode");
        label(u8"Present mode");
        builder.Property<&ExportPreset::overridesOrientation>("overridesOrientation");
        label(u8"Own orientation");
        builder.Property<&ExportPreset::orientation>("orientation");
        label(u8"Orientation");
        builder.Property<&ExportPreset::qualityLevelId>("qualityLevelId");
        label(u8"Quality level");
        builder.PropAttribute(foundation::settings::kSettingAssetTypeAttribute, String(u8"RenderQualityAsset"))
            .PropAttribute(foundation::settings::kSettingEmptyTextAttribute, String(u8"(the project's default)"));
        // The Android app's fields, for an Android64 preset alone.
        const auto android = [&builder] { builder.PropAttribute(kPresetPlatformAttribute, String(u8"Android64")); };
        RttiRegisterEnum_AndroidPackage();
        builder.Property<&ExportPreset::androidApplicationId>("androidApplicationId");
        label(u8"Application id");
        android();
        builder.Property<&ExportPreset::androidVersionCode>("androidVersionCode");
        label(u8"Version code");
        builder.PropAttribute("range", Float4{1.0f, 2100000000.0f, 1.0f, 0.0f}); // Google Play's ceiling
        android();
        builder.Property<&ExportPreset::androidVersionName>("androidVersionName");
        label(u8"Version name");
        android();
        builder.Property<&ExportPreset::androidLabel>("androidLabel");
        label(u8"App name");
        android();
        builder.Property<&ExportPreset::androidIcon>("androidIcon");
        label(u8"App icon");
        android();
        builder.Property<&ExportPreset::androidPackage>("androidPackage");
        label(u8"Package");
        android();
        builder.Property<&ExportPreset::androidKeystore>("androidKeystore");
        label(u8"Keystore");
        android();
        builder.Property<&ExportPreset::androidKeyAlias>("androidKeyAlias");
        label(u8"Key alias");
        android();
    }

    void RegisterExportPresetReflection()
    {
        (void)engine::project::ProjectSettings::StaticType(); // WindowMode's enumerators
        RttiRegisterValue_ExportPreset();
    }

    namespace
    {
        // The cooked-blob formats a target platform's runtime needs: WGSL for the web, SPIR-V (Vulkan)
        // and DXIL (DX12) for Windows, SPIR-V (Vulkan) for other desktops. Kept generous rather than
        // guessing the backend a desktop dist will pick at runtime.
        Array<shaders::CookedShaderFormat> FormatsForPlatform(StringView platform)
        {
            Array<shaders::CookedShaderFormat> formats;
            if (platform.StartsWith(u8"Web") || platform.StartsWith(u8"Wasm"))
            {
                formats.PushBack(shaders::CookedShaderFormat::Wgsl);
            }
            else if (platform.StartsWith(u8"Win"))
            {
                formats.PushBack(shaders::CookedShaderFormat::SpirV);
                formats.PushBack(shaders::CookedShaderFormat::Dxil);
            }
            else
            {
                formats.PushBack(shaders::CookedShaderFormat::SpirV);
            }
            return formats;
        }

        // The DXC runtime libraries (dxcompiler, dxil), which a cooked dist does not need, since the
        // shader pack replaces the runtime compiler. Matched by substring on the sidecar's file name.
        bool IsDxcRuntimeLib(StringView name)
        {
            const auto contains = [&](StringView needle)
            {
                if (needle.Size() > name.Size())
                {
                    return false;
                }
                for (usize i = 0; i + needle.Size() <= name.Size(); ++i)
                {
                    if (name.SubStr(i, needle.Size()) == needle)
                    {
                        return true;
                    }
                }
                return false;
            };
            return contains(u8"dxcompiler") || contains(u8"dxil");
        }

        // Cook the built-in shaders (from <dataRoot>/Shaders) and the project's custom shaders the
        // dist ships (`projectShaders`, their cooked ShaderSource products) for `platform`, and
        // write the dist's Data/Engine/Shaders/shaders.dpak plus its Data/.dataroot marker, the layout the
        // player's data-root discovery finds beside the executable. The dist renders from this pack
        // with no runtime compiler (see the ShaderSystem cooked path). Returns the variant count via
        // `outVariants`; Status carries any cook or compile failure.
        Status StageShaderPack(StringView outputDir, StringView dataRoot, StringView platform,
                               EditorProject& project, Span<const Guid> projectShaders,
                               u32& outVariants)
        {
            outVariants = 0;
            const String shaderDir = vfs::DataPath(dataRoot, shaders::kShaderFolder);
            if (!DirectoryExists(shaderDir.AsView()))
            {
                LOG_ERROR(u8"Export", u8"cannot cook shaders: no '{}' under the data root '{}'",
                          shaders::kShaderFolder, dataRoot);
                return Status{ErrorCode::NotFound};
            }
            shaders::Compiler* compiler = nullptr;
            if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() ||
                compiler == nullptr)
            {
                LOG_ERROR(u8"Export",
                                   u8"cannot cook shaders: the DXC compiler is unavailable");
                return Status{ErrorCode::Internal};
            }

            const Array<shaders::CookedShaderFormat> formats = FormatsForPlatform(platform);
            shaders::ShaderCookOptions opts;
            opts.shaderDir = shaderDir.AsView();
            opts.scratchDir = outputDir; // WGSL intermediates (deleted); unused for SPIR-V/DXIL
            opts.formats = Span<const shaders::CookedShaderFormat>(formats.Data(), formats.Size());

            shaders::CookedShaderPack pack;
            shaders::ShaderCookReport report = shaders::CookEngineShaders(*compiler, opts, pack);

            // The project's shaders: each program's stages under their entry points and lattices
            // (a surface shader's depth and pick beside its main one), every variant,
            // with the engine library on the include path (what their cook compiled against).
            const StringView includePaths[] = {shaderDir.AsView()};
            shaders::WgslTranslator translator(*compiler, outputDir);
            translator.SetValidateWithTint(opts.validateWgsl);
            for (const Guid& id : projectShaders)
            {
                foundation::content::Instance* product = project.CookedDb().GetInstance(id);
                RefPtr<ISerializable> object =
                    product != nullptr ? product->ReadObject() : RefPtr<ISerializable>{};
                const auto* source = Cast<shaders::ShaderSource>(object.Get());
                if (source == nullptr)
                {
                    report.errors.PushBack(String(u8"a custom shader's cooked product is missing"));
                    continue;
                }
                const String sourceName = Format(u8"{}.hlsl", source->name);
                for (const shaders::ShaderProgram& program : source->Programs())
                {
                    (void)shaders::CookShaderStage(
                        *compiler, translator, program.name.AsView(), shaders::ShaderStage::Vertex,
                        program.vertexSource, program.vertexEntryPoint, program.vertexVariantMask,
                        sourceName.AsView(), opts, Span<const StringView>(includePaths, 1), pack,
                        report);
                    (void)shaders::CookShaderStage(
                        *compiler, translator, program.name.AsView(),
                        shaders::ShaderStage::Fragment, program.fragmentSource,
                        program.fragmentEntryPoint, program.fragmentVariantMask,
                        sourceName.AsView(), opts, Span<const StringView>(includePaths, 1), pack,
                        report);
                }
            }
            report.success = report.errors.IsEmpty();
            compiler->Destroy();

            for (usize i = 0; i < report.errors.Size(); ++i)
            {
                LOG_ERROR(u8"Export", u8"shader cook: {}", report.errors[i]);
            }
            if (!report.success)
            {
                return Status{ErrorCode::Internal};
            }

            const String distData = PathJoin(outputDir, u8"Data");
            const String packPath = PathJoin(distData.AsView(), shaders::kShaderPackPath);
            if (!CreateDirectories(PathParent(packPath.AsView())))
            {
                LOG_ERROR(u8"Export", u8"could not create '{}'", PathParent(packPath.AsView()));
                return Status{ErrorCode::Internal};
            }
            FileStream out(packPath.AsView(), FileMode::Write);
            if (!out.IsValid() || !pack.Write(out).IsOk())
            {
                LOG_ERROR(u8"Export", u8"could not write '{}'", packPath);
                return Status{ErrorCode::Internal};
            }
            // The marker that makes <dist>/Data a data root for the player's discovery walk.
            constexpr StringView kMarkerText = u8"# Data root marker (foundation.vfs FindDataRoot). "
                                               u8"Staged by the export.\nversion: 1\n";
            const String marker = PathJoin(distData.AsView(), vfs::kDataRootMarker);
            if (!WriteFile(marker.AsView(),
                           Span<const byte>(reinterpret_cast<const byte*>(kMarkerText.Data()),
                                            kMarkerText.Size()))
                     .IsOk())
            {
                LOG_ERROR(u8"Export", u8"could not write '{}'", marker);
                return Status{ErrorCode::Internal};
            }
            outVariants = static_cast<u32>(pack.Count());
            return Status{};
        }
    }

    StringView ExportRootReasonName(ExportRootReason r)
    {
        switch (r)
        {
        case ExportRootReason::DefaultScene:
            return u8"default-scene";
        case ExportRootReason::StartupScript:
            return u8"startup-script";
        case ExportRootReason::Flag:
            return u8"always-export";
        case ExportRootReason::Group:
            return u8"always-export-group";
        case ExportRootReason::ManifestDefault:
            return u8"manifest-default";
        }
        return u8"?";
    }

    Array<ExportRoot> CollectExportRoots(EditorProject& project)
    {
        Array<ExportRoot> roots;
        HashMap<Guid, u8> seen;
        const auto add = [&](const Guid& id, ExportRootReason reason)
        {
            if (id.IsNil() || seen.Find(id) != nullptr)
            {
                return;
            }
            seen.InsertOrAssign(id, u8(1));
            ExportRoot root;
            root.id = id;
            root.reason = reason;
            if (foundation::content::Instance* inst = project.SourceDb().GetInstance(id))
            {
                root.name = inst->Path();
            }
            roots.PushBack(Move(root));
        };

        const engine::project::ProjectSettings& settings = project.Settings();

        add(settings.defaultSceneId, ExportRootReason::DefaultScene);

        // The startup game script is a cooked BehaviorClass asset, identified by guid. Seed it as a
        // reachability root so it, and the assets it loads through the normal AssetRef contract, ship.
        add(settings.runBehaviorId, ExportRootReason::StartupScript);

        // Every other asset the player binds at startup must ship, or the binding fails silently
        // in a pruned dist: each reflected asset setting, lists included (the other UI fonts). The
        // scene and the script above keep their own reasons (add keeps the first).
        engine::project::ForEachSettingAsset(
            settings, [&add](const PropertyInfo&, const Guid& id) { add(id, ExportRootReason::ManifestDefault); });

        // "Always Export": explicit instance flags, then group subtrees (membership is dynamic:
        // whatever is under the flagged folder now). A group that also contains the default scene
        // or a directly flagged instance is deduplicated by add, keeping the earlier reason.
        const ExportRootsSet& always = project.ExportRoots();
        for (const Guid& id : always.instances)
        {
            add(id, ExportRootReason::Flag);
        }
        for (const String& groupPath : always.groups)
        {
            Array<Guid> members;
            CollectGroupInstances(project.SourceDb(), groupPath.AsView(), members);
            for (const Guid& id : members)
            {
                add(id, ExportRootReason::Group);
            }
        }
        return roots;
    }

    Array<Guid> ExpandReachableRoots(EditorProject& project, const Array<ExportRoot>& seeds,
                                     const SceneReferenceScanner& scanner)
    {
        Array<Guid> out;
        HashMap<Guid, u8> seen;
        Array<Guid> queue;
        const auto push = [&](const Guid& id)
        {
            if (id.IsNil() || seen.Find(id) != nullptr)
            {
                return;
            }
            seen.InsertOrAssign(id, u8(1));
            out.PushBack(id);
            queue.PushBack(id);
        };
        for (const ExportRoot& r : seeds)
        {
            push(r.id);
        }

        usize head = 0;
        while (head < queue.Size())
        {
            const Guid id = queue[head++];
            foundation::content::Instance* inst = project.SourceDb().GetInstance(id);
            if (inst == nullptr)
            {
                continue;
            }
            const bool isSceneLike =
                inst->TypeName() == u8"SceneDocument" || inst->TypeName() == u8"PrefabDocument";
            if (!isSceneLike || !scanner)
            {
                continue;
            }

            SceneReferences refs;
            scanner(*inst, project.SourceDb(), refs);
            for (const Guid& g : refs.resources)
            {
                push(g);
            }
            for (const Guid& g : refs.prefabs)
            {
                push(g);
            }
        }
        return out;
    }

    Status CookReachable(EditorProject& project, BuilderRegistry& builders,
                         Span<const Guid> planRoots, bool cook, bool rebuild, ExportStats& stats,
                         Array<Guid>& outReachable, const ExportProgress& onProgress)
    {
        foundation::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        project.AddCodeFolder(sourcesMount);
        foundation::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView(), editor::EditorRootAllocator());
        JobSystem jobs(editor::EditorRootAllocator()); // export tool root
        CookDriver driver(editor::EditorRootAllocator(), project.SourceDb(), project.CookedDb(),
                          builders, &sourcesMount, &cacheMount, &jobs);
        CookPlan plan = driver.PlanFor(planRoots, rebuild);
        outReachable = plan.reachable;
        if (plan.unreadable > 0)
        {
            LOG_ERROR(u8"Export", u8"aborting - {} asset(s) the game uses did not read, so they cannot cook (the Cook lines above name them)",
                      plan.unreadable);
            return Status{ErrorCode::InvalidArgument};
        }

        if (cook)
        {
            CookProgress cookProgress;
            cookProgress.onItem = [&onProgress](usize done, usize total, StringView path, bool)
            {
                if (!onProgress)
                {
                    return;
                }
                const f32 frac =
                    (total > 0) ? 0.05f + (static_cast<f32>(done) / static_cast<f32>(total)) * 0.55f
                                : 0.6f;
                String step(u8"Cooking ");
                step += path;
                onProgress(step.AsView(), frac);
            };
            const CookStats cookStats = driver.Execute(plan, &cookProgress);
            stats.cooked = cookStats.cooked;
            stats.cookFailed = cookStats.failed;
            if (cookStats.failed > 0)
            {
                LOG_ERROR(u8"Export", u8"aborting - the cook has {} failure(s)",
                                   cookStats.failed);
                return Status{ErrorCode::Internal};
            }
        }
        return Status{};
    }

    String FormatPruningReport(const PruningReport& report)
    {
        String out(u8"Export reachability pruning report\n");
        out += u8"==================================\n";
        out += Format(u8"kept: {} instance(s) in the closure\n", report.keptCount);
        out += Format(u8"roots: {}\n", report.roots.Size());
        for (const ExportRoot& r : report.roots)
        {
            out += u8"  - ";
            out += r.name.IsEmpty() ? StringView(u8"(unresolved)") : r.name.AsView();
            out += u8"  [";
            out += ExportRootReasonName(r.reason);
            out += u8"]\n";
        }
        out += Format(u8"dropped: {} instance(s)\n", report.dropped.Size());
        for (const String& d : report.dropped)
        {
            out += u8"  - ";
            out += d.AsView();
            out += u8"\n";
        }
        return out;
    }

    // Whether this is a web or wasm target, the only kind that splits into BC and ASTC variants.
    static bool IsWebPlatform(StringView platform)
    {
        return platform.StartsWith(u8"Web") || platform.StartsWith(u8"Wasm");
    }

    Array<ContentVariant> VariantsForPlatform(EditorProject& project, StringView platform)
    {
        Array<ContentVariant> out;
        if (IsWebPlatform(platform))
        {
            // Two complete paks, one per compressed texture family the browser might support. The
            // cooked directories are siblings of the host Cooked/, matching Tools.Cook --target.
            const StringView keys[] = {StringView(u8"bc"), StringView(u8"astc")};
            for (const StringView key : keys)
            {
                ContentVariant v;
                v.key = String(key);
                v.pakName = String(u8"Content-");
                v.pakName.Append(key);
                v.pakName.Append(u8".pak");
                v.cookedDir = String(project.Directory());
                v.cookedDir.Append(u8"/Cooked-web-");
                v.cookedDir.Append(key); // Cooked-web-bc / Cooked-web-astc
                v.targetId = Format(u8"web-{}", key);
                out.PushBack(Move(v));
            }
        }
        else if (IsAndroidPlatform(platform))
        {
            // One pak, its textures ASTC (Android's GPUs do not read BC), cooked into a sibling of
            // the host Cooked/ as Tools.Cook --target android does.
            ContentVariant v;
            v.key = String(u8"android");
            v.pakName = String(engine::project::kDistContentPak);
            v.cookedDir = Format(u8"{}/Cooked-android", project.Directory());
            v.targetId = String(u8"android");
            out.PushBack(Move(v));
        }
        else
        {
            // Desktop: the single host pak from Cooked/.
            ContentVariant v;
            v.pakName = String(engine::project::kDistContentPak);
            v.cookedDir = PathJoin(project.Directory(), engine::project::kProjectCookedDir);
            out.PushBack(Move(v));
        }
        return out;
    }

    Status CookVariantTargets(EditorProject& project, BuilderRegistry& builders,
                              Span<const ContentVariant> variants, bool rebuild,
                              const ExportProgress& onProgress)
    {
        // The host cook already ran (ExportProject or CookReachable) and saved its records to
        // .cache/cook.db; load them once to gate the copy-forward for every target.
        foundation::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView(), editor::EditorRootAllocator());
        CookDb hostRecords{editor::EditorRootAllocator()};
        hostRecords.Load(cacheMount);

        foundation::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        project.AddCodeFolder(sourcesMount);
        JobSystem jobs(editor::EditorRootAllocator()); // export tool root
        usize failed = 0;
        for (const ContentVariant& v : variants)
        {
            if (v.key.IsEmpty())
            {
                continue; // desktop: the host DB already is this variant
            }
            if (onProgress)
            {
                String step(u8"Cooking variant ");
                step += v.key.AsView();
                onProgress(step.AsView(), 0.6f);
            }
            // The variant's cook target resolves to its capability profile (ASTC or BC).
            const String targetId = v.targetId;

            // Create Cooked-web-<key>/ and .cache/web-<key>/, carrying invariant products forward
            // from the host.
            const String cacheDir = PathJoin(
                PathJoin(project.Directory(), engine::project::kProjectCacheDir), targetId.AsView());
            (void)CreateDirectory(v.cookedDir.AsView());
            (void)CreateDirectory(PathJoin(project.Directory(),
                                           engine::project::kProjectCacheDir).AsView());
            (void)CreateDirectory(cacheDir.AsView());

            foundation::vfs::NativeFileSystem targetCookedMount(v.cookedDir.AsView(), editor::EditorRootAllocator());
            foundation::vfs::NativeFileSystem targetCacheMount(cacheDir.AsView(), editor::EditorRootAllocator());
            foundation::content::ContentDatabase targetDb(editor::EditorRootAllocator(), targetCookedMount, BinarySerializerFactory(),
                                                        engine::project::kCookedAssetExtension);

            const CookStats s = CookForTarget(editor::EditorRootAllocator(), project.SourceDb(),
                                              targetDb, project.CookedDb(),
                                              hostRecords, builders, &sourcesMount, &targetCacheMount,
                                              CookTargetFor(targetId.AsView()), &jobs, rebuild);
            failed += s.failed;
            LOG_INFO(u8"Export", u8"variant '{}' cooked {}, copied-forward {}, failed {}", v.key,
                     s.cooked, s.copiedForward, s.failed);
        }
        return (failed == 0) ? Status{} : Status{ErrorCode::Internal};
    }

    Status ExportContent(EditorProject& project, StringView outDir, ExportStats& stats,
                         const ExportProgress& onProgress,
                         const HashMap<Guid, Array<byte>>* sceneStreams,
                         const HashMap<Guid, u8>* reachable, const Array<ExportRoot>* roots,
                         PruningReport* outReport, Span<const ContentVariant> variants,
                         bool includeShaderSource)
    {

        // --- stage scenes ---
        if (onProgress)
        {
            onProgress(u8"Staging scenes...", 0.65f);
        }
        if (!CreateDirectory(outDir))
        {
            return Status{ErrorCode::NotSupported};
        }
        // The staging folder is this export's alone: what an export that failed left in it would be
        // packed with these scenes, so it starts empty, and it goes on every way out of here.
        const String stagingDir = PathJoin(outDir, u8".stage-scenes");
        detail::RemoveTreeRecursive(stagingDir.AsView());
        (void)CreateDirectory(stagingDir.AsView());
        struct StagingCleanup
        {
            StringView dir;
            ~StagingCleanup() { detail::RemoveTreeRecursive(dir); }
        } stagingCleanup{stagingDir.AsView()};
        foundation::vfs::NativeFileSystem stagingMount(stagingDir.AsView(), editor::EditorRootAllocator());
        Array<String> droppedScenes; // for the pruning report
        HashMap<String, u8> strippedShaders; // cooked products replaced by a staged, stripped copy
        {
            foundation::content::ContentDatabase staging(editor::EditorRootAllocator(), stagingMount, BinarySerializerFactory(),
                                                       engine::project::kCookedAssetExtension);
            Array<foundation::content::Instance*> scenes;
            detail::CollectScenes(*project.SourceDb().RootGroup(), scenes);
            usize staged = 0;
            for (foundation::content::Instance* scene : scenes)
            {
                if (reachable != nullptr && reachable->Find(scene->Id()) == nullptr)
                {
                    droppedScenes.PushBack(scene->Path()); // pruned: not reachable from any root
                    continue;
                }
                if (!detail::StageScene(*scene, staging, sceneStreams))
                {
                    LOG_ERROR(u8"Export", u8"failed to stage scene '{}'", scene->Path());
                    return Status{ErrorCode::Internal};
                }
                ++staged;
            }
            stats.scenesStaged = staged;

            // The project's custom shaders the dist ships: recorded for the shader pack step,
            // which cooks them to bytecode; staged without their HLSL unless the preset keeps it
            // (the project's own Cooked/ keeps its source for the editor, so the copy is staged).
            const StringView shaderType(reinterpret_cast<const utf8char*>(
                foundation::shaders::ShaderSource::StaticType().name));
            Array<foundation::content::Instance*> products;
            detail::CollectAllInstances(*project.CookedDb().RootGroup(), products);
            for (foundation::content::Instance* product : products)
            {
                if (product->TypeName() != shaderType ||
                    (reachable != nullptr && reachable->Find(product->Id()) == nullptr))
                {
                    continue;
                }
                stats.shaders.PushBack(product->Id());
                if (includeShaderSource)
                {
                    continue;
                }
                if (!detail::StageStrippedShader(*product, staging))
                {
                    LOG_ERROR(u8"Export", u8"failed to stage shader '{}'", product->Path());
                    return Status{ErrorCode::Internal};
                }
                strippedShaders.InsertOrAssign(product->Path(), u8(1));
            }
        }

        // --- 3. pack ---
        // When pruning, resolve the reachable guids to their cooked instance paths so the pack walk
        // can filter cooked files (a file belongs to instance P exactly when it begins with "P.").
        UniquePtr<HashMap<String, u8>> reachablePaths;
        usize keptProducts = 0;
        Array<String> droppedProducts;
        if (reachable != nullptr)
        {
            reachablePaths = MakeUnique<HashMap<String, u8>>(editor::EditorRootAllocator());
            Array<foundation::content::Instance*> cooked;
            detail::CollectAllInstances(*project.CookedDb().RootGroup(), cooked);
            for (foundation::content::Instance* product : cooked)
            {
                if (reachable->Find(product->Id()) != nullptr)
                {
                    reachablePaths->InsertOrAssign(product->Path(), u8(1));
                    ++keptProducts;
                }
            }
            // Dropped means the authored (non-scene) source assets excluded from the dist. It is
            // computed from the source DB, not the cooked DB: a scoped cook never produces the
            // unreachable assets, so they would not appear cooked, but they are exactly what pruning
            // left out, so the report must name them (scenes are reported through droppedScenes above).
            Array<foundation::content::Instance*> sources;
            detail::CollectAllInstances(*project.SourceDb().RootGroup(), sources);
            for (foundation::content::Instance* src : sources)
            {
                const bool isSceneLike =
                    src->TypeName() == u8"SceneDocument" || src->TypeName() == u8"PrefabDocument";
                if (!isSceneLike && reachable->Find(src->Id()) == nullptr)
                {
                    droppedProducts.PushBack(src->Path());
                }
            }
        }

        if (onProgress)
        {
            onProgress(u8"Packing content...", 0.78f);
        }
        // The variants to pack: the caller's list, or by default the single host Content.pak. Each
        // variant is a complete pak: its own cooked DB with the same staged scenes and the same
        // reachable set (the closure's guid set does not vary by variant).
        Array<ContentVariant> effectiveVariants;
        if (variants.IsEmpty())
        {
            ContentVariant desktop;
            desktop.pakName = String(engine::project::kDistContentPak);
            desktop.cookedDir = PathJoin(project.Directory(), engine::project::kProjectCookedDir);
            effectiveVariants.PushBack(Move(desktop));
        }
        else
        {
            for (const ContentVariant& v : variants)
            {
                ContentVariant copy;
                copy.key = String(v.key.AsView());
                copy.pakName = String(v.pakName.AsView());
                copy.cookedDir = String(v.cookedDir.AsView());
                copy.targetId = String(v.targetId.AsView());
                effectiveVariants.PushBack(Move(copy));
            }
        }
        // The startup game script needs no special staging: it is a cooked BehaviorClass asset in the
        // reachability closure, so it is already in the content DB pak like every other asset. The
        // dist manifest carries its guid (below); the player binds it from the content DB.
        for (const ContentVariant& v : effectiveVariants)
        {
            foundation::vfs::PakBuilder pak;
            foundation::vfs::NativeFileSystem cookedMount(v.cookedDir.AsView(), editor::EditorRootAllocator());
            if (!detail::PackTree(cookedMount, *cookedMount.AsEnumerable(), u8"", pak,
                                  stats.filesPacked, reachablePaths.Get(), &strippedShaders) ||
                !detail::PackTree(stagingMount, *stagingMount.AsEnumerable(), u8"", pak,
                                  stats.filesPacked))
            {
                LOG_ERROR(u8"Export", u8"packing failed for variant '{}'", v.pakName);
                return Status{ErrorCode::Internal};
            }
            const String pakPath = PathJoin(outDir, v.pakName.AsView());
            if (!pak.Write(pakPath.AsView()).IsOk())
            {
                LOG_ERROR(u8"Export", u8"failed to write '{}'", v.pakName);
                return Status{ErrorCode::Internal};
            }
        }

        // --- 4. dist manifest ---
        if (onProgress)
        {
            onProgress(u8"Writing manifest...", 0.9f);
        }
        {
            foundation::vfs::NativeFileSystem outMount(outDir, editor::EditorRootAllocator());
            // The player reads the same manifest the project writes, so every default it binds at
            // startup (theme, input map, bus layout, font, loading screen, MSAA) comes along.
            engine::project::ProjectSettings dist;
            if (!engine::project::CopyProjectSettings(project.Settings(), dist).IsOk())
            {
                LOG_ERROR(u8"Export", u8"failed to copy the project manifest for the dist");
                return Status{ErrorCode::Internal};
            }
            dist.mcpFolders.Clear(); // the authoring machine's folders: nothing a player reads
            if (!engine::project::SaveProjectSettings(*outMount.AsWritable(), dist,
                                                        engine::project::kDistManifestFile)
                     .IsOk())
            {
                LOG_ERROR(u8"Export", u8"failed to write the dist manifest");
                return Status{ErrorCode::Internal};
            }
        }

        // --- 5. pruning report (loud and auditable, since pruning can silently break an exported game) ---
        if (reachable != nullptr)
        {
            PruningReport report;
            report.pruned = true;
            if (roots != nullptr)
            {
                report.roots = *roots;
            }
            report.keptCount = stats.scenesStaged + keptProducts;
            for (const String& d : droppedScenes)
            {
                report.dropped.PushBack(String(d.AsView()));
            }
            for (const String& d : droppedProducts)
            {
                report.dropped.PushBack(String(d.AsView()));
            }

            const String text = FormatPruningReport(report);
            LOG_INFO(u8"Export", u8"pruned dist: {} kept, {} dropped ({} root(s))",
                              report.keptCount, report.dropped.Size(), report.roots.Size());
            {
                foundation::vfs::NativeFileSystem outMount(outDir, editor::EditorRootAllocator());
                (void)outMount.AsWritable()->Save(
                    u8"export-report.txt",
                    Span<const byte>(reinterpret_cast<const byte*>(text.CStr()), text.Size()));
            }
            if (outReport != nullptr)
            {
                *outReport = Move(report);
            }
        }

        return Status{}; // the staging folder goes with stagingCleanup
    }

    Status ExportProject(EditorProject& project, StringView outDir, BuilderRegistry& builders,
                         bool rebuild, ExportStats* outStats, const ExportProgress& onProgress,
                         const HashMap<Guid, Array<byte>>* sceneStreams,
                         Span<const ContentVariant> variants, bool includeShaderSource)
    {
        ExportStats stats;

        // --- cook ---
        if (onProgress)
        {
            onProgress(u8"Cooking content...", 0.05f);
        }
        foundation::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        project.AddCodeFolder(sourcesMount);
        foundation::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView(), editor::EditorRootAllocator());
        JobSystem jobs(editor::EditorRootAllocator()); // export tool root
        CookDriver driver(editor::EditorRootAllocator(), project.SourceDb(), project.CookedDb(),
                          builders, &sourcesMount, &cacheMount, &jobs);
        CookPlan plan = driver.Plan(rebuild);
        CookProgress cookProgress;
        cookProgress.onItem = [&onProgress](usize done, usize total, StringView path, bool)
        {
            if (!onProgress)
            {
                return;
            }
            const f32 frac =
                (total > 0) ? 0.05f + (static_cast<f32>(done) / static_cast<f32>(total)) * 0.55f
                            : 0.6f; // the cook takes 0.05..0.60 of the export's progress
            String step(u8"Cooking ");
            step += path;
            onProgress(step.AsView(), frac);
        };
        const CookStats cookStats = driver.Execute(plan, &cookProgress);
        stats.cooked = cookStats.cooked;
        stats.cookFailed = cookStats.failed;
        if (cookStats.failed > 0)
        {
            LOG_ERROR(u8"Export", u8"aborting - the cook has {} failure(s)",
                               cookStats.failed);
            if (outStats != nullptr)
            {
                *outStats = stats;
            }
            return Status{ErrorCode::Internal};
        }

        // --- variant target cooks (web: BC and ASTC), after the host cook and before the pack ---
        if (!variants.IsEmpty())
        {
            const Status vs = CookVariantTargets(project, builders, variants, rebuild, onProgress);
            if (!vs.IsOk())
            {
                LOG_ERROR(u8"Export", u8"aborting - a variant cook failed");
                if (outStats != nullptr)
                {
                    *outStats = stats;
                }
                return Status{ErrorCode::Internal};
            }
        }

        // --- content ---
        const Status s =
            ExportContent(project, outDir, stats, onProgress, sceneStreams, nullptr, nullptr,
                          nullptr, variants, includeShaderSource);
        if (outStats != nullptr)
        {
            *outStats = stats;
        }
        return s;
    }

    namespace
    {
#if PLATFORM_WINDOWS
        // Windows hosts both toolchains, and only one of them needs the developer environment set
        // in this process. cl.exe finds its headers and libraries only through the vcvars
        // variables (INCLUDE, LIB, VCToolsInstallDir); clang and clang-cl locate an MSVC
        // installation themselves (registry and Program Files probing), so demanding vcvars from a
        // clang-built editor would block a setup that works. The pinned compiler is the one that
        // built this editor, so this asks about that exact binary: its basename minus .exe is
        // "cl", which does not match "clang-cl" or "clang++".
        [[nodiscard]] bool ShipCompilerIsMsvcCl()
        {
            StringView path =
                StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER));
            for (usize i = path.Size(); i > 0; --i)
            {
                if (path.Data()[i - 1] == '/' || path.Data()[i - 1] == '\\')
                {
                    path = StringView(path.Data() + i, path.Size() - i);
                    break;
                }
            }
            if (path.Size() >= 4)
            {
                const StringView tail(path.Data() + path.Size() - 4, 4);
                if (tail == u8".exe" || tail == u8".EXE")
                {
                    path = StringView(path.Data(), path.Size() - 4);
                }
            }
            return path.Size() == 2 && (path.Data()[0] == 'c' || path.Data()[0] == 'C') &&
                   (path.Data()[1] == 'l' || path.Data()[1] == 'L');
        }

        // vcvars64.bat belonging to the pinned compiler. BUILDSYSTEM_CXX_COMPILER is
        // <vs>/VC/Tools/MSVC/<version>/bin/Host<arch>/<arch>/cl.exe, so <vs>/VC - which holds
        // Auxiliary/Build - is seven directories up: x64, Host<arch>, bin, <version>, MSVC, Tools,
        // VC. Deriving it from that path rather than searching (vswhere, Program Files) is the
        // point: a machine carrying several Visual Studios has several vcvars, and the plugin has
        // to be built by the one that built this editor.
        [[nodiscard]] String MsvcVcvarsPath()
        {
            StringView dir(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER));
            for (int i = 0; i < 7; ++i)
            {
                dir = PathParent(dir);
            }
            return PathJoin(dir, u8"Auxiliary/Build/vcvars64.bat");
        }

        // Puts the MSVC build environment in this process when it is not already there, so cl.exe
        // can find its headers and libraries. An environment that is already set wins: a Developer
        // Command Prompt, or a CI job that ran vcvars itself, keeps the toolchain it chose.
        //
        // vcvars64.bat only prints and sets; its effect has to be read back out of a shell that ran
        // it, which is what `set` after it does. It goes through a batch file rather than
        // `cmd /c "call ... && set"` because cmd parses its own command line and does not read the
        // backslash-escaped quotes a generic argument quoter produces around a path with spaces -
        // and "C:\Program Files" always has one. The batch file quotes the path itself, once.
        //
        // Success is judged by VCToolsInstallDir appearing, not by an exit code: the code belongs
        // to `set`, which succeeds whether or not vcvars did.
        [[nodiscard]] bool EnsureMsvcEnvironment()
        {
            if (GetEnvironmentVariable(u8"VCToolsInstallDir").HasValue())
            {
                return true;
            }
            const String vcvars = MsvcVcvarsPath();
            if (!FileExists(vcvars.AsView()))
            {
                LOG_ERROR(u8"Export",
                          u8"the MSVC build environment is not set, and the script that would set "
                          u8"it is missing: '{}'. Launch the editor from a Developer Command "
                          u8"Prompt, or repair the Visual Studio install that built this editor",
                          vcvars);
                return false;
            }
            const String script = PathJoin(GetTempDirectory().AsView(), u8"vcvars-dump.cmd");
            const String body = Format(u8"@echo off\r\ncall \"{}\" >nul\r\nset\r\n", vcvars);
            if (!WriteFile(script.AsView(),
                           Span<const byte>(reinterpret_cast<const byte*>(body.AsView().Data()),
                                            body.AsView().Size()))
                     .IsOk())
            {
                LOG_ERROR(u8"Export", u8"could not write the script '{}' that reads the MSVC environment", script);
                return false;
            }
            const Optional<String> comspec = GetEnvironmentVariable(u8"ComSpec");
            const String shell = comspec.HasValue() ? comspec.Value() : String(u8"C:/Windows/System32/cmd.exe");
            const StringView args[] = {u8"/c", script.AsView()};
            const ProcessResult dumped = RunProcess(shell.AsView(), Span<const StringView>(args, 2));
            (void)FileDelete(script.AsView());
            if (!dumped.Ran())
            {
                LOG_ERROR(u8"Export", u8"could not run '{}' to read the MSVC environment back", shell);
                return false;
            }
            // One NAME=VALUE per line; a line without '=' (a stray banner) is skipped.
            const StringView text = dumped.output.AsView();
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
            {
                if (i < text.Size() && text[i] != u8'\n')
                {
                    continue;
                }
                const StringView line = Trim(text.SubStr(start, i - start));
                start = i + 1;
                const usize eq = line.Find(u8'=');
                if (eq == StringView::kNotFound || eq == 0)
                {
                    continue;
                }
                (void)SetEnvironmentVariable(line.SubStr(0, eq),
                                             line.SubStr(eq + 1, line.Size() - eq - 1));
            }
            if (!GetEnvironmentVariable(u8"VCToolsInstallDir").HasValue())
            {
                LOG_ERROR(u8"Export",
                          u8"'{}' ran but did not set VCToolsInstallDir - its output was:\n{}", vcvars,
                          dumped.output);
                return false;
            }
            LOG_INFO(u8"Export", u8"MSVC build environment taken from '{}'", vcvars);
            return true;
        }
#endif // PLATFORM_WINDOWS (both callers are Windows-only; unused elsewhere it fails -Werror)

        // The project's package folders as ENGINE_PROJECT_PLUGINS takes them, ;-separated (empty:
        // none).
        [[nodiscard]] String ProjectPackageList(const EditorProject& project)
        {
            String list;
            for (const String& dir : ProjectPackageDirs(project))
            {
                list += list.IsEmpty() ? u8"" : u8";";
                list += dir.AsView();
            }
            return list;
        }

        // The manifest of the plugin `entry` names: its project package's, else the copy the
        // template's player lists, else the engine checkout's (Code/Plugins/<Name>/Plugin.xml).
        // False when there is none.
        [[nodiscard]] bool FindPluginManifest(const EditorProject& project, const ExportTemplate& tmpl,
                                              const foundation::runtime::EnabledPlugin& entry,
                                              foundation::runtime::PluginManifest& out)
        {
            if (!entry.path.IsEmpty())
            {
                return foundation::runtime::LoadPluginManifest(PathJoin(project.Directory(), entry.path.AsView()).AsView(), out,
                                                               editor::EditorRootAllocator())
                           .IsOk() &&
                       out.id == entry.id;
            }
            for (const PlayerPlugin& linked : ReadPlayerPlugins(tmpl.directory.AsView(), kPlayerBaseName))
            {
                if (linked.id == entry.id &&
                    foundation::runtime::LoadPluginManifest(linked.packageDir.AsView(), out, editor::EditorRootAllocator()).IsOk())
                {
                    return true;
                }
            }
            const String plugins = PathJoin(StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_ENGINE_ROOT)), u8"Code/Plugins");
            bool found = false;
            Array<String> folders;
            (void)ListDirectory(
                plugins.AsView(),
                [](void* ctx, StringView name, bool isDirectory)
                {
                    if (isDirectory)
                    {
                        static_cast<Array<String>*>(ctx)->PushBack(String(name));
                    }
                },
                &folders);
            for (const String& folder : folders)
            {
                if (!found &&
                    foundation::runtime::LoadPluginManifest(PathJoin(plugins.AsView(), folder.AsView()).AsView(), out,
                                                            editor::EditorRootAllocator())
                        .IsOk() &&
                    out.id == entry.id)
                {
                    found = true;
                }
            }
            return found;
        }

    }

    void detail::AbsentPluginTypes(const EditorProject& project, const foundation::runtime::PluginHost& plugins,
                                   StringView platform, Array<TypeId>& hidden, String& names)
    {
        for (const String& id : plugins.Loaded())
        {
            const foundation::runtime::EnabledPlugin* entry = nullptr;
            for (const foundation::runtime::EnabledPlugin& enabled : project.Settings().plugins)
            {
                entry = enabled.id == id ? &enabled : entry;
            }
            const foundation::runtime::PluginManifest* manifest = plugins.Manifest(id.AsView());
            if (entry == nullptr || manifest == nullptr ||
                foundation::runtime::EnablementOn(*entry, *manifest, platform) ==
                    foundation::runtime::PluginEnablement::Enabled)
            {
                continue;
            }
            plugins.TypesOf(id.AsView(), hidden);
            names.Append(names.IsEmpty() ? StringView(u8"'") : StringView(u8", '"));
            names.Append(id.AsView());
            names.Append(u8"'");
        }
    }

    String detail::CheckScriptsOn(EditorProject& project, const foundation::runtime::PluginHost* plugins,
                                  StringView platform)
    {
        if (plugins == nullptr)
        {
            return {};
        }
        Array<TypeId> hidden;
        String absent;
        AbsentPluginTypes(project, *plugins, platform, hidden, absent);
        if (hidden.IsEmpty())
        {
            return {};
        }
        Array<foundation::content::Instance*> instances;
        CollectAllInstances(*project.SourceDb().RootGroup(), instances);
        const StringView scriptType(reinterpret_cast<const utf8char*>(pipeline::ScriptClassAsset::StaticType().name));
        for (foundation::content::Instance* instance : instances)
        {
            if (instance->TypeName() != scriptType)
            {
                continue;
            }
            const RefPtr<ISerializable> object = instance->ReadObject();
            const auto* script = Cast<pipeline::ScriptClassAsset>(object.Get());
            if (script == nullptr)
            {
                continue;
            }
            const StringView language =
                script->language.IsEmpty() ? StringView(u8"angelscript") : script->language.AsView();
            pipeline::IScriptLanguageCook* cook = pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(language);
            Result<Array<byte>> bytes = ReadFile(project.SourceFile(script->fileName.View()).AsView());
            if (cook == nullptr || !bytes.HasValue())
            {
                continue; // the cook reports a script it cannot read or has no cook for
            }
            const StringView source(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
            pipeline::CookScriptErrorSink sink;
            String className;
            pipeline::FolderIncludeReader includes(project.SourcesRoot().AsView(), project.CodeRoot().AsView());
            if (cook->Check(source, script->fileName.View(), sink, className,
                            Span<const TypeId>(hidden.Data(), hidden.Size()), &includes))
            {
                continue;
            }
            String why = Format(u8"script '{}' cannot run on {}, where plugin(s) {} are not enabled", instance->Path(),
                                platform, absent);
            if (!sink.errors.IsEmpty())
            {
                why.Append(Format(u8": line {}: {}", sink.errors[0].line, sink.errors[0].message).AsView());
            }
            return why;
        }
        return {};
    }

    detail::PlayerBuildArguments detail::PlayerBuildArgumentsFor(Span<const String> enabled, Span<const String> definitions,
                                                                 StringView earlier)
    {
        PlayerBuildArguments out;
        // The script languages the project enables here, and no other: a language is built only for a
        // game that runs it (C# would otherwise need .NET's SDK for every native export).
        for (const ProjectSettings::LanguageBecamePlugin& language : ProjectSettings::kLanguagesBecamePlugins)
        {
            bool on = false;
            for (const String& id : enabled)
            {
                on = on || id == language.id;
            }
            out.arguments.PushBack(Format(u8"-D{}={}", language.buildOption, on ? StringView(u8"ON") : StringView(u8"OFF")));
        }
        // The plugins' definitions, each name remembered, so a name given before and not now (C# taken
        // out of the project) is dropped from the tree's cache instead of lingering in the player.
        Array<String> names;
        for (const String& definition : definitions)
        {
            const StringView text = definition.AsView();
            const usize equals = text.Find(u8"=");
            names.PushBack(String(text.SubStr(2, (equals == StringView::kNotFound ? text.Size() : equals) - 2)));
            out.arguments.PushBack(definition);
            out.given.Append(names.Back().AsView());
            out.given.Append(u8"\n");
        }
        usize at = 0;
        while (at < earlier.Size())
        {
            usize end = earlier.Find(u8"\n", at);
            end = end == StringView::kNotFound ? earlier.Size() : end;
            const StringView name = Trim(earlier.SubStr(at, end - at));
            bool given = name.IsEmpty();
            for (const String& now : names)
            {
                given = given || now.AsView() == name;
            }
            if (!given)
            {
                out.arguments.PushBack(Format(u8"-U{}", name));
            }
            at = end + 1;
        }
        return out;
    }

    detail::PlayerChoice detail::ChoosePlayer(EditorProject& project, const ExportTemplate& tmpl, StringView platform,
                                              const ExportPluginServices* hooks)
    {
        PlayerChoice choice;
        for (const foundation::runtime::EnabledPlugin& entry : project.Settings().plugins)
        {
            foundation::runtime::PluginManifest manifest;
            if (!FindPluginManifest(project, tmpl, entry, manifest))
            {
                choice.refusal = Format(u8"plugin '{}' is in no package the export can find (the project's, the "
                                        u8"template's or the engine's)",
                                        entry.id);
                return choice;
            }
            const foundation::runtime::PluginEnablement enablement = foundation::runtime::EnablementOn(entry, manifest, platform);
            if (enablement == foundation::runtime::PluginEnablement::NotEnabled)
            {
                continue;
            }
            if (enablement == foundation::runtime::PluginEnablement::Unsupported)
            {
                choice.refusal = Format(u8"plugin '{}' is enabled on {}, which it does not support", entry.id, platform);
                return choice;
            }
            choice.enabled.PushBack(entry.id);
            if (choice.build)
            {
                continue;
            }
            if (!entry.path.IsEmpty())
            {
                choice.build = true;
                choice.reason = Format(u8"the project's own package '{}' is enabled on {}", entry.id, platform);
                continue;
            }
            const TemplatePlugin* carried = nullptr;
            for (const TemplatePlugin& plugin : tmpl.plugins)
            {
                carried = plugin.id == entry.id ? &plugin : carried;
            }
            const Optional<foundation::runtime::PluginVersion> have =
                carried != nullptr ? foundation::runtime::ParsePluginVersion(carried->version.AsView()) : Optional<foundation::runtime::PluginVersion>{};
            const Optional<foundation::runtime::PluginVersion> want = foundation::runtime::ParsePluginVersion(manifest.version.AsView());
            if (carried == nullptr)
            {
                choice.build = true;
                choice.reason = Format(u8"template '{}' does not carry plugin '{}'", tmpl.id, entry.id);
            }
            else if (!have || !want || !foundation::runtime::VersionAtLeast(*have, *want))
            {
                choice.build = true;
                choice.reason = Format(u8"template '{}' carries plugin '{}' {}, older than {}", tmpl.id, entry.id,
                                       carried->version, manifest.version);
            }
        }
        // A script runs only where its language's plugin is enabled: one in a language the project
        // does not enable on this platform (C# on Android, which the csharp manifest leaves out) could
        // not run in the game the export makes.
        {
            Array<foundation::content::Instance*> instances;
            CollectAllInstances(*project.SourceDb().RootGroup(), instances);
            const StringView scriptType(reinterpret_cast<const utf8char*>(pipeline::ScriptClassAsset::StaticType().name));
            for (foundation::content::Instance* instance : instances)
            {
                if (instance->TypeName() != scriptType)
                {
                    continue;
                }
                const RefPtr<ISerializable> object = instance->ReadObject();
                const auto* script = Cast<pipeline::ScriptClassAsset>(object.Get());
                const StringView language =
                    script == nullptr || script->language.IsEmpty() ? StringView(u8"angelscript") : script->language.AsView();
                bool isPlugin = false;
                for (const ProjectSettings::LanguageBecamePlugin& official : ProjectSettings::kLanguagesBecamePlugins)
                {
                    isPlugin = isPlugin || official.id == language;
                }
                bool enabledHere = false;
                for (const String& id : choice.enabled)
                {
                    enabledHere = enabledHere || id == language;
                }
                if (isPlugin && !enabledHere)
                {
                    choice.refusal = Format(u8"script '{}' is written in {}, and the project does not enable plugin '{}' on {}",
                                            instance->Name(), language, language, platform);
                    return choice;
                }
            }
        }
        // What the loaded plugins' player hooks say for the plugins enabled here (a language compiled
        // into the player, C# for one), which a template's player cannot be.
        if (hooks != nullptr)
        {
            PlayerNeed need = hooks->AskPlayer(PlayerHookContext{&project, platform,
                                                                 Span<const String>(choice.enabled.Data(), choice.enabled.Size())});
            if (!need.refusal.IsEmpty())
            {
                choice.refusal = Move(need.refusal);
                return choice;
            }
            if (need.build && !choice.build)
            {
                choice.build = true;
                choice.reason = Move(need.reason);
            }
            choice.definitions = Move(need.definitions);
        }
        return choice;
    }

    namespace
    {
        // Stages the deploy files of the plugins `enabled` names for `platform`, beside the player in
        // `outDir`, from the package folders the player at `playerDir` lists. False (logged) when a
        // deploy file a manifest lists is missing.
        [[nodiscard]] bool StageDeployFiles(StringView playerDir, StringView playerBase, Span<const String> enabled,
                                            StringView platform, StringView outDir, usize& staged)
        {
            for (const PlayerPlugin& linked : ReadPlayerPlugins(playerDir, playerBase))
            {
                bool wanted = false;
                for (const String& id : enabled)
                {
                    wanted = wanted || id == linked.id;
                }
                foundation::runtime::PluginManifest manifest;
                if (!wanted ||
                    !foundation::runtime::LoadPluginManifest(linked.packageDir.AsView(), manifest, editor::EditorRootAllocator()).IsOk())
                {
                    continue;
                }
                const foundation::runtime::PluginPlatform* target = manifest.FindPlatform(platform);
                if (target == nullptr)
                {
                    continue;
                }
                for (const String& file : target->deploy)
                {
                    if (!FileCopyPreserving(PathJoin(linked.packageDir.AsView(), file.AsView()).AsView(),
                                            PathJoin(outDir, PathFilename(file.AsView())).AsView()))
                    {
                        LOG_ERROR(u8"Export", u8"plugin '{}': its deploy file '{}' for {} is missing", linked.id, file, platform);
                        return false;
                    }
                    ++staged;
                }
            }
            return true;
        }

        // Builds Engine.GamePlayer for a project with packages of its own: re-invokes cmake over the
        // engine checkout with the project's packages added (ENGINE_PROJECT_PLUGINS), a
        // persistent per-project build directory under the project's .cache (for incremental
        // relinks), and a per-project Bin suffix so ship outputs never collide with development
        // builds. Returns the built player's path. `web` builds the wasm ship player by the same
        // mechanism through the Emscripten toolchain ($EMSDK must be in the environment, so launch
        // the editor or CLI from a shell with emsdk_env.sh sourced), with a lower -j (wasm-ld with
        // ASYNCIFY uses a lot of memory); the output is the html, js and wasm trio under the
        // template basename.
        [[nodiscard]] Status BuildShipPlayer(EditorProject& project, StringView config, bool web, Span<const String> enabled,
                                             Span<const String> definitions, const ExportProgress& onProgress,
                                             String& outPlayer)
        {
            const StringView engineRoot =
                StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_ENGINE_ROOT));
            const StringView cmakePath =
                StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CMAKE_PATH));
            String toolchainDef;
            if (web)
            {
                const Optional<String> emsdk = GetEnvironmentVariable(u8"EMSDK");
                if (!emsdk.HasValue() || emsdk.Value().IsEmpty())
                {
                    LOG_ERROR(u8"Export",
                              u8"web native export needs the Emscripten SDK: EMSDK is not set - "
                              u8"launch the editor/CLI from a shell with emsdk_env.sh sourced");
                    return Status{ErrorCode::NotFound};
                }
                toolchainDef = Format(
                    u8"-DCMAKE_TOOLCHAIN_FILE={}/upstream/emscripten/cmake/Modules/Platform/"
                    u8"Emscripten.cmake",
                    emsdk.Value());
            }
#if PLATFORM_WINDOWS
            // cl.exe reads its headers and libraries out of the environment, so the desktop ship
            // build needs vcvars in this process. It is a menu action, and an editor is started
            // from a shortcut rather than a shell, so EnsureMsvcEnvironment sets it from the
            // compiler that built this editor instead of asking for a Developer Command Prompt. A
            // clang or clang-cl editor skips this, since clang finds MSVC itself.
            if (!web && ShipCompilerIsMsvcCl() && !EnsureMsvcEnvironment())
            {
                return Status{ErrorCode::NotFound}; // EnsureMsvcEnvironment says why
            }
#endif
            if (!DirectoryExists(PathJoin(engineRoot, u8"Code").AsView()))
            {
                LOG_ERROR(u8"Export",
                          u8"native ship link needs the engine source checkout at '{}' - not "
                          u8"found (relocated editor?)",
                          engineRoot);
                return Status{ErrorCode::NotFound};
            }
            // The project's packages, or none: a player of the engine's plugins alone, for a project
            // whose plugins the template does not carry.
            const String packages = ProjectPackageList(project);
            const String suffix = detail::ShipOutputSuffix(project.Settings().name.AsView());
            const String buildType = config.IsEmpty() ? String(u8"Release") : String(config);
            const String shipDir = PathJoin(
                project.Directory(),
                Format(u8"{}/ship-{}{}", engine::project::kProjectCacheDir,
                       web ? StringView(u8"web-") : StringView(u8""), buildType)
                    .AsView());

            if (onProgress)
            {
                onProgress(u8"Configuring native ship build...", 0.86f);
            }
            // Pin the ship toolchain to the one that built this editor (the system default may be
            // a different compiler; the checkout's build conventions stay authoritative).
            const String defCompiler =
                Format(u8"-DCMAKE_CXX_COMPILER={}",
                       StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER)));
            const String defBuildType = Format(u8"-DCMAKE_BUILD_TYPE={}", buildType);
            const String defPackages = packages.IsEmpty() ? String(u8"-DENGINE_GAME_PLAYER=ON")
                                                          : Format(u8"-DENGINE_PROJECT_PLUGINS={}", packages);
            const String defSuffix = Format(u8"-DBUILDSYSTEM_OUTPUT_SUFFIX={}", suffix);
            // Web: the toolchain file selects the compiler (never pin clang++ under it).
            const StringView compilerOrToolchain =
                web ? toolchainDef.AsView() : defCompiler.AsView();
            const StringView fixedArgs[] = {u8"-S", engineRoot, u8"-B", shipDir.AsView(), u8"-G", u8"Ninja", compilerOrToolchain,
                                            defBuildType.AsView(), defPackages.AsView(), defSuffix.AsView()};
            Array<StringView> cfgArgs;
            for (const StringView arg : fixedArgs)
            {
                cfgArgs.PushBack(arg);
            }
            // The languages the project enables here and the plugins' player definitions, and what an
            // earlier configure of this tree was given that this one is not, dropped from its cache.
            const String givenFile = PathJoin(shipDir.AsView(), u8"plugin-definitions.txt");
            String earlier;
            if (Result<Array<byte>> bytes = ReadFile(givenFile.AsView()); bytes.HasValue())
            {
                earlier = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            }
            const detail::PlayerBuildArguments playerArgs = detail::PlayerBuildArgumentsFor(enabled, definitions, earlier.AsView());
            for (const String& arg : playerArgs.arguments)
            {
                cfgArgs.PushBack(arg.AsView());
            }
            const ProcessResult configured =
                RunProcess(cmakePath, Span<const StringView>(cfgArgs.Data(), cfgArgs.Size()));
            if (!configured.Ok())
            {
                LOG_ERROR(u8"Export", u8"native ship configure failed ({}):\n{}",
                          configured.exitCode, configured.output);
                return Status{ErrorCode::Internal};
            }
            (void)WriteFile(givenFile.AsView(),
                            Span<const byte>(reinterpret_cast<const byte*>(playerArgs.given.Data()), playerArgs.given.Size()));

            if (onProgress)
            {
                onProgress(u8"Building native ship player (this can take a while)...", 0.88f);
            }
            const StringView buildArgs[] = {u8"--build", shipDir.AsView(), u8"--target",
                                            u8"Engine.GamePlayer", u8"-j",
                                            web ? StringView(u8"2") : StringView(u8"4")};
            const ProcessResult built =
                RunProcess(cmakePath, Span<const StringView>(buildArgs, 6));
            if (!built.Ok())
            {
                LOG_ERROR(u8"Export", u8"native ship build failed ({}) - tail:\n{}",
                          built.exitCode, built.output);
                return Status{ErrorCode::Internal};
            }

            // The player lands under the checkout's suffixed Bin; the platform-compiler tag is the
            // build's own concern, so search for it rather than rebuild the name.
            const String binRoot = Format(u8"{}/Bin/{}", engineRoot, buildType);
            vfs::NativeFileSystem bin(binRoot.AsView(), editor::EditorRootAllocator());
            if (vfs::IEnumerableFileSystem* enumerable = bin.AsEnumerable())
            {
                Array<vfs::DirEntry> entries;
                if (enumerable->Enumerate(u8"", entries).IsOk())
                {
                    for (const vfs::DirEntry& entry : entries)
                    {
                        if (entry.isDirectory && entry.name.AsView().EndsWith(suffix.AsView()))
                        {
                            // Web: the page of the html, js and wasm trio; desktop: the executable.
                            const String candidate =
                                Format(u8"{}/{}/{}", binRoot, entry.name,
                                       web ? StringView(u8"Engine.GamePlayer.html")
                                           : StringView(u8"Engine.GamePlayer"));
                            if (FileExists(candidate.AsView()))
                            {
                                outPlayer = candidate;
                                return Status{};
                            }
                        }
                    }
                }
            }
            LOG_ERROR(u8"Export", u8"native ship build succeeded but Engine.GamePlayer was not "
                                  u8"found under '{}/*{}'",
                      binRoot, suffix);
            return Status{ErrorCode::NotFound};
        }
    }

    Status BuildProjectPlugins(EditorProject& project)
    {
        const StringView engineRoot =
            StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_ENGINE_ROOT));
        const StringView cmakePath =
            StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CMAKE_PATH));
        const String packages = ProjectPackageList(project);
        if (packages.IsEmpty())
        {
            return Status{ErrorCode::NotFound};
        }
        if (!DirectoryExists(PathJoin(engineRoot, u8"Code").AsView()))
        {
            LOG_ERROR(u8"Export", u8"the project plugins' build needs the engine checkout at '{}'",
                      engineRoot);
            return Status{ErrorCode::NotFound};
        }
#if PLATFORM_WINDOWS
        // The same contract as the ship build, with the same narrow gate: only cl.exe needs vcvars
        // set in this process; clang and clang-cl find MSVC on their own.
        if (ShipCompilerIsMsvcCl() && !EnsureMsvcEnvironment())
        {
            return Status{ErrorCode::NotFound}; // EnsureMsvcEnvironment says why
        }
#endif
        const String suffix =
            Format(u8"-Dev-{}", detail::ShipOutputSuffix(project.Settings().name.AsView())
                                    .AsView()
                                    .Data() + 6); // reuse the sanitizer, swap the prefix
        const String devDir = PathJoin(project.Directory(), engine::project::kProjectPluginsBuildDir);

        const String defCompiler =
            Format(u8"-DCMAKE_CXX_COMPILER={}",
                   StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER)));
        const String defPackages = Format(u8"-DENGINE_PROJECT_PLUGINS={}", packages);
        const String defSuffix = Format(u8"-DBUILDSYSTEM_OUTPUT_SUFFIX={}", suffix);
        // The editor's own configuration, not a fixed one. A plugin is loaded into this process, so
        // a mismatch puts two C runtimes in it (a Debug plugin brings ucrtbased and VCRUNTIME140D
        // alongside the editor's ucrtbase and VCRUNTIME140) and the plugin's inline code, built
        // against different library internals, runs against the engine DLLs already loaded. It
        // links and loads; it fails later, where memory allocated on one side is freed on the
        // other. The compiler is pinned to this editor's for the same reason, two lines above.
        const String defBuild = Format(u8"-DCMAKE_BUILD_TYPE={}", GetBuildConfigName());
        const StringView cfgArgs[] = {u8"-S",
                                      engineRoot,
                                      u8"-B",
                                      devDir.AsView(),
                                      u8"-G",
                                      u8"Ninja",
                                      defCompiler.AsView(),
                                      defBuild.AsView(),
                                      u8"-DENGINE_SHARED_LIBS=ON",
                                      defPackages.AsView(),
                                      defSuffix.AsView()};
        // Logged at each step, because this runs for minutes: it configures a build of the engine
        // beside the plugins, so the first one compiles the whole tree. Without these lines the
        // only sign of it is a notice at the end, and a caller driving the editor over MCP (or
        // reading the log after the fact) cannot tell a running build from a finished one.
        LOG_INFO(u8"Export", u8"project plugins: configuring {} in '{}'", GetBuildConfigName(), devDir);
        const ProcessResult configured =
            RunProcess(cmakePath, Span<const StringView>(cfgArgs, 11));
        if (!configured.Ok())
        {
            LOG_ERROR(u8"Export", u8"the project plugins' configure failed ({}):\n{}",
                      configured.exitCode, configured.output);
            return Status{ErrorCode::Internal};
        }
        LOG_INFO(u8"Export", u8"project plugins: building (the first build compiles the engine too)");
        const StringView buildArgs[] = {u8"--build", devDir.AsView(), u8"--target",
                                        u8"ProjectPlugins", u8"-j", u8"4"};
        const ProcessResult built = RunProcess(cmakePath, Span<const StringView>(buildArgs, 6));
        if (!built.Ok())
        {
            LOG_ERROR(u8"Export", u8"the project plugins' build failed ({}) - tail:\n{}",
                      built.exitCode, built.output);
            return Status{ErrorCode::Internal};
        }
        // The configure writes this, so it is there from the configure on and says nothing about
        // whether the libraries it names were built. Check the libraries themselves.
        const String listed = PathJoin(devDir.AsView(), engine::project::kPluginLibrariesFile);
        if (!FileExists(listed.AsView()))
        {
            LOG_ERROR(u8"Export", u8"the project plugins' build left no '{}'", listed);
            return Status{ErrorCode::NotFound};
        }
        for (const engine::project::BuiltPluginPart& part : engine::project::ReadBuiltPluginParts(
                 project.Directory(), EditorRootAllocator()))
        {
            if (!FileExists(part.library.AsView()))
            {
                LOG_ERROR(u8"Export", u8"the project plugins' build listed '{}' but did not build it",
                          part.library);
                return Status{ErrorCode::NotFound};
            }
        }
        LOG_INFO(u8"Export", u8"project plugins: built - run Project > Reload Project Plugins to load them");
        return Status{};
    }

    namespace
    {
        // The Android export: the template carries the player as the library the app loads and the
        // app's Gradle project (android/, SDL's Java glue in android/sdl-java/); the export copies
        // the project into the project's cache, stages what the app carries beside it (the player as
        // libmain.so, the content and the engine's data as assets, the icon as a resource), runs the
        // project's Gradle wrapper with the preset's values and copies the APK or bundle out.
        constexpr StringView kAndroidTemplateProject = u8"android/";

        // Copies every file under `from` to `to`, folders made as needed, leaving out the top-level
        // folder `skip` (empty: none). False when a file does not copy.
        [[nodiscard]] bool CopyTree(StringView from, StringView to, StringView skip = {})
        {
            vfs::NativeFileSystem fs(from, editor::EditorRootAllocator());
            Array<vfs::DirEntry> entries;
            if (!fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
            {
                return false;
            }
            if (!CreateDirectories(to))
            {
                return false;
            }
            for (const vfs::DirEntry& entry : entries)
            {
                if (!skip.IsEmpty() && entry.name.AsView() == skip)
                {
                    continue;
                }
                const String source = PathJoin(from, entry.name.AsView());
                const String target = PathJoin(to, entry.name.AsView());
                if (entry.isDirectory ? !CopyTree(source.AsView(), target.AsView())
                                      : !FileCopyPreserving(source.AsView(), target.AsView()))
                {
                    return false;
                }
            }
            return true;
        }

        // android:screenOrientation for how the game is held: the preset's own, else the project's.
        [[nodiscard]] StringView AndroidScreenOrientation(const EditorProject& project, const ExportPreset& preset)
        {
            const ScreenOrientation held =
                preset.overridesOrientation ? preset.orientation : project.Settings().orientation;
            switch (held)
            {
            case ScreenOrientation::Landscape:
                return u8"userLandscape";
            case ScreenOrientation::Portrait:
                return u8"userPortrait";
            case ScreenOrientation::Any:
                break;
            }
            return u8"fullUser";
        }

        // The file name the app's APK or bundle takes in the dist: the preset's player name, else
        // the project's, without what a desktop player's name carries.
        [[nodiscard]] String AndroidAppName(const EditorProject& project, const ExportPreset& preset)
        {
            return detail::SanitizeName(preset.playerName.IsEmpty() ? project.Settings().name.AsView()
                                                                    : preset.playerName.AsView());
        }
    }

    String AndroidWorkDir(const EditorProject& project, const ExportPreset& preset)
    {
        // Absolute: Gradle reads a relative path against its own project folder, not this process's.
        const String work = PathJoin(project.Directory(),
                                     Format(u8"{}/android/{}", engine::project::kProjectCacheDir,
                                            detail::SanitizeName(preset.name.AsView()))
                                         .AsView());
        return PathIsAbsolute(work.AsView()) ? work : PathJoin(GetCurrentDirectory().AsView(), work.AsView());
    }

    String CheckAndroidExport(const EditorProject& project, const ExportPreset& preset, const ExportTemplate* tmpl)
    {
        if (String why = CheckAndroidApplicationId(preset.androidApplicationId.AsView()); !why.IsEmpty())
        {
            return why;
        }
        if (!preset.androidIcon.IsEmpty())
        {
            if (!preset.androidIcon.AsView().EndsWith(u8".png"))
            {
                return Format(u8"the app icon '{}' is not a PNG", preset.androidIcon);
            }
            if (!FileExists(PathJoin(project.Directory(), preset.androidIcon.AsView()).AsView()))
            {
                return Format(u8"the app icon '{}' is not in the project", preset.androidIcon);
            }
        }
        if (!preset.androidKeystore.IsEmpty())
        {
            if (!FileExists(PathJoin(project.Directory(), preset.androidKeystore.AsView()).AsView()))
            {
                return Format(u8"the keystore '{}' is not there", preset.androidKeystore);
            }
            if (preset.androidKeyAlias.IsEmpty())
            {
                return String(u8"a keystore needs the alias of the key that signs");
            }
            const Optional<String> password = GetEnvironmentVariable(kAndroidStorePasswordVariable);
            if (!password.HasValue() || password->IsEmpty())
            {
                return Format(u8"the keystore's password is not given: set {} (and {} when the key's own differs)",
                              kAndroidStorePasswordVariable, kAndroidKeyPasswordVariable);
            }
        }
        const Optional<String> sdk = GetEnvironmentVariable(u8"ANDROID_HOME");
        if (!sdk.HasValue() || sdk->IsEmpty() || !DirectoryExists(sdk->AsView()))
        {
            return String(u8"an Android export runs Gradle, which needs the Android SDK: set ANDROID_HOME to it (in the editor, "
                          u8"Preferences > Export > Android SDK)");
        }
        if (tmpl != nullptr)
        {
            bool hasProject = false;
            for (const String& sidecar : tmpl->sidecars)
            {
                hasProject = hasProject || sidecar.AsView().StartsWith(kAndroidTemplateProject);
            }
            if (!hasProject)
            {
                return Format(u8"template '{}' carries no Android app project", tmpl->id);
            }
        }
        return {};
    }

    Status StageAndroidApp(const EditorProject& project, const ExportPreset& preset, const ExportTemplate& tmpl,
                           StringView contentDir, StringView dataRoot, StringView workDir)
    {
        const String gradleProject = PathJoin(workDir, u8"project");
        const String staged = PathJoin(workDir, u8"staged");
        const String assets = PathJoin(staged.AsView(), u8"assets");
        // What the last export staged goes; the project and Gradle's outputs stay, for its next build.
        const StringView stagedFolders[] = {u8"jniLibs", u8"assets", u8"res"};
        for (const StringView folder : stagedFolders)
        {
            const String old = PathJoin(staged.AsView(), folder);
            if (DirectoryExists(old.AsView()) && !RemoveDirectoryRecursive(old.AsView()))
            {
                return Status{ErrorCode::Internal};
            }
        }

        // The app's Gradle project, from the template.
        for (const String& sidecar : tmpl.sidecars)
        {
            const StringView file = sidecar.AsView();
            if (!file.StartsWith(kAndroidTemplateProject))
            {
                continue;
            }
            const String target = PathJoin(gradleProject.AsView(),
                                           file.SubStr(kAndroidTemplateProject.Size(), file.Size() - kAndroidTemplateProject.Size()));
            if (!CreateDirectories(PathParent(target.AsView())) ||
                !FileCopyPreserving(PathJoin(tmpl.directory.AsView(), file).AsView(), target.AsView()))
            {
                LOG_ERROR(u8"Export", u8"the Android project's '{}' did not copy from template '{}'", file, tmpl.id);
                return Status{ErrorCode::NotFound};
            }
        }

        // The player, as the library the app's activity loads.
        const String libraries = PathJoin(staged.AsView(), u8"jniLibs/arm64-v8a");
        if (!CreateDirectories(libraries.AsView()) ||
            !FileCopyPreserving(PathJoin(tmpl.directory.AsView(), tmpl.playerBinary.AsView()).AsView(),
                                PathJoin(libraries.AsView(), u8"libmain.so").AsView()))
        {
            return Status{ErrorCode::NotFound};
        }

        // The game (its packs and player.xml) in assets/Game; the shader pack the content export
        // staged and the engine's other data in assets/Data, where the player reads them in place.
        const String game = PathJoin(assets.AsView(), u8"Game");
        if (!CopyTree(contentDir, game.AsView(), u8"Data") ||
            !CopyTree(PathJoin(contentDir, u8"Data").AsView(), PathJoin(assets.AsView(), u8"Data").AsView()) ||
            !CopyTree(PathJoin(dataRoot, u8"Engine").AsView(), PathJoin(assets.AsView(), u8"Data/Engine").AsView(),
                      u8"Shaders"))
        {
            LOG_ERROR(u8"Export", u8"the app's assets did not stage under '{}'", assets);
            return Status{ErrorCode::Internal};
        }
        (void)FileDelete(PathJoin(game.AsView(), u8"export-report.txt").AsView()); // the report stays beside the dist
        for (const String& sidecar : tmpl.sidecars)
        {
            if (!sidecar.AsView().StartsWith(kAndroidTemplateProject) &&
                !FileCopyPreserving(PathJoin(tmpl.directory.AsView(), sidecar.AsView()).AsView(),
                                    PathJoin(assets.AsView(), sidecar.AsView()).AsView()))
            {
                return Status{ErrorCode::NotFound}; // the notices, which every app carries
            }
        }

        // The launcher icon.
        if (!preset.androidIcon.IsEmpty())
        {
            const String icons = PathJoin(staged.AsView(), u8"res/mipmap-xxxhdpi");
            if (!CreateDirectories(icons.AsView()) ||
                !FileCopyPreserving(PathJoin(project.Directory(), preset.androidIcon.AsView()).AsView(),
                                    PathJoin(icons.AsView(), u8"ic_launcher.png").AsView()))
            {
                return Status{ErrorCode::NotFound};
            }
        }
        return Status{};
    }

    Array<String> AndroidGradleArguments(const EditorProject& project, const ExportPreset& preset,
                                         const ExportTemplate& tmpl, StringView workDir)
    {
        const bool debug = tmpl.config.AsView() == u8"Debug";
        Array<String> args;
        args.PushBack(String(u8"-p"));
        args.PushBack(PathJoin(workDir, u8"project"));
        args.PushBack(String(u8"--no-daemon")); // an export leaves no Gradle running
        args.PushBack(String(u8"--quiet"));
        args.PushBack(Format(u8"-PengineStaged={}", PathJoin(workDir, u8"staged")));
        args.PushBack(Format(u8"-PengineApplicationId={}", preset.androidApplicationId));
        args.PushBack(Format(u8"-PengineVersionCode={}", preset.androidVersionCode));
        args.PushBack(Format(u8"-PengineVersionName={}", preset.androidVersionName));
        args.PushBack(Format(u8"-PengineLabel={}", preset.androidLabel.IsEmpty() ? project.Settings().name.AsView()
                                                                                 : preset.androidLabel.AsView()));
        args.PushBack(Format(u8"-PengineOrientation={}", AndroidScreenOrientation(project, preset)));
        if (!preset.androidKeystore.IsEmpty())
        {
            // Absolute, as the work folder is: Gradle reads a relative path against its project.
            const String keystore = PathJoin(project.Directory(), preset.androidKeystore.AsView());
            args.PushBack(Format(u8"-PengineKeystore={}", PathIsAbsolute(keystore.AsView())
                                                              ? keystore
                                                              : PathJoin(GetCurrentDirectory().AsView(), keystore.AsView())));
            args.PushBack(Format(u8"-PengineKeyAlias={}", preset.androidKeyAlias));
        }
        const StringView variant = debug ? StringView(u8"Debug") : StringView(u8"Release");
        if (preset.androidPackage != AndroidPackage::Bundle)
        {
            args.PushBack(Format(u8"assemble{}", variant));
        }
        if (preset.androidPackage != AndroidPackage::Apk)
        {
            args.PushBack(Format(u8"bundle{}", variant));
        }
        return args;
    }

    Status BuildAndroidApp(const EditorProject& project, const ExportPreset& preset, const ExportTemplate& tmpl,
                           StringView workDir, StringView outputDir, ExportResult& result)
    {
        const Array<String> args = AndroidGradleArguments(project, preset, tmpl, workDir);
        Array<StringView> views;
        for (const String& arg : args)
        {
            views.PushBack(arg.AsView());
        }
#if PLATFORM_WINDOWS
        const String wrapper = PathJoin(workDir, u8"project/gradlew.bat");
#else
        const String wrapper = PathJoin(workDir, u8"project/gradlew");
#endif
        const ProcessResult built = RunProcess(wrapper.AsView(), Span<const StringView>(views.Data(), views.Size()));
        if (!built.Ok())
        {
            LOG_ERROR(u8"Export", u8"the Android app's Gradle build failed ({}); it needs a JDK 17 (JAVA_HOME, java "
                                  u8"on the PATH, or in the editor Preferences > Export > JDK) and, the first time, the "
                                  u8"network:\n{}",
                      built.exitCode, built.output);
            result.refusal = String(u8"the Android app's Gradle build failed (the log has its output)");
            return Status{ErrorCode::Internal};
        }

        // Gradle's outputs, under the staged folder (the project's build.gradle puts them there).
        const bool debug = tmpl.config.AsView() == u8"Debug";
        const StringView variant = debug ? StringView(u8"debug") : StringView(u8"release");
        const String outputs = PathJoin(workDir, u8"staged/gradle/app/outputs");
        const String name = AndroidAppName(project, preset);
        if (preset.androidPackage != AndroidPackage::Bundle)
        {
            const String apk = Format(u8"{}/apk/{}/app-{}.apk", outputs, variant, variant);
            if (!FileCopyPreserving(apk.AsView(), PathJoin(outputDir, Format(u8"{}.apk", name).AsView()).AsView()))
            {
                LOG_ERROR(u8"Export", u8"Gradle made no APK at '{}'", apk);
                return Status{ErrorCode::NotFound};
            }
            ++result.filesStaged;
        }
        if (preset.androidPackage != AndroidPackage::Apk)
        {
            const String bundle = Format(u8"{}/bundle/{}/app-{}.aab", outputs, variant, variant);
            if (!FileCopyPreserving(bundle.AsView(), PathJoin(outputDir, Format(u8"{}.aab", name).AsView()).AsView()))
            {
                LOG_ERROR(u8"Export", u8"Gradle made no bundle at '{}'", bundle);
                return Status{ErrorCode::NotFound};
            }
            ++result.filesStaged;
        }
        return Status{};
    }

    String PresetQualityRefusal(const engine::project::ProjectSettings& settings, const ExportPreset& preset)
    {
        if (preset.qualityLevelId.IsNil() || settings.IsQualityLevel(preset.qualityLevelId))
        {
            return String();
        }
        return Format(u8"preset '{}' starts at a quality level the project does not list ({}): add it to "
                      u8"the project's quality levels, or pick one of them",
                      preset.name, preset.qualityLevelId);
    }

    Status ApplyPresetOverrides(StringView outDir, const ExportPreset& preset)
    {
        if (!preset.overridesRender && !preset.overridesWindow && !preset.overridesOrientation &&
            preset.qualityLevelId.IsNil())
        {
            return Status{};
        }
        foundation::vfs::NativeFileSystem mount(outDir, editor::EditorRootAllocator());
        engine::project::ProjectSettings dist;
        if (!engine::project::LoadProjectSettings(mount, dist, engine::project::kDistManifestFile).IsOk())
        {
            LOG_ERROR(u8"Export", u8"the dist manifest did not read back for the preset's overrides");
            return Status{ErrorCode::Internal};
        }
        if (preset.overridesRender)
        {
            dist.renderWidth = preset.renderWidth;
            dist.renderHeight = preset.renderHeight;
            dist.renderFit = preset.renderFit;
        }
        if (preset.overridesWindow)
        {
            dist.windowWidth = preset.windowWidth;
            dist.windowHeight = preset.windowHeight;
            dist.windowMode = preset.windowMode;
            dist.windowResizable = preset.windowResizable;
            dist.presentMode = preset.presentMode;
        }
        if (preset.overridesOrientation)
        {
            dist.orientation = preset.orientation;
        }
        if (!preset.qualityLevelId.IsNil())
        {
            if (const String refusal = PresetQualityRefusal(dist, preset); !refusal.IsEmpty())
            {
                LOG_ERROR(u8"Export", u8"{}", refusal);
                return Status{ErrorCode::InvalidArgument};
            }
            dist.defaultQualityId = preset.qualityLevelId;
        }
        if (!engine::project::SaveProjectSettings(*mount.AsWritable(), dist, engine::project::kDistManifestFile)
                 .IsOk())
        {
            LOG_ERROR(u8"Export", u8"failed to write the preset's overrides into the dist manifest");
            return Status{ErrorCode::Internal};
        }
        return Status{};
    }

    Status ExportOne(EditorProject& project, const ExportPreset& preset,
                     const TemplateRegistry& templates, BuilderRegistry& builders,
                     StringView outRoot, StringView dataRoot, bool rebuild, ExportResult* outResult,
                     const ExportProgress& onProgress, bool cook,
                     const HashMap<Guid, Array<byte>>* sceneStreams,
                     const SceneReferenceScanner* scanner,
                     const Array<Guid>* precomputedReachableRoots, ExportPlugins plugins, bool contentOnly)
    {
        const ExportTemplate* tmpl = contentOnly ? nullptr : templates.Resolve(preset);
        if (tmpl == nullptr && !contentOnly)
        {
            LOG_ERROR(u8"Export",
                               u8"no export template for preset '{}' (platform '{}') - import one",
                               preset.name, preset.platform);
            return Status{ErrorCode::NotFound};
        }

        ExportResult result;

        // A preset starts at one of the project's quality levels, or the export would ship a level
        // the game's options menu does not know: refused before any cook.
        if (String refusal = PresetQualityRefusal(project.Settings(), preset); !refusal.IsEmpty())
        {
            LOG_ERROR(u8"Export", u8"{}", refusal);
            result.refusal = Move(refusal);
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::InvalidArgument};
        }

        // The player this dist runs, and whether the project's plugins allow the export at all. The
        // preset says which kind of export it is: a template export never builds a player, so a
        // project that needs its own is refused and told so; a native export always builds one.
        detail::PlayerChoice choice;
        if (tmpl != nullptr)
        {
            choice = detail::ChoosePlayer(project, *tmpl, preset.platform.AsView(), plugins.hooks);
        }
        if (choice.refusal.IsEmpty() && preset.kind == ExportKind::Template && choice.build)
        {
            choice.refusal = Format(u8"{}, so the game needs its own player: make '{}' a native export", choice.reason, preset.name);
        }
        if (choice.refusal.IsEmpty() && preset.kind == ExportKind::Native && !contentOnly)
        {
            if (!choice.build)
            {
                choice.reason = String(u8"the preset is a native export");
            }
            choice.build = true;
        }
        if (!choice.refusal.IsEmpty())
        {
            LOG_ERROR(u8"Export", u8"preset '{}': {}", preset.name, choice.refusal);
            result.refusal = choice.refusal;
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::InvalidArgument};
        }
        // The project's scripts against what the platform has of its plugins, before any cook.
        if (const String scripts = detail::CheckScriptsOn(project, plugins.host, preset.platform.AsView());
            !scripts.IsEmpty())
        {
            LOG_ERROR(u8"Export", u8"preset '{}': {}", preset.name, scripts);
            result.refusal = scripts;
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::InvalidArgument};
        }
        // An Android app: what its preset gives the app and what its build needs, before any cook.
        const bool android = IsAndroidPlatform(preset.platform.AsView()) && !contentOnly;
        if (android)
        {
            String why = CheckAndroidExport(project, preset, tmpl);
            if (why.IsEmpty() && choice.build)
            {
                // TODO(export): build the project's player for Android (the NDK toolchain over the
                // ship build) as the desktop and the web build theirs.
                why = String(u8"a game with its own native packages does not export to Android yet: the "
                             u8"project's player is not built for it");
            }
            if (!why.IsEmpty())
            {
                LOG_ERROR(u8"Export", u8"preset '{}': {}", preset.name, why);
                result.refusal = why;
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return Status{ErrorCode::InvalidArgument};
            }
        }
        if (tmpl != nullptr)
        {
            result.playerReason = choice.build ? choice.reason : Format(u8"template '{}' carries every plugin enabled on {}", tmpl->id, preset.platform);
            LOG_INFO(u8"Export", u8"preset '{}': {} player ({})", preset.name, choice.build ? u8"a built" : u8"the template's",
                     result.playerReason);
        }

        // Soft engine-version match, like EditorProject::Open's project-manifest check: a template
        // built against a different engine version may be binary-incompatible with the cooked
        // content, but that is not known, so warn and keep going rather than block. An empty version
        // means an older or hand-written template with no stamp; skip the check.
        if (tmpl != nullptr && !tmpl->engineVersion.IsEmpty() &&
            tmpl->engineVersion != engine::project::kEngineVersionString)
        {
            LOG_WARNING(u8"Export",
                                 u8"template '{}' was built against engine {} but this build is {} "
                                 u8"- exporting anyway",
                                 tmpl->id, tmpl->engineVersion,
                                 engine::project::kEngineVersionString);
            result.engineVersionWarning = String(u8"Template '");
            result.engineVersionWarning += tmpl->id;
            result.engineVersionWarning += u8"' targets engine ";
            result.engineVersionWarning += tmpl->engineVersion;
            result.engineVersionWarning += u8" (this build is ";
            result.engineVersionWarning += engine::project::kEngineVersionString;
            result.engineVersionWarning += u8").";
        }

        const String subdir = preset.outputSubdir.IsEmpty()
                                  ? detail::SanitizeName(preset.name.AsView())
                                  : String(preset.outputSubdir.AsView());
        result.outputDir = PathJoin(outRoot, subdir.AsView());

        // Ensure the output dir (and outRoot) exist before the content pipeline writes into it.
        (void)CreateDirectories(result.outputDir.AsView());
        // An Android app carries the game inside it, so its content is exported into the app's work
        // folder (fresh each time) and the dist gets the app.
        const String workDir = android ? AndroidWorkDir(project, preset) : String{};
        const String contentDir = android ? PathJoin(workDir.AsView(), u8"content") : result.outputDir;
        if (android && ((DirectoryExists(contentDir.AsView()) && !RemoveDirectoryRecursive(contentDir.AsView())) ||
                        !CreateDirectories(contentDir.AsView())))
        {
            return Status{ErrorCode::Internal};
        }

        // Closure pruning is opt-in per preset. It needs the scene-to-asset edges discovered, either
        // by a live scene-reference scanner (the CLI, which loads scenes on its own single thread) or
        // by a precomputed reachable-root set (the editor, which pre-scans on the main thread and
        // passes the guids into this background job, since scene loading is main-thread only).
        // Without either, content must not be dropped silently, so fall back to packing everything
        // with a warning.
        const bool haveScanner = (scanner != nullptr && *scanner);
        const bool havePrecomputed = (precomputedReachableRoots != nullptr);
        bool prune = preset.pruneToReachable;
        if (prune && !haveScanner && !havePrecomputed)
        {
            LOG_WARNING(
                u8"Export",
                u8"preset '{}' requests pruning but no scene-reference scanner or precomputed root "
                u8"set was supplied - exporting everything",
                preset.name);
            prune = false;
        }

        // The content variants this preset ships: the single host pak for desktop, BC and ASTC for
        // Web. They are passed into every content path so the pack produces one pak per variant.
        const Array<ContentVariant> variants =
            VariantsForPlatform(project, preset.platform.AsView());
        const Span<const ContentVariant> variantSpan(variants.Data(), variants.Size());

        Status contentStatus;
        if (prune)
        {
            // Seed the roots, expand the scene-graph edges, take the PlanFor closure, then cook,
            // stage and pack only that. The scene-edge expansion is either precomputed (the editor's
            // main-thread pre-scan) or run inline through the scanner (CLI). `seeds` is still
            // recomputed here for the report (metadata only, safe in the background); it drives the
            // display, while planRoots drives the cook and the pack.
            const Array<ExportRoot> seeds = CollectExportRoots(project);
            const Array<Guid> planRoots = havePrecomputed
                                              ? *precomputedReachableRoots
                                              : ExpandReachableRoots(project, seeds, *scanner);
            Array<Guid> reachableList;
            contentStatus = CookReachable(project, builders,
                                          Span<const Guid>(planRoots.Data(), planRoots.Size()),
                                          cook, rebuild, result.content, reachableList, onProgress);
            // Variant target cooks (web) after the host closure cook; no-op for desktop.
            if (contentStatus.IsOk())
            {
                contentStatus =
                    CookVariantTargets(project, builders, variantSpan, rebuild, onProgress);
            }
            if (contentStatus.IsOk())
            {
                HashMap<Guid, u8> reachable;
                for (const Guid& g : reachableList)
                {
                    reachable.InsertOrAssign(g, u8(1));
                }
                contentStatus =
                    ExportContent(project, contentDir.AsView(), result.content, onProgress,
                                  sceneStreams, &reachable, &seeds, &result.pruning, variantSpan,
                                  preset.includeShaderSource);
            }
        }
        else if (cook)
        {
            contentStatus = ExportProject(project, contentDir.AsView(), builders, rebuild,
                                          &result.content, onProgress, sceneStreams, variantSpan,
                                          preset.includeShaderSource);
        }
        else
        {
            // The editor already cooked the host through CookService; the variant targets still
            // need cooking.
            contentStatus = CookVariantTargets(project, builders, variantSpan, rebuild, onProgress);
            if (contentStatus.IsOk())
            {
                contentStatus =
                    ExportContent(project, contentDir.AsView(), result.content, onProgress,
                                  sceneStreams, nullptr, nullptr, nullptr, variantSpan,
                                  preset.includeShaderSource);
            }
        }
        if (!contentStatus.IsOk() || !ApplyPresetOverrides(contentDir.AsView(), preset).IsOk())
        {
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::Internal};
        }

        if (contentOnly)
        {
            // What the game reads and no player: the shader pack beside the content, and done.
            u32 shaderVariants = 0;
            const Status packStatus = StageShaderPack(
                result.outputDir.AsView(), dataRoot, preset.platform.AsView(), project,
                Span<const Guid>(result.content.shaders.Data(), result.content.shaders.Size()), shaderVariants);
            if (packStatus.IsOk())
            {
                result.filesStaged += 2; // the pack and the marker
                LOG_INFO(u8"Export", u8"preset '{}': content only, at '{}' ({} shader variants)", preset.name,
                         result.outputDir, shaderVariants);
            }
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return packStatus.IsOk() ? Status{} : Status{ErrorCode::Internal};
        }

        // The dist's last steps, whichever player it runs: the preset's extra files, then the
        // project's plugins' export hooks.
        const auto finish = [&]() -> Status
        {
            // Preset additionalFiles (game extras, relative to the project) go to <outDir>/<basename>.
            for (const String& extra : preset.additionalFiles)
            {
                const StringView base = detail::BaseName(extra.AsView());
                if (detail::CopyFilePreserving(project.Directory(), extra.AsView(),
                                               result.outputDir.AsView(), base))
                {
                    ++result.filesStaged;
                }
                else
                {
                    LOG_WARNING(u8"Export", u8"additional file '{}' not found", extra);
                }
            }

            // The project's plugins' export hooks, over the staged dist.
            if (const ExportPluginServices* hooks = plugins.hooks; hooks != nullptr && hooks->Count() > 0)
            {
                if (onProgress)
                {
                    onProgress(u8"Running the plugins' export hooks...", 0.99f);
                }
                const String failed = hooks->Run(ExportHookContext{&project, &preset, result.outputDir.AsView()});
                if (!failed.IsEmpty())
                {
                    LOG_ERROR(u8"Export", u8"preset '{}': export hook {}", preset.name, failed);
                    result.refusal = Format(u8"export hook {}", failed);
                    if (outResult != nullptr)
                    {
                        *outResult = result;
                    }
                    return Status{ErrorCode::Internal};
                }
            }

            if (onProgress)
            {
                onProgress(u8"Done", 1.0f);
            }
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{};
        };

        if (android)
        {
            if (onProgress)
            {
                onProgress(u8"Cooking shaders...", 0.88f);
            }
            u32 shaderVariants = 0;
            Status built = StageShaderPack(contentDir.AsView(), dataRoot, preset.platform.AsView(), project,
                                           Span<const Guid>(result.content.shaders.Data(), result.content.shaders.Size()),
                                           shaderVariants);
            if (built.IsOk())
            {
                if (onProgress)
                {
                    onProgress(u8"Staging the Android app...", 0.9f);
                }
                built = StageAndroidApp(project, preset, *tmpl, contentDir.AsView(), dataRoot, workDir.AsView());
            }
            if (built.IsOk())
            {
                if (onProgress)
                {
                    onProgress(u8"Building the Android app with Gradle (this can take a while)...", 0.92f);
                }
                built = BuildAndroidApp(project, preset, *tmpl, workDir.AsView(), result.outputDir.AsView(), result);
            }
            if (!built.IsOk())
            {
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return built;
            }
            // The pruning report beside the app, where a desktop dist has it.
            const String report = PathJoin(contentDir.AsView(), u8"export-report.txt");
            if (FileExists(report.AsView()))
            {
                (void)FileCopyPreserving(report.AsView(), PathJoin(result.outputDir.AsView(), u8"export-report.txt").AsView());
            }
            return finish();
        }

        if (onProgress)
        {
            onProgress(u8"Staging player...", 0.93f);
        }
        // Player: <template dir>/<playerBinary> is copied to <outDir>/<name>. The name is the
        // preset's playerName (else the template binary), with the target platform's executable
        // extension ensured, since Windows needs .exe or the OS will not launch it.
        //
        // For native projects the player is not the template's: it is Engine.GamePlayer, freshly
        // built with the game's native module statically linked (for Web presets too, as the wasm
        // build). Ship players ignore the manifest's dlopen module, since the static plugin takes
        // precedence.
        const bool nativeShip = choice.build;
        const bool nativeWeb = nativeShip && (preset.platform.AsView().StartsWith(u8"Web") ||
                                              preset.platform.AsView().StartsWith(u8"Wasm"));
        String shipPlayerPath;
        if (nativeShip)
        {
            const Status shipStatus = BuildShipPlayer(project, preset.config.AsView(), nativeWeb,
                                                      Span<const String>(choice.enabled.Data(), choice.enabled.Size()),
                                                      Span<const String>(choice.definitions.Data(), choice.definitions.Size()),
                                                      onProgress, shipPlayerPath);
            if (!shipStatus.IsOk())
            {
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return shipStatus;
            }
        }
        // Web native ships as Engine.GamePlayer.{html,js,wasm}: the trio cross-references by
        // basename, so a preset playerName cannot rename it (desktop renames freely).
        const String outName =
            nativeWeb ? String(u8"Engine.GamePlayer.html")
                      : detail::PlayerOutputName(preset.platform.AsView(),
                                                 preset.playerName.AsView(),
                                                 nativeShip ? StringView(u8"Engine.GamePlayer")
                                                            : tmpl->playerBinary.AsView());
        const StringView shipPlayerName =
            nativeWeb ? StringView(u8"Engine.GamePlayer.html") : StringView(u8"Engine.GamePlayer");
        String shipPlayerDir;
        if (nativeShip)
        {
            // Split the built player's absolute path into (dir, name) for the copy helper.
            const StringView full = shipPlayerPath.AsView();
            for (usize i = full.Size(); i > 0; --i)
            {
                if (full.Data()[i - 1] == '/')
                {
                    shipPlayerDir = String(StringView(full.Data(), i - 1));
                    break;
                }
            }
        }
        const bool playerStaged =
            nativeShip
                ? detail::CopyFilePreserving(shipPlayerDir.AsView(), shipPlayerName,
                                             result.outputDir.AsView(), outName.AsView())
                : detail::CopyFilePreserving(tmpl->directory.AsView(), tmpl->playerBinary.AsView(),
                                             result.outputDir.AsView(), outName.AsView());
        if (!playerStaged)
        {
            LOG_ERROR(u8"Export", u8"failed to stage player '{}' from {}",
                               nativeShip ? StringView(u8"Engine.GamePlayer")
                                          : tmpl->playerBinary.AsView(),
                               nativeShip ? shipPlayerPath.AsView() : tmpl->id.AsView());
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::Internal};
        }
        ++result.filesStaged;
        // The deploy files of the plugins the project enables on this platform, beside the player.
        if (!StageDeployFiles(nativeShip ? shipPlayerDir.AsView() : tmpl->directory.AsView(),
                              nativeShip ? StringView(u8"Engine.GamePlayer") : kPlayerBaseName,
                              Span<const String>(choice.enabled.Data(), choice.enabled.Size()), preset.platform.AsView(),
                              result.outputDir.AsView(), result.filesStaged))
        {
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::NotFound};
        }
        if (nativeWeb)
        {
            // The rest of the ship trio (the js glue and the wasm), with the same basenames.
            const StringView trio[] = {StringView(u8"Engine.GamePlayer.js"),
                                       StringView(u8"Engine.GamePlayer.wasm")};
            for (StringView part : trio)
            {
                if (!detail::CopyFilePreserving(shipPlayerDir.AsView(), part,
                                                result.outputDir.AsView(), part))
                {
                    LOG_ERROR(u8"Export", u8"failed to stage web ship part '{}' from '{}'", part,
                              shipPlayerDir);
                    if (outResult != nullptr)
                    {
                        *outResult = result;
                    }
                    return Status{ErrorCode::Internal};
                }
                ++result.filesStaged;
            }
        }

        // Cooked engine shaders: Data/Engine/Shaders/shaders.dpak (and Data/.dataroot) beside the
        // player, so the dist renders with no runtime compiler, found by the same data-root
        // discovery every executable uses. A cook failure is fatal, since a dist without shaders
        // cannot render.
        if (onProgress)
        {
            onProgress(u8"Cooking shaders...", 0.95f);
        }
        {
            u32 shaderVariants = 0;
            const Status packStatus = StageShaderPack(
                result.outputDir.AsView(), dataRoot, preset.platform.AsView(), project,
                Span<const Guid>(result.content.shaders.Data(), result.content.shaders.Size()),
                shaderVariants);
            if (!packStatus.IsOk())
            {
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return Status{ErrorCode::Internal};
            }
            result.filesStaged += 2; // the pack and the marker
            LOG_INFO(u8"Export", u8"staged Data/{} ({} variants)", shaders::kShaderPackPath,
                     shaderVariants);
        }

        if (onProgress && !tmpl->sidecars.IsEmpty())
        {
            onProgress(u8"Staging runtime libs...", 0.96f);
        }
        // Template sidecars (runtime libraries) from the template directory. A dist renders from its
        // cooked shader pack and never compiles shaders. The player build does not list DXC, but an
        // older template still might, so DXC libraries are dropped here and the dist ships no
        // dxcompiler or dxil.
        for (const String& sidecar : tmpl->sidecars)
        {
            if (nativeWeb && (sidecar.AsView().EndsWith(u8".js") ||
                              sidecar.AsView().EndsWith(u8".wasm")))
            {
                continue; // the ship build provided the trio; never overwrite it with the template's
            }
            if (IsDxcRuntimeLib(sidecar.AsView()))
            {
                LOG_INFO(u8"Export", u8"omitting DXC sidecar '{}' (dist renders from the "
                                              u8"cooked shader pack)",
                                  sidecar);
                continue;
            }
            if (detail::CopyFilePreserving(tmpl->directory.AsView(), sidecar.AsView(),
                                           result.outputDir.AsView(), sidecar.AsView()))
            {
                ++result.filesStaged;
            }
            else
            {
                LOG_WARNING(u8"Export", u8"sidecar '{}' not found in template '{}'",
                                     sidecar, tmpl->id);
            }
        }

        // Symbols (PDB, DWARF) are left out of the dist by default; stage them only when the preset
        // opts in. sidecars[] always stage; symbols[] are gated here.
        if (preset.stageSymbols)
        {
            for (const String& symbol : tmpl->symbols)
            {
                if (detail::CopyFilePreserving(tmpl->directory.AsView(), symbol.AsView(),
                                               result.outputDir.AsView(), symbol.AsView()))
                {
                    ++result.filesStaged;
                }
                else
                {
                    LOG_WARNING(u8"Export",
                                         u8"symbol file '{}' not found in template '{}'", symbol,
                                         tmpl->id);
                }
            }
        }

        return finish();
    }

    Status ExportAll(EditorProject& project, Span<const ExportPreset> presets,
                     const TemplateRegistry& templates, BuilderRegistry& builders,
                     StringView outRoot, StringView dataRoot, bool rebuild,
                     const ExportProgress& onProgress, bool cook,
                     const HashMap<Guid, Array<byte>>* sceneStreams,
                     const SceneReferenceScanner* scanner,
                     const Array<Guid>* precomputedReachableRoots, ExportPlugins plugins)
    {
        // The reachable-root closure is project-level (not per-preset), so one precomputed set
        // seeds every pruning preset in the run.
        usize ok = 0;
        const usize n = presets.Size();
        for (usize i = 0; i < n; ++i)
        {
            const ExportPreset& preset = presets[i];
            // Scale each preset's 0..1 into its slice (i..i+1)/n and prefix its name.
            const ExportProgress scoped = [&onProgress, i, n, &preset](StringView step, f32 frac)
            {
                if (!onProgress)
                {
                    return;
                }
                String s(preset.name.AsView());
                s += u8": ";
                s += step;
                onProgress(s.AsView(), (static_cast<f32>(i) + frac) / static_cast<f32>(n));
            };
            ExportResult result;
            if (ExportOne(project, preset, templates, builders, outRoot, dataRoot, rebuild, &result,
                          scoped, cook, sceneStreams, scanner, precomputedReachableRoots, plugins)
                    .IsOk())
            {
                ++ok;
                LOG_INFO(u8"Export", u8"exported '{}' -> {} ({} files staged)",
                                  preset.name, result.outputDir, result.filesStaged);
            }
            else
            {
                LOG_ERROR(u8"Export", u8"preset '{}' failed", preset.name);
            }
        }
        return (ok == n) ? Status{} : Status{ErrorCode::Internal};
    }
}
