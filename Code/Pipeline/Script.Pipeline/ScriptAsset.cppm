// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script - the `script.pipeline` module (tooling).
//
// Source-side script authoring and cook, backend-neutral: no language syntax lives here.
//   * ScriptClassAsset (pipeline::Asset): the copied script file and its language id
//     (defaulted from the imported file's extension). The asset is just source bytes, a
//     language id and cooked metadata; nothing language-specific.
//   * IScriptLanguageCook: the per-language cook service. Each language library provides
//     one (AngelScript: angelscript.pipeline) and registers it into
//     ScriptLanguageCookRegistry, keyed by languageId (as ScriptBackendRegistry is). A cook
//     compile-checks and harvests metadata; the New-Asset starter is its NewAssetTemplate().
//   * ScriptClassAssetBuilder: a thin shell that resolves the cook by the asset's language
//     and delegates NewAssetTemplate / Cook. It never names a language or a backend type.
//   * ScriptFileImporter: drop-import for any extension a registered backend claims; no
//     options dialog.
//   * Shared cook conventions (not language syntax): the on<Upper>(...) handler scan
//     (ScanScriptHandlers), the C-family comment stripper (StripScriptComments), the top-level
//     `class Name` scan (ListScriptClassNames), the choice of the class a script plays
//     (ChooseBehaviorClass), and the shared `startCoroutine(` surface detection
//     (ScriptReferencesCoroutineStart). Every cook reuses these.
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module script.pipeline;

import foundation.core;
import pipeline.core;
import foundation.vfs; // SourcePath: an included file declared as a cook input
import pipeline.importer;
import foundation.content;
import foundation.script;
import foundation.script.resource;

using namespace foundation::core;
using namespace foundation::script;

export namespace pipeline{
    namespace content = foundation::content;

