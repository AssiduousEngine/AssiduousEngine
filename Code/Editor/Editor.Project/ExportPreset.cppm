// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_preset partition.
//
// Export presets are named, per-platform descriptions of how to produce a shippable dist: which
// export template (player and runtime sidecars) to use, plus game-specific extra files and output
// naming. They are project-local and can be committed, since they reference a template by id or
// platform, never by an absolute path. They are saved to <project>/export_presets.xml (versioned
// XML in the ProjectSettings shape). The Tools.Export CLI and the editor's Export menu both drive
// the export from these, so a preset produces the same dist whichever of them triggers it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.project:export_preset;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import foundation.settings;
import engine.project; // WindowMode (a preset's own window)

using namespace foundation::core;

export namespace editor
{
    namespace vfs = foundation::vfs;

    inline constexpr StringView kExportPresetsFile = u8"export_presets.xml";

    // A platform a game exports to: its tag (the Bin/<Config>/<Platform> name a template, a preset
    // and a plugin package's platforms use), the built-in icon its templates get unless their maker
    // names another, and how its build names the player made from a base name ("Engine.Player"):
    // an executable, a page, or the library an Android app loads.
    struct ExportPlatform
    {
        StringView tag;
        StringView icon;
        StringView playerPrefix;
        StringView playerSuffix;
    };

    /// The platforms a game exports to. A new package supports them all, and the tools that take a
    /// platform name them from here.
    inline constexpr ExportPlatform kExportPlatforms[] = {
        {u8"Linux64", u8"linux", u8"", u8""},
        {u8"Win64", u8"windows", u8"", u8".exe"},
        {u8"Web", u8"web", u8"", u8".html"},
        {u8"Android64", u8"phone", u8"lib", u8".so"},
    };

    /// The export platform tagged `tag`, or null when no game exports to it.
    [[nodiscard]] inline const ExportPlatform* FindExportPlatform(StringView tag) noexcept
    {
        for (const ExportPlatform& platform : kExportPlatforms)
        {
            if (platform.tag == tag)
            {
                return &platform;
            }
        }
        return nullptr;
    }

    /// Whether `platform` is Android's: its export builds an app around the player.
    [[nodiscard]] inline bool IsAndroidPlatform(StringView platform) noexcept
    {
        return platform == u8"Android64";
    }

    /// The file `platform`'s build makes of a player called `base` ("libEngine.Player.so" on
    /// Android); `base` itself on a platform not among the export platforms.
    [[nodiscard]] inline String ExportPlayerFile(StringView platform, StringView base)
    {
        const ExportPlatform* known = FindExportPlatform(platform);
        if (known == nullptr)
        {
            return String(base);
        }
        String out(known->playerPrefix);
        out += base;
        out += known->playerSuffix;
        return out;
    }

    /// The export platforms' tags, comma separated ("Linux64, Win64, ..."), for a message or a
    /// tool's description.
    [[nodiscard]] inline String ExportPlatformList()
    {
        String out;
        for (const ExportPlatform& platform : kExportPlatforms)
        {
            if (!out.IsEmpty())
            {
                out += u8", ";
            }
            out += platform.tag;
        }
        return out;
    }

    /// What an Android export builds: an APK to install (on a phone, or from a store that takes
    /// one), an Android App Bundle for Google Play, or both.
    enum class AndroidPackage : u8
    {
        Apk,
        Bundle,
        Both,
    };

    /// Where an Android export reads its keystore's passwords: the environment, never a preset. The
    /// key's own is the keystore's unless it is given.
    inline constexpr StringView kAndroidStorePasswordVariable = u8"ANDROID_KEYSTORE_PASSWORD";
    inline constexpr StringView kAndroidKeyPasswordVariable = u8"ANDROID_KEY_PASSWORD";

    /// The platform attribute on a preset field: the field is that platform's alone (the Android
    /// app's id is no desktop's), so an editor of presets shows it only for a preset of it.
    inline constexpr const char* kPresetPlatformAttribute = "platform";

