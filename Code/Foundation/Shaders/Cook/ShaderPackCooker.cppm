// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine-shader cook: enumerates the built-in HLSL corpus and precompiles it into a
// CookedShaderPack.
//
// The export-time step that removes the runtime compiler from dists. For each stage file under
// the shader dir it parses the variant directive, drift-lints (fails on a flag that is #ifdef'd
// but undeclared), enumerates the power set of the declared mask, and for every variant and
// requested backend format emits a blob into the pack: SPIR-V and DXIL through DXC, WGSL through
// the WgslTranslator. Runs on the dev or CI host only; the dist just reads the pack.

module;
#include "Core/Prelude.h"

export module foundation.shaders:pack_cook;

import foundation.core;
import :types;
import :flags;
import :variants;
import :properties;
import :pack;
import :compiler;
import :wgsl_cook;

using namespace foundation::core;

export namespace foundation::shaders
{
    struct ShaderCookOptions
    {
        StringView shaderDir;                    // source and #include root (e.g. Data/Engine/Shaders)
        StringView scratchDir;                   // WgslTranslator intermediates (must exist)
        Span<const CookedShaderFormat> formats;  // which backend blobs to emit
        bool validateWgsl = true;                // run tint over the WGSL
        // SPIR-V target for the SpirV blobs. It is vulkan1.1 because the SpirV bucket serves
        // both Vulkan and native WebGPU (wgpu-native and naga), and naga rejects the SPIR-V 1.4+
        // instructions (OpCopyLogical) that DXC emits when targeting vulkan1.3; a 1.3 pack
        // panics wgpu-native at pipeline creation. Matches the dev-mode WebGPU compile.
        StringView spirvTargetEnv = u8"vulkan1.1";
    };

    struct ShaderCookReport
    {
        bool success = false;
        usize filesCooked = 0;    // stage files processed
        usize variantsCooked = 0; // blobs emitted, one per variant and format
        Array<String> errors;     // lint and compile diagnostics (each one line)
    };

    // Forward declarations (definitions below CookEngineShaders).
    [[nodiscard]] bool ParseStageFile(StringView fileName, ShaderStage& outStage,
                                      StringView& outStem);
    [[nodiscard]] String FormatError(StringView file, StringView msg);
    bool CookOne(Compiler& compiler, WgslTranslator& translator, StringView source,
                 ShaderStage stage, StringView entryPoint, ShaderFlags flags,
                 CookedShaderFormat format, const ShaderCookOptions& opts,
                 Span<const StringView> includePaths, StringView stem, StringView fileName,
                 CookedShaderPack& pack, ShaderCookReport& report);

    // Cook one shader stage into the pack: record its lattice and entry point, then emit a blob
    // for every declared variant in every requested format. What the corpus cook does per stage file, and
    // what an export does per stage of a project's shader (its expanded source, VSMain/PSMain).
    // `sourceName` names the source in errors. Returns false (errors in the report) if any fails.
    bool CookShaderStage(Compiler& compiler, WgslTranslator& translator, StringView name,
                         ShaderStage stage, StringView source, StringView entryPoint,
                         ShaderFlags mask, StringView sourceName, const ShaderCookOptions& opts,
                         Span<const StringView> includePaths, CookedShaderPack& pack,
                         ShaderCookReport& report);

