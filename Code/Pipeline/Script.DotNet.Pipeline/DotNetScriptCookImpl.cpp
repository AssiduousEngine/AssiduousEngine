// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script.DotNet - the C# cook (see DotNetScriptCook.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module script.dotnet.pipeline;

import foundation.core;
import foundation.script;
import foundation.script.resource;
import foundation.script.dotnet;
import script.pipeline;
import pipeline.importer; // FileNameOf, FileStemOf

using namespace foundation::core;
using namespace foundation::script;

namespace pipeline
{
    namespace
    {
        constexpr StringView kLanguage = u8"csharp";

        bool IsIdentChar(utf8char c)
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_';
        }

        // The next space-separated word of `line` from `at`, `at` moved past it.
        StringView NextWord(StringView line, usize& at)
        {
            while (at < line.Size() && line[at] == u8' ')
                ++at;
            const usize begin = at;
            while (at < line.Size() && line[at] != u8' ')
                ++at;
            return line.SubStr(begin, at - begin);
        }

        String RestOf(StringView line, usize at)
        {
            if (at < line.Size() && line[at] == u8' ')
                ++at;
            String text;
            for (usize i = at; i < line.Size(); ++i)
            {
                if (line[i] == u8'\\' && i + 1 < line.Size() && line[i + 1] == u8'n')
                {
                    text.PushBack(u8'\n');
                    ++i;
                    continue;
                }
                text.PushBack(line[i]);
            }
            return text;
        }

        // Numbers separated by spaces, into `out` (as many as there are, up to `count`).
        usize Numbers(StringView text, f32* out, usize count)
        {
            usize found = 0;
            usize at = 0;
            while (found < count)
            {
                const StringView word = NextWord(text, at);
                if (word.IsEmpty())
                    break;
                const Optional<f64> value = ParseFloat(word);
                out[found++] = value.HasValue() ? static_cast<f32>(value.Value()) : 0.0f;
            }
            return found;
        }

        // A field's kind and default as the runtime spelled them.
        bool PropertyFrom(StringView name, StringView kind, StringView defaultText, ScriptPropertyDesc& out)
        {
            out.name = String(name);
            out.hash = ScriptPropertyNameHash(name);
            ScriptPropertyValue& value = out.defaultValue;
            if (kind == StringView(u8"float") || kind == StringView(u8"int"))
            {
                out.type = kind == StringView(u8"float") ? ScriptPropertyType::Float : ScriptPropertyType::Int;
                const Optional<f64> number = ParseFloat(defaultText);
                value.number = number.HasValue() ? number.Value() : 0.0;
            }
            else if (kind == StringView(u8"bool"))
            {
                out.type = ScriptPropertyType::Bool;
                value.boolean = defaultText == StringView(u8"true");
            }
            else if (kind == StringView(u8"string"))
            {
                out.type = ScriptPropertyType::String;
                value.text = String(defaultText);
            }
            else if (kind == StringView(u8"vec3"))
            {
                out.type = ScriptPropertyType::Vec3;
                f32 v[3] = {0.0f, 0.0f, 0.0f};
                (void)Numbers(defaultText, v, 3);
                value.vector = Float3{v[0], v[1], v[2]};
            }
            else if (kind == StringView(u8"color"))
            {
                out.type = ScriptPropertyType::Color;
                f32 c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                (void)Numbers(defaultText, c, 4);
                value.color = Color{c[0], c[1], c[2], c[3]};
            }
            else if (kind == StringView(u8"entity"))
            {
                out.type = ScriptPropertyType::Entity;
            }
            else
            {
                return false;
            }
            value.kind = out.type;
            return true;
        }

        ScriptPropertyDesc* FindProperty(ScriptAssemblyClass& c, StringView name)
        {
            for (ScriptPropertyDesc& p : c.properties)
                if (p.name.AsView() == name)
                    return &p;
            return nullptr;
        }

        // The folder a project builds in: one per project file, kept between builds so a build
        // after the first is incremental.
        String BuildFolderFor(StringView projectPath)
        {
            constexpr utf8char kDigits[] = u8"0123456789abcdef";
            const u64 hash = HashBytes(projectPath.Data(), projectPath.Size());
            String name(u8"csharp-build-");
            for (i32 shift = 60; shift >= 0; shift -= 4)
                name.PushBack(kDigits[(hash >> shift) & 0xFu]);
            return PathJoin(GetTempDirectory().AsView(), name.AsView());
        }

        bool WriteText(StringView path, StringView text)
        {
            (void)CreateDirectories(PathParent(path));
            return WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk();
        }

