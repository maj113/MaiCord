#pragma once

// The expectation sink and byte helpers the parts of the core test executable (tests/support/test_main_shared.hpp,
// which includes this header) and the Linux transport executable share. The sink is defined by test_main.cpp, or by
// the entry of tests/protocol/http/linux_main.cpp.

#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

// Records a failed expectation under name.
void expect(bool condition, const char* name) noexcept;

template <usize Count> constexpr ConstSlice<u8> byte_literal(const char (&text)[Count]) noexcept {
    static_assert(Count > 0);
    return ConstSlice<u8>{reinterpret_cast<const u8*>(text), static_cast<u32>(Count - 1)};
}

// Sentinel bytes proving an output buffer was left untouched on failure.
inline void guard_fill(u8* data, usize bytes, u8 value = 0xA5) noexcept {
    for (usize index = 0; index < bytes; ++index)
        data[index] = value;
}

inline bool guard_intact(const u8* data, usize bytes, u8 value = 0xA5) noexcept {
    for (usize index = 0; index < bytes; ++index)
        if (data[index] != value)
            return false;
    return true;
}

inline bool bytes_equal(ConstSlice<u8> left, ConstSlice<u8> right) noexcept {
    return left.count == right.count && maicord::bytes_equal(left.data, right.data, left.count);
}

} // namespace maicord
