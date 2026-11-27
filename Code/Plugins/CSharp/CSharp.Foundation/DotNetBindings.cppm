// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CSharp.Foundation - the :bindings partition.
//
// The C# source of the engine's bound types, generated from reflection, so the C# side always
// matches the engine it is built with. A value type (a trivially copied struct of fields: Float3,
// Quaternion, Color) becomes a C# struct with each field at the offset measured from the C++
// reflection, its size stated, and a check C# runs at start; its reflected methods become C#
// methods that pack their arguments as DotNetValues and call the bridge with the method found once
// by index.

module;
#include "Core/Prelude.h"

export module csharp.foundation:bindings;

import foundation.core;

using namespace foundation::core;

export namespace csharp
{
    /// The registered value types the generator can bind: trivially copied, no identity, no
    /// reference or container, every property a field whose type is a scalar or another such type.
    [[nodiscard]] Array<const TypeInfo*> BindableValueTypes();

    /// `types` and the types their members reach that are not among them: the enums, the values of
    /// their own bytes and the reflected records their properties, parameters, results and list
    /// elements are (a component's enum, an EntityRef, a sequence's cast entry), which C# binds beside
    /// them although the engine never registers them.
    [[nodiscard]] Array<const TypeInfo*> ReachableTypes(Span<const TypeInfo* const> types);

    /// The C# source binding `types` (a namespace `Engine`), with a `Layouts.FirstMismatch()` C#
    /// calls at start. A member C# cannot have (a type it has no form for, a name it cannot take, an
    /// overload it cannot tell apart) is left out, and named in `leftOut` with why: "Type.member: why".
    [[nodiscard]] String GenerateCSharpBindings(Span<const TypeInfo* const> types, Array<String>* leftOut = nullptr);
}