    // Inline-expand `#include "file"` directives against the shader root, so the drift-lint
    // sees the .hlsli bodies where the real flag #ifdefs live. A stage file is often just the
    // directive and one include, so linting only its own text would pass a cook that silently
    // strips a used flag in dist. Quoted includes only (the corpus has no angle includes),
    // depth-limited and de-duplicated. Unreadable includes are skipped here; DXC reports them
    // during the actual compile.
    inline void AppendExpandedSource(StringView shaderDir, StringView source, u32 depth,
                                     Array<String>& visited, String& out)
    {
        constexpr u32 kMaxIncludeDepth = 8;
        const usize n = source.Size();
        const char8_t* d = source.Data();
        usize lineStart = 0;
        while (lineStart <= n)
        {
            usize lineEnd = lineStart;
            while (lineEnd < n && d[lineEnd] != u8'\n')
            {
                ++lineEnd;
            }
            const StringView line = source.SubStr(lineStart, lineEnd - lineStart);

            usize k = 0;
            while (k < line.Size() && (line.Data()[k] == u8' ' || line.Data()[k] == u8'\t'))
            {
                ++k;
            }
            const StringView trimmed = line.SubStr(k, line.Size() - k);
            bool expanded = false;
            if (depth < kMaxIncludeDepth && trimmed.StartsWith(u8"#include"))
            {
                const usize firstQuote = [&]() -> usize
                {
                    for (usize i = 8; i < trimmed.Size(); ++i)
                    {
                        if (trimmed.Data()[i] == u8'"')
                        {
                            return i;
                        }
                    }
                    return trimmed.Size();
                }();
                usize closeQuote = trimmed.Size();
                for (usize i = firstQuote + 1; i < trimmed.Size(); ++i)
                {
                    if (trimmed.Data()[i] == u8'"')
                    {
                        closeQuote = i;
                        break;
                    }
                }
                if (firstQuote < trimmed.Size() && closeQuote < trimmed.Size())
                {
                    const StringView includeName =
                        trimmed.SubStr(firstQuote + 1, closeQuote - firstQuote - 1);
                    bool seen = false;
                    for (const String& v : visited)
                    {
                        if (v.AsView() == includeName)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        visited.PushBack(String(includeName));
                        String includePath(shaderDir);
                        includePath += u8"/";
                        includePath += includeName;
                        const Result<Array<byte>> bytes = ReadFile(includePath.AsView());
                        if (bytes.HasValue())
                        {
                            const Array<byte>& b = bytes.Value();
                            AppendExpandedSource(
                                shaderDir,
                                StringView(reinterpret_cast<const char8_t*>(b.Data()), b.Size()),
                                depth + 1, visited, out);
                            expanded = true;
                        }
                    }
                    else
                    {
                        expanded = true; // already inlined once: don't re-scan or keep the line
                    }
                }
            }
            if (!expanded)
            {
                out += line;
                out += u8"\n";
            }

            if (lineEnd >= n)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }
    }

    // Byte order, so a listing's order (and the pack's bytes) is reproducible across hosts:
    // ListDirectory yields filesystem order (readdir/FindFirstFile).
    inline void SortByBytes(Array<String>& names)
    {
        names.Sort(
            [](const String& a, const String& b)
            {
                const usize n = a.Size() < b.Size() ? a.Size() : b.Size();
                for (usize i = 0; i < n; ++i)
                {
                    if (a[i] != b[i])
                    {
                        return a[i] < b[i];
                    }
                }
                return a.Size() < b.Size();
            });
    }

    // Every .hlsli under `shaderDir` and its subfolders, as an include names it ("wind.hlsli",
    // "surface/forward.hlsli"), each folder's in byte order. Depth-limited like the include
    // expansion.
    inline void CollectShaderLibraryFiles(StringView shaderDir, StringView relative, u32 depth,
                                          Array<String>& out)
    {
        constexpr u32 kMaxFolderDepth = 8;
        struct Listing
        {
            Array<String> files;
            Array<String> folders;
        } listing;
        String folder(shaderDir);
        if (!relative.IsEmpty())
        {
            folder += u8"/";
            folder += relative;
        }
        (void)ListDirectory(
            folder.AsView(),
            [](void* ctx, StringView name, bool isDir)
            {
                Listing& l = *static_cast<Listing*>(ctx);
                if (isDir)
                {
                    if (name != StringView(u8".") && name != StringView(u8".."))
                    {
                        l.folders.PushBack(String(name));
                    }
                }
                else if (name.EndsWith(u8".hlsli"))
                {
                    l.files.PushBack(String(name));
                }
            },
            &listing);
        SortByBytes(listing.files);
        SortByBytes(listing.folders);
        for (const String& file : listing.files)
        {
            String path(relative);
            if (!relative.IsEmpty())
            {
                path += u8"/";
            }
            path += file.AsView();
            out.PushBack(Move(path));
        }
        if (depth >= kMaxFolderDepth)
        {
            return;
        }
        for (const String& sub : listing.folders)
        {
            String path(relative);
            if (!relative.IsEmpty())
            {
                path += u8"/";
            }
            path += sub.AsView();
            CollectShaderLibraryFiles(shaderDir, path.AsView(), depth + 1, out);
        }
    }

