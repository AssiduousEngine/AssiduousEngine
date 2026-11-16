// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the engine's side of the managed runtime (see DotNetRuntime.cppm).

module;
#include "Core/Prelude.h"

module foundation.script.dotnet;

import foundation.core;

using namespace foundation::core;

namespace foundation::script::dotnet
{
    namespace
    {
        constexpr StringView kRuntimeType = u8"Engine.Runtime, Engine.Runtime";
        constexpr i32 kManagedMissing = -2; // Runtime.cs: what was asked for is not there

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

    Status DotNetRuntime::FailManaged(i32 rc)
    {
        i32 length = 0;
        const utf8char* text = m_lastError(&length);
        m_failure = String(StringView(text, static_cast<usize>(length)));
        return Status{rc == kManagedMissing ? ErrorCode::NotFound : ErrorCode::Unknown};
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
        m_release = reinterpret_cast<decltype(m_release)>(find(u8"Release"));
        m_describeClass = reinterpret_cast<decltype(m_describeClass)>(find(u8"DescribeClass"));
        m_lastError = reinterpret_cast<decltype(m_lastError)>(find(u8"LastError"));
        auto* invoke = reinterpret_cast<decltype(m_invoke)>(find(u8"Invoke"));
        if (initialize == nullptr || m_loadAssembly == nullptr || m_createInstance == nullptr || m_getField == nullptr ||
            m_release == nullptr || m_describeClass == nullptr || m_lastError == nullptr || invoke == nullptr)
            return Fail(m_host.Failure());
        if (const i32 rc = initialize(&EngineFunctions(allocator)); rc != 0)
            return FailManaged(rc);
        m_invoke = invoke; // started: every entry point found and the layouts agree
        m_failure.Clear();
        return Status{};
    }

    Status DotNetRuntime::LoadAssembly(Span<const byte> image, Span<const byte> symbols)
    {
        if (!IsStarted())
            return Fail(u8"the runtime is not started");
        if (const i32 rc = m_loadAssembly(image.Data(), static_cast<i32>(image.Size()), symbols.Data(), static_cast<i32>(symbols.Size()));
            rc != 0)
            return FailManaged(rc);
        return Status{};
    }

    Result<u64> DotNetRuntime::CreateInstance(StringView className, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        Array<DotNetValue> values = Lend(args);
        isize instance = 0;
        if (const i32 rc = m_createInstance(className.Data(), SizeOf(className), values.Data(), static_cast<i32>(values.Size()), &instance);
            rc != 0)
            return Err(FailManaged(rc).Code());
        return static_cast<u64>(instance);
    }

    Result<Variant> DotNetRuntime::Invoke(u64 instance, StringView method, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        Array<DotNetValue> values = Lend(args);
        DotNetValue result{};
        if (const i32 rc = m_invoke(static_cast<isize>(instance), method.Data(), SizeOf(method), values.Data(),
                                    static_cast<i32>(values.Size()), &result);
            rc != 0)
            return Err(FailManaged(rc).Code());
        return ToVariant(result);
    }

    Result<Variant> DotNetRuntime::GetField(u64 instance, StringView name)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        DotNetValue result{};
        if (const i32 rc = m_getField(static_cast<isize>(instance), name.Data(), SizeOf(name), &result); rc != 0)
            return Err(FailManaged(rc).Code());
        return ToVariant(result);
    }

    void DotNetRuntime::Release(u64 instance)
    {
        if (IsStarted() && instance != 0)
            m_release(static_cast<isize>(instance));
    }

    Result<String> DotNetRuntime::DescribeClass(StringView className, Span<const Variant> args)
    {
        if (!IsStarted())
            return Err(Fail(u8"the runtime is not started").Code());
        Array<DotNetValue> values = Lend(args);
        const utf8char* text = nullptr;
        i32 length = 0;
        if (const i32 rc = m_describeClass(className.Data(), SizeOf(className), values.Data(), static_cast<i32>(values.Size()), &text,
                                           &length);
            rc != 0)
            return Err(FailManaged(rc).Code());
        return String(StringView(text, static_cast<usize>(length)));
    }
}
