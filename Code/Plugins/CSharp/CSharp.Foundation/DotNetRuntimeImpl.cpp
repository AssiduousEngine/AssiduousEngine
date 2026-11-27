// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CSharp.Foundation - the engine's side of the managed runtime (see DotNetRuntime.cppm).

module;
#include "Core/Prelude.h"

module csharp.foundation;

import foundation.core;

using namespace foundation::core;

namespace csharp
{
    namespace
    {
        constexpr StringView kRuntimeType = u8"Engine.Runtime, Engine.Runtime";
        // What an entry point returns when the class, constructor, method or field it was given
        // does not exist (Runtime.cs's Missing); any other nonzero code is a failure.
        constexpr i32 kManagedNotFound = -2;

        i32 SizeOf(StringView text) { return static_cast<i32>(text.Size()); }
    }

    String DefaultRuntimeFolder()
    {
        const String executable = ExecutablePath();
        return PathJoin(PathParent(executable.AsView()), u8"DotNet");
    }

    Status DotNetRuntime::Fail(StringView why)
    {
        m_failure = String(why);
        return Status{ErrorCode::Internal};
    }

    Status DotNetRuntime::FailManaged(i32 returnCode)
    {
        i32 length = 0;
        const utf8char* text = m_lastError(&length);
        m_failure = String(StringView(text, static_cast<usize>(length)));
        return Status{returnCode == kManagedNotFound ? ErrorCode::NotFound : ErrorCode::Unknown};
    }

    Array<DotNetValue> DotNetRuntime::Lend(Span<const Variant> args) const
    {
        Array<DotNetValue> values;
        values.Reserve(args.Size());
        for (const Variant& arg : args)
            values.PushBack(LendValue(arg));
        return values;
    }

    Status DotNetRuntime::Start(IAllocator& allocator, StringView folder)
    {
        if (IsStarted())
            return Status{};
        if (StartCompiledIn(allocator))
            return IsStarted() ? Status{} : Status{ErrorCode::Unknown};
        const String root = folder.IsEmpty() ? DefaultRuntimeFolder() : String(folder);
        const String assembly = PathJoin(root.AsView(), u8"Engine.Runtime.dll");
        const String config = PathJoin(root.AsView(), u8"Engine.Runtime.runtimeconfig.json");
        if (!FileExists(assembly.AsView()))
            return Fail(Format(u8"the engine's managed runtime is not at {}", assembly).AsView());
        if (!m_host.Start(config.AsView()).IsOk())
            return Fail(m_host.Failure());
        auto find = [&](StringView method) { return m_host.EntryPoint(assembly.AsView(), kRuntimeType, method); };
        auto* initialize = reinterpret_cast<i32 (*)(const DotNetEngineFunctions*)>(find(u8"Initialize"));
        m_loadAssembly = reinterpret_cast<decltype(m_loadAssembly)>(find(u8"LoadAssembly"));
        m_createInstance = reinterpret_cast<decltype(m_createInstance)>(find(u8"CreateInstance"));
        m_getField = reinterpret_cast<decltype(m_getField)>(find(u8"GetField"));
        m_bind = reinterpret_cast<decltype(m_bind)>(find(u8"Bind"));
        m_invokeBound = reinterpret_cast<decltype(m_invokeBound)>(find(u8"InvokeBound"));
        m_release = reinterpret_cast<decltype(m_release)>(find(u8"Release"));
        m_releaseOwner = reinterpret_cast<decltype(m_releaseOwner)>(find(u8"ReleaseOwner"));
        m_loadedAssemblyCount = reinterpret_cast<decltype(m_loadedAssemblyCount)>(find(u8"LoadedAssemblyCount"));
        m_describeClass = reinterpret_cast<decltype(m_describeClass)>(find(u8"DescribeClass"));
        m_describeAssembly = reinterpret_cast<decltype(m_describeAssembly)>(find(u8"DescribeAssembly"));
        m_lastError = reinterpret_cast<decltype(m_lastError)>(find(u8"LastError"));
        auto* invoke = reinterpret_cast<decltype(m_invoke)>(find(u8"Invoke"));
        if (initialize == nullptr || m_loadAssembly == nullptr || m_createInstance == nullptr || m_getField == nullptr ||
            m_bind == nullptr || m_invokeBound == nullptr ||
            m_release == nullptr || m_releaseOwner == nullptr || m_loadedAssemblyCount == nullptr || m_describeClass == nullptr || m_describeAssembly == nullptr || m_lastError == nullptr ||
            invoke == nullptr)
            return Fail(m_host.Failure());
        if (const i32 returnCode = initialize(&EngineFunctions(allocator)); returnCode != 0)
            return FailManaged(returnCode);
        m_invoke = invoke; // started: every entry point found and the layouts agree
        m_failure.Clear();
        return Status{};
    }

