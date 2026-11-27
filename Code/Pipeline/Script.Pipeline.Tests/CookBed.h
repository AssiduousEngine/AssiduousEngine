// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shared cook harness for the neutral-builder pipeline tests. The ScriptClassAssetBuilder is
// backend-neutral: it resolves a per-language cook through the registry, so an
// AngelScript-source test (AngelScriptPipelineTests.cpp) and a Luau-source test
// (LuauPipelineTests.cpp) drive the same path from source through builder to factory, each in its
// own file with no #ifdefs. This bed registers every enabled backend's cook so a test of either
// language cooks; the builder picks the cook by the asset's declared language.
#pragma once

#include <doctest/doctest.h>
#include <filesystem>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.script;
import foundation.script.resource;
import script.pipeline;
#ifdef HAS_PLUGIN_ANGELSCRIPT
import angelscript.foundation;
import angelscript.pipeline;
#endif
#ifdef HAS_PLUGIN_LUAU
import luau.foundation;
import luau.pipeline;
#endif

namespace scriptpipe
{
    using namespace foundation::core;
    using namespace pipeline;
    using namespace foundation::script;
    namespace content = foundation::content;

    struct CookBed
    {
        String srcDir;
        String outDir;
        UniquePtr<foundation::vfs::NativeFileSystem> sources;
        UniquePtr<foundation::vfs::NativeFileSystem> output;
        UniquePtr<content::ContentDatabase> outputDb;

        explicit CookBed(StringView tag)
        {
            // Cook-error logs reach the test output (silent otherwise).
            static bool logReady = []()
            {
                static ConsoleSink sink;
                GlobalLogger().AddSink(&sink);
                return true;
            }();
            (void)logReady;
            // The composition-root registration for whatever backends are compiled in (idempotent);
            // the builder resolves the per-language cook from the registry.
#ifdef HAS_PLUGIN_ANGELSCRIPT
            angelscript::RegisterAngelScriptScriptCook();
#endif
#ifdef HAS_PLUGIN_LUAU
            luau::RegisterLuauScriptCook();
#endif
            RegisterScriptResource();
            RegisterScriptAssets();
            srcDir = String(u8"scratch_scriptpipe_src_");
            srcDir += tag;
            outDir = String(u8"scratch_scriptpipe_out_");
            outDir += tag;
            RemoveTree(srcDir.AsView());
            RemoveTree(outDir.AsView());
            REQUIRE(CreateDirectory(srcDir.AsView()));
            sources =
                MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), srcDir.AsView(), DefaultAllocator());
            output =
                MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), outDir.AsView(), DefaultAllocator());
            outputDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *output,
                                                            BinarySerializerFactory(), u8".rasset");
        }
        ~CookBed()
        {
            outputDb = nullptr;
            sources = nullptr;
            output = nullptr;
            RemoveTree(srcDir.AsView());
            RemoveTree(outDir.AsView());
        }

        void WriteSource(StringView fileName, StringView text)
        {
            String path(srcDir.AsView());
            path.Append(u8"/");
            path.Append(fileName);
            REQUIRE(
                WriteFile(path.AsView(),
                          Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()))
                    .IsOk());
        }

        [[nodiscard]] Status Cook(StringView fileName, StringView language, content::Instance*& outInstance)
        {
            ScriptClassAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName);
            asset.language = String(language);
            ScriptClassAssetBuilder builder;
            pipeline::AssetBuildContext ctx{foundation::core::DefaultAllocator()};
            ctx.sources = sources.Get();
            if (outInstance == nullptr)
            {
                outInstance = outputDb->RootGroup()->CreateInstance(
                    u8"cooked", BehaviorClassSource::StaticType());
            }
            ctx.output = outInstance;
            return builder.Build(asset, ctx);
        }

        // A scratch dir holds only test-written sources and cooked .rasset artifacts, whose names vary
        // per language (.as / .luau); a recursive remove keeps the harness backend-agnostic.
        static void RemoveTree(StringView dir)
        {
            std::error_code ec;
            std::filesystem::remove_all(
                std::filesystem::path(reinterpret_cast<const char*>(String(dir).CStr())), ec);
        }
    };
}
