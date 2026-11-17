// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the `:runtime` partition.
//
// The engine's managed runtime (Engine.Runtime.dll, built beside the binaries under DotNet/) as the
// engine sees it: started once per process on the installed .NET, it loads an application's assembly from
// its image and creates, calls and reads the behaviours in it. An instance is a handle the runtime
// holds for the engine until Release. Values cross as Variants, through the bridge.

module;
#include "Core/Prelude.h"

export module foundation.script.dotnet:runtime;

import foundation.core;
import :host;
import :bridge;

using namespace foundation::core;

export namespace foundation::script::dotnet
{
    /// Where the runtime is looked for: DotNet/ beside the running executable.
    [[nodiscard]] String DefaultRuntimeFolder();

    class DotNetRuntime
    {
    public:
        DotNetRuntime() = default;
        DotNetRuntime(const DotNetRuntime&) = delete;
        DotNetRuntime& operator=(const DotNetRuntime&) = delete;

        /// Starts the runtime and hands it the engine's functions, made from `allocator`: the one
        /// compiled into this program when there is one (an exported player), else .NET with the
        /// runtime in `folder` (DefaultRuntimeFolder when empty). Failure() says why it did not.
        Status Start(IAllocator& allocator, StringView folder = {});
        [[nodiscard]] bool IsStarted() const noexcept { return m_invoke != nullptr; }
        /// Whether the runtime started is the one compiled into this program.
        [[nodiscard]] bool IsCompiledIn() const noexcept { return m_compiledIn; }

        /// Loads an application's assembly for `owner` (a script context: one run's C#) from its image
        /// (and its debug symbols, when given, so a fault names its file and line); for that owner its
        /// classes are found before earlier ones. An image already current for the owner is not loaded
        /// again; another owner loads its own copy, so two runs never share a class's statics. An
        /// assembly whose classes all have newer ones unloads when the last instance of its classes is
        /// released. Compiled in, nothing loads and every owner finds the classes compiled in.
        Status LoadAssembly(u64 owner, Span<const byte> image, Span<const byte> symbols = {});

        /// An instance of `className` as `owner` finds it, by its public constructor of args.Size()
        /// parameters.
        [[nodiscard]] Result<u64> CreateInstance(u64 owner, StringView className, Span<const Variant> args);
        /// Calls `method` on `instance`; "x=" sets its field x. The result, empty for none.
        [[nodiscard]] Result<Variant> Invoke(u64 instance, StringView method, Span<const Variant> args);
        /// Reads `instance`'s public field `name`.
        [[nodiscard]] Result<Variant> GetField(u64 instance, StringView name);
        void Release(u64 instance);
        /// Lets `owner` go (its script context is going): its classes are no longer found, and each of
        /// its assemblies unloads when the last instance of it is released.
        void ReleaseOwner(u64 owner);

        /// How many application assemblies are loaded (in use, or unloading and not yet collected), after
        /// a full collection: what a test of unloading reads.
        [[nodiscard]] u32 LoadedAssemblyCount();

        /// What `className`, as `owner` finds it, offers (DescribeClass's lines, see Runtime.cs), an
        /// instance made with `args` to read its defaults from.
        [[nodiscard]] Result<String> DescribeClass(u64 owner, StringView className, Span<const Variant> args);

        /// Every public class of an assembly image, for the cook ("class NAME" before each class's
        /// lines, see Runtime.cs), without keeping the image loaded.
        [[nodiscard]] Result<String> DescribeAssembly(Span<const byte> image);

        /// Why the last call failed: the host's reason, or the managed one. A failed call's error is
        /// NotFound when what it asked for is not there (a class, a method, a field).
        [[nodiscard]] StringView Failure() const noexcept { return m_failure.AsView(); }

    private:
        Status Fail(StringView why);
        // Takes the compiled-in runtime when this program has one (DotNetCompiledIn.cpp): true when
        // it does, started or not; false when there is none.
        bool StartCompiledIn(IAllocator& allocator);
        Status FailManaged(i32 returnCode); // NotFound when what it named does not exist, else Unknown
        Array<DotNetValue> Lend(Span<const Variant> args) const;

        DotNetHost m_host;
        String m_failure;
        i32 (*m_loadAssembly)(isize, const byte*, i32, const byte*, i32) = nullptr;
        i32 (*m_createInstance)(isize, const utf8char*, i32, DotNetValue*, i32, isize*) = nullptr;
        i32 (*m_invoke)(isize, const utf8char*, i32, DotNetValue*, i32, DotNetValue*) = nullptr;
        i32 (*m_getField)(isize, const utf8char*, i32, DotNetValue*) = nullptr;
        void (*m_release)(isize) = nullptr;
        void (*m_releaseOwner)(isize) = nullptr;
        i32 (*m_loadedAssemblyCount)() = nullptr;
        i32 (*m_describeClass)(isize, const utf8char*, i32, DotNetValue*, i32, const utf8char**, i32*) = nullptr;
        i32 (*m_describeAssembly)(const byte*, i32, const utf8char**, i32*) = nullptr;
        const utf8char* (*m_lastError)(i32*) = nullptr;
        bool m_compiledIn = false;
    };
}
