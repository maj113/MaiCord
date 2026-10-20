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

    __declspec(noinline) Utf8View add(Utf8View text) noexcept {
        const u32 take = minimum(text.bytes, capacity - used);
        copy_bytes(data + used, text.data, take);
        used += take;
        return {data + used - take, take};
    }
    // Decimal digits; the most significant ones that fit when out of room.
    Utf8View number(u64 value) noexcept {
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
