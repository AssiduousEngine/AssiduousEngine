// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script.DotNet - the `script.dotnet.pipeline` module (tooling).
//
// The C# cook. A C# project (a .csproj, a ScriptAssemblyAsset) builds with `dotnet build`: every
// .cs under its folder, against the engine's managed runtime, into one assembly with its debug
// symbols; the runtime then describes every public class (its handlers, its inspector fields with
// their defaults, tooltips and replication). Each .cs asset's class cooks from that description.
// A build's errors come back at their source file and line.

module;
#include "Core/Prelude.h"

export module script.dotnet.pipeline;

import foundation.core;
import foundation.script;
import foundation.script.resource;
import script.pipeline;

using namespace foundation::core;
using namespace foundation::script;

export namespace pipeline
{
    /// The engine's managed runtime a project builds against (Engine.Runtime.dll), where the
    /// engine looks for it: DotNet/ beside the running executable.
    [[nodiscard]] String EngineRuntimeAssemblyPath();

    /// The C# classes `source` declares (`class Name`, after any modifiers), in order.
    [[nodiscard]] Array<String> ListCSharpClassNames(StringView source);

    /// Parses a description the runtime wrote (Runtime.cs's DescribeAssembly) into `out`'s classes;
    /// a "warning" line goes to `sink` as a warning.
    void ParseAssemblyDescription(StringView text, ScriptAssemblySource& out, CookScriptErrorSink& sink);

    /// The build's errors and warnings in `output` (MSBuild's "file(line,col): error CODE: text"),
    /// each once, to `sink`, with a file under `root` named by its path relative to it.
    void ReportBuildOutput(StringView output, StringView root, CookScriptErrorSink& sink);

    /// Registers the C# cooks (the class cook and the project build), made from `allocator`, and the
    /// C# backend. Idempotent.
    void RegisterDotNetScriptCook(IAllocator& allocator);
}
