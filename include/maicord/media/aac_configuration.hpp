#pragma once
#include "maicord/core/byte_range.hpp"

namespace maicord {
struct AacConfiguration {
    u32 core_sample_rate{}, sample_rate{};
    u16 frames_per_packet{};
    u8 core_channels{}, channels{};
    bool sbr{}, ps{};
};
static_assert(sizeof(AacConfiguration) == 16);

// Bounded explicit AAC-LC/SBR/PS metadata. No inference from compressed audio,
// no 960-frame mode, PCE, core dependency or unknown nonzero extension bits.
// Output uses the post-SBR/PS PCM domain. Failure leaves output unchanged.
__declspec(noinline) inline bool aac_configuration_decode(ConstSlice<u8> source,
                                                          AacConfiguration* output) noexcept {
    if (!output || source.count < 2 || source.count > 16 || !byte_range_valid(source) ||
        !byte_range_valid({reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        byte_ranges_overlap(source, {reinterpret_cast<const u8*>(output), sizeof(*output)}))
        return false;
    struct Bits {
        ConstSlice<u8> bytes;
        u32 at{};
        bool take(u32 n, u32* out) noexcept {
            if (n > 24 || n > bytes.count * 8 - at)
                return false;
            u32 value = 0;
            for (u32 i = 0; i < n; ++i, ++at)
                value = (value << 1) | ((bytes[at / 8] >> (7 - at % 8)) & 1u);
            *out = value;
            return true;
        }
        bool frequency(u32* out) noexcept {
            static constexpr u32 rates[]{96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                  22050, 16000, 12000, 11025, 8000,  7350};
            u32 index = 0;
            if (!take(4, &index))
                return false;
            if (index == 15)
                return take(24, out);
            if (index >= 13)
                return false;
            *out = rates[index];
            return true;
        }
    } bits{source};
    AacConfiguration next{};
    u32 object = 0, channel = 0, flags = 0;
    if (!bits.take(5, &object) || (object != 2 && object != 5 && object != 29) ||
        !bits.frequency(&next.core_sample_rate) || !bits.take(4, &channel) ||
        (channel != 1 && channel != 2))
        return false;
    next.core_channels = static_cast<u8>(channel);
    next.channels = next.core_channels;
    next.sample_rate = next.core_sample_rate;
    const bool explicit_extension = object == 5 || object == 29;
    if (explicit_extension) {
        next.sbr = true;
        next.ps = object == 29;
        if (!bits.frequency(&next.sample_rate) || !bits.take(5, &object) || object != 2)
            return false;
    }
    if (!bits.take(3, &flags) || flags)
        return false;
    if (!explicit_extension && source.count * 8 - bits.at >= 16) {
        const u32 saved = bits.at;
        u32 sync = 0;
        if (!bits.take(11, &sync))
            return false;
        if (sync == 0x2b7) {
            u32 extension = 0, present = 0;
            if (!bits.take(5, &extension) || extension != 5 || !bits.take(1, &present))
                return false;
            next.sbr = present != 0;
            if (next.sbr && !bits.frequency(&next.sample_rate))
                return false;
        } else
            bits.at = saved;
    }
    if (next.sbr && source.count * 8 - bits.at >= 12) {
        const u32 saved = bits.at;
        u32 sync = 0, present = 0;
        if (!bits.take(11, &sync))
            return false;
        if (sync == 0x548) {
            if (!bits.take(1, &present))
                return false;
            next.ps = next.ps || present != 0;
        } else
            bits.at = saved;
    }
    while (bits.at < source.count * 8) {
        u32 trailing = 0;
        if (!bits.take(1, &trailing) || trailing)
            return false;
    }
    static constexpr u32 rates[]{48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    bool supported = false;
    for (const auto rate : rates)
        supported = supported || next.sample_rate == rate;
    if (!supported || (next.sbr && next.sample_rate != next.core_sample_rate * 2) ||
        (next.ps && (!next.sbr || next.core_channels != 1)))
        return false;
    next.frames_per_packet = next.sbr ? 2048 : 1024;
    if (next.ps)
        next.channels = 2;
    *output = next;
    return true;
}
} // namespace maicord
