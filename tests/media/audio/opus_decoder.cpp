#include "maicord/media/audio/opus_decoder.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_opus_decoder(void (*check)(bool, const char*) noexcept) noexcept {
    const u8 silence[]{0xf8, 0xff, 0xfe}; // One 20 ms CELT silence frame.
    const u8 longest[]{0xfb, 6, 0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe,
                      0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe}; // Six equal 20 ms frames.
    const u8 missing_count[]{0xff}, over_duration[]{0xfb, 7}, bad_lengths[]{0xfa, 0xff};
    static u8 oversized[1277]; // One frame of 1276 bytes: a byte more than the format allows.
    oversized[0] = 0xf8;
    static OpusDecoder decoder, saved;
    static i16 output[kOpusDecoderMaximumFrames * 2 + 2], first[1920], neutral[1920];
    const auto unchanged = [] noexcept { return bytes_equal(&saved, &decoder, sizeof(decoder)); };
    for (u32 channels = 1; channels <= 2; ++channels) {
        opus_decoder_start(&decoder, channels);
        copy_bytes(&saved, &decoder, sizeof(decoder));
        for (auto& sample : output) sample = 12345;
        check(!opus_decode(&decoder, {silence, 3}, {output, 960 * channels - 1}) && !opus_decode(&decoder, {}, {output, 11520}) &&
                  !opus_decode(&decoder, {silence, 3}, {nullptr, 11520}) &&
                  !opus_decode(&decoder, {oversized, sizeof(oversized)}, {output, 11520}) &&
                  !opus_decode(&decoder, {missing_count, 1}, {output, 11520}) &&
                  !opus_decode(&decoder, {over_duration, 2}, {output, 11520}) &&
                  !opus_decode(&decoder, {bad_lengths, 2}, {output, 11520}) && unchanged() && output[0] == 12345 &&
                  output[11521] == 12345,
              "Opus packets: short PCM, empty, oversized, countless, longer than 120 ms and overrunning packets decode nothing and leave state and PCM alone");
        check(!opus_decode_lost(&decoder, {}, 0, {output, 11520}) && !opus_decode_lost(&decoder, {}, 961, {output, 11520}) &&
                  !opus_decode_lost(&decoder, {}, 5880, {output, 11520 + 2}) &&
                  !opus_decode_lost(&decoder, {}, 960, {output, 960 * channels - 1}) &&
                  !opus_decode_lost(&decoder, {bad_lengths, 2}, 960, {output, 11520}) && unchanged() && output[0] == 12345,
              "Opus loss: a duration that is no whole number of 2.5 ms up to 120 ms, short PCM or a malformed next packet conceal nothing");
        u32 frames = opus_decode(&decoder, {silence, 3}, {output, 960 * channels});
        bool silent = frames == 960;
        for (u32 i = 0; i < 960 * channels; ++i) silent = silent && output[i] == 0;
        check(silent && output[960 * channels] == 12345, "Opus PCM: a 20 ms silence frame decodes to exactly 960 frames of zeros");
        copy_bytes(first, output, 960 * channels * static_cast<u32>(sizeof(i16)));
        opus_decoder_reset(&decoder);
        frames = opus_decode(&decoder, {silence, 3}, {output, 960 * channels});
        check(frames == 960 && bytes_equal(first, output, 960 * channels * static_cast<u32>(sizeof(i16))),
              "Opus reset: the first packet reproduces its PCM after the history is dropped");
        for (auto& sample : output) sample = 12345;
        frames = opus_decode(&decoder, {longest, sizeof(longest)}, {output, kOpusDecoderMaximumFrames * channels});
        silent = frames == kOpusDecoderMaximumFrames;
        for (u32 i = 0; i < kOpusDecoderMaximumFrames * channels; ++i) silent = silent && output[i] == 0;
        check(silent && output[kOpusDecoderMaximumFrames * channels] == 12345,
              "Opus boundary: a six-frame 120 ms packet decodes all 5760 frames per channel and not one sample more");
        for (auto& sample : output) sample = 12345;
        check(opus_decode_lost(&decoder, {}, 960, {output, 960 * channels}) == 960 && output[960 * channels] == 12345 &&
                  opus_decode_lost(&decoder, {silence, 3}, 480, {output, 480 * channels}) == 480 &&
                  !opus_packet_has_fec({silence, 3}) && !opus_packet_has_fec({}) && !opus_decoder_final_range(&decoder),
              "Opus loss: concealment fills the asked duration, also before a packet without a redundant copy");
        // Gain through the decoded output: a CELT packet with an arbitrary payload decodes to the neutral PCM scaled
        // by the header gain, which a reset keeps; the endpoints mute and saturate.
        const u8 noise[]{0xf8, 0x5a, 0x3c, 0x96, 0xe1, 0x27, 0xb4, 0x0f, 0x78, 0xc3, 0x69, 0x1e, 0xd2, 0x8b, 0x45, 0xf0, 0x3a, 0x97};
        const auto peak = [&](const i16* pcm) noexcept {
            i32 value = 0;
            for (u32 i = 0; i < 960 * channels; ++i) {
                const i32 magnitude = pcm[i] < 0 ? -static_cast<i32>(pcm[i]) : pcm[i];
                if (magnitude > value) value = magnitude;
            }
            return value;
        };
        opus_decoder_start(&decoder, channels);
        const i32 neutral_peak = opus_decode(&decoder, {noise, sizeof(noise)}, {neutral, 960 * channels}) == 960 ? peak(neutral) : 0;
        check(neutral_peak > 8 && opus_decoder_final_range(&decoder), "Opus gain: the reference packet decodes to audible PCM at neutral gain");
        const i32 gains[]{-32768, -1536, 1536, 32767};
        for (i32 gain : gains) {
            opus_decoder_start(&decoder, channels, gain);
            opus_decoder_reset(&decoder);
            const i32 scaled = opus_decode(&decoder, {noise, sizeof(noise)}, {output, 960 * channels}) == 960 ? peak(output) : -1;
            const bool expected = gain == -32768 ? scaled == 0
                                : gain == -1536  ? scaled * 2 >= neutral_peak - 2 && scaled * 2 <= neutral_peak + 2
                                : gain == 1536   ? scaled >= neutral_peak * 2 - 2 && scaled <= neutral_peak * 2 + 2
                                                 : scaled >= 32767; // either rail of PCM16
            check(expected, "Opus gain: the header gain in 1/256 dB scales the decoded PCM, endpoints included, and survives a reset");
        }
        opus_decoder_start(&decoder, channels);
        check(opus_decode(&decoder, {noise, sizeof(noise)}, {output, 960 * channels}) == 960 &&
                  bytes_equal(neutral, output, 960 * channels * static_cast<u32>(sizeof(i16))),
              "Opus start: restarting a decoder leaves nothing of the stream before");
    }
}
} // namespace maicord
