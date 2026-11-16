// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the `:runtime` partition.
//
// The engine's managed runtime (Engine.Runtime.dll, built beside the binaries under DotNet/) as the
// engine sees it: started once per process on the installed .NET, it loads a game's assembly from
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

        /// Starts .NET with the runtime in `folder` (DefaultRuntimeFolder when empty) and hands it
        /// the engine's functions, made from `allocator`. Failure() says why it did not.
        Status Start(IAllocator& allocator, StringView folder = {});
        [[nodiscard]] bool IsStarted() const noexcept { return m_invoke != nullptr; }

        /// Loads a game's assembly from its image (and its debug symbols, when given, so a fault
        /// names its file and line); its classes are found before earlier ones.
        Status LoadAssembly(Span<const byte> image, Span<const byte> symbols = {});

        /// An instance of `className`, by its public constructor of args.Size() parameters.
        [[nodiscard]] Result<u64> CreateInstance(StringView className, Span<const Variant> args);
        /// Calls `method` on `instance`; "x=" sets its field x. The result, empty for none.
        [[nodiscard]] Result<Variant> Invoke(u64 instance, StringView method, Span<const Variant> args);
        /// Reads `instance`'s public field `name`.
        [[nodiscard]] Result<Variant> GetField(u64 instance, StringView name);
        void Release(u64 instance);

        /// What `className` offers (DescribeClass's lines, see Runtime.cs), an instance made with
        /// `args` to read its defaults from.
        [[nodiscard]] Result<String> DescribeClass(StringView className, Span<const Variant> args);

        /// Every public class of an assembly image, for the cook ("class NAME" before each class's
        /// lines, see Runtime.cs), without keeping the image loaded.
        [[nodiscard]] Result<String> DescribeAssembly(Span<const byte> image);

        /// Why the last call failed: the host's reason, or the managed one. A failed call's error is
        /// NotFound when what it asked for is not there (a class, a method, a field).
        [[nodiscard]] StringView Failure() const noexcept { return m_failure.AsView(); }

    private:
        Status Fail(StringView why);
        Status FailManaged(i32 rc); // NotFound for what is not there, else Unknown
        Array<DotNetValue> Lend(Span<const Variant> args) const;

        DotNetHost m_host;
        String m_failure;
        i32 (*m_loadAssembly)(const byte*, i32, const byte*, i32) = nullptr;
        i32 (*m_createInstance)(const utf8char*, i32, DotNetValue*, i32, isize*) = nullptr;
        i32 (*m_invoke)(isize, const utf8char*, i32, DotNetValue*, i32, DotNetValue*) = nullptr;
        i32 (*m_getField)(isize, const utf8char*, i32, DotNetValue*) = nullptr;
        void (*m_release)(isize) = nullptr;
        i32 (*m_describeClass)(const utf8char*, i32, DotNetValue*, i32, const utf8char**, i32*) = nullptr;
        i32 (*m_describeAssembly)(const byte*, i32, const utf8char**, i32*) = nullptr;
        const utf8char* (*m_lastError)(i32*) = nullptr;
    };
}
