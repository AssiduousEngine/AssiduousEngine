// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the :bindings partition.
//
// The C# source of the engine's bound types, generated from reflection, so the C# side always
// matches the engine it is built with. A value type (a trivially copied struct of fields: Float3,
// Quaternion, Color) becomes a C# struct with each field at the offset measured from the C++
// reflection, its size stated, and a check C# runs at start; its reflected methods become C#
// methods that pack their arguments as DotNetValues and call the bridge with the method found once
// by index.

module;
#include "Core/Prelude.h"

export module foundation.script.dotnet:bindings;

import foundation.core;

using namespace foundation::core;

export namespace foundation::script::dotnet
{
    /// The registered value types the generator can bind: trivially copied, no identity, no
    /// reference or container, every property a field whose type is a scalar or another such type.
    [[nodiscard]] Array<const TypeInfo*> BindableValueTypes();

    /// The C# source binding `types` (a namespace `Engine`), with a `Layouts.FirstMismatch()` C#
    /// calls at start. Methods whose signature holds a type C# has no form for are left out.
    [[nodiscard]] String GenerateCSharpBindings(Span<const TypeInfo* const> types);
}
