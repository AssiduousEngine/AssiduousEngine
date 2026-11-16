// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the .NET host.
//
// The engine hosts .NET (it is never hosted by it): it finds the installed runtime's hostfxr,
// starts the runtime from a runtimeconfig.json, and asks for pointers to a C# assembly's
// [UnmanagedCallersOnly] methods. From there C# and the engine call each other through plain
// function pointers, the same boundary a NativeAOT build uses, so one C# codebase serves both.
//
// The hostfxr ABI is declared here rather than taken from the SDK's headers: a handful of C
// functions, stable and documented (docs.microsoft.com, "Write a custom .NET host").

module;
#include "Core/Prelude.h"

export module foundation.script.dotnet;

import foundation.core;

using namespace foundation::core;

namespace foundation::script::dotnet::detail
{
#if PLATFORM_WINDOWS
    using HostChar = wchar_t; // hostfxr's char_t
#else
    using HostChar = char;
#endif
    using HostfxrHandle = void*;
    struct HostfxrInitializeParameters
    {
        usize size;
        const HostChar* hostPath;
        const HostChar* dotnetRoot;
    };
    using InitializeForRuntimeConfigFn = i32 (*)(const HostChar* runtimeConfigPath,
                                                 const HostfxrInitializeParameters* parameters,
                                                 HostfxrHandle* handle);
    using GetRuntimeDelegateFn = i32 (*)(HostfxrHandle handle, i32 type, void** delegate);
    using CloseFn = i32 (*)(HostfxrHandle handle);
    using LoadAssemblyAndGetFunctionPointerFn = i32 (*)(const HostChar* assemblyPath, const HostChar* typeName,
                                                        const HostChar* methodName, const HostChar* delegateTypeName,
                                                        void* reserved, void** delegate);
    constexpr i32 kLoadAssemblyAndGetFunctionPointer = 5; // hdt_load_assembly_and_get_function_pointer
    // UNMANAGEDCALLERSONLY_METHOD: the delegate type that says "an [UnmanagedCallersOnly] method".
    inline const HostChar* UnmanagedCallersOnly() noexcept { return reinterpret_cast<const HostChar*>(static_cast<isize>(-1)); }

    // `text` as hostfxr takes it: UTF-8 on POSIX, UTF-16 on Windows.
    struct HostString
    {
#if PLATFORM_WINDOWS
        WideString value;
        explicit HostString(StringView text) : value(ToWide(text)) {}
        [[nodiscard]] const HostChar* CStr() const noexcept { return reinterpret_cast<const HostChar*>(value.CStr()); }
#else
        String value;
        explicit HostString(StringView text) : value(text) {}
        [[nodiscard]] const HostChar* CStr() const noexcept { return reinterpret_cast<const HostChar*>(value.CStr()); }
#endif
    };

    // A version folder's numbers ("10.0.15" is 10, 0, 15; a suffix's numbers follow).
    inline void ParseVersion(StringView name, Array<u32>& out)
    {
        out.Clear();
        u32 value = 0;
        bool any = false;
        for (const utf8char c : name)
        {
            if (c >= u8'0' && c <= u8'9')
            {
                value = value * 10u + static_cast<u32>(c - u8'0');
                any = true;
            }
            else if (any)
            {
                out.PushBack(value);
                value = 0;
                any = false;
            }
        }
        if (any)
            out.PushBack(value);
    }

    // The highest version folder under `fxr` (versions compared number by number).
    inline String HighestVersion(StringView fxr)
    {
        struct Best
        {
            String name;
            Array<u32> parts;
        } best;
        (void)ListDirectory(fxr,
                            [](void* ctx, StringView name, bool isDirectory)
                            {
                                if (!isDirectory)
                                    return;
                                auto& state = *static_cast<Best*>(ctx);
                                Array<u32> parts;
                                ParseVersion(name, parts);
                                bool better = state.name.IsEmpty();
                                for (usize i = 0; !better && i < parts.Size(); ++i)
                                {
                                    const u32 held = i < state.parts.Size() ? state.parts[i] : 0u;
                                    if (parts[i] != held)
                                    {
                                        better = parts[i] > held;
                                        break;
                                    }
                                }
                                if (better)
                                {
                                    state.name = String(name);
                                    state.parts = Move(parts);
                                }
                            },
                            &best);
        return best.name;
    }

