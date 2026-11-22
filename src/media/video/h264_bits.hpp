#pragma once

#include "maicord/media/video/h264_parameters.hpp"

namespace maicord {

// A bounded EBSP reader. Emulation-prevention bytes are consumed lazily, with
// the byte that holds the next bit; neither a padded input nor a temporary
// de-escaped allocation is required.
struct H264Bits {
    ConstSlice<u8> source;
    u32 offset;
    u8 byte, remaining, zeroes;
    H264Error error;

    // The next RBSP byte into byte, 8 bits remaining; false with error set.
    __declspec(noinline) bool load() noexcept {
        if (offset == source.count) { error = H264Error::truncated; return false; }
        u8 next = source.data[offset++];
        if (zeroes == 2) {
            if (next == 3) {
                if (offset == source.count) { error = H264Error::truncated; return false; }
                next = source.data[offset++];
                if (next > 3) { error = H264Error::malformed; return false; }
                zeroes = 0;
            } else if (next < 3) { error = H264Error::malformed; return false; }
        }
        zeroes = next ? 0 : static_cast<u8>(zeroes + 1u);
        byte = next;
        remaining = 8;
        return true;
    }
    __declspec(noinline) u32 read(u32 count) noexcept {
        if (count > 32) { error = H264Error::invalid_input; return 0; }
        u32 value = 0;
        while (count && error == H264Error::none && (remaining || load())) {
            const u32 take = count < remaining ? count : remaining;
            remaining = static_cast<u8>(remaining - take);
            value = (value << take) | ((byte >> remaining) & ((1u << take) - 1u));
            count -= take;
        }
        return value;
    }
    bool bit() noexcept {
        if (!remaining || error != H264Error::none) return read(1) != 0;
        --remaining;
        return (byte >> remaining) & 1u;
    }
    // Exp-Golomb. The prefix zeros are counted a byte at a time, the unread bits of byte at the top of a word (more
    // than 32 are malformed); a suffix inside the same byte is taken at once.
    __declspec(noinline) u64 code() noexcept {
        u32 zero_count = 0;
        for (;;) {
            if (error != H264Error::none || (!remaining && !load())) return 0;
            const u32 unread = u32{byte} << (32u - remaining);
            const u32 zeros = unread ? static_cast<u32>(__builtin_clz(unread)) : remaining;
            zero_count += zeros;
            if (zero_count > 32) { error = H264Error::malformed; return 0; }
            if (unread) {
                remaining = static_cast<u8>(remaining - zeros - 1u);
                break;
            }
            remaining = 0;
        }
        if (zero_count > remaining) return ((1ull << zero_count) - 1u) + read(zero_count);
        remaining = static_cast<u8>(remaining - zero_count);
        return ((1ull << zero_count) - 1u) + ((byte >> remaining) & ((1u << zero_count) - 1u));
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