        class CSharpAssemblyCook final : public IScriptAssemblyCook
        {
        public:
            explicit CSharpAssemblyCook(IAllocator& allocator) : m_allocator(&allocator) {}

            [[nodiscard]] StringView ProjectExtension() const override { return u8"csproj"; }
            [[nodiscard]] StringView SourceExtension() const override { return u8"cs"; }
            [[nodiscard]] u32 CookVersion() const override { return 1; }

            [[nodiscard]] bool Build(StringView projectPath, StringView project, Span<const ScriptAssemblyFile> sources,
                                     CookScriptErrorSink& sink, ScriptAssemblySource& out) override
            {
                const String dotnetRoot = foundation::script::dotnet::FindDotNetRoot();
                if (dotnetRoot.IsEmpty())
                    return Refuse(sink, projectPath, u8"no .NET SDK found (set DOTNET_ROOT, or put dotnet on PATH)");
                const String runtime = EngineRuntimeAssemblyPath();
                if (!FileExists(runtime.AsView()))
                    return Refuse(sink, projectPath, Format(u8"the engine's managed runtime is not at {}", runtime).AsView());

                // The project and its sources, written where the build reads them: the sources
                // folder afresh (a removed file must not linger), the intermediates kept.
                const String folder = BuildFolderFor(projectPath);
                const String root = PathJoin(folder.AsView(), u8"src");
                (void)RemoveDirectoryRecursive(root.AsView());
                const String projectFile = PathJoin(root.AsView(), projectPath);
                if (!WriteText(projectFile.AsView(), project))
                    return Refuse(sink, projectPath, Format(u8"could not write the build's copy at {}", projectFile).AsView());
                for (const ScriptAssemblyFile& source : sources)
                    if (!WriteText(PathJoin(root.AsView(), source.path.AsView()).AsView(), source.text.AsView()))
                        return Refuse(sink, source.path.AsView(), u8"could not write the build's copy");

                const String output = PathJoin(folder.AsView(), u8"out");
                const String name(FileStemOf(FileNameOf(projectPath)));
                const String intermediates = Format(u8"-p:BaseIntermediateOutputPath={}/", PathJoin(folder.AsView(), u8"obj"));
                const String engineRuntime = Format(u8"-p:EngineRuntime={}", runtime);
                const String assemblyName = Format(u8"-p:AssemblyName={}", name);
                const StringView args[] = {u8"build", projectFile.AsView(), u8"-c", u8"Debug", u8"-o", output.AsView(),
                                           intermediates.AsView(), engineRuntime.AsView(), assemblyName.AsView(),
                                           u8"-p:DebugType=portable", u8"-nologo", u8"-v:q", u8"-clp:NoSummary",
                                           u8"--disable-build-servers"}; // none left resident after the cook
#if PLATFORM_WINDOWS
                const String dotnet = PathJoin(dotnetRoot.AsView(), u8"dotnet.exe");
#else
                const String dotnet = PathJoin(dotnetRoot.AsView(), u8"dotnet");
#endif
                const ProcessResult built = RunProcess(dotnet.AsView(), Span<const StringView>{args, sizeof(args) / sizeof(args[0])},
                                                       10u * 60u * 1000u);
                ReportBuildOutput(built.output.AsView(), root.AsView(), sink);
                if (!built.Ok())
                {
                    if (!HasError(sink))
                        return Refuse(sink, projectPath,
                                      built.timedOut ? StringView(u8"dotnet build took more than ten minutes")
                                                     : Format(u8"dotnet build failed (exit {})", built.exitCode).AsView());
                    return false;
                }

                const Result<Array<byte>> image = ReadFile(PathJoin(output.AsView(), Format(u8"{}.dll", name).AsView()).AsView());
                if (!image.HasValue())
                    return Refuse(sink, projectPath, u8"dotnet build wrote no assembly");
                out.image = image.Value();
                if (const Result<Array<byte>> symbols = ReadFile(PathJoin(output.AsView(), Format(u8"{}.pdb", name).AsView()).AsView());
                    symbols.HasValue())
                    out.symbols = symbols.Value();

                foundation::script::dotnet::DotNetRuntime* dotnetRuntime = foundation::script::dotnet::ProcessRuntime(*m_allocator);
                if (dotnetRuntime == nullptr)
                    return Refuse(sink, projectPath, u8"the managed runtime did not start, so the classes cannot be described");
                const Result<String> described = dotnetRuntime->DescribeAssembly(Span<const byte>{out.image.Data(), out.image.Size()});
                if (!described.HasValue())
                    return Refuse(sink, projectPath, dotnetRuntime->Failure());
                ParseAssemblyDescription(described.Value().AsView(), out, sink);
                return true;
            }

