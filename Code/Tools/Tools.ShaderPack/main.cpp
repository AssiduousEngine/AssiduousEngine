// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.ShaderPack - a headless CLI that cooks the engine shader corpus into a CookedShaderPack
// (shaders.dpak). This is the shader part of what the exporter does, standing alone: enumerate a
// shader directory (Data/Engine/Shaders), compile every stage and variant to the requested backend
// blobs, and write the pack. It needs DXC (SPIR-V, DXIL) and, for WGSL, the vendored naga and tint
// (paths baked into foundation.shaders), so it is a desktop authoring tool. The web build ships the
// WGSL pack it produces and loads it at runtime, since there is no compiler in the browser.
//
//   Tools.ShaderPack <shaderDir> <output.dpak> [wgsl] [spirv] [dxil]
//   Tools.ShaderPack --entries <dir> <output.dpak> [spirv] [dxil]
//
// Defaults to wgsl when no formats are given. --entries cooks shaders that name their entry
// points in their own comments (`// entry: vertex VSMain`, the RHI samples' under
// Data/Samples/RHI), every .hlsl under <dir>, each entry under "<file>:<entry>"; it defaults to
// spirv, the format a device without a shader compiler (Android) reads.

#include "Core/Prelude.h"

#include <cstdio>

import foundation.core;
import foundation.shaders;

using namespace foundation::core;
namespace shaders = foundation::shaders;

namespace
{
    void PrintUsage()
    {
        std::fprintf(stderr,
                     "usage: Tools.ShaderPack <shaderDir> <output.dpak> [wgsl] [spirv] [dxil]\n"
                     "       Tools.ShaderPack --entries <dir> <output.dpak> [spirv] [dxil]\n");
    }
}

int main(int argc, char** argv)
{
    const bool entries = argc > 1 && StringView(reinterpret_cast<const char8_t*>(argv[1])) == u8"--entries";
    const int first = entries ? 2 : 1;
    if (argc < first + 2)
    {
        PrintUsage();
        return 2;
    }

    const StringView shaderDir(reinterpret_cast<const char8_t*>(argv[first]));
    const StringView outputPath(reinterpret_cast<const char8_t*>(argv[first + 1]));

    Array<shaders::CookedShaderFormat> formats;
    for (int i = first + 2; i < argc; ++i)
    {
        const StringView flag(reinterpret_cast<const char8_t*>(argv[i]));
        if (flag == u8"wgsl")
        {
            formats.PushBack(shaders::CookedShaderFormat::Wgsl);
        }
        else if (flag == u8"spirv")
        {
            formats.PushBack(shaders::CookedShaderFormat::SpirV);
        }
        else if (flag == u8"dxil")
        {
            formats.PushBack(shaders::CookedShaderFormat::Dxil);
        }
        else
        {
            std::fprintf(stderr, "Tools.ShaderPack: unknown format '%s'\n", argv[i]);
            PrintUsage();
            return 2;
        }
    }
    if (formats.IsEmpty())
    {
        formats.PushBack(entries ? shaders::CookedShaderFormat::SpirV : shaders::CookedShaderFormat::Wgsl);
    }

    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        std::fprintf(stderr, "Tools.ShaderPack: DXC runtime unavailable\n");
        return 1;
    }

    const StringView scratchDir = u8".shaderpackcook-scratch";
    CreateDirectories(scratchDir); // WgslTranslator's intermediates; the folder must exist

    shaders::ShaderCookOptions opts;
    opts.shaderDir = shaderDir;
    opts.scratchDir = scratchDir;
    opts.formats = Span<const shaders::CookedShaderFormat>(formats.Data(), formats.Size());

    shaders::CookedShaderPack pack;
    shaders::ShaderCookReport report;
    if (entries)
    {
        shaders::ShaderEntryCookOptions entryOptions;
        entryOptions.shaderDir = shaderDir;
        entryOptions.formats = Span<const shaders::CookedShaderFormat>(formats.Data(), formats.Size());
        report = shaders::CookEntryShaders(*compiler, entryOptions, pack);
    }
    else
    {
        report = shaders::CookEngineShaders(*compiler, opts, pack);
    }
    compiler->Destroy();

    for (usize i = 0; i < report.errors.Size(); ++i)
    {
        std::fprintf(stderr, "cook error: %s\n",
                     reinterpret_cast<const char*>(report.errors[i].CStr()));
    }
    std::fprintf(stderr, "Tools.ShaderPack: files=%u variants=%u success=%d\n",
                 static_cast<unsigned>(report.filesCooked),
                 static_cast<unsigned>(report.variantsCooked), report.success ? 1 : 0);
    if (!report.success)
    {
        return 1;
    }

    FileStream out(outputPath, FileMode::Write);
    if (!out.IsValid() || !pack.Write(out).IsOk())
    {
        std::fprintf(stderr, "Tools.ShaderPack: could not write '%s'\n", argv[first + 1]);
        return 1;
    }
    std::fprintf(stderr, "Tools.ShaderPack: wrote '%s' (%u entries)\n", argv[first + 1],
                 static_cast<unsigned>(pack.Count()));
    return 0;
}
