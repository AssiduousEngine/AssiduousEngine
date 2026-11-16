// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the :bridge partition.
//
// What C# calls the engine through: one table of C functions, handed to C# at initialisation, the
// same under CoreCLR and NativeAOT (no symbols exported, no DllImport to resolve). It is generic:
// written once over the engine's reflection, it serves every bound type. A value crosses as a
// DotNetValue: a type and the bytes of a trivially copied value, UTF-8 text, or an object. The C#
// side (generated per type) packs its arguments into DotNetValues and calls Invoke with the
// method it looked up once by index. Anything that is not a plain value (an object, a component, a
// member of either) crosses as a handle: a copy of the engine's Variant, which C# owns and releases,
// so the engine's own lifetime rules (a component handle found again on every access, a borrow that
// pins its owner) hold for C# as for the other languages.

module;
#include "Core/Prelude.h"

export module foundation.script.dotnet:bridge;

import foundation.core;

using namespace foundation::core;

export namespace foundation::script::dotnet
{
    enum class DotNetValueKind : u32
    {
        Empty = 0,  ///< nothing (a void return)
        Bytes = 1,  ///< `size` bytes at `data`, a trivially copied value of `type`
        Text = 2,   ///< `size` bytes of UTF-8 at `data`
        Handle = 3, ///< a Variant* at `data`: the receiver of a result owns it (releaseHandle)
    };

    /// A value crossing between C# and the engine. The same field order in C# (pointers as nint),
    /// so the layout follows the platform's pointer size on both sides.
    struct DotNetValue
    {
        const TypeInfo* type = nullptr;
        void* data = nullptr;
        u64 size = 0;
        DotNetValueKind kind = DotNetValueKind::Empty;
        u32 reserved = 0;
    };

    /// The engine's functions as C# sees them. Every one returns 0 or a negative ErrorCode, and
    /// a failure leaves its reason in LastError.
    struct DotNetEngineFunctions
    {
        u32 size = sizeof(DotNetEngineFunctions); ///< the table's size: a C# side built for another refuses
        const TypeInfo* (*typeById)(u64 id) = nullptr;
        const MethodInfo* (*methodAt)(const TypeInfo* type, u32 index) = nullptr;
        const PropertyInfo* (*propertyAt)(const TypeInfo* type, u32 index) = nullptr;
        /// Calls `method` on `self` (null or Empty for a static), `args` in order; the result goes to
        /// `result` (its bytes into result->data, which must hold result->size; text into a buffer
        /// the bridge keeps until the thread's next call).
        i32 (*invoke)(const MethodInfo* method, const DotNetValue* self, const DotNetValue* args, u32 count,
                      DotNetValue* result) = nullptr;
        i32 (*getProperty)(const PropertyInfo* property, const DotNetValue* self, DotNetValue* result) = nullptr;
        i32 (*setProperty)(const PropertyInfo* property, const DotNetValue* self, const DotNetValue* value) = nullptr;
        /// Constructs `type` with `args` (its reflected constructor of that many arguments).
        i32 (*construct)(const TypeInfo* type, const DotNetValue* args, u32 count, DotNetValue* result) = nullptr;
        /// The engine's size of `type` (a value type whose layout C# holds opaquely checks it).
        u32 (*typeSize)(const TypeInfo* type) = nullptr;
        /// Gives back a handle a result handed over.
        void (*releaseHandle)(void* handle) = nullptr;
        /// The reason the calling thread's last failed call failed, UTF-8, valid until its next call.
        const utf8char* (*lastError)(u64* length) = nullptr;
    };

    /// The table, filled; handles are made from `allocator` (the host's), the first call's.
    [[nodiscard]] const DotNetEngineFunctions& EngineFunctions(IAllocator& allocator) noexcept;

    /// A DotNetValue's value as a Variant (empty when it cannot be one: a type with no recorded
    /// value operations).
    [[nodiscard]] Variant ToVariant(const DotNetValue& value);
    /// `variant` into `out`: a plain value's bytes into out.data when they fit out.size (else
    /// OutOfRange), text into the thread's buffer, anything else as a handle the caller owns.
    [[nodiscard]] Status FromVariant(const Variant& variant, DotNetValue& out);
}
