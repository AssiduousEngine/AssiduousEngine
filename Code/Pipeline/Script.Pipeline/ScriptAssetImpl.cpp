// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script - reflection implementation unit: ScriptClassAsset's reflected surface.
//
// Kept out of the ScriptAsset.cppm interface (REFLECT_MEMBERS bodies make GCC emit a gcm cluster).
// ScriptClassAsset::StaticType() gains its `language` and `className` properties here. There are no enums, so the
// reflection comes with StaticType() and needs no registrar.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE: the API registration a check waits on

#include <cstring>

module script.pipeline;

import foundation.core;
import foundation.profiler;
import foundation.script.facades; // RegisterScriptFacadeReflection
import pipeline.core;

using namespace foundation::core;
using namespace foundation::script;

namespace pipeline{
    ScriptLanguageCookRegistry& ScriptLanguageCookRegistry::Get()
    {
        static ScriptLanguageCookRegistry instance;
        return instance;
    }

    bool ResolveScriptInclude(StringView from, StringView include, String& out)
    {
        if (include.IsEmpty() || PathIsAbsolute(include) || include.Find(StringView(u8":")) != StringView::kNotFound)
        {
            return false;
        }
        Array<StringView> parts;
        // Appends `path`'s components, "." skipped and ".." taking one back; false past the root.
        const auto append = [&parts](StringView path)
        {
            usize start = 0;
            for (usize i = 0; i <= path.Size(); ++i)
            {
                if (i == path.Size() || path[i] == u8'/' || path[i] == u8'\\')
                {
                    const StringView part = path.SubStr(start, i - start);
                    start = i + 1;
                    if (part.IsEmpty() || part == StringView(u8"."))
                    {
                        continue;
                    }
                    if (part == StringView(u8".."))
                    {
                        if (parts.IsEmpty())
                        {
                            return false;
                        }
                        parts.PopBack();
                        continue;
                    }
                    parts.PushBack(part);
                }
            }
            return true;
        };
        if (!append(PathParent(from)) || !append(include) || parts.IsEmpty())
        {
            return false;
        }
        out = String{};
        for (usize i = 0; i < parts.Size(); ++i)
        {
            if (i > 0)
            {
                out.PushBack(u8'/');
            }
            out.Append(parts[i]);
        }
        return true;
    }

    RefPtr<IScriptManager> MakeCheckManager(IAllocator& allocator, StringView languageId, StringView forWhat,
                                            Span<const TypeId> hidden)
    {
        RefPtr<IScriptManager> manager;
        {
            PROFILE_SCOPE("Script.Cook.MakeManager");
            manager = CreateScriptManagerForLanguage(languageId, allocator);
        }
        if (manager.Get() == nullptr)
        {
            LOG_ERROR(u8"Script", u8"'{}': no {} backend registered", forWhat, languageId);
            return manager;
        }
        PROFILE_SCOPE("Script.Cook.RegisterApi");
        RegisterCoreTypes();
        RegisterScriptFacadeReflection();
        RegisterReflectedTypes(*manager, GlobalTypeRegistry(), hidden);
        return manager;
    }

    IScriptManager* CachedCheckManager::Current()
    {
        // The registrars are idempotent; called first so the generation read after them is the
        // one the manager is made from.
        RegisterCoreTypes();
        RegisterScriptFacadeReflection();
        const u64 generation = GlobalTypeRegistry().Generation();
        if (m_manager.Get() != nullptr && generation == m_generation)
        {
            return m_manager.Get();
        }
        m_manager = MakeCheckManager(*m_allocator, m_language.AsView(), u8"check manager", Span<const TypeId>{});
        m_generation = generation; // read before registering: a type added meanwhile rebuilds next time
        if (m_manager.Get() != nullptr)
        {
            ++m_buildCount;
        }
        return m_manager.Get();
    }

    bool CachedCheckManager::Run(const Function<bool(IScriptManager&)>& check)
    {
        ScopedLock lock(m_lock);
        IScriptManager* manager = Current();
        return manager != nullptr && check(*manager);
    }

    void CachedCheckManager::Prepare()
    {
        ScopedLock lock(m_lock);
        (void)Current();
    }

    Array<ScriptApiType> CachedCheckManager::DescribeApi()
    {
        ScopedLock lock(m_lock);
        IScriptManager* manager = Current();
        if (manager == nullptr)
        {
            return {};
        }
        if (m_apiGeneration != m_generation)
        {
            m_api = manager->DescribeBoundApi();
            m_apiGeneration = m_generation;
        }
        return m_api;
    }