            [[nodiscard]] String NewProjectTemplate(StringView name) const override
            {
                // The runtime's path is this engine's, for an IDE's completion; the cook passes its own.
                return Format(u8"<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                              u8"  <!-- The game's C#: every .cs in this folder and below builds into one assembly. -->\n"
                              u8"  <PropertyGroup>\n"
                              u8"    <TargetFramework>net10.0</TargetFramework>\n"
                              u8"    <AssemblyName>{}</AssemblyName>\n"
                              u8"    <Nullable>enable</Nullable>\n"
                              u8"    <EngineRuntime Condition=\"'$(EngineRuntime)' == ''\">{}</EngineRuntime>\n"
                              u8"  </PropertyGroup>\n"
                              u8"  <ItemGroup>\n"
                              u8"    <Reference Include=\"Engine.Runtime\">\n"
                              u8"      <HintPath>$(EngineRuntime)</HintPath>\n"
                              u8"      <Private>false</Private>\n"
                              u8"    </Reference>\n"
                              u8"  </ItemGroup>\n"
                              u8"</Project>\n",
                              name, EngineRuntimeAssemblyPath());
            }

        private:
            static bool Refuse(CookScriptErrorSink& sink, StringView where, StringView why)
            {
                sink.OnError(ScriptError{ScriptErrorKind::Compile, where, -1, -1, why});
                return false;
            }

            static bool HasError(const CookScriptErrorSink& sink)
            {
                for (const CookScriptErrorSink::Entry& e : sink.errors)
                    if (e.kind != ScriptErrorKind::Warning)
                        return true;
                return false;
            }

            IAllocator* m_allocator; // the managed runtime's, when the cook starts it
        };

        class CSharpClassCook final : public IScriptLanguageCook
        {
        public:
            [[nodiscard]] String NewAssetTemplate(ScriptTier tier, StringView className) const override
            {
                const StringView name = className.IsEmpty() ? StringView(u8"NewBehavior") : className;
                switch (tier)
                {
                case ScriptTier::Level:
                    return String(u8"using Engine;\n\n"
                                  u8"// The per-scene script: one instance per scene, made with the scene (reserved name Level).\n"
                                  u8"public class Level\n{\n"
                                  u8"    readonly Scene m_scene;\n\n"
                                  u8"    public Level(Scene scene)\n    {\n        m_scene = scene;\n    }\n\n"
                                  u8"    public void onStart()\n    {\n        Log.info(\"Level started\");\n    }\n\n"
                                  u8"    // Gameplay dt; runs only while the scene simulates.\n"
                                  u8"    public void onUpdate(float dt)\n    {\n    }\n\n"
                                  u8"    public void onStop()\n    {\n    }\n}\n");
                case ScriptTier::Game:
                    return String(u8"using Engine;\n\n"
                                  u8"// The game orchestrator (mandatory name Game): one per run.\n"
                                  u8"public class Game\n{\n"
                                  u8"    // Runs once at start: load the opening scene here.\n"
                                  u8"    public void launch()\n    {\n        Log.info(\"Game launched\");\n    }\n\n"
                                  u8"    // Game-wide update. Per-scene logic belongs in a Level.\n"
                                  u8"    public void update(float dt)\n    {\n    }\n\n"
                                  u8"    public void exit()\n    {\n    }\n}\n");
                case ScriptTier::Behavior:
                default:
                    return Format(u8"using Engine;\n\n"
                                  u8"// A behaviour: attach it through a Script component's behaviour slot. The engine makes it\n"
                                  u8"// with its entity and calls its handlers by name (onStart, onUpdate, on<Event>). Every .cs\n"
                                  u8"// of the project builds into one assembly, so classes here can use each other.\n"
                                  u8"public class {}\n{{\n"
                                  u8"    readonly Entity m_entity;\n\n"
                                  u8"    // Public fields are the inspector's properties: float, int, bool, string, Float3, Color,\n"
                                  u8"    // Entity. [Tooltip(\"...\")] describes one; [Replicated] keeps it the same on every peer.\n"
                                  u8"    public float speed = 1.0f;\n\n"
                                  u8"    public {}(Entity entity)\n    {{\n        m_entity = entity;\n    }}\n\n"
                                  u8"    public void onStart()\n    {{\n        Log.info($\"{} started on {{m_entity.name()}}\");\n    }}\n\n"
                                  u8"    public void onUpdate(float dt)\n    {{\n"
                                  u8"        // Drift along +X at `speed` units a second.\n"
                                  u8"        Float3 p = m_entity.position();\n"
                                  u8"        m_entity.setPosition(p.x + speed * dt, p.y, p.z);\n    }}\n}}\n",
                                  name, name, name);
                }
            }

