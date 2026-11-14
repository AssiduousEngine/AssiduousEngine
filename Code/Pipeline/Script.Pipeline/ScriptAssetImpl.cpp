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
