// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The .NET host: it finds the installed runtime, starts it from the probe's runtimeconfig.json and
// makes the same round trip as the web test (a table of the host's functions to C#, a call back,
// managed work, a value back), and says why when a method is not there.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <string>

import foundation.core;
import csharp.foundation;

using namespace foundation::core;
using namespace csharp;

namespace
{
    struct EngineTable
    {
        int (*add)(int, int);
        void (*log)(const unsigned char*, int);
    };
    int Add(int a, int b) { return a + b; }
    String g_logged;
    void Log(const unsigned char* text, int length)
    {
        g_logged = String(StringView(reinterpret_cast<const utf8char*>(text), static_cast<usize>(length)));
    }
    String ProbePath(StringView file) { return PathJoin(StringView(reinterpret_cast<const utf8char*>(PROBE_DIR)), file); }
}

TEST_CASE("dotnet host: the installed runtime is found, starts, and a C# method calls back")
{
    const String root = FindDotNetRoot();
    REQUIRE_FALSE(root.IsEmpty());
    CHECK_FALSE(HostfxrPathIn(root.AsView()).IsEmpty());

    DotNetHost host;
    const Status started = host.Start(ProbePath(u8"Probe.runtimeconfig.json").AsView());
    INFO(std::string(reinterpret_cast<const char*>(host.Failure().Data()), host.Failure().Size()));
    REQUIRE(started.IsOk());
    auto* initialize = reinterpret_cast<int (*)(EngineTable*)>(
        host.EntryPoint(ProbePath(u8"Probe.dll").AsView(), u8"Probe.Entry, Probe", u8"Initialize"));
    INFO(std::string(reinterpret_cast<const char*>(host.Failure().Data()), host.Failure().Size()));
    REQUIRE(initialize != nullptr);
    EngineTable table{&Add, &Log};
    CHECK(initialize(&table) == 1042);
    CHECK(g_logged == StringView(u8"C# on CoreCLR: sum 42"));

    // A method that is not there: null, and the host says which.
    CHECK(host.EntryPoint(ProbePath(u8"Probe.dll").AsView(), u8"Probe.Entry, Probe", u8"Missing") == nullptr);
    CHECK(host.Failure().Contains(u8"Missing"));
}