    // Every effect block the library's includes declare, into
    // `out`: what the cook records in the pack, and where a tool with no shader system (the
    // headless MCP host's material_set_shader) reads the effects' tables from. A malformed block,
    // or two effects sharing a name or a flag, is left out with an error naming the file (and
    // line) in `errors`. Returns false when any is.
    inline bool CollectShaderLibraryEffects(StringView shaderDir, ShaderEffectLibrary& out,
                                            Array<String>& errors)
    {
        Array<String> files;
        CollectShaderLibraryFiles(shaderDir, StringView{}, 0, files);
        bool ok = true;
        for (const String& file : files)
        {
            String path(shaderDir);
            path += u8"/";
            path += file.AsView();
            const Result<Array<byte>> bytes = ReadFile(path.AsView());
            if (!bytes.HasValue())
            {
                errors.PushBack(FormatError(file.AsView(), u8"could not read source"));
                ok = false;
                continue;
            }
            const StringView text(reinterpret_cast<const char8_t*>(bytes.Value().Data()),
                                  bytes.Value().Size());
            if (!HasShaderEffect(text))
            {
                continue;
            }
            for (ShaderEffectBlock& block : ParseShaderEffects(text))
            {
                if (!block.IsOk())
                {
                    for (const ShaderPropertyError& error : block.errors)
                    {
                        String where(file.AsView());
                        where += Format(u8":{}", error.line);
                        errors.PushBack(FormatError(where.AsView(), error.message.AsView()));
                    }
                    ok = false;
                    continue;
                }
                String error;
                if (!out.Add(Move(block.effect), error))
                {
                    String where(file.AsView());
                    where += Format(u8":{}", block.firstLine);
                    errors.PushBack(FormatError(where.AsView(), error.AsView()));
                    ok = false;
                }
            }
        }
        return ok;
    }

    // Record every effect block the corpus's includes declare into the pack, so a game reads the
    // effects' tables without the sources.
    // A malformed block, or two effects sharing a name or a flag, is a cook error naming the
    // file (and line). Returns false when any is.
    inline bool CookShaderEffects(StringView shaderDir, CookedShaderPack& pack,
                                  ShaderCookReport& report)
    {
        ShaderEffectLibrary library;
        bool ok = CollectShaderLibraryEffects(shaderDir, library, report.errors);
        for (const ShaderEffect& effect : library.All())
        {
            // The library refused a second name or flag already, as the pack would.
            String error;
            if (!pack.AddEffect(effect, error))
            {
                report.errors.PushBack(FormatError(effect.name.AsView(), error.AsView()));
                ok = false;
            }
        }
        return ok;
    }