            [[nodiscard]] bool Cook(StringView, StringView assetName, StringView, CookScriptErrorSink& sink, ScriptClassSource&,
                                    IScriptIncludeReader*) override
            {
                sink.OnError(ScriptError{ScriptErrorKind::Compile, assetName, -1, -1,
                                         u8"a C# class builds with its project (a .csproj in its folder or one above)"});
                return false;
            }

            // A C# file compiles with its project, at cook; alone it is not checked. The class it
            // plays is still named, as a check names it.
            [[nodiscard]] bool Check(StringView source, StringView, StringView preferredClass, CookScriptErrorSink& sink, String& className,
                                     Span<const TypeId>, IScriptIncludeReader*) override
            {
                const Array<String> declared = ListCSharpClassNames(source);
                String problem;
                className = ChooseScriptClass(Span<const String>{declared.Data(), declared.Size()}, preferredClass, problem);
                if (!problem.IsEmpty())
                {
                    sink.OnError(ScriptError{ScriptErrorKind::Compile, StringView{}, -1, -1, problem.AsView()});
                    return false;
                }
                return true;
            }

            void ListIncludes(StringView, Array<String>&) const override {}
            void PrepareCheck() override {}
            [[nodiscard]] Array<ScriptApiType> DescribeApi() override { return {}; }
            [[nodiscard]] u32 CheckManagerBuildCount() const override { return 0; }
            [[nodiscard]] bool BuildsInAssembly() const override { return true; }

