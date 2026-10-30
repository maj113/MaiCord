#pragma once

#include "maicord/media/h264_parameters.hpp"

namespace maicord {

// A bounded EBSP reader. Emulation-prevention bytes are consumed lazily;
// neither a padded input nor a temporary de-escaped allocation is required.
struct H264Bits {
    ConstSlice<u8> source;
    u32 offset{};
    u8 byte{}, remaining{}, zeroes{};
    H264Error error{};

    __declspec(noinline) u32 read(u32 count) noexcept {
        if (count > 32) { error = H264Error::invalid_input; return 0; }
        u32 value = 0;
        while (count && error == H264Error::none) {
            if (!remaining) {
                if (offset == source.count) { error = H264Error::truncated; break; }
                u8 next = source.data[offset++];
                if (zeroes == 2) {
                    if (next == 3) {
                        if (offset == source.count) { error = H264Error::truncated; break; }
                        next = source.data[offset++];
                        if (next > 3) { error = H264Error::malformed; break; }
                        zeroes = 0;
                    } else if (next < 3) { error = H264Error::malformed; break; }
                }
                zeroes = next ? 0 : static_cast<u8>(zeroes + 1u);
                byte = next;
                remaining = 8;
            }
            const u32 take = count < remaining ? count : remaining;
            remaining = static_cast<u8>(remaining - take);
            value = (value << take) | ((byte >> remaining) & ((1u << take) - 1u));
            count -= take;
        }
        return value;
    }
    bool bit() noexcept { return read(1) != 0; }
    __declspec(noinline) u64 code() noexcept {
        u32 zero_count = 0;
#pragma clang loop unroll(disable)
        while (!bit() && error == H264Error::none) {
            if (++zero_count > 32) { error = H264Error::malformed; return 0; }
        }
        if (error != H264Error::none) return 0;
        return ((1ull << zero_count) - 1u) + read(zero_count);
    }
    u32 ue(u32 maximum = ~0u) noexcept {
        const u64 value = code();
        if (value > maximum) error = H264Error::malformed;
        return error == H264Error::none ? static_cast<u32>(value) : 0;
    }
    i32 se(i32 minimum = (-2'147'483'647 - 1), i32 maximum = 2'147'483'647) noexcept {
        const u64 value = code();
        const i64 signed_value = (value & 1u) ? static_cast<i64>((value + 1u) / 2u)
                                             : -static_cast<i64>(value / 2u);
        if (signed_value < minimum || signed_value > maximum) error = H264Error::malformed;
        return error == H264Error::none ? static_cast<i32>(signed_value) : 0;
    }
    bool finish() noexcept {
        if (error != H264Error::none) return false;
        if (!bit()) { if (error == H264Error::none) error = H264Error::malformed; return false; }
        if ((remaining && read(remaining)) || offset != source.count) error = H264Error::malformed;
        return error == H264Error::none;
    }
};

} // namespace maicord
