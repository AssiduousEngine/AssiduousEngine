// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A development package's Engine part built as a shared library, loaded by the plugin host's
// shared-part test. Its entry is the manifest-named symbol a part exports; it counts its loads
// and unloads through a C symbol the test reads.

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;

using namespace foundation::runtime;

#if defined(_WIN32)
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
    int g_loads = 0;

    void Load(PluginContext&) { ++g_loads; }
    void Unload(PluginContext&) { --g_loads; }
}

// The stamp from the build (util_plugin_abi): this engine's, or another's for the refused copy. The
// copy built as an older engine's part has none, as a part built before stamps existed.
#if !defined(TEST_PLUGIN_PART_NO_ABI)
extern "C" PLUGIN_EXPORT const char8_t* PluginPart_shared_probe_Engine_Abi() { return PLUGIN_ABI_STAMP; }
#endif

extern "C" PLUGIN_EXPORT const PluginPart* PluginPart_shared_probe_Engine()
{
    static constexpr PluginPart part{u8"shared_probe", PluginPartKind::Engine, &Load, &Unload};
    return &part;
}

extern "C" PLUGIN_EXPORT int TestPluginPartLoads() { return g_loads; }