            [[nodiscard]] bool CookFromAssembly(StringView source, StringView assetName, StringView preferredClass,
                                                const ScriptAssemblySource& assembly, CookScriptErrorSink& sink,
                                                ScriptClassSource& out) override
            {
                const Array<String> declared = ListCSharpClassNames(source);
                String problem;
                const String className = ChooseScriptClass(Span<const String>{declared.Data(), declared.Size()}, preferredClass, problem);
                if (!problem.IsEmpty())
                {
                    sink.OnError(ScriptError{ScriptErrorKind::Compile, assetName, -1, -1, problem.AsView()});
                    return false;
                }
                out.language = String(kLanguage);
                out.sourceName = String(assetName);
                out.source = String(source);
                // A class the assembly does not describe (static, abstract, not public: none the engine
                // can make) is a helper the other files use, and so is a file with no class.
                const ScriptAssemblyClass* described = className.IsEmpty() ? nullptr : assembly.FindClass(className.AsView());
                if (described == nullptr)
                {
                    return true;
                }
                out.className = className;
                out.handlers = described->handlers;
                out.properties = described->properties;
                return true;
            }
        };
    }

    String EngineRuntimeAssemblyPath()
    {
        return PathJoin(foundation::script::dotnet::DefaultRuntimeFolder().AsView(), u8"Engine.Runtime.dll");
    }

    Array<String> ListCSharpClassNames(StringView source)
    {
        const String text = StripScriptComments(source);
        const StringView view = text.AsView();
        Array<String> found;
        const StringView keyword = u8"class";
        for (usize at = view.Find(keyword); at != StringView::kNotFound; at = view.Find(keyword, at + keyword.Size()))
        {
            // A whole word, followed by a name.
            if ((at > 0 && IsIdentChar(view[at - 1])) || at + keyword.Size() >= view.Size() || view[at + keyword.Size()] != u8' ')
                continue;
            usize begin = at + keyword.Size();
            while (begin < view.Size() && view[begin] == u8' ')
                ++begin;
            usize end = begin;
            while (end < view.Size() && IsIdentChar(view[end]))
                ++end;
            if (end > begin)
                found.PushBack(String(view.SubStr(begin, end - begin)));
        }
        return found;
    }

    void ParseAssemblyDescription(StringView text, ScriptAssemblySource& out, CookScriptErrorSink& sink)
    {
        ScriptAssemblyClass* current = nullptr;
        usize lineStart = 0;
        while (lineStart < text.Size())
        {
            usize lineEnd = text.Find(u8'\n', lineStart);
            if (lineEnd == StringView::kNotFound)
                lineEnd = text.Size();
            const StringView line = text.SubStr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            usize at = 0;
            const StringView what = NextWord(line, at);
            if (what == StringView(u8"class"))
            {
                ScriptAssemblyClass c;
                c.className = String(NextWord(line, at));
                out.classes.PushBack(Move(c));
                current = &out.classes.Back();
            }
            else if (what == StringView(u8"warning"))
            {
                const String message = RestOf(line, at);
                sink.OnError(ScriptError{ScriptErrorKind::Warning, StringView{}, -1, -1, message.AsView()});
            }
            else if (current == nullptr)
            {
                continue;
            }
            else if (what == StringView(u8"method"))
            {
                current->handlers.PushBack(String(NextWord(line, at)));
            }
            else if (what == StringView(u8"field"))
            {
                const StringView name = NextWord(line, at);
                const StringView kind = NextWord(line, at);
                const String defaultText = RestOf(line, at);
                ScriptPropertyDesc property;
                if (PropertyFrom(name, kind, defaultText.AsView(), property))
                    current->properties.PushBack(Move(property));
            }
            else if (what == StringView(u8"tooltip"))
            {
                const StringView name = NextWord(line, at);
                if (ScriptPropertyDesc* property = FindProperty(*current, name))
                    property->description = RestOf(line, at);
            }
            else if (what == StringView(u8"replicated"))
            {
                if (ScriptPropertyDesc* property = FindProperty(*current, NextWord(line, at)))
                    property->replicated = true;
            }
        }
    }

    void ReportBuildOutput(StringView output, StringView root, CookScriptErrorSink& sink)
    {
        Array<String> seen; // MSBuild repeats each problem in its summary
        usize lineStart = 0;
        while (lineStart < output.Size())
        {
            usize lineEnd = output.Find(u8'\n', lineStart);
            if (lineEnd == StringView::kNotFound)
                lineEnd = output.Size();
            StringView line = output.SubStr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            while (!line.IsEmpty() && (line[line.Size() - 1] == u8'\r' || line[line.Size() - 1] == u8' '))
                line = line.SubStr(0, line.Size() - 1);
            ScriptErrorKind kind = ScriptErrorKind::Compile;
            usize marker = line.Find(StringView(u8"): error "));
            if (marker == StringView::kNotFound)
            {
                marker = line.Find(StringView(u8"): warning "));
                kind = ScriptErrorKind::Warning;
            }
            if (marker == StringView::kNotFound)
                continue;
            bool known = false;
            for (const String& s : seen)
                known = known || s.AsView() == line;
            if (known)
                continue;
            seen.PushBack(String(line));
            // "file(line,col)": the position, then the file before it.
            const usize open = line.FindLast(u8'(', marker);
            if (open == StringView::kNotFound)
                continue;
            StringView file = line.SubStr(0, open);
            i32 row = -1;
            i32 column = -1;
            {
                const StringView position = line.SubStr(open + 1, marker - open - 1);
                const usize comma = position.Find(u8',');
                const Optional<f64> r = ParseFloat(comma == StringView::kNotFound ? position : position.SubStr(0, comma));
                row = r.HasValue() ? static_cast<i32>(r.Value()) : -1;
                if (comma != StringView::kNotFound)
                {
                    const Optional<f64> c = ParseFloat(position.SubStr(comma + 1, position.Size() - comma - 1));
                    column = c.HasValue() ? static_cast<i32>(c.Value()) : -1;
                }
            }
            if (file.Size() > root.Size() && file.SubStr(0, root.Size()) == root)
                file = file.SubStr(root.Size() + 1, file.Size() - root.Size() - 1);
            // The message: past "error CODE: ", without MSBuild's trailing "[project]".
            const usize textAt = marker + (kind == ScriptErrorKind::Warning ? 11u : 9u);
            StringView message = line.SubStr(textAt, line.Size() - textAt);
            if (const usize bracket = message.FindLast(u8'['); bracket != StringView::kNotFound && bracket > 0)
                message = message.SubStr(0, bracket - 1);
            sink.OnError(ScriptError{kind, file, row, column, message});
        }
    }

    void RegisterDotNetScriptCook(IAllocator& allocator)
    {
        foundation::script::dotnet::RegisterDotNetScriptBackend();
        ScriptLanguageCookRegistry::Get().Register(String(kLanguage), MakeUnique<CSharpClassCook>(allocator));
        ScriptAssemblyCookRegistry::Get().Register(String(kLanguage), MakeUnique<CSharpAssemblyCook>(allocator, allocator));
    }
}
