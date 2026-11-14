// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script - :script_numbers partition. How a number crosses between a script language
// (which has one number type, a double) and the reflected types a native callee takes: every
// backend reads and writes them the same way, so no backend forgets a width.
module;
#include "Core/Prelude.h"

export module foundation.script:script_numbers;

import foundation.core;

export namespace foundation::script
{
    // Static members rather than free functions: gcc 15 cannot read back a module partition whose
    // namespace-scope inline function instantiates a template.
    struct ScriptNumbers
    {
        /// The number `value` holds, as a double (a bool as 0 or 1); `ok` false when it holds none.
        [[nodiscard]] static double Of(const core::Variant& value, bool& ok) noexcept
        {
            using namespace core;
            ok = true;
            if (const f64* d = value.TryGet<f64>())
                return *d;
            if (const f32* f = value.TryGet<f32>())
                return static_cast<double>(*f);
            if (const i32* i = value.TryGet<i32>())
                return static_cast<double>(*i);
            if (const i64* i = value.TryGet<i64>())
                return static_cast<double>(*i);
            if (const u32* u = value.TryGet<u32>())
                return static_cast<double>(*u);
            if (const u64* u = value.TryGet<u64>())
                return static_cast<double>(*u);
            if (const i16* i = value.TryGet<i16>())
                return static_cast<double>(*i);
            if (const u16* u = value.TryGet<u16>())
                return static_cast<double>(*u);
            if (const i8* i = value.TryGet<i8>())
                return static_cast<double>(*i);
            if (const u8* u = value.TryGet<u8>())
                return static_cast<double>(*u);
            if (const bool* b = value.TryGet<bool>())
                return *b ? 1.0 : 0.0;
            ok = false;
            return 0.0;
        }

        /// A script's number as the reflected type a callee takes (`expected`); a double when it
        /// names no numeric type.
        [[nodiscard]] static core::Variant As(double d, const core::TypeInfo* expected)
        {
            using namespace core;
            if (expected == &TypeOf<f32>())
                return Variant::From<f32>(static_cast<f32>(d));
            if (expected == &TypeOf<i32>())
                return Variant::From<i32>(static_cast<i32>(d));
            if (expected == &TypeOf<i64>())
                return Variant::From<i64>(static_cast<i64>(d));
            if (expected == &TypeOf<u32>())
                return Variant::From<u32>(static_cast<u32>(d));
            if (expected == &TypeOf<u64>())
                return Variant::From<u64>(static_cast<u64>(d));
            if (expected == &TypeOf<i16>())
                return Variant::From<i16>(static_cast<i16>(d));
            if (expected == &TypeOf<u16>())
                return Variant::From<u16>(static_cast<u16>(d));
            if (expected == &TypeOf<i8>())
                return Variant::From<i8>(static_cast<i8>(d));
            if (expected == &TypeOf<u8>())
                return Variant::From<u8>(static_cast<u8>(d));
            if (expected == &TypeOf<bool>())
                return Variant::From<bool>(d != 0.0);
            return Variant::From<f64>(d);
        }
    };
}
