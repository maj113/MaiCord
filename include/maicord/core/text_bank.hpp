#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

// Accumulates short UI strings in a caller-owned buffer. Appends truncate at
// capacity (cutting bytes, not characters) and return views of what they
// stored, which stay valid until the bank is reset.
struct TextBank {
    char* data;
    u32 capacity, used;

    __declspec(noinline) Utf8View add(const Utf8View& text) noexcept {
        const u32 take = minimum(text.bytes, capacity - used);
        copy_bytes(data + used, text.data, take);
        used += take;
        return {data + used - take, take};
    }
    __declspec(noinline) void append(const char* source, u32 bytes) noexcept {
        const u32 take = minimum(bytes, capacity - used);
        copy_bytes(data + used, source, take);
        used += take;
    }
    // A literal passes its address and length in registers; an unused result costs nothing.
    template <usize Count> __forceinline Utf8View add(const char (&text)[Count]) noexcept {
        const u32 start = used;
        append(text, Count - 1);
        return {data + start, used - start};
    }
    // Decimal digits; the most significant ones that fit when out of room.
    __declspec(noinline) Utf8View number(u64 value) noexcept {
        char digits[20];
        return add({digits, decimal_write(digits, value)});
    }
    // Everything appended since `start` (an earlier `used`).
    Utf8View since(u32 start) const noexcept {
        return {data + start, used - start};
    }
    Utf8View view() const noexcept {
        return {data, used};
    }
};

} // namespace maicord
