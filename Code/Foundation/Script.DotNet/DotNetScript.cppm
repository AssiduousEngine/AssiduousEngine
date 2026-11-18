// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the `:backend` partition.
//
// C# as a script language beside AngelScript and Luau (language "csharp", files ".cs"). Its classes
// do not compile from source at run time: a project's C# builds into one assembly at cook, carried
// as each class's bytecode, and the context loads that assembly (once while it is current, however
// many classes share it) into the process's managed runtime. A rebuilt assembly loads beside the
// one it replaces, whose context unloads when its last instance goes. Behaviours are then made, called and read as the other
// languages' are. A call into C# cannot be stopped part way, so the budget's seconds and heap are
// not held to.

module;
#include "Core/Prelude.h"

export module foundation.script.dotnet:backend;

import foundation.core;
import foundation.script;
import :runtime;

using namespace foundation::core;

export namespace foundation::script::dotnet
{
    /// The language's id, as a ScriptClass records it.
    inline constexpr StringView kCSharpLanguage = u8"csharp";

    /// The runtime every C# context in the process runs in (.NET starts once per process), started
    /// on first use with `allocator`. Null, with the reason logged, when it does not start.
    [[nodiscard]] DotNetRuntime* ProcessRuntime(IAllocator& allocator);

    [[nodiscard]] RefPtr<IScriptManager> CreateDotNetScriptManager(IAllocator& allocator);

    /// Registers C# with the backend registry. Idempotent.
    void RegisterDotNetScriptBackend();

    /// The start of the name of the folder a C# project builds in (in the temp folder, a hash
    /// after it). The build compiles a copy of the project's sources in its src/ folder, so a
    /// stack frame names that copy.
    inline constexpr StringView kCSharpBuildFolderPrefix = u8"csharp-build-";

    /// A C# exception's stack frames ("at <function> in <file>:line <n>"), innermost first. A
    /// frame in the build's copy of a source names the project's source path (Code/Weapon.cs); a
    /// frame without a source (the engine's own code) has no file.
    [[nodiscard]] Array<ScriptStackFrame> ParseCSharpStack(StringView trace);
}