    u64 ScriptSurfaceHash(const TypeRegistry& registry)
    {
        const auto text = [](u64 hash, const char* value)
        {
            const usize size = value != nullptr ? std::strlen(value) : 0;
            return HashBytes(value, size + 1, hash); // the terminator keeps "ab"+"c" apart from "a"+"bc"
        };
        const auto number = [](u64 hash, u64 value) { return HashBytes(&value, sizeof(value), hash); };
        u64 sum = 0;
        for (const TypeInfo* type : registry.All())
        {
            if (!(registry.DomainOf(type->id) == kRuntimeTypeDomain))
            {
                continue;
            }
            u64 h = number(kFnv1a64OffsetBasis, type->id);
            h = number(h, type->base != nullptr ? type->base->id : 0);
            for (u32 i = 0; i < type->propertyCount; ++i)
            {
                h = text(h, type->properties[i].name);
                h = number(h, type->properties[i].type != nullptr ? type->properties[i].type->id : 0);
            }
            // A type's id, or 0 for none (void, an unresolved parameter).
            const auto typeId = [](const TypeInfo* (*get)()) -> u64
            {
                const TypeInfo* t = get != nullptr ? get() : nullptr;
                return t != nullptr ? t->id : 0;
            };
            for (u32 i = 0; i < type->methodCount; ++i)
            {
                const MethodInfo& method = type->methods[i];
                h = text(h, method.name);
                h = number(h, (static_cast<u64>(method.paramCount) << 2) | (method.isStatic ? 2u : 0u) |
                                  (method.isConst ? 1u : 0u));
                // The signature, not only its arity: bytecode binds the declaration it compiled
                // against, so a parameter or return type changed at the same count must recook.
                h = number(h, typeId(method.returnType));
                for (u32 p = 0; p < method.paramCount; ++p)
                {
                    h = number(h, typeId(method.params[p].type));
                }
            }
            for (u32 i = 0; i < type->enumeratorCount; ++i)
            {
                h = text(h, type->enumerators[i].name);
                h = number(h, static_cast<u64>(type->enumerators[i].value)); // a constant compiled in
            }
            h = number(h, type->constructorCount);
            for (u32 i = 0; i < type->constructorCount; ++i)
            {
                const ConstructorInfo& constructor = type->constructors[i];
                for (u32 p = 0; p < constructor.paramCount; ++p)
                {
                    h = number(h, typeId(constructor.params[p].type));
                }
            }
            sum += HashInteger(h); // a sum: the registration order does not matter
        }
        return sum;
    }

    ScriptAssemblyCookRegistry& ScriptAssemblyCookRegistry::Get()
    {
        static ScriptAssemblyCookRegistry instance;
        return instance;
    }

    namespace
    {
        void FindAssemblies(content::Group& group, StringView language, StringView file, content::Instance*& best, usize& bestDepth)
        {
            for (content::Instance* instance : group.Instances())
            {
                if (instance->TypeName() != StringView(u8"ScriptAssemblyAsset"))
                {
                    continue;
                }
                RefPtr<ISerializable> object = instance->ReadObject();
                const auto* project = Cast<ScriptAssemblyAsset>(object.Get());
                if (project == nullptr || project->language.AsView() != language)
                {
                    continue;
                }
                const StringView folder = PathParent(project->fileName.View());
                if (!folder.IsEmpty() && !PathIsInside(file, folder))
                {
                    continue;
                }
                // The nearest project: the deepest folder holding the file.
                if (best == nullptr || folder.Size() + 1 > bestDepth)
                {
                    best = instance;
                    bestDepth = folder.Size() + 1;
                }
            }
            for (content::Group* child : group.Groups())
            {
                FindAssemblies(*child, language, file, best, bestDepth);
            }
        }

        void ListSources(foundation::vfs::IEnumerableFileSystem& files, StringView folder, StringView extension, Array<String>& out)
        {
            Array<foundation::vfs::DirEntry> entries;
            if (!files.Enumerate(folder, entries).IsOk())
            {
                return;
            }
            for (const foundation::vfs::DirEntry& entry : entries)
            {
                const String path = folder.IsEmpty() ? entry.name : PathJoin(folder, entry.name.AsView());
                if (entry.isDirectory)
                {
                    // An IDE's build of the project writes into these; they are not sources.
                    if (entry.name.AsView() != StringView(u8"bin") && entry.name.AsView() != StringView(u8"obj"))
                    {
                        ListSources(files, path.AsView(), extension, out);
                    }
                    continue;
                }
                if (pipeline::FileExtensionLower(entry.name.AsView()).AsView() == extension)
                {
                    out.PushBack(path);
                }
            }
        }
    }

    content::Instance* FindScriptAssemblyFor(content::IContentDatabase& sourceDb, StringView language, StringView file)
    {
        content::Instance* best = nullptr;
        usize bestDepth = 0;
        if (content::Group* root = sourceDb.RootGroup())
        {
            FindAssemblies(*root, language, file, best, bestDepth);
        }
        return best;
    }

    Array<String> ListSourcesUnder(foundation::vfs::IFileSystem& sources, StringView folder, StringView extension)
    {
        Array<String> out;
        if (foundation::vfs::IEnumerableFileSystem* files = sources.AsEnumerable())
        {
            ListSources(*files, folder, extension, out);
        }
        // Sorted, so the build reads its sources in one order on every machine.
        out.Sort(
            [](const String& a, const String& b)
            {
                const StringView x = a.AsView();
                const StringView y = b.AsView();
                for (usize i = 0; i < x.Size() && i < y.Size(); ++i)
                    if (x[i] != y[i])
                        return x[i] < y[i];
                return x.Size() < y.Size();
            });
        return out;
    }

    REFLECT_MEMBERS(ScriptAssemblyAsset, "rtti::pipeline::script")
    {
        builder.Attribute("displayName", String(u8"Script project"))
            .Attribute("category", String(u8"Scripting"))
            .Property<&ScriptAssemblyAsset::language>("language")
            .PropAttribute("displayName", String(u8"Language"))
            .PropAttribute("description", String(u8"Backend id (e.g. \"csharp\")"));
    }

    REFLECT_MEMBERS(ScriptClassAsset, "rtti::pipeline::script")
    {
        // v1 adds "className"; v0 payloads still read (the legacy reader) and leave it empty.
        builder.DataVersion(1)
            .ReadsDataVersionsFrom(0)
            .Attribute("displayName", String(u8"Script"))
            .Attribute("category", String(u8"Scripting"))
            .Property<&ScriptClassAsset::language>("language")
            .PropAttribute("displayName", String(u8"Language"))
            .PropAttribute("description", String(u8"Backend id (e.g. \"angelscript\", \"luau\")"))
            .Property<&ScriptClassAsset::className>("className")
            .PropAttribute("displayName", String(u8"Class"))
            .PropAttribute("description", String(u8"The class it plays when its file declares several (empty: the one "
                                                 u8"named like its file, or its only one)"));
    }
}