    // A hostfxr status as it is documented and searched for: 0x80008083.
    inline String StatusText(i32 rc)
    {
        constexpr utf8char kDigits[] = u8"0123456789abcdef";
        String text(u8"0x");
        const u32 value = static_cast<u32>(rc);
        for (i32 shift = 28; shift >= 0; shift -= 4)
            text.PushBack(kDigits[(value >> shift) & 0xFu]);
        return text;
    }

    inline StringView HostfxrFileName() noexcept
    {
#if PLATFORM_WINDOWS
        return u8"hostfxr.dll";
#elif PLATFORM_MACOS
        return u8"libhostfxr.dylib";
#else
        return u8"libhostfxr.so";
#endif
    }
}

export namespace foundation::script::dotnet
{
    /// The hostfxr library of the .NET install at `root` (its newest host/fxr version); empty when
    /// `root` holds none.
    [[nodiscard]] inline String HostfxrPathIn(StringView root)
    {
        if (root.IsEmpty())
            return String{};
        const String fxr = PathJoin(root, u8"host/fxr");
        const String version = detail::HighestVersion(fxr.AsView());
        if (version.IsEmpty())
            return String{};
        const String path = PathJoin(PathJoin(fxr.AsView(), version.AsView()).AsView(), detail::HostfxrFileName());
        return FileExists(path.AsView()) ? path : String{};
    }

    /// The folder .NET is installed in: DOTNET_ROOT, else the folder of the `dotnet` command on
    /// PATH with its links followed (a snap's or a package manager's `dotnet` is a link into the
    /// install), else the standard places. Empty when none holds a runtime host.
    [[nodiscard]] inline String FindDotNetRoot()
    {
        if (const Optional<String> root = GetEnvironmentVariable(u8"DOTNET_ROOT"); root.HasValue())
            if (!HostfxrPathIn(root.Value().AsView()).IsEmpty())
                return root.Value();
        if (const Optional<String> path = GetEnvironmentVariable(u8"PATH"); path.HasValue())
        {
#if PLATFORM_WINDOWS
            constexpr utf8char kListSeparator = u8';';
            const StringView command = u8"dotnet.exe";
#else
            constexpr utf8char kListSeparator = u8':';
            const StringView command = u8"dotnet";
#endif
            const StringView list = path.Value().AsView();
            usize start = 0;
            for (usize i = 0; i <= list.Size(); ++i)
            {
                if (i < list.Size() && list[i] != kListSeparator)
                    continue;
                const StringView folder = list.SubStr(start, i - start);
                start = i + 1;
                if (folder.IsEmpty())
                    continue;
                const String candidate = PathJoin(folder, command);
                if (!FileExists(candidate.AsView()))
                    continue;
                const String resolved = PathResolveLinks(candidate.AsView());
                const String root(PathParent(resolved.AsView()));
                if (!HostfxrPathIn(root.AsView()).IsEmpty())
                    return root;
            }
        }
#if PLATFORM_WINDOWS
        const StringView standard[] = {u8"C:/Program Files/dotnet"};
#elif PLATFORM_MACOS
        const StringView standard[] = {u8"/usr/local/share/dotnet", u8"/opt/homebrew/opt/dotnet/libexec"};
#else
        const StringView standard[] = {u8"/usr/lib/dotnet", u8"/usr/share/dotnet", u8"/usr/lib64/dotnet"};
#endif
        for (const StringView root : standard)
            if (!HostfxrPathIn(root).IsEmpty())
                return String(root);
        return String{};
    }

    /// One .NET runtime in the process, started from a runtimeconfig.json. hostfxr allows one
    /// runtime per process, so there is one host.
    class DotNetHost
    {
    public:
        DotNetHost() = default;
        DotNetHost(const DotNetHost&) = delete;
        DotNetHost& operator=(const DotNetHost&) = delete;
        ~DotNetHost()
        {
            // The host context is left open, not closed: the runtime never unloads, and the process's
            // host outlives the static destruction at exit, where hostfxr_close reached hostfxr's own
            // freed state and aborted the editor on quit ("double free or corruption"). Stop closes it
            // while the process runs. hostfxr stays loaded for the same reason: a hostfxr loaded again
            // cannot reach the runtime (a later Start then failed with 0x80008081).
            m_handle = nullptr;
            m_load = nullptr;
            m_library.Detach();
        }