    /// Why `id` cannot be an Android application id, or empty when it can: two or more parts
    /// joined by '.', each a letter followed by letters, digits or '_' ("com.studio.game").
    [[nodiscard]] inline String CheckAndroidApplicationId(StringView id)
    {
        if (id.IsEmpty())
        {
            return String(u8"an Android export needs an application id (as com.studio.game)");
        }
        usize parts = 0;
        usize start = 0;
        for (usize i = 0; i <= id.Size(); ++i)
        {
            if (i < id.Size() && id[i] != utf8char('.'))
            {
                continue;
            }
            const StringView part = id.SubStr(start, i - start);
            const auto letter = [](utf8char c)
            { return (c >= utf8char('a') && c <= utf8char('z')) || (c >= utf8char('A') && c <= utf8char('Z')); };
            if (part.IsEmpty() || !letter(part[0]))
            {
                return Format(u8"'{}' is not an application id: each part, between the dots, starts with a letter", id);
            }
            for (const utf8char c : part)
            {
                if (!letter(c) && !(c >= utf8char('0') && c <= utf8char('9')) && c != utf8char('_'))
                {
                    return Format(u8"'{}' is not an application id: its parts hold letters, digits and '_'", id);
                }
            }
            ++parts;
            start = i + 1;
        }
        if (parts < 2)
        {
            return Format(u8"'{}' is not an application id: it has two parts or more (as com.studio.game)", id);
        }
        return {};
    }

