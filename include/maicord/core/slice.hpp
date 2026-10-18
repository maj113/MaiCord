#pragma once

#include "maicord/core/memory.hpp"

namespace maicord {

template <typename T> struct Slice {
    T* data;
    u32 count;

    constexpr bool empty() const noexcept {
        return count == 0;
    }

    constexpr T& operator[](u32 index) const noexcept {
        return data[index];
    }
};

template <typename T> struct ConstSlice {
    const T* data;
    u32 count;

    constexpr bool empty() const noexcept {
        return count == 0;
    }

    constexpr const T& operator[](u32 index) const noexcept {
        return data[index];
    }
};

struct Utf8View {
    const char* data;
    u32 bytes;

    constexpr bool empty() const noexcept {
        return bytes == 0;
    }
};

inline bool utf8_equal(Utf8View left, Utf8View right) noexcept {
    return left.bytes == right.bytes && bytes_equal(left.data, right.data, left.bytes);
}

inline bool utf8_starts_with(Utf8View text, Utf8View prefix) noexcept {
    return text.bytes >= prefix.bytes && bytes_equal(text.data, prefix.data, prefix.bytes);
}

template <usize Count> constexpr Utf8View utf8_literal(const char (&text)[Count]) noexcept {
    static_assert(Count > 0);
    static_assert(Count - 1 <= 0xFFFFFFFFull);
    return Utf8View{text, static_cast<u32>(Count - 1)};
}

} // namespace maicord
