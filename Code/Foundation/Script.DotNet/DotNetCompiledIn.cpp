// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the managed runtime's entry points by their C names, for a player the
// runtime and its game are compiled into ahead of time (a native export with C#).
//
// Every name has a stand-in here, defined weakly, which a program without the compiled runtime
// keeps: Present() answers 0 and the runtime starts .NET through hostfxr as in the editor. A
// program linked with the compiled runtime (its whole archive) takes the runtime's definitions
// instead, and Present() answers 1.

module;
#include "Core/Prelude.h"

module foundation.script.dotnet;

import foundation.core;

using namespace foundation::core;

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC has no weak definitions: each name falls back to its stand-in only when nothing defines it.
#define DOTNET_STAND_IN(name) __pragma(comment(linker, "/alternatename:" #name "=" #name "_standin"))
#define DOTNET_STAND_IN_NAME(name) name##_standin
#else
#define DOTNET_STAND_IN(name)
#define DOTNET_STAND_IN_NAME(name) __attribute__((weak)) name
#endif

extern "C"
{
    DOTNET_STAND_IN(assiduous_dotnet_Present)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_Present)() { return 0; }

    // The rest are never called unless Present() answers 1, when the runtime's own replace them.
    DOTNET_STAND_IN(assiduous_dotnet_Initialize)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_Initialize)(const void*) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_LoadAssembly)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_LoadAssembly)(const byte*, i32, const byte*, i32) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_CreateInstance)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_CreateInstance)(const utf8char*, i32, void*, i32, isize*) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_Invoke)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_Invoke)(isize, const utf8char*, i32, void*, i32, void*) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_GetField)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_GetField)(isize, const utf8char*, i32, void*) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_Release)
    void DOTNET_STAND_IN_NAME(assiduous_dotnet_Release)(isize) {}
    DOTNET_STAND_IN(assiduous_dotnet_LoadedAssemblyCount)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_LoadedAssemblyCount)() { return 0; }
    DOTNET_STAND_IN(assiduous_dotnet_DescribeClass)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_DescribeClass)(const utf8char*, i32, void*, i32, const utf8char**, i32*) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_DescribeAssembly)
    i32 DOTNET_STAND_IN_NAME(assiduous_dotnet_DescribeAssembly)(const byte*, i32, const utf8char**, i32*) { return -1; }
    DOTNET_STAND_IN(assiduous_dotnet_LastError)
    const utf8char* DOTNET_STAND_IN_NAME(assiduous_dotnet_LastError)(i32* length)
    {
        if (length != nullptr)
            *length = 0;
        return u8"";
    }
}

namespace foundation::script::dotnet
{
    bool DotNetRuntime::StartCompiledIn(IAllocator& allocator)
    {
        if (assiduous_dotnet_Present() != 1)
            return false;
        m_loadAssembly = reinterpret_cast<decltype(m_loadAssembly)>(&assiduous_dotnet_LoadAssembly);
        m_createInstance = reinterpret_cast<decltype(m_createInstance)>(&assiduous_dotnet_CreateInstance);
        m_getField = reinterpret_cast<decltype(m_getField)>(&assiduous_dotnet_GetField);
        m_release = reinterpret_cast<decltype(m_release)>(&assiduous_dotnet_Release);
        m_loadedAssemblyCount = reinterpret_cast<decltype(m_loadedAssemblyCount)>(&assiduous_dotnet_LoadedAssemblyCount);
        m_describeClass = reinterpret_cast<decltype(m_describeClass)>(&assiduous_dotnet_DescribeClass);
        m_describeAssembly = reinterpret_cast<decltype(m_describeAssembly)>(&assiduous_dotnet_DescribeAssembly);
        m_lastError = reinterpret_cast<decltype(m_lastError)>(&assiduous_dotnet_LastError);
        if (assiduous_dotnet_Initialize(&EngineFunctions(allocator)) != 0)
        {
            (void)FailManaged(-1);
            return true; // compiled in, and it failed to start: Failure() says why
        }
        m_invoke = reinterpret_cast<decltype(m_invoke)>(&assiduous_dotnet_Invoke);
        m_compiledIn = true;
        m_failure.Clear();
        return true;
    }
}
