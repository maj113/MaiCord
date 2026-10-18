#pragma once
#include "maicord/protocol/mls_objects.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
// Internal composition cursor: borrowed immutable input, sticky first error.
// The input must not alias this cursor. Structural decoders own local cursors
// and publish their candidate views only after the complete object succeeds.
struct MlsReader {
    ConstSlice<u8> input;
    u32 used{}, error_offset{};
    MlsWireError error{};
    explicit MlsReader(ConstSlice<u8> data) noexcept : input(data) {
        if (!byte_range_valid(data))
            fail(MlsWireError::invalid_input, 0);
    }
    void fail(MlsWireError value, u32 offset) noexcept {
        if (error == MlsWireError::none) {
            error = value;
            error_offset = offset;
        }
    }
    bool need(u32 count) noexcept {
        if (error != MlsWireError::none)
            return false;
        if (used > input.count || count > input.count - used) {
            fail(MlsWireError::truncated, used);
            return false;
        }
        return true;
    }
    ConstSlice<u8> take(u32 count) noexcept {
        if (!need(count))
            return {};
        // Do not perform arithmetic on a null pointer for a valid empty input.
        ConstSlice<u8> result{input.data ? input.data + used : nullptr, count};
        used += count;
        return result;
    }
    u64 integer(u32 count) noexcept {
        const auto span = take(count);
        u64 value = 0;
        for (u32 i = 0; i != span.count; ++i)
            value = (value << 8) | span.data[i];
        return value;
    }
    u32 length() noexcept {
        if (!need(1))
            return 0;
        const u32 start = used, prefix = input.data[used] >> 6;
        if (prefix == 3) {
            fail(MlsWireError::invalid_length_prefix, start);
            return 0;
        }
        const u32 count = 1u << prefix;
        if (!need(count))
            return 0;
        u32 value = input.data[used] & 0x3f;
        for (u32 i = 1; i != count; ++i)
            value = (value << 8) | input.data[used + i];
        if ((count == 2 && value < 64) || (count == 4 && value < 16384)) {
            fail(MlsWireError::noncanonical_length, start);
            return 0;
        }
        used += count;
        return value;
    }
    ConstSlice<u8> vector() noexcept {
        const u32 count = length();
        return take(count);
    }
    ConstSlice<u8> integers16() noexcept {
        const auto value = vector();
        if (value.count & 1)
            fail(MlsWireError::malformed, used - value.count);
        return value;
    }
    MlsDecodeResult result() const noexcept {
        return {error == MlsWireError::none ? used : 0, error_offset, error};
    }
};
} // namespace maicord
