// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.DotNetBindings <out.cs> [--left-out <list>]: writes the C# bindings of the engine's bound
// types, generated from its reflection, so C# is built against the engine it runs with, and names
// every member it leaves out. With --left-out it fails unless what it left out is exactly the list's
// lines (LeftOut.txt beside it): no member goes missing from C# unnoticed.
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
        std::fprintf(stderr, "usage: Tools.DotNetBindings <out.cs> [--left-out <list>]\n");
        return 2;
    }
    // The surface an application's scripts see: the core types, every facade, every component.
    RegisterCoreTypes();
    engine::RegisterAllScriptFacades();
    engine::RegisterAllSceneComponentReflection();
    const Array<const TypeInfo*> types = GlobalTypeRegistry().All();
    Array<String> leftOut;
    const String source =
        foundation::script::dotnet::GenerateCSharpBindings(Span<const TypeInfo* const>{types.Data(), types.Size()}, &leftOut);
    const StringView path(reinterpret_cast<const utf8char*>(argv[1]));
    if (!WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(source.Data()), source.Size())).IsOk())
    {
        std::fprintf(stderr, "Tools.DotNetBindings: could not write %s\n", argv[1]);
        return 1;
    }
    std::printf("Tools.DotNetBindings: %zu types registered\n", static_cast<size_t>(types.Size()));
    // What C# cannot have, named so it is never missing unnoticed.
    for (const String& member : leftOut)
        std::printf("Tools.DotNetBindings: left out %.*s\n", static_cast<int>(member.Size()), reinterpret_cast<const char*>(member.Data()));
    if (argc < 4 || StringView(reinterpret_cast<const utf8char*>(argv[2])) != StringView(u8"--left-out"))
        return 0;
    const Result<Array<byte>> file = ReadFile(StringView(reinterpret_cast<const utf8char*>(argv[3])));
    if (!file.HasValue())
    {
        std::fprintf(stderr, "Tools.DotNetBindings: could not read %s\n", argv[3]);
        return 1;
    }
    // The list's lines, comments and blank lines skipped.
    const StringView text(reinterpret_cast<const utf8char*>(file.Value().Data()), file.Value().Size());
    Array<String> listed;
    for (usize start = 0; start < text.Size();)
    {
        usize end = text.Find(u8'\n', start);
        if (end == StringView::kNotFound)
            end = text.Size();
        StringView line = text.SubStr(start, end - start);
        if (!line.IsEmpty() && line[line.Size() - 1] == u8'\r') // a checkout with Windows line endings
            line = line.SubStr(0, line.Size() - 1);
        if (!line.IsEmpty() && line[0] != u8'#')
            listed.PushBack(String(line));
        start = end + 1;
    }
    const auto has = [](const Array<String>& lines, StringView line)
    {
        for (const String& l : lines)
            if (l.AsView() == line)
                return true;
        return false;
    };
    int differences = 0;
    for (const String& member : leftOut)
        if (!has(listed, member.AsView()))
        {
            std::fprintf(stderr, "Tools.DotNetBindings: left out but not listed: %.*s\n", static_cast<int>(member.Size()),
                         reinterpret_cast<const char*>(member.Data()));
            ++differences;
        }
    for (const String& line : listed)
        if (!has(leftOut, line.AsView()))
        {
            std::fprintf(stderr, "Tools.DotNetBindings: listed but C# has it now: %.*s\n", static_cast<int>(line.Size()),
                         reinterpret_cast<const char*>(line.Data()));
            ++differences;
        }
    return differences == 0 ? 0 : 1;
}
