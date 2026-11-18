// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

export module foundation.core:inline_array;

import :base;
import :allocator;
import :span;

export namespace foundation::core
{
    // =======================================================================
    // InlineArray - up to Capacity elements in its own storage (on the stack, for a local), each
    // constructed when it is added and destroyed with the array. For a short list made and
    // dropped often, such as a call's arguments, where a heap array would allocate and a
    // FixedArray would construct and destroy every slot.
    // =======================================================================
    template <typename T, usize Capacity>
    class InlineArray
    {
    public:
        InlineArray() noexcept = default;
        InlineArray(const InlineArray&) = delete;
        InlineArray& operator=(const InlineArray&) = delete;
        ~InlineArray() { Clear(); }

        void Add(const T& item)
        {
            DIAGNOSTIC_ASSERT(m_count < Capacity);
            Construct<T>(Items() + m_count, item);
            ++m_count;
        }
        void Add(T&& item)
        {
            DIAGNOSTIC_ASSERT(m_count < Capacity);
            Construct<T>(Items() + m_count, Move(item));
            ++m_count;
        }

        void Clear() noexcept
        {
            for (usize i = 0; i < m_count; ++i)
                Destruct(Items() + i);
            m_count = 0;
        }

        [[nodiscard]] usize Size() const noexcept { return m_count; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_count == 0; }
        [[nodiscard]] bool IsFull() const noexcept { return m_count == Capacity; }
        [[nodiscard]] static constexpr usize CapacityValue() noexcept { return Capacity; }

        [[nodiscard]] T& operator[](usize i) noexcept { return Items()[i]; }
        [[nodiscard]] const T& operator[](usize i) const noexcept { return Items()[i]; }
        [[nodiscard]] T* Data() noexcept { return Items(); }
        [[nodiscard]] const T* Data() const noexcept { return Items(); }

        [[nodiscard]] T* begin() noexcept { return Items(); }
        [[nodiscard]] const T* begin() const noexcept { return Items(); }
        [[nodiscard]] T* end() noexcept { return Items() + m_count; }
        [[nodiscard]] const T* end() const noexcept { return Items() + m_count; }

        [[nodiscard]] Span<T> AsSpan() noexcept { return Span<T>(Items(), m_count); }
        [[nodiscard]] Span<const T> View() const noexcept { return Span<const T>(Items(), m_count); }

    private:
        [[nodiscard]] T* Items() noexcept { return reinterpret_cast<T*>(m_storage); }
        [[nodiscard]] const T* Items() const noexcept { return reinterpret_cast<const T*>(m_storage); }

        alignas(T) byte m_storage[sizeof(T) * Capacity];
        usize m_count = 0;
    };
}