    // Fill `pack` from every stage file under opts.shaderDir. On any error the pack is left partially
    // filled and success is false; the caller decides whether to ship (a dist should not).
    inline ShaderCookReport CookEngineShaders(Compiler& compiler, const ShaderCookOptions& opts,
                                              CookedShaderPack& pack)
    {
        ShaderCookReport report;

        // Enumerate the shader dir (stage files only, sorted for determinism).
        struct Collector
        {
            Array<String> files;
        } collector;
        const bool listed = ListDirectory(
            opts.shaderDir,
            [](void* ctx, StringView name, bool isDir)
            {
                if (!isDir)
                {
                    static_cast<Collector*>(ctx)->files.PushBack(String(name));
                }
            },
            &collector);
        if (!listed)
        {
            report.errors.PushBack(String(u8"could not list the shader directory"));
            return report;
        }
        SortByBytes(collector.files);

        // The effects' tables, from the includes that declare them.
        (void)CookShaderEffects(opts.shaderDir, pack, report);

        const StringView includePaths[] = {opts.shaderDir};

        WgslTranslator translator(compiler, opts.scratchDir);
        translator.SetValidateWithTint(opts.validateWgsl);

        for (const String& fileName : collector.files)
        {
            ShaderStage stage = ShaderStage::Vertex;
            StringView stem;
            if (!ParseStageFile(fileName.AsView(), stage, stem))
            {
                continue; // .hlsli include or non-shader file
            }

            String pathBuf(opts.shaderDir);
            pathBuf += u8"/";
            pathBuf += fileName.AsView();
            const Result<Array<byte>> srcBytes = ReadFile(pathBuf.AsView());
            if (!srcBytes.HasValue())
            {
                report.errors.PushBack(FormatError(fileName.AsView(), u8"could not read source"));
                continue;
            }
            const Array<byte>& sb = srcBytes.Value();
            const StringView source(reinterpret_cast<const char8_t*>(sb.Data()), sb.Size());

            // Variant model: the declared mask and the drift-lint (a flag used but undeclared is
            // a hard error). The directive comes from the stage file, but the lint runs over the
            // include-expanded text, since the flag #ifdefs live in the shared .hlsli bodies.
            const VariantDirective directive = ParseVariantDirective(source);
            Array<StringView> undeclared;
            String expandedSource;
            {
                Array<String> visitedIncludes;
                AppendExpandedSource(opts.shaderDir, source, 0, visitedIncludes, expandedSource);
            }
            FindUndeclaredFlagUses(expandedSource.AsView(), directive.mask, undeclared);
            if (!undeclared.IsEmpty())
            {
                String msg(u8"uses undeclared variant flag(s):");
                for (usize i = 0; i < undeclared.Size(); ++i)
                {
                    msg += u8" ";
                    msg += undeclared[i];
                }
                msg += u8" (add them to the // variants: directive)";
                report.errors.PushBack(FormatError(fileName.AsView(), msg.AsView()));
                continue;
            }

            ++report.filesCooked;
            (void)CookShaderStage(compiler, translator, stem, stage, source, kCorpusEntryPoint,
                                  directive.mask, fileName.AsView(), opts,
                                  Span<const StringView>(includePaths, 1), pack, report);
        }

        report.success = report.errors.IsEmpty();
        return report;
    }

    // --- shaders that name their entry points (an RHI sample's) ------------

    // A shader the RHI binds directly, often several entry points in one file, names them in its
    // own comments, one line each, and may name its shader model (6_0 when it does not):
    //   // entry: vertex VSMain
    //   // entry: fragment PSMain
    //   // model: 6_5
    // A ray tracing library's entry has no name ("// entry: raygen"). It compiles with
    // DirectCompileOptions, as it does when it is compiled as it runs, so a cooked one is the same.
    struct ShaderEntryDirective
    {
        ShaderStage stage = ShaderStage::Vertex;
        String entry;
    };

    struct ShaderEntryDirectives
    {
        Array<ShaderEntryDirective> entries;
        String shaderModel = String(u8"6_0");
    };

