// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The C++ half of the web test, built with the engine's Emscripten: it hands the C# library its
// table and checks what comes back. Exit 0 when the round trip held.
#include <cstdint>
#include <cstdio>
#include <unistd.h> // getentropy

struct EngineTable
{
    int (*add)(int, int);
    void (*log)(const unsigned char*, int);
};

extern "C" int Managed_Initialize(EngineTable* table);

// The .NET runtime asks its JavaScript host for random bytes; a native host gives them from
// Emscripten's getentropy (at most 256 bytes a call).
extern "C" int32_t SystemJS_RandomBytes(uint8_t* buffer, int32_t length)
{
    for (int32_t offset = 0; offset < length; offset += 256)
    {
        const int32_t chunk = length - offset < 256 ? length - offset : 256;
        if (getentropy(buffer + offset, static_cast<size_t>(chunk)) != 0)
            return -1;
    }
    return 0;
}

static int Add(int a, int b) { return a + b; }
static void Log(const unsigned char* text, int length) { std::printf("%.*s\n", length, reinterpret_cast<const char*>(text)); }

int main()
{
    EngineTable table{&Add, &Log};
    const int result = Managed_Initialize(&table);
    std::printf("host: C# returned %d (expected 1042)\n", result);
    return result == 1042 ? 0 : 1;
}