        /// Loads hostfxr from `dotnetRoot` (FindDotNetRoot when empty) and starts the runtime the
        /// config names. On failure Failure() says why.
        Status Start(StringView runtimeConfigPath, StringView dotnetRoot = {})
        {
            Stop();
            m_root = dotnetRoot.IsEmpty() ? FindDotNetRoot() : String(dotnetRoot);
            const String hostfxr = HostfxrPathIn(m_root.AsView());
            if (hostfxr.IsEmpty())
                return Fail(u8"no .NET install found (set DOTNET_ROOT, or put dotnet on PATH)");
            if (!m_library.Load(hostfxr.AsView()).IsOk())
                return Fail(Format(u8"{} did not load", hostfxr).AsView());
            const auto initialize =
                m_library.GetSymbol<detail::InitializeForRuntimeConfigFn>(u8"hostfxr_initialize_for_runtime_config");
            const auto getDelegate = m_library.GetSymbol<detail::GetRuntimeDelegateFn>(u8"hostfxr_get_runtime_delegate");
            m_close = m_library.GetSymbol<detail::CloseFn>(u8"hostfxr_close");
            if (initialize == nullptr || getDelegate == nullptr || m_close == nullptr)
                return Fail(u8"hostfxr lacks the functions a host needs");
            const detail::HostString config(runtimeConfigPath);
            const detail::HostString root(m_root.AsView());
            const detail::HostfxrInitializeParameters parameters{sizeof(detail::HostfxrInitializeParameters), nullptr, root.CStr()};
            if (const i32 rc = initialize(config.CStr(), &parameters, &m_handle); rc < 0 || m_handle == nullptr)
                return Fail(Format(u8"the runtime did not start from {} (hostfxr {})", runtimeConfigPath, detail::StatusText(rc)).AsView());
            void* load = nullptr;
            if (const i32 rc = getDelegate(m_handle, detail::kLoadAssemblyAndGetFunctionPointer, &load); rc < 0 || load == nullptr)
                return Fail(Format(u8"no assembly loader from the runtime (hostfxr {})", detail::StatusText(rc)).AsView());
            m_load = reinterpret_cast<detail::LoadAssemblyAndGetFunctionPointerFn>(load);
            m_failure.Clear();
            return Status{};
        }

        /// A pointer to `typeName`'s `methodName` in the assembly at `assemblyPath`, a static
        /// [UnmanagedCallersOnly] method; `typeName` is assembly-qualified ("Ns.Type, Assembly").
        /// Null, with Failure() saying why, when it is not there.
        [[nodiscard]] void* EntryPoint(StringView assemblyPath, StringView typeName, StringView methodName)
        {
            if (m_load == nullptr)
            {
                (void)Fail(u8"the runtime is not started");
                return nullptr;
            }
            const detail::HostString assembly(assemblyPath);
            const detail::HostString type(typeName);
            const detail::HostString method(methodName);
            void* entry = nullptr;
            if (const i32 rc = m_load(assembly.CStr(), type.CStr(), method.CStr(), detail::UnmanagedCallersOnly(), nullptr, &entry);
                rc < 0 || entry == nullptr)
            {
                (void)Fail(Format(u8"{}.{} not found in {} (hostfxr {})", typeName, methodName, assemblyPath,
                                  detail::StatusText(rc)).AsView());
                return nullptr;
            }
            return entry;
        }

        /// Closes the host context. The runtime itself stays loaded for the process (CoreCLR does
        /// not unload); a later Start in the same process reuses it.
        void Stop() noexcept
        {
            if (m_handle != nullptr && m_close != nullptr)
                (void)m_close(m_handle);
            m_handle = nullptr;
            m_load = nullptr;
        }

        [[nodiscard]] bool IsStarted() const noexcept { return m_load != nullptr; }
        [[nodiscard]] StringView Root() const noexcept { return m_root.AsView(); }
        [[nodiscard]] StringView Failure() const noexcept { return m_failure.AsView(); }

    private:
        Status Fail(StringView why)
        {
            m_failure = String(why);
            return Status{ErrorCode::NotFound};
        }

        DynamicLibrary m_library;
        detail::HostfxrHandle m_handle = nullptr;
        detail::CloseFn m_close = nullptr;
        detail::LoadAssemblyAndGetFunctionPointerFn m_load = nullptr;
        String m_root;
        String m_failure;
    };
}