    // "vertex", "fragment", "compute", "mesh", "task", "raygen", "closesthit", "anyhit", "miss",
    // "intersection", "callable".
    [[nodiscard]] inline bool ParseShaderStageName(StringView name, ShaderStage& out)
    {
        struct Named
        {
            StringView name;
            ShaderStage stage;
        };
        static constexpr Named kNames[] = {
            {u8"vertex", ShaderStage::Vertex},       {u8"fragment", ShaderStage::Fragment},
            {u8"compute", ShaderStage::Compute},     {u8"mesh", ShaderStage::Mesh},
            {u8"task", ShaderStage::Task},           {u8"raygen", ShaderStage::RayGen},
            {u8"closesthit", ShaderStage::ClosestHit}, {u8"anyhit", ShaderStage::AnyHit},
            {u8"miss", ShaderStage::Miss},           {u8"intersection", ShaderStage::Intersection},
            {u8"callable", ShaderStage::Callable},
        };
        for (const Named& named : kNames)
        {
            if (named.name == name)
            {
                out = named.stage;
                return true;
            }
        }
        return false;
    }

    // The `// entry:` and `// model:` lines of `source`; false, with `error`, for a line it cannot
    // read (an unknown stage). A source with no entry line names none (out.entries is empty).
    [[nodiscard]] inline bool ParseEntryDirectives(StringView source, ShaderEntryDirectives& out, String& error)
    {
        usize start = 0;
        while (start <= source.Size())
        {
            usize end = source.Find(u8'\n', start);
            if (end == StringView::kNotFound)
            {
                end = source.Size();
            }
            const StringView line = Trimmed(source.SubStr(start, end - start));
            start = end + 1;
            if (line.StartsWith(u8"// entry:"))
            {
                const StringView rest = Trimmed(line.SubStr(9, line.Size() - 9));
                usize space = rest.Find(u8' ');
                if (space == StringView::kNotFound)
                {
                    space = rest.Size();
                }
                ShaderEntryDirective directive;
                if (!ParseShaderStageName(rest.SubStr(0, space), directive.stage))
                {
                    error = Format(u8"unknown stage in '{}'", line);
                    return false;
                }
                directive.entry = String(Trimmed(rest.SubStr(space, rest.Size() - space)));
                out.entries.PushBack(Move(directive));
            }
            else if (line.StartsWith(u8"// model:"))
            {
                out.shaderModel = String(Trimmed(line.SubStr(9, line.Size() - 9)));
            }
            if (end == source.Size())
            {
                break;
            }
        }
        return true;
    }

    // The pack's name for one entry: the file, relative to the folder cooked ("Sample001_Triangle/
    // Triangle.hlsl"), and the entry ("VSMain"), as "Sample001_Triangle/Triangle.hlsl:VSMain".
    [[nodiscard]] inline String EntryShaderName(StringView file, StringView entry)
    {
        return Format(u8"{}:{}", file, entry);
    }

    struct ShaderEntryCookOptions
    {
        StringView shaderDir;                   // the folder cooked, walked whole (Data/Samples/RHI)
        Span<const CookedShaderFormat> formats; // SPIR-V and DXIL (WGSL is the engine shaders' alone)
    };

    namespace detail
    {
        inline void CollectShaderFiles(StringView root, StringView relative, Array<String>& out)
        {
            struct Listing
            {
                Array<String> files;
                Array<String> folders;
            } listing;
            const String folder = relative.IsEmpty() ? String(root) : Format(u8"{}/{}", root, relative);
            (void)ListDirectory(
                folder.AsView(),
                [](void* ctx, StringView name, bool isDir)
                {
                    Listing& l = *static_cast<Listing*>(ctx);
                    (isDir ? l.folders : l.files).PushBack(String(name));
                },
                &listing);
            SortByBytes(listing.files);
            SortByBytes(listing.folders);
            for (const String& file : listing.files)
            {
                if (file.AsView().EndsWith(u8".hlsl"))
                {
                    out.PushBack(relative.IsEmpty() ? String(file) : Format(u8"{}/{}", relative, file));
                }
            }
            for (const String& sub : listing.folders)
            {
                CollectShaderFiles(root, relative.IsEmpty() ? sub.AsView() : Format(u8"{}/{}", relative, sub).AsView(), out);
            }
        }
    }