    // One export target, a plain value type (copyable and movable) so a project can hold an
    // Array<ExportPreset>; only the root set below is a versioned-payload object.
    //
    // It references an ExportTemplate (see :export_template) rather than an absolute path, so a
    // preset is portable and can be committed: `templateId` picks a template exactly, or when blank
    // the installed template for `platform` is used (the host's implicit template counts).
    // `additionalFiles` are game-specific extras (icon, config, data) staged into the dist on top of
    // the template's own runtime sidecars.
    struct ExportPreset
    {
        String name;         // "Windows Desktop"
        String platform;     // "Win64" or "Linux64" (the Bin/<Config>/<Platform> tag)
        String config;       // "Debug", "Release" or "RelWithDebInfo"; "" means Release (the default)
        String templateId;   // which template; "" resolves by (platform, config)
        String playerName;   // output executable name; "" uses the template's player basename
        String outputSubdir; // output directory relative to the export root; "" uses the sanitized `name`
        Array<String> additionalFiles; // game-specific extra files (beyond the template's sidecars)
        bool stageSymbols =
            false; // stage the template's symbols[] into the dist (by default they are left out)
        bool pruneToReachable = false; // ship only the closure of the entry points (by default
                                       // everything is packed, for teams that do not manage
                                       // reachability)
        // This platform draws at its own resolution rather than the project's (a handheld's native
        // panel, say): the dist's player.xml carries these instead.
        bool overridesRender = false;
        u32 renderWidth = 0;
        u32 renderHeight = 0;
        FitMode renderFit = FitMode::Letterbox;
        // This platform's window differs from the project's (fullscreen on a console-like device,
        // say): the dist's player.xml carries these instead.
        bool overridesWindow = false;
        u32 windowWidth = 1280;
        u32 windowHeight = 720;
        engine::project::WindowMode windowMode = engine::project::WindowMode::Windowed;
        bool windowResizable = true;
        // This platform is held its own way (a phone-facing Web preset says Landscape, so a game
        // that takes any shape on a desktop is presented turned on a phone held upright).
        bool overridesOrientation = false;
        ScreenOrientation orientation = ScreenOrientation::Any;
        // The dist keeps its custom shaders' HLSL (a game that wants modders to read them). Off:
        // the pack carries the shaders as bytecode and the dist ships no shader source, as the
        // engine's own shaders ship none.
        bool includeShaderSource = false;
        // The quality level this platform starts at, one of the project's levels (a phone's lower
        // than a desktop's); nil is the project's default.
        Guid qualityLevelId;
        // An Android export's app (Android64 presets alone): its id, the version a store and a
        // phone compare (the code) and the one a person reads (the name), the name under its icon
        // (empty: the project's), the launcher icon (a square PNG in the project, 512 pixels or
        // more; empty: the platform's), what it builds, and the release keystore it is signed with
        // (empty: the developer's debug key) by the key `androidKeyAlias`. The keystore's
        // passwords are never in a preset: the export asks for them, or reads them from the
        // environment.
        String androidApplicationId;
        u32 androidVersionCode = 1;
        String androidVersionName = String(u8"1.0");
        String androidLabel;
        String androidIcon;
        AndroidPackage androidPackage = AndroidPackage::Apk;
        String androidKeystore;
        String androidKeyAlias;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "platform", platform);
            foundation::core::Serialize(ar, "templateId", templateId);
            foundation::core::Serialize(ar, "playerName", playerName);
            foundation::core::Serialize(ar, "outputSubdir", outputSubdir);
            foundation::core::Serialize(ar, "additionalFiles", additionalFiles);
            // The config axis: a preset selects (platform, config) and opts symbols in or out
            // (config "" resolves to Release).
            foundation::core::Serialize(ar, "config", config);
            foundation::core::Serialize(ar, "stageSymbols", stageSymbols);
            // Closure pruning (false packs the whole cooked directory).
            foundation::core::Serialize(ar, "pruneToReachable", pruneToReachable);
            // The display overrides are appended, so a preset file written without them reads none.
            SerializeAppended(ar, "overridesRender", overridesRender);
            SerializeAppended(ar, "renderWidth", renderWidth);
            SerializeAppended(ar, "renderHeight", renderHeight);
            SerializeAppended(ar, "renderFit", renderFit);
            SerializeAppended(ar, "overridesWindow", overridesWindow);
            SerializeAppended(ar, "windowWidth", windowWidth);
            SerializeAppended(ar, "windowHeight", windowHeight);
            SerializeAppended(ar, "windowMode", windowMode);
            SerializeAppended(ar, "windowResizable", windowResizable);
            SerializeAppended(ar, "overridesOrientation", overridesOrientation);
            SerializeAppended(ar, "orientation", orientation);
            SerializeAppended(ar, "includeShaderSource", includeShaderSource);
            if (ar.Mode() != SerializeMode::Read || ar.HasKey("qualityLevelId")) // appended
            {
                ar.Key("qualityLevelId");
                ar.GuidValue(qualityLevelId);
            }
            SerializeAppended(ar, "androidApplicationId", androidApplicationId);
            SerializeAppended(ar, "androidVersionCode", androidVersionCode);
            SerializeAppended(ar, "androidVersionName", androidVersionName);
            SerializeAppended(ar, "androidLabel", androidLabel);
            SerializeAppended(ar, "androidIcon", androidIcon);
            SerializeAppended(ar, "androidPackage", androidPackage);
            SerializeAppended(ar, "androidKeystore", androidKeystore);
            SerializeAppended(ar, "androidKeyAlias", androidKeyAlias);
        }
    };

    // ExportPreset's reflection: every field a preset editor sets, each with a `label` (the
    // engine.project settings convention), and the sizes with their `range`. The MCP preset tools
    // build their arguments, checks and answers from it. Call before reading TypeOf<ExportPreset>().
    void RegisterExportPresetReflection();

    // ADL hook so Serialize(ar, Array<ExportPreset>&) resolves each element to the member above.
    // It frames each element as its own object node (the generic Array<T> helper does not), so the
    // per-element keys stay separate on read. Declared before ExportPresetSet so it is visible at
    // that template's instantiation point.
    inline void Serialize(ISerializer& ar, ExportPreset& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }

    // A project's set of export presets: the root, versioned payload of export_presets.xml.
    class ExportPresetSet final : public ISerializable
    {
        RTTI_OBJECT(ExportPresetSet, ISerializable)
    public:
        Array<ExportPreset> presets;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "presets", presets);
        }

        // Finds a preset by name (case-sensitive); null when absent.
        [[nodiscard]] const ExportPreset* Find(StringView presetName) const
        {
            for (const ExportPreset& p : presets)
            {
                if (p.name.AsView() == presetName)
                {
                    return &p;
                }
            }
            return nullptr;
        }
    };

    // Fills `out` with the built-in default: one preset targeting the host platform, sourcing the
    // player from the driving tool's own directory. Used when a project has no export_presets.xml.
    inline void DefaultExportPresets(ExportPresetSet& out)
    {
        ExportPreset host;
        host.platform = String(GetHostPlatformName());
        host.name = host.platform;
        host.name += u8" Desktop";
        host.outputSubdir = host.platform;
        out.presets.Clear();
        out.presets.PushBack(Move(host));
    }

    // Reads export_presets.xml from `root`. NotFound when absent (the caller falls back to defaults).
    [[nodiscard]] inline Status LoadExportPresets(vfs::IFileSystem& root, ExportPresetSet& out,
                                                  StringView fileName = kExportPresetsFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(*stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportPresetSet::StaticType());
        out.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        return ctx->serializer->IsOk() ? Status{} : ctx->serializer->GetStatus();
    }

    // Writes export_presets.xml to `root`.
    [[nodiscard]] inline Status SaveExportPresets(vfs::IWritableFileSystem& writable,
                                                  ExportPresetSet& presets,
                                                  StringView fileName = kExportPresetsFile)
    {
        MemoryStream buffer;
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportPresetSet::StaticType());
        presets.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        if (!ctx->serializer->IsOk())
        {
            return ctx->serializer->GetStatus();
        }
        ctx->Flush(buffer);
        return writable.Save(fileName, buffer.Bytes());
    }

    // Editor-level export preferences: a Settings section (foundation.settings store), not
    // project-local and not in export_presets.xml. It holds the templates-root override: when
    // non-empty it wins over $ENV_TEMPLATES_DIR and the built-in <user-data>/templates default (see
    // ResolveTemplatesRoot in :export_template), letting the user point the editor at a shared or
    // checked-out templates directory. Empty (the default) keeps the environment-then-default
    // behaviour.
    // It also holds the tools an Android export runs: the Android SDK's folder and a JDK's, each
    // empty (the default) for the environment's (ANDROID_HOME, JAVA_HOME or java on the PATH).
    class EditorExportSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorExportSettings, ISerializable)
    public:
        String templatesRoot; // "" means $ENV_TEMPLATES_DIR, else <user-data>/templates
        String androidSdk;    // "" means $ANDROID_HOME
        String javaHome;      // "" means $JAVA_HOME, else java on the PATH

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "templatesRoot", templatesRoot);
            SerializeAppended(ar, "androidSdk", androidSdk);
            SerializeAppended(ar, "javaHome", javaHome);
        }
    };

    /// The environment variables an Android export's tools read: the SDK (Gradle's Android plugin and
    /// the export's check) and the JDK (the Gradle wrapper).
    inline constexpr StringView kAndroidHomeVariable = u8"ANDROID_HOME";
    inline constexpr StringView kJavaHomeVariable = u8"JAVA_HOME";

    /// What the process was started with of those variables, kept so a setting cleared later gives
    /// them back.
    struct ExportToolEnvironment
    {
        Optional<String> androidHome;
        Optional<String> javaHome;

        [[nodiscard]] static ExportToolEnvironment Current()
        {
            return ExportToolEnvironment{GetEnvironmentVariable(kAndroidHomeVariable), GetEnvironmentVariable(kJavaHomeVariable)};
        }
    };

    /// Sets this process's ANDROID_HOME and JAVA_HOME from `settings`, where the exports it runs and
    /// their Gradle builds read them: a folder the settings name, else what the process was started
    /// with (`inherited`).
    inline void ApplyExportToolEnvironment(const EditorExportSettings& settings, const ExportToolEnvironment& inherited)
    {
        const auto apply = [](StringView variable, const String& chosen, const Optional<String>& started)
        {
            const StringView value = !chosen.IsEmpty() ? chosen.AsView() : (started.HasValue() ? started->AsView() : StringView{});
            (void)SetEnvironmentVariable(variable, value);
        };
        apply(kAndroidHomeVariable, settings.androidSdk, inherited.androidHome);
        apply(kJavaHomeVariable, settings.javaHome, inherited.javaHome);
    }

    namespace settings = foundation::settings;

    RTTI_DEFINE_OBJECT_VERSIONED(ExportPresetSet, "rtti::editor::editor", 3)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorExportSettings, "rtti::editor::editor", 1)
}
