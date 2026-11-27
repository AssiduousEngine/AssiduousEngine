// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CSharp.Tooling - the `csharp.tooling` module: what the export asks the csharp plugin. A project's
// C# is compiled into its own player (NativeAOT on the desktop, NativeAOT-LLVM on the web), so the
// export builds one, configured with the folder of the project's C# project.

module;
#include "Core/Prelude.h"

export module csharp.tooling;

import foundation.core;
import editor.project;

using namespace foundation::core;

export namespace csharp
{
    /// The folder of the project's C# project (its .csproj, the one a player compiles in), absolute,
    /// into `folder`: true when the project has one. More than one sets `refusal`. An IDE's bin and
    /// obj folders are passed over.
    [[nodiscard]] bool FindProjectFolder(const editor::EditorProject& project, String& folder, String& refusal);

    /// What a web player's C# is built with: the bindings this editor's build generated (a web build
    /// cannot run the generator) and the emsdk NativeAOT-LLVM's bitcode is built with.
    struct WebBuild
    {
        String bindings;
        String emsdk;
    };

    /// Where this editor finds them: the bindings beside it (DotNet/Engine.g.cs), the emsdk where
    /// DOTNET_EMSDK says, else ~/emsdk-3.1.56.
    [[nodiscard]] WebBuild FindWebBuild();

    /// The export's player hook: with csharp enabled on the platform and a C# project, a player of the
    /// project's own with its C# compiled in. On the web, `web` must name what that build needs.
    [[nodiscard]] editor::PlayerNeed PlayerNeedFor(const editor::PlayerHookContext& context, const WebBuild& web);
}