    // Source asset: the script file, the backend that compiles it, and the class it plays.
    class ScriptClassAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ScriptClassAsset, pipeline::Asset)
    public:
        String language; // backend id ("angelscript"), defaulted from the file extension

        [[nodiscard]] bool IsCode() const override { return true; }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "language", language);
            // Version 1 recorded the class it played; the class is now the one deriving from a
            // kind, so an asset of that version has the name read past.
            if (ar.Mode() == SerializeMode::Read && ar.Version() == 1)
            {
                String className;
                foundation::core::Serialize(ar, "className", className);
            }
        }
    };

    // Source asset: a project file (a C# .csproj) whose folder's sources of its language, every one
    // under it, build together into one assembly; each class asset there names the assembly.
    class ScriptAssemblyAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ScriptAssemblyAsset, pipeline::Asset)
    public:
        String language; // backend id ("csharp"), from the project file's extension

        [[nodiscard]] bool IsCode() const override { return true; }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName: the project file
            foundation::core::Serialize(ar, "language", language);
        }
    };

    // ---- shared cook helpers (unit-testable without a project; not language syntax) ----

    /// Every top-level `class Name` in `source`, in order; empty when it declares none (a
    /// utility module). Line and block comments are ignored. `class Name` is the C-family
    /// convention (AngelScript's); Luau lists its own.
    [[nodiscard]] inline Array<String> ListScriptClassNames(StringView source);

    /// A class a script declares, with the kind it derives from (None: it derives from no kind).
    struct DeclaredScriptClass
    {
        String name;
        foundation::script::BehaviorKind kind = foundation::script::BehaviorKind::None;
    };

    /// The behavior class a script plays: the one class among `declared` that derives from a kind,
    /// its kind in `kind`. A script whose classes derive from none is a module of helpers: empty,
    /// kind None, no problem. Several that derive from a kind is no guess: empty, with `problem`
    /// naming them, since a file holds one behavior class.
    [[nodiscard]] inline String ChooseBehaviorClass(Span<const DeclaredScriptClass> declared,
                                                    foundation::script::BehaviorKind& kind, String& problem);

    /// The declared lifecycle/event/message handlers out of the `on<Upper>(...)` naming
    /// convention, by source scan (comments stripped). Both languages use it, so it is a
    /// shared cook helper, not language syntax.
    [[nodiscard]] inline Array<String> ScanScriptHandlers(StringView source);

    /// Strips `//` line comments and `/* */` block comments (string literals respected).
    /// C-family; both languages use it.
    [[nodiscard]] inline String StripScriptComments(StringView source);

    /// True when the (comment-stripped) source references the shared `startCoroutine(`
    /// coroutine surface, a convention shared by every backend's coroutine facade rather than
    /// language syntax. A language whose coroutine opt-in has additional markers layers
    /// them on top inside its own cook.
    [[nodiscard]] inline bool ScriptReferencesCoroutineStart(StringView source);

    namespace detail
    {
        [[nodiscard]] inline bool IsIdentChar(utf8char c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') ||
                   (c >= u8'0' && c <= u8'9') || c == u8'_';
        }

        // First occurrence of `needle` in `haystack` (byte scan).
        [[nodiscard]] inline bool Contains(StringView haystack, StringView needle) noexcept
        {
            if (needle.IsEmpty() || needle.Size() > haystack.Size())
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
        }

        [[nodiscard]] inline usize CountNewlines(StringView text) noexcept
        {
            usize n = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] == u8'\n')
                {
                    ++n;
                }
            }
            return n;
        }
    }

    inline String StripScriptComments(StringView source)
    {
        String out;
        out.Reserve(source.Size());
        bool inString = false;
        bool inLineComment = false;
        bool inBlockComment = false;
        for (usize i = 0; i < source.Size(); ++i)
        {
            const utf8char c = source[i];
            const utf8char next = (i + 1 < source.Size()) ? source[i + 1] : utf8char(0);
            if (inLineComment)
            {
                if (c == u8'\n')
                {
                    inLineComment = false;
                    out.PushBack(c);
                }
                continue;
            }
            if (inBlockComment)
            {
                if (c == u8'*' && next == u8'/')
                {
                    inBlockComment = false;
                    ++i;
                }
                else if (c == u8'\n')
                {
                    out.PushBack(c);
                } // keep line numbers stable-ish
                continue;
            }
            if (inString)
            {
                if (c == u8'\\')
                {
                    out.PushBack(c);
                    if (next != 0)
                    {
                        out.PushBack(next);
                        ++i;
                    }
                    continue;
                }
                if (c == u8'"')
                {
                    inString = false;
                }
                out.PushBack(c);
                continue;
            }
            if (c == u8'"')
            {
                inString = true;
                out.PushBack(c);
                continue;
            }
            if (c == u8'/' && next == u8'/')
            {
                inLineComment = true;
                ++i;
                continue;
            }
            if (c == u8'/' && next == u8'*')
            {
                inBlockComment = true;
                ++i;
                continue;
            }
            out.PushBack(c);
        }
        return out;
    }

    inline Array<String> ListScriptClassNames(StringView source)
    {
        const String stripped = StripScriptComments(source);
        const StringView text = stripped.AsView();
        Array<String> found;
        bool lineStart = true;
        for (usize i = 0; i < text.Size(); ++i)
        {
            const utf8char c = text[i];
            if (c == u8'\n')
            {
                lineStart = true;
                continue;
            }
            if (lineStart && (c == u8' ' || c == u8'\t'))
            {
                continue;
            }
            if (lineStart)
            {
                lineStart = false;
                const StringView keyword = u8"class ";
                if (i + keyword.Size() < text.Size() && text.SubStr(i, keyword.Size()) == keyword)
                {
                    usize begin = i + keyword.Size();
                    while (begin < text.Size() && text[begin] == u8' ')
                    {
                        ++begin;
                    }
                    usize end = begin;
                    while (end < text.Size() && detail::IsIdentChar(text[end]))
                    {
                        ++end;
                    }
                    if (end > begin)
                    {
                        found.PushBack(String(text.SubStr(begin, end - begin)));
                    }
                }
            }
        }
        return found;
    }

    inline String ChooseBehaviorClass(Span<const DeclaredScriptClass> declared, foundation::script::BehaviorKind& kind,
                                      String& problem)
    {
        problem = String{};
        kind = foundation::script::BehaviorKind::None;
        const DeclaredScriptClass* chosen = nullptr;
        String names;
        usize count = 0;
        for (const DeclaredScriptClass& entry : declared)
        {
            if (entry.kind == foundation::script::BehaviorKind::None)
            {
                continue;
            }
            chosen = &entry;
            ++count;
            if (!names.IsEmpty())
            {
                names.Append(u8", ");
            }
            names.Append(entry.name.AsView());
        }
        if (count > 1)
        {
            problem = Format(u8"it declares the behavior classes {}: a script holds one, so move the others "
                             u8"to scripts of their own",
                             names.AsView());
            return {};
        }
        if (chosen == nullptr)
        {
            return {};
        }
        kind = chosen->kind;
        return chosen->name;
    }

    inline Array<String> ScanScriptHandlers(StringView source)
    {
        // Any method declared as `on<Upper>...(` is a dispatchable handler: the fixed
        // lifecycle set (onStart/onUpdate/...), the reserved event handlers
        // (onContactBegin, onTriggerEnter, ...), and user message handlers reached by
        // `entity.send("heal", ...)`, which calls `onHeal(...)`. The runtime dispatch gate is
        // BehaviorClass::HasHandler, so harvesting the whole convention here is what makes
        // custom messages and events cost nothing per frame. A lowercase letter after
        // `on` (e.g. `onlyOnce`) is not a handler; nor is a getter (`onFoo {`, no parens):
        // handlers take an argument list.
        const String stripped = StripScriptComments(source);
        const StringView text = stripped.AsView();
        Array<String> found;
        for (usize i = 0; i + 2 < text.Size(); ++i)
        {
            if (text[i] != u8'o' || text[i + 1] != u8'n')
            {
                continue;
            }
            if (i > 0 && detail::IsIdentChar(text[i - 1]))
            {
                continue;
            }
            const utf8char third = text[i + 2];
            if (third < u8'A' || third > u8'Z')
            {
                continue;
            } // "on" then an uppercase letter only
            usize end = i + 2;
            while (end < text.Size() && detail::IsIdentChar(text[end]))
            {
                ++end;
            }
            usize after = end;
            while (after < text.Size() && (text[after] == u8' ' || text[after] == u8'\t'))
            {
                ++after;
            }
            if (after >= text.Size() || text[after] != u8'(')
            {
                continue;
            } // method, not getter
            const StringView name = text.SubStr(i, end - i);
            bool duplicate = false;
            for (const String& existing : found)
            {
                if (existing.AsView() == name)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                found.PushBack(String(name));
            }
        }
        return found;
    }

    inline bool ScriptReferencesCoroutineStart(StringView source)
    {
        const String stripped = StripScriptComments(source);
        return detail::Contains(stripped.AsView(), u8"startCoroutine(");
    }

    // ---- shared property-harvest record parsing (not language syntax) ----
    //
    // Every cook that harvests inspector properties (AngelScript's declared-property probe,
    // Luau's construct-and-walk probe) emits the same record string so the parse is shared:
    //   name \x1F typeString \x1F default \x1F description   (records joined by \x1E)
    // where default is "~" (none), "n:<num>", "b:true|false", "s:<text>" or "l:<a,b,c[,d]>".
    // The language-specific part is only the probe that produces this string; the parse below
    // is a shared cook convention, like ScanScriptHandlers.

    /// Parses "a,b,c[,d]" into up to 4 floats; returns the count parsed. Harvest payloads are
    /// exponent-free, so this is a minimal sign/digits/dot parse.
    [[nodiscard]] inline u32 ParseFloatList(StringView text, f32 (&out)[4])
    {
        u32 count = 0;
        usize begin = 0;
        for (usize i = 0; i <= text.Size() && count < 4; ++i)
        {
            if (i == text.Size() || text[i] == u8',')
            {
                const StringView piece = text.SubStr(begin, i - begin);
                begin = i + 1;
                if (piece.IsEmpty())
                {
                    continue;
                }
                f64 value = 0.0;
                f64 scale = 1.0;
                bool negative = false;
                bool afterDot = false;
                for (usize j = 0; j < piece.Size(); ++j)
                {
                    const utf8char c = piece[j];
                    if (j == 0 && c == u8'-')
                    {
                        negative = true;
                        continue;
                    }
                    if (c == u8'.')
                    {
                        afterDot = true;
                        continue;
                    }
                    if (c < u8'0' || c > u8'9')
                    {
                        continue;
                    }
                    if (afterDot)
                    {
                        scale *= 0.1;
                        value += (c - u8'0') * scale;
                    }
                    else
                    {
                        value = value * 10.0 + (c - u8'0');
                    }
                }
                out[count++] = static_cast<f32>(negative ? -value : value);
            }
        }
        return count;
    }

    /// One harvest record as a property desc. Record layout: name \x1F typeString \x1F default
    /// \x1F description. Returns false (with outError set) on a malformed record or unknown type.
    [[nodiscard]] inline bool ParseHarvestRecord(StringView record, BehaviorProperty& out,
                                                 String& outError)
    {
        StringView fields[4];
        u32 fieldCount = 0;
        usize begin = 0;
        for (usize i = 0; i <= record.Size() && fieldCount < 4; ++i)
        {
            if (i == record.Size() || record[i] == utf8char(0x1F))
            {
                fields[fieldCount++] = record.SubStr(begin, i - begin);
                begin = i + 1;
            }
        }
        if (fieldCount < 2 || fields[0].IsEmpty())
        {
            outError = String(u8"malformed property record");
            return false;
        }
        out.name = String(fields[0]);
        out.hash = BehaviorPropertyNameHash(fields[0]);
        out.description = fieldCount > 3 ? String(fields[3]) : String{};
        if (!ParseScriptPropertyType(fields[1], out.type, out.assetType))
        {
            outError = String(u8"property '");
            outError += fields[0];
            outError += u8"' has unknown type '";
            outError += fields[1];
            outError +=
                u8"' (valid: float, int, bool, string, color, vec3, entity, asset:<TypeName>)";
            return false;
        }

        // Default value: typed from the serialized payload; "~" is the type's default.
        BehaviorPropertyValue& value = out.defaultValue;
        value.kind = out.type;
        const StringView payload = fieldCount > 2 ? fields[2] : StringView(u8"~");
        if (payload == u8"~" || payload.Size() < 2)
        {
            return true;
        }
        const utf8char tag = payload[0];
        const StringView body = payload.SubStr(2, payload.Size() - 2);
        switch (out.type)
        {
        case BehaviorPropertyType::Float:
        case BehaviorPropertyType::Int:
            if (tag == u8'n')
            {
                f32 numbers[4] = {};
                if (ParseFloatList(body, numbers) > 0)
                {
                    value.number = static_cast<f64>(numbers[0]);
                    if (out.type == BehaviorPropertyType::Int)
                    {
                        value.number = static_cast<f64>(static_cast<i64>(value.number));
                    }
                }
            }
            break;
        case BehaviorPropertyType::Bool:
            value.boolean = (tag == u8'b' && body == u8"true");
            break;
        case BehaviorPropertyType::String:
            if (tag == u8's')
            {
                value.text = String(body);
            }
            break;
        case BehaviorPropertyType::Color:
            if (tag == u8'l')
            {
                f32 numbers[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                const u32 parsed = ParseFloatList(body, numbers);
                if (parsed >= 3)
                {
                    value.color =
                        Color{numbers[0], numbers[1], numbers[2], parsed >= 4 ? numbers[3] : 1.0f};
                }
            }
            break;
        case BehaviorPropertyType::Vec3:
            if (tag == u8'l')
            {
                f32 numbers[4] = {};
                if (ParseFloatList(body, numbers) >= 3)
                {
                    value.vector = Float3{numbers[0], numbers[1], numbers[2]};
                }
            }
            break;
        case BehaviorPropertyType::Entity:
        case BehaviorPropertyType::Asset:
            // Only null defaults are expressible in script; guids come from overrides.
            break;
        case BehaviorPropertyType::None:
        default:
            break;
        }
        return true;
    }

    // ---- cook error plumbing (shared by every cook service) ----

    // Captures compile/runtime errors during a cook's compile/harvest (ScriptError already
    // carries the file and line the cook error report needs).
    class CookScriptErrorSink final : public IScriptErrorHandler
    {
    public:
        struct Entry
        {
            ScriptErrorKind kind;
            String module;
            i32 line;
            i32 column;
            String message;
        };
        Array<Entry> errors;

        void OnError(const ScriptError& error) override
        {
            Entry entry{error.kind, String(error.module), error.line, error.column,
                        String(error.message)};
            errors.PushBack(Move(entry));
        }
    };

    /// A cook problem past the compile (a property the runtime could not fill, a replicated list
    /// naming no property), reported with the compile errors so whoever asked for the cook
    /// (script_validate, the asset cook) reads why, and logged. Shared by every cook.
    inline void ReportCookProblem(CookScriptErrorSink& sink, StringView assetName, const String& message)
    {
        LOG_ERROR(u8"Script", u8"'{}': {} - cook failed", assetName, message);
        sink.OnError(ScriptError{ScriptErrorKind::Compile, assetName, -1, -1, message.AsView()});
    }

    /// Logs a cook's captured compile/runtime errors as cook errors with the asset's
    /// file/line. `preludeLines` is the line count any language framing injected ahead of
    /// the user source, subtracted back so reported lines match the authored file. Shared
    /// by every cook (the reporting format is not language-specific).
    inline void ReportScriptCookErrors(StringView fileName, const CookScriptErrorSink& sink,
                                       i32 preludeLines = 0)
    {
        bool reported = false;
        for (const CookScriptErrorSink::Entry& e : sink.errors)
        {
            const i32 line = e.line > preludeLines ? e.line - preludeLines : e.line;
            const StringView file = e.module.IsEmpty() ? fileName : e.module.AsView();
            if (e.kind == ScriptErrorKind::Warning)
            {
                LogScriptLine(LogLevel::Warning, file, line, e.column, e.message.AsView()); // not why it failed
                continue;
            }
            LogScriptLine(LogLevel::Error, file, line, e.column, Format(u8"{} - cook failed", e.message).AsView());
            reported = true;
        }
        if (!reported)
        {
            LOG_ERROR(u8"Script", u8"'{}': compile failed - cook failed", fileName);
        }
    }

    // ---- the per-language cook service ----

    /// A language's cook: the compile-check, the metadata harvest and the New-Asset starter. The
    /// only place a language's specifics live on the cook side; the neutral builder
    /// resolves one by languageId and delegates. Implemented per language in its own
    /// library (script.<lang>.pipeline), registered via ScriptLanguageCookRegistry.
    /// A script manager for `languageId` with the engine's API registered: the core types, the
    /// facades and every reflected type but `hidden`, as the runtime registers them, so a script
    /// checks and cooks exactly as it runs. Null (logged, naming `forWhat`) when no backend is
    /// registered for the language.
    [[nodiscard]] RefPtr<foundation::script::IScriptManager> MakeCheckManager(IAllocator& allocator, StringView languageId,
                                                                              StringView forWhat, Span<const TypeId> hidden);

    /// The script manager a language's checks share. Registering the API is nearly all of a
    /// check's time (about half a second), compiling a few milliseconds, so it is made once, on
    /// first use, and again only when the registered types change (TypeRegistry::Generation: a
    /// plugin loaded or unloaded). Its lock serialises the checks, which an agent's tool may run
    /// from another thread. Each cook owns one.
    class CachedCheckManager
    {
    public:
        CachedCheckManager(IAllocator& allocator, StringView languageId) : m_allocator(&allocator), m_language(languageId, allocator) {}

        /// Runs `check` on the manager, under the lock. False when none could be made.
        [[nodiscard]] bool Run(const Function<bool(foundation::script::IScriptManager&)>& check);
        /// Makes the manager now if it is missing or out of date (for a worker, ahead of the first check).
        void Prepare();
        /// The engine's API, described for completion and the API browser.
        [[nodiscard]] Array<foundation::script::ScriptApiType> DescribeApi();
        /// The engine's API as the declarations file an outside editor reads.
        [[nodiscard]] foundation::script::ScriptDeclarations Declarations();
        /// How many times the manager has been made.
        [[nodiscard]] u32 BuildCount() const noexcept { return m_buildCount; }

    private:
        // The manager, made again if the registered types changed since; the caller holds m_lock.
        [[nodiscard]] foundation::script::IScriptManager* Current();

        IAllocator* m_allocator; // the manager's
        String m_language;
        Mutex m_lock;
        RefPtr<foundation::script::IScriptManager> m_manager;
        u64 m_generation = 0;
        u32 m_buildCount = 0;
        Array<foundation::script::ScriptApiType> m_api;
        u64 m_apiGeneration = ~u64(0);
    };

    /// Where a script's `#include "<include>"` points: relative to the folder of `from` (the file
    /// that includes it, a path in the project's sources), with "." and ".." resolved. False for an
    /// absolute path, a drive or scheme, or one that would leave the sources.
    [[nodiscard]] bool ResolveScriptInclude(StringView from, StringView include, String& out);

    /// Reads the files a script includes, through whatever its caller reads the project's sources
    /// with (the cook's mount, the editor's project). `path` is resolved (ResolveScriptInclude).
    class IScriptIncludeReader
    {
    public:
        virtual ~IScriptIncludeReader() = default;
        [[nodiscard]] virtual bool Read(StringView path, String& out) = 0;
    };

    /// Included files read from a folder on disk: a project's sources root (the editor's check,
    /// an agent's validation), where the cook's builder reads through its mount instead.
    class FolderIncludeReader final : public IScriptIncludeReader
    {
    public:
        /// Reads includes from the project's sources folder `root`, and those starting Code/ from its
        /// code folder `codeRoot` when given.
        explicit FolderIncludeReader(StringView root, StringView codeRoot = {}) : m_root(root), m_codeRoot(codeRoot) {}
        [[nodiscard]] bool Read(StringView path, String& out) override
        {
            if (m_root.IsEmpty())
            {
                return false;
            }
            Result<Array<byte>> bytes = ReadFile(foundation::vfs::SourceFileOnDisk(m_root.AsView(), m_codeRoot.AsView(), path).AsView());
            if (!bytes.HasValue())
            {
                return false;
            }
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            return true;
        }

    private:
        String m_root;
        String m_codeRoot;
    };

    class IScriptLanguageCook
    {
    public:
        virtual ~IScriptLanguageCook() = default;

        /// The New-Asset starter source for a behavior of `kind` (Entity, Scene or Run), its class
        /// `className` (a valid identifier: ClassNameFor makes one; empty is "NewBehavior").
        [[nodiscard]] virtual String NewAssetTemplate(foundation::script::BehaviorKind kind,
                                                      StringView className) const = 0;

        /// A cook-fingerprint contribution beyond the shared builder Version: a bytecode-emitting
        /// cook returns its compiler version here so a vendor bump recooks (bytecode is
        /// version-locked). Default 0: a source-only cook contributes nothing.
        [[nodiscard]] virtual u32 CookVersion() const { return 0; }

        /// Compile-check `source` (named `assetName` for error reporting) and harvest its
        /// metadata into `out` (language, className, handlers, usesCoroutines, and any
        /// property metadata the language supports); report cook errors through `sink`;
        /// return success. The class is ChooseBehaviorClass's over what the source declares (the
        /// one deriving from a kind); two that derive from one fail the cook. On failure the
        /// builder does not write, so the last good cooked record survives (live instances keep
        /// running the old class).
        /// The files a language includes are read through `includes` (null: none can be read, so a
        /// script that includes fails), and recorded in `out.includes`.
        [[nodiscard]] virtual bool Cook(StringView source, StringView assetName, CookScriptErrorSink& sink,
                                        BehaviorClassSource& out, IScriptIncludeReader* includes = nullptr) = 0;

        /// Compile-check `source` as Cook would, without running any of it and without the
        /// harvest: errors to `sink`, the class it plays (chosen as Cook chooses it; empty when it
        /// declares none) to `className`. The surface is every
        /// registered type less `hidden` (a platform without some plugins: their types), and a
        /// use of a hidden type is an error. For checking a script as it is typed, and for each
        /// platform it ships to. Returns whether it compiled.
        [[nodiscard]] virtual bool Check(StringView source, StringView assetName, CookScriptErrorSink& sink,
                                         String& className,
                                         Span<const TypeId> hidden, IScriptIncludeReader* includes = nullptr) = 0;

        /// The paths `source`'s include directives name, as written, in order (none for a language
        /// without one): the builder resolves them into the files a cook depends on.
        virtual void ListIncludes(StringView source, Array<String>& out) const = 0;

        /// Builds what Check uses before the first check needs it, so a project's first script
        /// page does not wait for it. Callable from a worker thread.
        virtual void PrepareCheck() = 0;

        /// The API a script checked here can use (every registered type), described for
        /// completion and the API browser.
        [[nodiscard]] virtual Array<foundation::script::ScriptApiType> DescribeApi() = 0;

        /// The same API as the declarations file an outside editor reads to complete and check
        /// the language (IScriptManager::Declarations). Empty for a language without one.
        [[nodiscard]] virtual foundation::script::ScriptDeclarations Declarations() { return {}; }

        /// How many times Check has made the manager it keeps for checks: once, and again
        /// each time the registered types change. 0 for a language whose every check builds its own.
        [[nodiscard]] virtual u32 CheckManagerBuildCount() const = 0;

        /// Whether the language's classes build together into their project's assembly (C#):
        /// the class builder then cooks a class with CookFromAssembly, from the assembly the
        /// project's build described, and not with Cook.
        [[nodiscard]] virtual bool BuildsInAssembly() const { return false; }

        /// Cooks the class `source` plays (chosen as Cook chooses it) from `assembly`, its
        /// project's built assembly: its handlers and properties into `out`. Errors to `sink`.
        [[nodiscard]] virtual bool CookFromAssembly(StringView source, StringView assetName,
                                                    const ScriptAssemblySource& assembly, CookScriptErrorSink& sink,
                                                    BehaviorClassSource& out)
        {
            (void)source;
            (void)assetName;
            (void)assembly;
            (void)sink;
            (void)out;
            return false;
        }
    };

    /// One source file of a project assembly: its path in the sources and its text.
    struct ScriptAssemblyFile
    {
        String path;
        String text;
    };

    /// A language's project build (C#): the project file and every source of the language under
    /// its folder, built into one assembly with every public class described. Registered in
    /// ScriptAssemblyCookRegistry by language, beside the language's class cook.
    class IScriptAssemblyCook
    {
    public:
        virtual ~IScriptAssemblyCook() = default;

        /// The project file's extension, lowercase without the dot ("csproj").
        [[nodiscard]] virtual StringView ProjectExtension() const = 0;
        /// The sources' extension ("cs").
        [[nodiscard]] virtual StringView SourceExtension() const = 0;
        /// Folded into the builder's version: a toolchain change rebuilds.
        [[nodiscard]] virtual u32 CookVersion() const { return 0; }

        /// Builds `projectPath` (its text `project`) with `sources` into `out`: the image, its
        /// symbols, every public class's description. Errors (each at its file and line) to `sink`.
        [[nodiscard]] virtual bool Build(StringView projectPath, StringView project, Span<const ScriptAssemblyFile> sources,
                                         CookScriptErrorSink& sink, ScriptAssemblySource& out) = 0;

        /// The project file a project named `name` starts with.
        [[nodiscard]] virtual String NewProjectTemplate(StringView name) const = 0;

        /// Files this machine's tools read beside a project (an IDE's path to the engine), each a
        /// path relative to the project's folder and its text. Written when the project is made and
        /// at every cook, they keep the project file itself free of any one machine's paths; never
        /// an input to the build, and kept out of version control. None by default.
        [[nodiscard]] virtual Array<ScriptAssemblyFile> LocalFiles() const { return {}; }
    };

    class ScriptAssemblyCookRegistry
    {
    public:
        // Not inline (ScriptAssetImpl.cpp): one registry across shared libraries.
        [[nodiscard]] static ScriptAssemblyCookRegistry& Get();

        /// Idempotent by languageId.
        void Register(String languageId, UniquePtr<IScriptAssemblyCook> cook)
        {
            for (Entry& existing : m_cooks)
                if (existing.languageId == languageId)
                {
                    existing.cook = Move(cook);
                    return;
                }
            m_cooks.PushBack(Entry{Move(languageId), Move(cook)});
        }

        /// Removes `languageId`'s cook (the plugin that registered it unloading); false when none
        /// is registered.
        bool Unregister(StringView languageId)
        {
            for (usize i = 0; i < m_cooks.Size(); ++i)
            {
                if (m_cooks[i].languageId == languageId)
                {
                    m_cooks.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] IScriptAssemblyCook* FindByLanguage(StringView languageId)
        {
            for (Entry& entry : m_cooks)
                if (entry.languageId == languageId)
                    return entry.cook.Get();
            return nullptr;
        }

        /// The language whose project files have `extension` (lowercase, no dot); empty for none.
        [[nodiscard]] StringView LanguageOfProject(StringView extension) const
        {
            for (const Entry& entry : m_cooks)
                if (entry.cook.Get() != nullptr && entry.cook->ProjectExtension() == extension)
                    return entry.languageId.AsView();
            return StringView{};
        }

        [[nodiscard]] u32 CombinedCookVersion() const
        {
            u32 total = 0;
            for (const Entry& entry : m_cooks)
                if (entry.cook.Get() != nullptr)
                    total += entry.cook->CookVersion();
            return total;
        }

    private:
        struct Entry
        {
            String languageId;
            UniquePtr<IScriptAssemblyCook> cook;
        };
        Array<Entry> m_cooks;
    };

    /// The project assembly a `language` source file at `file` (a path in the sources) builds into:
    /// the ScriptAssemblyAsset of that language whose project file's folder holds `file`, the
    /// nearest when projects nest. Null when no project holds it. Defined in ScriptAssetImpl.cpp.
    [[nodiscard]] content::Instance* FindScriptAssemblyFor(content::IContentDatabase& sourceDb, StringView language,
                                                           StringView file);

    /// Every file under `folder` (a path in the sources; empty for the root) whose extension is
    /// `extension`, through `sources`, sorted; build output folders (bin, obj) are passed over.
    /// Defined in ScriptAssetImpl.cpp.
    [[nodiscard]] Array<String> ListSourcesUnder(foundation::vfs::IFileSystem& sources, StringView folder, StringView extension);

    /// The cook registry (shaped like ScriptBackendRegistry): a language library registers its
    /// cook here, keyed by languageId; the builder resolves through it, never by type.
    class ScriptLanguageCookRegistry
    {
    public:
        // Not inline (defined in ScriptAssetImpl.cpp): language libraries register their cooks
        // and the builder resolves from another library, so in a shared-library build a
        // per-library copy of the registry would find no cook for the language.
        [[nodiscard]] static ScriptLanguageCookRegistry& Get();

        /// Idempotent by languageId (a re-register replaces the cook, which suits hot reload).
        void Register(String languageId, UniquePtr<IScriptLanguageCook> cook)
        {
            for (Entry& existing : m_cooks)
            {
                if (existing.languageId == languageId)
                {
                    existing.cook = Move(cook);
                    return;
                }
            }
            Entry entry;
            entry.languageId = Move(languageId);
            entry.cook = Move(cook);
            m_cooks.PushBack(Move(entry));
        }

        /// Removes `languageId`'s cook (the plugin that registered it unloading); false when none
        /// is registered.
        bool Unregister(StringView languageId)
        {
            for (usize i = 0; i < m_cooks.Size(); ++i)
            {
                if (m_cooks[i].languageId == languageId)
                {
                    m_cooks.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }

        /// The languages a cook is registered for, in registration order: what a project can write
        /// scripts in.
        [[nodiscard]] Array<String> Languages() const
        {
            Array<String> languages;
            for (const Entry& entry : m_cooks)
            {
                languages.PushBack(entry.languageId);
            }
            return languages;
        }

        [[nodiscard]] IScriptLanguageCook* FindByLanguage(StringView languageId)
        {
            for (Entry& entry : m_cooks)
            {
                if (entry.languageId == languageId)
                {
                    return entry.cook.Get();
                }
            }
            return nullptr;
        }

        /// Has every cook build what its checks use (PrepareCheck), so the first check of each
        /// language does not wait for it. For a worker thread, while no cook is registered.
        void PrepareChecks()
        {
            for (Entry& entry : m_cooks)
            {
                if (entry.cook.Get() != nullptr)
                {
                    entry.cook->PrepareCheck();
                }
            }
        }

        /// Sum of every registered cook's CookVersion(), folded into the ScriptClassAssetBuilder
        /// fingerprint so a compiler/vendor bump in any bytecode language recooks the script
        /// packs. Order-independent (a sum), and 0 when every cook is source-only.
        [[nodiscard]] u32 CombinedCookVersion() const
        {
            u32 total = 0;
            for (const Entry& entry : m_cooks)
            {
                if (entry.cook.Get() != nullptr)
                {
                    total += entry.cook->CookVersion();
                }
            }
            return total;
        }

    private:
        struct Entry
        {
            String languageId;
            UniquePtr<IScriptLanguageCook> cook;
        };
        Array<Entry> m_cooks;
    };

    /// A hash of the script surface the player has: every runtime-domain type in `registry`, its id,
    /// members and their shapes, whatever order they were registered in. A script's bytecode is
    /// bound to it, so its change (a plugin enabled, removed or updated) recooks the scripts.
    /// Editor-only types stay out, so every host that cooks gets the same answer for the same
    /// build and plugins. Defined in ScriptAssetImpl.cpp.
    [[nodiscard]] u64 ScriptSurfaceHash(const TypeRegistry& registry);

    // Cooks a ScriptClassAsset to a BehaviorClassSource by delegating to the language cook the
    // asset's language resolves to. A thin shell: it reads the source, resolves the
    // cook, and delegates, with no language syntax and no VM handling.
    class ScriptClassAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ScriptClassAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &BehaviorClassSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override
        {
            // Base cook-logic version (2: the cooked record gained a bytecode field; 3: each
            // property a replicated flag; 4: the record carries data version 1, so the reader
            // refuses one from before the flag; 5: the class is the asset's choice, never a guess
            // among several; 6: the record carries the included files; 7: the behavior kind; 8: the
            // record is BehaviorClassSource, its type id renamed), plus
            // every language cook's compiler version, so a Luau (or other bytecode) vendor bump
            // recooks every script pack automatically.
            return 8 + ScriptLanguageCookRegistry::Get().CombinedCookVersion();
        }

        // The script surface the products compile against and their bytecode binds to.
        [[nodiscard]] u64 EnvironmentHash() const override { return ScriptSurfaceHash(GlobalTypeRegistry()); }

        // Every file the script includes, and every file those include: each is a cook input, so
        // editing an included file recooks the scripts that include it.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx,
                              pipeline::AssetDependencies& out) override
        {
            const ScriptClassAsset& scriptAsset = static_cast<const ScriptClassAsset&>(asset);
            IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(LanguageOf(scriptAsset));
            if (cook == nullptr)
            {
                return; // Build says so
            }
            // A class built into its project's assembly reads the assembly (which cooks first, and
            // whose change recooks the class) and refers to it at run time.
            if (cook->BuildsInAssembly())
            {
                // A scan's context carries the source database as `db` (a build's as `sourceDb`).
                content::IContentDatabase* sourceDb = ctx.sourceDb != nullptr ? ctx.sourceDb : ctx.db;
                if (sourceDb != nullptr)
                {
                    if (content::Instance* project = FindScriptAssemblyFor(*sourceDb, LanguageOf(scriptAsset), scriptAsset.fileName.View()))
                    {
                        out.reads.PushBack(project->Id());
                        out.references.PushBack(project->Id());
                    }
                }
                return;
            }
            Array<String> pending;
            pending.PushBack(String(scriptAsset.fileName.View()));
            Array<String> seen;
            while (!pending.IsEmpty())
            {
                const String file = Move(pending.Back());
                pending.PopBack();
                String text;
                if (!ReadSourceText(ctx, file.AsView(), text).IsOk())
                {
                    continue; // the cook reports a file it cannot read
                }
                Array<String> named;
                cook->ListIncludes(text.AsView(), named);
                for (const String& include : named)
                {
                    String resolved;
                    if (!ResolveScriptInclude(file.AsView(), include.AsView(), resolved))
                    {
                        continue; // the cook reports a path that leaves the sources
                    }
                    bool known = false;
                    for (const String& s : seen)
                    {
                        known = known || s.AsView() == resolved.AsView();
                    }
                    if (known)
                    {
                        continue;
                    }
                    seen.PushBack(resolved);
                    out.files.PushBack(foundation::vfs::SourcePath(resolved.AsView()));
                    pending.PushBack(Move(resolved));
                }
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const ScriptClassAsset& scriptAsset = static_cast<const ScriptClassAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            String source;
            const Status read = ReadSourceText(ctx, scriptAsset.fileName.View(), source);
            if (!read.IsOk())
            {
                LOG_ERROR(u8"Script", u8"'{}': source file missing - cook failed",
                                   scriptAsset.fileName.View());
                return read;
            }

            const StringView language = LanguageOf(scriptAsset);

            // The cook comes from the registry, by language, never a named cook type. A language is
            // a plugin, so no cook means the project does not enable it (or the host did not load
            // it): said with the plugin to enable.
            IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(language);
            if (cook == nullptr)
            {
                LOG_ERROR(u8"Script", u8"'{}' is written in {}, whose plugin is not loaded here - enable plugin '{}' to cook it",
                          scriptAsset.fileName.View(), language, language);
                return Status{ErrorCode::NotSupported};
            }

            BehaviorClassSource cooked;
            CookScriptErrorSink sink;
            if (cook->BuildsInAssembly())
            {
                const Status fromAssembly = CookFromProject(*cook, scriptAsset, source.AsView(), ctx, sink, cooked);
                if (!fromAssembly.IsOk())
                {
                    return fromAssembly;
                }
            }
            else
            {
                MountIncludes includes{&ctx};
                if (!cook->Cook(source.AsView(), scriptAsset.fileName.View(), sink, cooked, &includes))
                {
                    return Status{ErrorCode::InvalidArgument};
                }
            }
            // A cook that succeeded may still have warnings to say.
            for (const CookScriptErrorSink::Entry& e : sink.errors)
            {
                if (e.kind == ScriptErrorKind::Warning)
                {
                    LogScriptLine(LogLevel::Warning, e.module.IsEmpty() ? scriptAsset.fileName.View() : e.module.AsView(), e.line,
                                  e.column, e.message.AsView());
                }
            }
            return ctx.output->WriteObject(cooked);
        }

    private:
        [[nodiscard]] static StringView LanguageOf(const ScriptClassAsset& scriptAsset)
        {
            return scriptAsset.language.IsEmpty() ? StringView(u8"angelscript") : scriptAsset.language.AsView();
        }

        // A class of a language that builds per project: its description from the project's cooked
        // assembly, and the record naming that assembly.
        [[nodiscard]] static Status CookFromProject(IScriptLanguageCook& cook, const ScriptClassAsset& scriptAsset, StringView source,
                                                    pipeline::AssetBuildContext& ctx, CookScriptErrorSink& sink, BehaviorClassSource& cooked)
        {
            const StringView file = scriptAsset.fileName.View();
            content::Instance* project =
                ctx.sourceDb != nullptr ? FindScriptAssemblyFor(*ctx.sourceDb, LanguageOf(scriptAsset), file) : nullptr;
            if (project == nullptr)
            {
                LOG_ERROR(u8"Script", u8"'{}': no {} project holds it (a project file in its folder or one above) - cook failed", file,
                          LanguageOf(scriptAsset));
                return Status{ErrorCode::NotFound};
            }
            content::Instance* built = ctx.db != nullptr ? ctx.db->GetInstance(project->Id()) : nullptr;
            RefPtr<ISerializable> record = built != nullptr ? built->ReadObject() : RefPtr<ISerializable>{};
            const ScriptAssemblySource* assembly = Cast<ScriptAssemblySource>(record.Get());
            if (assembly == nullptr)
            {
                LOG_ERROR(u8"Script", u8"'{}': its project '{}' has not built - cook failed", file, project->Path());
                return Status{ErrorCode::NotFound};
            }
            if (!cook.CookFromAssembly(source, file, *assembly, sink, cooked))
            {
                for (const CookScriptErrorSink::Entry& e : sink.errors)
                {
                    LogScriptLine(LogLevel::Error, e.module.IsEmpty() ? file : e.module.AsView(), e.line, e.column, e.message.AsView());
                }
                return Status{ErrorCode::InvalidArgument};
            }
            cooked.assembly = project->Id();
            return Status{};
        }

        // Included files, read from the cook's sources mount.
        struct MountIncludes final : IScriptIncludeReader
        {
            explicit MountIncludes(const pipeline::AssetBuildContext* buildContext) : ctx(buildContext) {}
            [[nodiscard]] bool Read(StringView path, String& out) override { return ReadSourceText(*ctx, path, out).IsOk(); }
            const pipeline::AssetBuildContext* ctx;
        };
    };

    // Builds a project's script assembly: the project file and every source of its language under
    // its folder, through the language's assembly cook.
    class ScriptAssemblyAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override { return &ScriptAssemblyAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &ScriptAssemblySource::StaticType(); }
        [[nodiscard]] u32 Version() const override { return 1 + ScriptAssemblyCookRegistry::Get().CombinedCookVersion(); }

        // The engine surface the assembly is built against (the runtime's generated bindings).
        [[nodiscard]] u64 EnvironmentHash() const override { return ScriptSurfaceHash(GlobalTypeRegistry()); }

        // Every source under the project's folder is an input: an edit, a new file or a removed
        // one rebuilds it.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx, pipeline::AssetDependencies& out) override
        {
            const ScriptAssemblyAsset& project = static_cast<const ScriptAssemblyAsset&>(asset);
            IScriptAssemblyCook* cook = ScriptAssemblyCookRegistry::Get().FindByLanguage(project.language.AsView());
            if (cook == nullptr || ctx.sources == nullptr)
            {
                return; // Build says so
            }
            for (const String& file : ListSourcesUnder(*ctx.sources, PathParent(project.fileName.View()), cook->SourceExtension()))
            {
                out.files.PushBack(foundation::vfs::SourcePath(file.AsView()));
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override
        {
            const ScriptAssemblyAsset& project = static_cast<const ScriptAssemblyAsset&>(asset);
            if (ctx.output == nullptr || ctx.sources == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            IScriptAssemblyCook* cook = ScriptAssemblyCookRegistry::Get().FindByLanguage(project.language.AsView());
            if (cook == nullptr)
            {
                LOG_ERROR(u8"Script", u8"'{}' is a {} project, whose plugin is not loaded here - enable plugin '{}' to build it",
                          project.fileName.View(), project.language, project.language);
                return Status{ErrorCode::NotSupported};
            }
            String text;
            if (const Status read = ReadSourceText(ctx, project.fileName.View(), text); !read.IsOk())
            {
                LOG_ERROR(u8"Script", u8"'{}': project file missing - cook failed", project.fileName.View());
                return read;
            }
            Array<ScriptAssemblyFile> sources;
            for (const String& file : ListSourcesUnder(*ctx.sources, PathParent(project.fileName.View()), cook->SourceExtension()))
            {
                ScriptAssemblyFile source;
                source.path = file;
                if (!ReadSourceText(ctx, file.AsView(), source.text).IsOk())
                {
                    LOG_ERROR(u8"Script", u8"'{}': unreadable - '{}' cook failed", file, project.fileName.View());
                    return Status{ErrorCode::NotFound};
                }
                sources.PushBack(Move(source));
            }
            WriteLocalFiles(*cook, ctx, PathParent(project.fileName.View()));
            ScriptAssemblySource built;
            built.language = project.language;
            CookScriptErrorSink sink;
            const bool ok = cook->Build(project.fileName.View(), text.AsView(), Span<const ScriptAssemblyFile>{sources.Data(), sources.Size()},
                                        sink, built);
            for (const CookScriptErrorSink::Entry& e : sink.errors)
            {
                const StringView where = e.module.IsEmpty() ? project.fileName.View() : e.module.AsView();
                LogScriptLine(e.kind == ScriptErrorKind::Warning ? LogLevel::Warning : LogLevel::Error, where, e.line, e.column,
                              e.message.AsView());
            }
            if (!ok)
            {
                return Status{ErrorCode::InvalidArgument}; // nothing written: the last good assembly stays
            }
            return ctx.output->WriteObject(built);
        }

    private:
        // The cook's local files beside the project, written only when they differ, so a cook that
        // changes nothing touches nothing a file watcher sees.
        static void WriteLocalFiles(const IScriptAssemblyCook& cook, pipeline::AssetBuildContext& ctx, StringView folder)
        {
            foundation::vfs::IWritableFileSystem* writable = ctx.sources->AsWritable();
            if (writable == nullptr)
            {
                return;
            }
            for (const ScriptAssemblyFile& file : cook.LocalFiles())
            {
                const String path = folder.IsEmpty() ? file.path : PathJoin(folder, file.path.AsView());
                String current;
                if (ReadSourceText(ctx, path.AsView(), current).IsOk() && current == file.text)
                {
                    continue;
                }
                if (!writable->Save(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(file.text.Data()), file.text.Size()))
                         .IsOk())
                {
                    LOG_WARNING(u8"Script", u8"'{}': could not write '{}' beside the project", folder, path);
                }
            }
        }
    };

    // ---- native behaviors ----

    /// Source asset: a behavior written in C++. It names the class by its qualified reflected name
    /// ("rtti::game::Orb"), which a project's plugin registers; it cooks into an ordinary
    /// BehaviorClass of language `native`, so an entity, a scene or the project refers to it as
    /// it refers to a script's class. No file: the class is the plugin's.
    class NativeBehaviorAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(NativeBehaviorAsset, pipeline::Asset)
    public:
        String className;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            foundation::core::Serialize(ar, "className", className);
        }
    };

    /// The kind a native class is, read from its base chain (the engine's bases are named for the
    /// kinds in kNativeBehaviorNamespace); None when it derives from none of them.
    [[nodiscard]] BehaviorKind NativeBehaviorKindOf(const TypeInfo& type) noexcept;

    /// What a native class declares, as a script's cook harvests a script's: its kind, its handlers
    /// (its reflected methods named `on<Name>`), and its properties (the fields it marks
    /// kBehaviorPropertyAttribute, each a float, an integer, a bool, a String, a Color, a Float3,
    /// an Entity, or an asset's Guid carrying `assetType`), their defaults read from an object of
    /// it made with `allocator`. False, with `why`, when it is no native class, cannot be made, or
    /// marks a field of another type.
    [[nodiscard]] bool HarvestNativeBehaviorClass(const TypeInfo& type, IAllocator& allocator, BehaviorClassSource& out,
                                                  String& why);

    /// A hash of every registered native class's declaration (its name, kind, marked fields and
    /// handlers): what the Native Behavior cook depends on, so a class rebuilt with a property more
    /// recooks its asset.
    [[nodiscard]] u64 NativeBehaviorsHash(const TypeRegistry& registry);

    /// Cooks a Native Behavior asset: the class's declaration, harvested from its reflection.
    class NativeBehaviorAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override { return &NativeBehaviorAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &BehaviorClassSource::StaticType(); }
        [[nodiscard]] u32 Version() const override { return 1; }
        [[nodiscard]] u64 EnvironmentHash() const override { return NativeBehaviorsHash(GlobalTypeRegistry()); }
        [[nodiscard]] Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override;
    };

    // OS-file importer (editor drag-drop): accepts any extension a registered script
    // backend claims (language-clean), copies the file into Sources/, and creates
    // a ScriptClassAsset whose language records the owning backend. No options dialog.
    class ScriptFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Script"; }
        [[nodiscard]] bool ImportsCode() const override { return true; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return ScriptBackendRegistry::Get().FindByExtension(extension) != nullptr;
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"ScriptClassAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath,
                                                        u8"ScriptClassAsset");
        }

        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator&) const override
        {
            return {}; // no options dialog: the drop imports immediately
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               content::Group& group, const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            const String extension = pipeline::FileExtensionLower(sourcePath);
            const ScriptBackendDesc* backend =
                ScriptBackendRegistry::Get().FindByExtension(extension.AsView());
            if (backend == nullptr)
            {
                return Err(ErrorCode::NotSupported);
            }

            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            pipeline::ImportClashKind clash = pipeline::ImportClashKind::None;
            Result<content::Instance*> claimed =
                pipeline::ClaimSingleAsset(context, group, options, sourcePath, ScriptClassAsset::StaticType(), fileName, &clash,
                                           foundation::vfs::kSourcesCodeFolder); // code goes in the project's Code/
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();

            ScriptClassAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            asset.language = String(backend->languageId.AsView());
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // OS-file importer for a project file (a C# .csproj): any extension a registered project build
    // claims, copied into Sources/ as a ScriptAssemblyAsset of that language.
    class ScriptAssemblyFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Script project"; }
        [[nodiscard]] bool ImportsCode() const override { return true; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return !ScriptAssemblyCookRegistry::Get().LanguageOfProject(extension).IsEmpty();
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath, const pipeline::ImportOptions*, Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath);
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"ScriptAssemblyAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group, StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"ScriptAssemblyAsset");
        }

        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator&) const override { return {}; }

        [[nodiscard]] Result<content::Instance*> Import(StringView sourcePath, const pipeline::ImportContext& context, content::Group& group,
                                                        const pipeline::ImportOptions* options, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            const String extension = pipeline::FileExtensionLower(sourcePath);
            const StringView language = ScriptAssemblyCookRegistry::Get().LanguageOfProject(extension.AsView());
            if (language.IsEmpty())
            {
                return Err(ErrorCode::NotSupported);
            }
            String fileName;
            Result<content::Instance*> claimed =
                pipeline::ClaimSingleAsset(context, group, options, sourcePath, ScriptAssemblyAsset::StaticType(), fileName, nullptr,
                                           foundation::vfs::kSourcesCodeFolder);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            ScriptAssemblyAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            asset.language = String(language);
            if (const Status written = claimed.Value()->WriteObject(asset); !written.IsOk())
            {
                return Err(written.Code());
            }
            return claimed.Value();
        }
    };

    // ---- ScriptPage editing model ----

    /// The headless half of the in-editor ScriptPage: the edit buffer for one script asset's
    /// source file plus the save and compile-check loop, factored out of the UI so it is
    /// unit-testable without a window. Save writes the source file (the page drives the recook
    /// and hot reload through EditorContext::RequestCook, the same path an external edit
    /// takes). Validate() compile-checks the current buffer through the language cook's Check,
    /// which compiles as the product build does but runs none of the source: it captures the
    /// ScriptError file/line/column and message for inline surfacing but never writes a product,
    /// so a failing edit leaves the last-good cooked BehaviorClass untouched (live instances keep
    /// running the old class).
    class ScriptSourceDocument
    {
    public:
        struct CompileError
        {
            ScriptErrorKind kind = ScriptErrorKind::Compile;
            String module; // reporting module/file (empty = the asset's own file)
            i32 line = 0;  // as the cook's error handler captured it (may include the
                           // backend's framing prelude offset; the message is authoritative)
            i32 column = -1; // 1-based, or -1 when the backend does not say
            String message;
        };

        /// Bind to an asset's source file: the project's Sources/ root, the asset's file name,
        /// its language id (empty defaults to AngelScript, matching the builder) and the project's
        /// Code/ folder, which a file name starting Code/ is in.
        void Bind(StringView sourcesRoot, StringView fileName, StringView language, StringView codeRoot = {})
        {
            m_sourcesRoot = String(sourcesRoot);
            m_codeRoot = String(codeRoot);
            m_fileName = String(fileName);
            m_language = language.IsEmpty() ? String(u8"angelscript") : String(language);
        }

        [[nodiscard]] StringView FileName() const noexcept { return m_fileName.AsView(); }
        [[nodiscard]] StringView Language() const noexcept { return m_language.AsView(); }

        /// Read the bound source file into the edit buffer (and mark it as the saved baseline).
        [[nodiscard]] Status Load()
        {
            const String path = foundation::vfs::SourceFileOnDisk(m_sourcesRoot.AsView(), m_codeRoot.AsView(), m_fileName.AsView());
            Result<Array<byte>> bytes = ReadFile(path.AsView());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            const Array<byte>& data = bytes.Value();
            m_source =
                String(StringView(reinterpret_cast<const utf8char*>(data.Data()), data.Size()));
            m_saved = m_source;
            return Status{};
        }

        [[nodiscard]] StringView Source() const noexcept { return m_source.AsView(); }
        void SetSource(StringView source) { m_source = String(source); }
        [[nodiscard]] bool IsModified() const { return m_source.AsView() != m_saved.AsView(); }

        /// Persist the edit buffer to the bound source file (clears IsModified on success).
        /// The caller drives the recook and hot reload afterwards (EditorContext::RequestCook).
        [[nodiscard]] Status Save()
        {
            const String path = foundation::vfs::SourceFileOnDisk(m_sourcesRoot.AsView(), m_codeRoot.AsView(), m_fileName.AsView());
            const Status written = WriteFile(
                path.AsView(),
                Span<const byte>(reinterpret_cast<const byte*>(m_source.Data()), m_source.Size()));
            if (written.IsOk())
            {
                m_saved = m_source;
            }
            return written;
        }

        /// Compile-check the current buffer through the language cook's Check (nothing runs, no
        /// product write). Fills Errors() with the captured ScriptErrors and, on success, the
        /// class name. Returns whether it compiled. An unknown language surfaces one config error.
        [[nodiscard]] bool Validate()
        {
            m_errors.Clear();
            m_className = String{};
            IScriptLanguageCook* cook =
                ScriptLanguageCookRegistry::Get().FindByLanguage(m_language.AsView());
            if (cook == nullptr)
            {
                CompileError e;
                e.message = Format(u8"written in {}, whose plugin is not loaded here - enable plugin '{}' to check it",
                                   m_language, m_language);
                m_errors.PushBack(Move(e));
                m_lastCompileOk = false;
                return false;
            }
            CookScriptErrorSink sink;
            String className;
            FolderIncludeReader includes(m_sourcesRoot.AsView(), m_codeRoot.AsView()); // the files it includes, from the project
            const bool ok = cook->Check(m_source.AsView(), m_fileName.AsView(), sink, className,
                                        Span<const TypeId>{}, &includes);
            for (const CookScriptErrorSink::Entry& entry : sink.errors)
            {
                CompileError e;
                e.kind = entry.kind;
                e.module = entry.module;
                e.line = entry.line;
                e.column = entry.column;
                e.message = entry.message;
                m_errors.PushBack(Move(e));
            }
            if (ok)
            {
                m_className = Move(className);
            }
            m_lastCompileOk = ok;
            return ok;
        }

        [[nodiscard]] Span<const CompileError> Errors() const noexcept
        {
            return Span<const CompileError>{m_errors.Data(), m_errors.Size()};
        }
        [[nodiscard]] StringView ClassName() const noexcept { return m_className.AsView(); }
        [[nodiscard]] bool LastCompileOk() const noexcept { return m_lastCompileOk; }

    private:
        String m_sourcesRoot;
        String m_codeRoot;
        String m_fileName;
        String m_language;
        String m_source;
        String m_saved;
        String m_className;
        Array<CompileError> m_errors;
        bool m_lastCompileOk = true;
    };

    // Registers the asset type for content-DB construction and deserialization.
    inline void RegisterScriptAssets()
    {
        GlobalTypeRegistry().Register(ScriptClassAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ScriptClassAsset>();
        GlobalTypeRegistry().Register(ScriptAssemblyAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ScriptAssemblyAsset>();
        GlobalTypeRegistry().Register(NativeBehaviorAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<NativeBehaviorAsset>();
    }

    // ScriptClassAsset::StaticType() is defined with its reflected properties in ScriptAssetImpl.cpp.

    /// The placeholder a behaviour starter spells its class name with.
    inline constexpr StringView kStarterClassName = u8"{ClassName}";

    /// `starter` with every kStarterClassName replaced by `className` ("NewBehavior" when empty).
    [[nodiscard]] inline String FillStarterClassName(StringView starter, StringView className)
    {
        const StringView name = className.IsEmpty() ? StringView(u8"NewBehavior") : className;
        String out;
        usize i = 0;
        while (i < starter.Size())
        {
            if (i + kStarterClassName.Size() <= starter.Size() &&
                starter.SubStr(i, kStarterClassName.Size()) == kStarterClassName)
            {
                out.Append(name);
                i += kStarterClassName.Size();
                continue;
            }
            out.PushBack(starter[i]);
            ++i;
        }
        return out;
    }

    /// A behaviour's class name from its asset's name: the ASCII letters, digits and underscores,
    /// a leading digit prefixed with an underscore; `fallback` when nothing is left. So a script
    /// asset and the class it holds share a name ("Player Controller" holds PlayerController).
    [[nodiscard]] inline String ClassNameFor(StringView assetName, StringView fallback)
    {
        String out;
        for (const utf8char c : assetName)
        {
            const bool letter = (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z');
            const bool digit = c >= u8'0' && c <= u8'9';
            if (letter || digit || c == u8'_')
            {
                out.PushBack(c);
            }
        }
        if (out.IsEmpty())
        {
            return String(fallback);
        }
        if (out[0] >= u8'0' && out[0] <= u8'9')
        {
            String prefixed(u8"_");
            prefixed.Append(out.AsView());
            return prefixed;
        }
        return out;
    }

    /// The stem a new behavior of `kind` is named by when the caller names none
    /// ("NewEntityBehavior").
    [[nodiscard]] inline StringView BehaviorBaseName(foundation::script::BehaviorKind kind) noexcept
    {
        switch (kind)
        {
        case foundation::script::BehaviorKind::Scene:
            return u8"NewSceneBehavior";
        case foundation::script::BehaviorKind::Run:
            return u8"NewRunBehavior";
        case foundation::script::BehaviorKind::Entity:
        default:
            return u8"NewEntityBehavior";
        }
    }

    /// A new script asset: the language cook's starter for a behavior of `kind` written to the
    /// code folder as "<name>.<extension>" (its class named after the asset), and a
    /// ScriptClassAsset linking it with its language. Null when the language has no cook or a
    /// write is refused.
    [[nodiscard]] foundation::content::Instance* CreateScriptInstance(
        const AssetCreationContext& context, StringView languageId, StringView extension,
        foundation::script::BehaviorKind kind, StringView baseName);
    /// A new project file for a language whose classes build together (a C# .csproj): the project
    /// build's template written to the sources folder as "<name><dottedExtension>", and its
    /// ScriptAssemblyAsset. Null when the language has no project build or a write is refused.
    [[nodiscard]] foundation::content::Instance* CreateScriptProjectInstance(const AssetCreationContext& context, StringView languageId,
                                                                             StringView dottedExtension);
    /// File > New's script creators: three per registered backend with a cook (Behavior,
    /// Level, Game), and a project creator for one whose classes build together. Answers how many
    /// it registered. Register the cooks first.
    usize RegisterScriptCreators(AssetCreatorRegistry& registry);

    /// The Native Behavior creator's label, as File > New and asset_create name it.
    inline constexpr StringView kNativeBehaviorCreator = u8"Native Behavior";

    /// The registered native class whose own name is `name` ("Spinner" names rtti::game::Spinner),
    /// or null when none is, or several are.
    [[nodiscard]] const TypeInfo* NativeBehaviorClassNamed(StringView name);

    /// File > New's Native Behavior: an asset naming the registered native class of the asset's
    /// own name, when there is one (the class is the asset's field to set otherwise).
    void RegisterNativeBehaviorCreator(AssetCreatorRegistry& registry);
}