    // Fill `pack` with every entry the .hlsl files under opts.shaderDir name, each under its
    // EntryShaderName, in every requested format. A file that names no entry is left out.
    inline ShaderCookReport CookEntryShaders(Compiler& compiler, const ShaderEntryCookOptions& opts, CookedShaderPack& pack)
    {
        ShaderCookReport report;
        report.success = true;
        Array<String> files;
        detail::CollectShaderFiles(opts.shaderDir, StringView{}, files);
        for (const String& file : files)
        {
            const String path = Format(u8"{}/{}", opts.shaderDir, file);
            const Result<Array<byte>> bytes = ReadFile(path.AsView());
            if (!bytes.HasValue())
            {
                report.errors.PushBack(FormatError(file.AsView(), u8"could not read source"));
                report.success = false;
                continue;
            }
            const StringView source(reinterpret_cast<const char8_t*>(bytes.Value().Data()), bytes.Value().Size());
            ShaderEntryDirectives directives;
            String error;
            if (!ParseEntryDirectives(source, directives, error))
            {
                report.errors.PushBack(FormatError(file.AsView(), error.AsView()));
                report.success = false;
                continue;
            }
            if (directives.entries.IsEmpty())
            {
                continue;
            }
            ++report.filesCooked;
            for (const ShaderEntryDirective& directive : directives.entries)
            {
                const String name = EntryShaderName(file.AsView(), directive.entry.AsView());
                for (const CookedShaderFormat format : opts.formats)
                {
                    if (format == CookedShaderFormat::Wgsl)
                    {
                        report.errors.PushBack(FormatError(file.AsView(), u8"WGSL is not cooked for entry shaders"));
                        report.success = false;
                        continue;
                    }
                    const ShaderTarget target = format == CookedShaderFormat::Dxil ? ShaderTarget::DXIL : ShaderTarget::SPIRV;
                    CompileOptions options = DirectCompileOptions(target, directives.shaderModel.AsView());
                    options.sourceName = file.AsView();
                    CompileResult result{};
                    const Status compiled = compiler.compile(reinterpret_cast<const u8*>(source.Data()), source.Size(), directive.stage,
                                                             directive.entry.AsView(), target, options, result);
                    if (!compiled.IsOk() || !result.success)
                    {
                        report.errors.PushBack(FormatError(
                            name.AsView(), result.messages != nullptr ? StringView(reinterpret_cast<const char8_t*>(result.messages))
                                                                      : StringView(u8"DXC compile failed")));
                        report.success = false;
                        compiler.freeResult(result);
                        continue;
                    }
                    pack.Add(name.AsView(), directive.stage, ShaderFlags::None, format,
                             Span<const byte>(reinterpret_cast<const byte*>(result.bytecode), result.bytecodeSize));
                    pack.AddEntryPoint(name.AsView(), directive.stage, directive.entry.AsView());
                    compiler.freeResult(result);
                    ++report.variantsCooked;
                }
            }
        }
        return report;
    }

    // --- helpers -----------------------------------------------------------

    // "<stem>.<vs|ps|cs>.hlsl" -> (stage, stem); false for .hlsli and anything else. Matches the
    // FileShaderSourceProvider naming so cooked and dev names agree.
    [[nodiscard]] inline bool ParseStageFile(StringView fileName, ShaderStage& outStage,
                                             StringView& outStem)
    {
        StringView suffix;
        if (fileName.EndsWith(u8".vs.hlsl"))
        {
            outStage = ShaderStage::Vertex;
            suffix = u8".vs.hlsl";
        }
        else if (fileName.EndsWith(u8".ps.hlsl"))
        {
            outStage = ShaderStage::Fragment;
            suffix = u8".ps.hlsl";
        }
        else if (fileName.EndsWith(u8".cs.hlsl"))
        {
            outStage = ShaderStage::Compute;
            suffix = u8".cs.hlsl";
        }
        else
        {
            return false;
        }
        outStem = fileName.SubStr(0, fileName.Size() - suffix.Size());
        return !outStem.IsEmpty();
    }

