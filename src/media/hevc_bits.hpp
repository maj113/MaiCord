#pragma once
#include "maicord/media/hevc.hpp"

namespace maicord {
struct HevcBits {
    ConstSlice<u8> source{};
    u32 cursor{}, position{};
    u8 current{}, remaining{}, zeros{};
    HevcError error{};
    __declspec(noinline) u8 byte() noexcept {
        if (error != HevcError::none)
            return 0;
        if (cursor >= source.count) {
            error = HevcError::truncated;
            return 0;
        }
        u8 value = source[cursor++];
        if (zeros >= 2 && value == 3) {
            if (cursor >= source.count || source[cursor] > 3) {
                error = HevcError::malformed;
                return 0;
            }
            value = source[cursor++];
            zeros = 0;
        } else if (zeros >= 2 && value < 3) {
            error = HevcError::malformed;
            return 0;
        }
        zeros = value == 0 ? static_cast<u8>(zeros + 1) : 0;
        return value;
    }
    __declspec(noinline) u32 read(u32 count) noexcept {
        if (count > 32) {
            error = HevcError::invalid_input;
            return 0;
        }
        u32 result{};
        while (count && error == HevcError::none) {
            if (!remaining) {
                current = byte();
                remaining = 8;
                if (error != HevcError::none) {
                    --remaining;
                    ++position;
                    result <<= 1;
                    break;
                }
            }
            const u32 take = count < remaining ? count : remaining;
            remaining = static_cast<u8>(remaining - take);
            result = (result << take) | ((current >> remaining) & ((1u << take) - 1u));
            position += take;
            count -= take;
        }
        return result;
    }
    bool bit() noexcept {
        return read(1) != 0;
    }
    __declspec(noinline) u32 ue(u32 maximum = 0x7fffffffu) noexcept {
        u32 zeros_count{};
        while (error == HevcError::none && !bit())
            if (++zeros_count > 31) {
                error = HevcError::malformed;
                return 0;
            }
        const u64 value = ((u64{1} << zeros_count) - 1) + read(zeros_count);
        if (value > maximum) {
            error = HevcError::malformed;
            return 0;
        }
        return static_cast<u32>(value);
    }
    __declspec(noinline) i32 se(i32 minimum, i32 maximum) noexcept {
        const u32 code = ue();
        const i64 value = (code & 1u) ? static_cast<i64>((u64{code} + 1) / 2) : -static_cast<i64>(code / 2);
        if (value < minimum || value > maximum) {
            error = HevcError::malformed;
            return 0;
        }
        return static_cast<i32>(value);
    }
    void trailing() noexcept {
        if (!bit())
            error = HevcError::malformed;
        while (remaining && error == HevcError::none)
            if (bit())
                error = HevcError::malformed;
        if (cursor != source.count && error == HevcError::none)
            error = HevcError::malformed;
    }
};
inline HevcBits hevc_payload(ConstSlice<u8> nal, u8 type) noexcept {
    HevcBits b{};
    if (!nal.data || nal.count < 3)
        b.error = HevcError::invalid_input;
    else if ((nal[0] & 0x80u) || ((nal[0] >> 1) & 63u) != type || !(nal[1] & 7u))
        b.error = HevcError::malformed;
    else if ((nal[0] & 1u) || (nal[1] & 0xf8u))
        b.error = HevcError::unsupported;
    else
        b.source = {nal.data + 2, nal.count - 2};
    return b;
}
inline u32 hevc_ceil_log2(u32 value) noexcept {
    u32 bits{};
    for (u32 v = value > 0 ? value - 1 : 0; v; v >>= 1)
        ++bits;
    return bits;
}
HevcError hevc_parse_reference_set(HevcBits&, const HevcReferenceSet*, u32 index, u32 count,
                                   HevcReferenceSet*, u8* reference_delta_count = nullptr) noexcept;
} // namespace maicord
