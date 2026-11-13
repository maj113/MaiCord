#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/media/audio/opus_decoder.hpp"

namespace maicord {
// Decodes an opus_demo bitstream (per packet: big-endian length, big-endian
// encoder final range, payload) the way upstream's run_vectors.sh drives
// `opus_demo -d`: an empty packet is lost and concealed with the previous
// duration, the last loss before a packet carrying LBRR is recovered with FEC,
// and every packet not adjacent to a loss must leave the decoder's final range
// equal to the encoder's. PCM is appended to `pcm` and hashed (FNV-1a).
struct OpusVectorDecode {
    u32 packets, lost, recovered, range_errors, decode_errors; // recovered: losses decoded from FEC
    usize samples; // per channel
    u64 digest;
    bool overflow;
};
inline constexpr u32 kOpusVectorMaximumPacket = 1500;

inline u32 opus_vector_big_endian(const u8* p) noexcept {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
}
// `frame` holds kOpusDecoderMaximumFrames * channels samples; `pcm` may be null to only hash.
inline OpusVectorDecode opus_vector_decode(OpusDecoder* decoder, u32 channels, const u8* bitstream, usize bytes, i16* frame,
                                           i16* pcm, usize capacity) noexcept {
    OpusVectorDecode result{};
    u64 hash = 0xcbf29ce484222325ull;
    u32 lost_count = 0, last_duration = 0;
    bool lost_prev = true;
    const u8 *cursor = bitstream, *end = bitstream + bytes;
    const Slice<i16> out{frame, kOpusDecoderMaximumFrames * channels};
    while (end - cursor >= 8) {
        const u32 length = opus_vector_big_endian(cursor), encoder_range = opus_vector_big_endian(cursor + 4);
        cursor += 8;
        if (length > kOpusVectorMaximumPacket || static_cast<usize>(end - cursor) < length) {
            ++result.decode_errors;
            break;
        }
        const ConstSlice<u8> packet{cursor, length};
        cursor += length;
        ++result.packets;
        const bool lost = length == 0;
        const u32 runs = lost ? 0 : 1 + lost_count;
        if (lost) ++lost_count, ++result.lost;
        for (u32 run = 0; run < runs; ++run) {
            u32 samples;
            if (run + 1 == lost_count && opus_packet_has_fec(packet)) {
                ++result.recovered;
                samples = opus_decode_lost(decoder, packet, last_duration, out);
            } else if (run < lost_count)
                samples = opus_decode_lost(decoder, {}, last_duration, out);
            else
                samples = opus_decode(decoder, packet, out);
            if (!samples) {
                ++result.decode_errors;
                continue;
            }
            last_duration = samples;
            const usize count = static_cast<usize>(samples) * channels;
            const auto* sample_bytes = reinterpret_cast<const u8*>(frame);
            for (usize i = 0; i < count * 2; ++i) hash = (hash ^ sample_bytes[i]) * 0x100000001b3ull;
            if (pcm) {
                if (result.samples * channels + count > capacity) {
                    result.overflow = true;
                    pcm = nullptr;
                } else {
                    copy_bytes(pcm + result.samples * channels, frame, static_cast<u32>(count * 2));
                }
            }
            result.samples += samples;
        }
        if (encoder_range && !lost && !lost_prev && opus_decoder_final_range(decoder) != encoder_range) ++result.range_errors;
        lost_prev = lost;
        if (!lost) lost_count = 0;
    }
    if (cursor != end) ++result.decode_errors;
    result.digest = hash;
    return result;
}
} // namespace maicord