    [[nodiscard]] inline String FormatError(StringView file, StringView msg)
    {
        String s(file);
        s += u8": ";
        s += msg;
        return s;
    }

    inline bool CookShaderStage(Compiler& compiler, WgslTranslator& translator, StringView name,
                                ShaderStage stage, StringView source, StringView entryPoint,
                                ShaderFlags mask, StringView sourceName,
                                const ShaderCookOptions& opts, Span<const StringView> includePaths,
                                CookedShaderPack& pack, ShaderCookReport& report)
    {
        // The lattice and entry, so the dist runtime canonicalizes requests onto the cooked
        // variants (dev == dist behaviour) and its modules run the stage's function.
        pack.AddDeclaredMask(name, stage, mask);
        pack.AddEntryPoint(name, stage, entryPoint);

        Array<ShaderFlags> variants;
        EnumerateVariants(mask, variants);
        bool ok = true;
        for (const ShaderFlags flags : variants)
        {
            for (const CookedShaderFormat format : opts.formats)
            {
                if (CookOne(compiler, translator, source, stage, entryPoint, flags, format, opts,
                            includePaths, name, sourceName, pack, report))
                {
                    ++report.variantsCooked;
                }
                else
                {
                    ok = false;
                }
            }
        }
        return ok;
    }

    // Compile or translate the blob for one variant and format and Add it to the pack. On failure,
    // records an error and returns false.
    inline bool CookOne(Compiler& compiler, WgslTranslator& translator, StringView source,
                        ShaderStage stage, StringView entryPoint, ShaderFlags flags,
                        CookedShaderFormat format, const ShaderCookOptions& opts,
                        Span<const StringView> includePaths, StringView stem,
                        StringView fileName, CookedShaderPack& pack, ShaderCookReport& report)
    {
        if (format == CookedShaderFormat::Wgsl)
        {
            const WgslCookResult wr =
                translator.Translate(source, stage, entryPoint, flags, includePaths);
            if (!wr.success)
            {
                report.errors.PushBack(FormatError(fileName, wr.error.AsView()));
                return false;
            }
            pack.Add(stem, stage, flags, format,
                     Span<const byte>(reinterpret_cast<const byte*>(wr.wgsl.Data()),
                                      wr.wgsl.Size()));
            return true;
        }

        // SPIR-V (Vulkan) or DXIL (DX12) through DXC.
        Array<ShaderDefine> defines;
        AppendDefines(flags, defines);
        CompileOptions co{};
        co.shaderModel = u8"6_0";
        co.optimizationLevel = 3;
        co.defines = Span<const ShaderDefine>(defines.Data(), defines.Size());
        co.includePaths = includePaths;
        co.preserveInterface = ParsePreserveInterfaceDirective(source);
        co.sourceName = fileName;

        ShaderTarget target = ShaderTarget::SPIRV;
        if (format == CookedShaderFormat::SpirV)
        {
            co.spirvTargetEnvironment = opts.spirvTargetEnv;
            co.bindingShifts = BindingShifts::Standard();
            co.bindingShiftSets = 4;
        }
        else // Dxil
        {
            target = ShaderTarget::DXIL;
        }

        CompileResult cr{};
        const Status st =
            compiler.compile(reinterpret_cast<const u8*>(source.Data()), source.Size(), stage,
                             entryPoint, target, co, cr);
        if (st != ErrorCode::Ok || !cr.success)
        {
            report.errors.PushBack(FormatError(
                fileName, cr.messages != nullptr
                              ? StringView(reinterpret_cast<const char8_t*>(cr.messages))
                              : StringView(u8"DXC compile failed")));
            compiler.freeResult(cr);
            return false;
        }
        pack.Add(stem, stage, flags, format,
                 Span<const byte>(reinterpret_cast<const byte*>(cr.bytecode), cr.bytecodeSize));
        compiler.freeResult(cr);
        return true;
    }
}