    Status DotNetRuntime::LoadAssembly(u64 owner, Span<const byte> image, Span<const byte> symbols)
    {
        if (!IsStarted())
            return Fail(u8"the runtime is not started");
        if (const i32 returnCode = m_loadAssembly(static_cast<isize>(owner), image.Data(), static_cast<i32>(image.Size()), symbols.Data(),
                                                  static_cast<i32>(symbols.Size()));
            returnCode != 0)
            return FailManaged(returnCode);
        return Status{};
    }

    Result<u64> DotNetRuntime::CreateInstance(u64 owner, StringView className, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        Array<DotNetValue> values = Lend(args);
        isize instance = 0;
        if (const i32 returnCode = m_createInstance(static_cast<isize>(owner), className.Data(), SizeOf(className), values.Data(), static_cast<i32>(values.Size()), &instance);
            returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return static_cast<u64>(instance);
    }

    Result<Variant> DotNetRuntime::Invoke(u64 instance, StringView method, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        Array<DotNetValue> values = Lend(args);
        DotNetValue result{};
        if (const i32 returnCode = m_invoke(static_cast<isize>(instance), method.Data(), SizeOf(method), values.Data(),
                                    static_cast<i32>(values.Size()), &result);
            returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return ToVariant(result);
    }

    Result<i32> DotNetRuntime::Bind(u64 instance, StringView method, u32 count)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        i32 id = -1;
        if (const i32 returnCode = m_bind(static_cast<isize>(instance), method.Data(), SizeOf(method), static_cast<i32>(count), &id);
            returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return id;
    }

    Result<Variant> DotNetRuntime::InvokeBound(i32 id, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        // A handler's few arguments on the stack; more than that, on the heap.
        constexpr usize kOnStack = 8;
        DotNetValue onStack[kOnStack];
        Array<DotNetValue> onHeap;
        DotNetValue* values = onStack;
        if (args.Size() > kOnStack)
        {
            onHeap = Lend(args);
            values = onHeap.Data();
        }
        else
            for (usize i = 0; i < args.Size(); ++i)
                onStack[i] = LendValue(args[i]);
        DotNetValue result{};
        if (const i32 returnCode = m_invokeBound(id, values, static_cast<i32>(args.Size()), &result); returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return ToVariant(result);
    }

    Result<Variant> DotNetRuntime::GetField(u64 instance, StringView name)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        DotNetValue result{};
        if (const i32 returnCode = m_getField(static_cast<isize>(instance), name.Data(), SizeOf(name), &result); returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return ToVariant(result);
    }

    void DotNetRuntime::Release(u64 instance)
    {
        if (IsStarted() && instance != 0)
            m_release(static_cast<isize>(instance));
    }

    void DotNetRuntime::ReleaseOwner(u64 owner)
    {
        if (IsStarted())
            m_releaseOwner(static_cast<isize>(owner));
    }

    u32 DotNetRuntime::LoadedAssemblyCount()
    {
        return IsStarted() ? static_cast<u32>(m_loadedAssemblyCount()) : 0u;
    }

    Result<String> DotNetRuntime::DescribeClass(u64 owner, StringView className, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        Array<DotNetValue> values = Lend(args);
        const utf8char* text = nullptr;
        i32 length = 0;
        if (const i32 returnCode = m_describeClass(static_cast<isize>(owner), className.Data(), SizeOf(className), values.Data(), static_cast<i32>(values.Size()), &text,
                                           &length);
            returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return String(StringView(text, static_cast<usize>(length)));
    }

    Result<String> DotNetRuntime::DescribeAssembly(Span<const byte> image)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        const utf8char* text = nullptr;
        i32 length = 0;
        if (const i32 returnCode = m_describeAssembly(image.Data(), static_cast<i32>(image.Size()), &text, &length); returnCode != 0)
            return Err(FailManaged(returnCode).Code());
        return String(StringView(text, static_cast<usize>(length)));
    }
}
