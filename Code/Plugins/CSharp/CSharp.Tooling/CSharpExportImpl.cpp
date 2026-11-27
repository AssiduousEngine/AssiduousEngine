// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

module;
#include "Core/Prelude.h"

module csharp.tooling;

import foundation.core;
import foundation.vfs;
import script.pipeline; // ListSourcesUnder
import editor.project;

using namespace foundation::core;

namespace csharp
{
    bool FindProjectFolder(const editor::EditorProject& project, String& folder, String& refusal)
    {
        folder.Clear();
        refusal.Clear();
        foundation::vfs::NativeFileSystem sourceFs(project.SourcesRoot().AsView(), project.Allocator());
        project.AddCodeFolder(sourceFs); // where a project's C# lives
        Array<String> found = pipeline::ListSourcesUnder(sourceFs, StringView{}, u8"csproj");
        if (found.IsEmpty())
        {
            return false;
        }
        if (found.Size() > 1)
        {
            refusal = Format(u8"the project has {} C# projects ({} and {}): a player compiles one in", found.Size(), found[0],
                             found[1]);
            return true;
        }
        folder = project.SourceFile(PathParent(found[0].AsView()));
        return true;
    }

    WebBuild FindWebBuild()
    {
        WebBuild web;
        web.bindings = PathJoin(PathParent(ExecutablePath().AsView()), u8"DotNet/Engine.g.cs");
        const Optional<String> fromEnvironment = GetEnvironmentVariable(u8"DOTNET_EMSDK");
        const Optional<String> home = GetEnvironmentVariable(u8"HOME");
        web.emsdk = fromEnvironment.HasValue() ? fromEnvironment.Value()
                    : home.HasValue()          ? PathJoin(home.Value().AsView(), u8"emsdk-3.1.56")
                                               : String{};
        return web;
    }

    editor::PlayerNeed PlayerNeedFor(const editor::PlayerHookContext& context, const WebBuild& web)
    {
        editor::PlayerNeed need;
        bool enabled = false;
        for (const String& id : context.enabled)
        {
            enabled = enabled || id == StringView(u8"csharp");
        }
        String folder;
        if (!enabled || context.project == nullptr || !FindProjectFolder(*context.project, folder, need.refusal) ||
            !need.refusal.IsEmpty())
        {
            return need;
        }
        need.build = true;
        need.reason = String(u8"the project's C# is compiled into its player");
        need.definitions.PushBack(Format(u8"-DENGINE_PROJECT_CSHARP={}", folder));
        if (context.platform.StartsWith(u8"Web") || context.platform.StartsWith(u8"Wasm"))
        {
            // The web compiles the C# with NativeAOT-LLVM: under the emsdk its bitcode is built with,
            // against the bindings this editor's build generated.
            if (!FileExists(web.bindings.AsView()))
            {
                need.refusal = Format(u8"C# for the web needs the C# bindings this editor's build generates, at '{}' - not found",
                                      web.bindings);
                return need;
            }
            if (web.emsdk.IsEmpty() || !FileExists(PathJoin(web.emsdk.AsView(), u8"emsdk_env.sh").AsView()))
            {
                need.refusal = Format(u8"C# for the web needs emsdk 3.1.56 (the one NativeAOT-LLVM builds with), at '{}' or "
                                      u8"where DOTNET_EMSDK says - not found",
                                      web.emsdk);
                return need;
            }
            need.definitions.PushBack(Format(u8"-DDOTNET_BINDINGS={}", web.bindings));
            need.definitions.PushBack(Format(u8"-DDOTNET_EMSDK={}", web.emsdk));
        }
        return need;
    }
}
