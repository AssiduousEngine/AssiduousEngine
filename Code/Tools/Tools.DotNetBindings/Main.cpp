// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.DotNetBindings <out.cs>: writes the C# bindings of the engine's bound types, generated
// from its reflection, so C# is built against the engine it runs with.
#include "Core/Prelude.h"
#include <cstdio>

import foundation.core;
import foundation.script.dotnet;
import engine.composition;

using namespace foundation::core;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: Tools.DotNetBindings <out.cs>\n");
        return 2;
    }
    // The surface an application's scripts see: the core types, every facade, every component.
    RegisterCoreTypes();
    engine::RegisterAllScriptFacades();
    engine::RegisterAllSceneComponentReflection();
    const Array<const TypeInfo*> types = GlobalTypeRegistry().All();
    const String source =
        foundation::script::dotnet::GenerateCSharpBindings(Span<const TypeInfo* const>{types.Data(), types.Size()});
    const StringView path(reinterpret_cast<const utf8char*>(argv[1]));
    if (!WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(source.Data()), source.Size())).IsOk())
    {
        std::fprintf(stderr, "Tools.DotNetBindings: could not write %s\n", argv[1]);
        return 1;
    }
    std::printf("Tools.DotNetBindings: %zu types registered\n", static_cast<size_t>(types.Size()));
    return 0;
}
