#include "maicord/media/opus_decoder.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <opus.h>

namespace maicord {
void test_opus_decoder(void (*check)(bool, const char*) noexcept) noexcept {
    const u8 silence[]{0xf8, 0xff, 0xfe}; // One20ms CELT silence frame.
    const u8 longest[]{0xfb, 6, 0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe,
                      0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe}; // Six equal20ms frames.
    const u8 missing_count[]{0xff}, over_duration[]{0xfb, 7}, bad_lengths[]{0xfa, 0xff};
    check(!opus_decoder_storage_bytes(0) && !opus_decoder_storage_bytes(3) &&
              opus_decoder_storage_bytes(1) && opus_decoder_storage_bytes(2),
          "Opus storage: official dynamic mono/stereo sizes and invalid channel refusal");
    OpusDecoder absent{};
    i16 output[kOpusDecoderMaximumFrames * 2 + 2], first[1920];
    check(opus_decoder_decode(&absent, {silence, 3}, {output, 1920}).error == OpusDecodeError::invalid_state &&
              opus_decoder_reset(&absent).error == OpusDecodeError::invalid_state &&
              opus_decoder_gain(&absent, 0).error == OpusDecodeError::invalid_state &&
              opus_decoder_initialize(nullptr, {}, 1).error == OpusDecodeError::invalid_input,
          "Opus lifecycle: inactive and null callers never enter the native decoder");
    for (u32 channels = 1; channels <= 2; ++channels) {
        const u32 bytes = opus_decoder_storage_bytes(channels);
        Arena arena{};
        const bool allocated = bytes && arena_initialize(&arena, static_cast<usize>(bytes) * 2 + 64, 0, 4096);
        check(allocated, "Opus storage: caller reserves exact reported decoder and comparison storage");
        if (!allocated) return;
        struct Release { Arena& arena; ~Release() noexcept { arena_release(&arena); } } release{arena};
        auto* storage = static_cast<u8*>(arena_allocate(&arena, bytes, kOpusDecoderStateAlignment));
        auto* saved = static_cast<u8*>(arena_allocate(&arena, bytes, kOpusDecoderStateAlignment));
        check(storage && saved, "Opus storage: caller commits aligned state without codec allocation");
        if (!storage || !saved) return;
        OpusDecoder decoder{};
        check(opus_decoder_initialize(&decoder, {storage, bytes - 1}, channels).error == OpusDecodeError::capacity &&
                  !decoder.initialized &&
                  opus_decoder_initialize(&decoder, {storage + 1, bytes}, channels).error == OpusDecodeError::invalid_input &&
                  opus_decoder_initialize(&decoder, {storage, bytes}, 3).error == OpusDecodeError::invalid_input,
              "Opus admission: short or misaligned storage and unsupported channels preserve inactive metadata");
        const auto initialized = opus_decoder_initialize(&decoder, {storage, bytes}, channels);
        check(initialized.ok() && decoder.initialized && decoder.state == storage && decoder.state_bytes == bytes && decoder.channels == channels,
              "Opus lifecycle: real native init binds only caller-owned48k mono/stereo state");
        if (!initialized.ok()) return;
        copy_bytes(saved, storage, bytes);
        for (auto& sample : output) sample = 12345;
        check(opus_decoder_initialize(&decoder, {storage, bytes}, channels).error == OpusDecodeError::busy &&
                  opus_decoder_decode(&decoder, {silence, 3}, {output, 960 * channels - 1}).error == OpusDecodeError::capacity &&
                  opus_decoder_decode(&decoder, {}, {output, 11520}).error == OpusDecodeError::invalid_input &&
                  opus_decoder_decode(&decoder, {silence, kOpusDecoderMaximumPacketBytes + 1}, {output, 11520}).error == OpusDecodeError::invalid_input &&
                  opus_decoder_decode(&decoder, {storage, 3}, {output, 11520}).error == OpusDecodeError::invalid_input &&
                  opus_decoder_decode(&decoder, {reinterpret_cast<const u8*>(output), 3}, {output, 11520}).error == OpusDecodeError::invalid_input &&
                  opus_decoder_decode(&decoder, {silence, 3}, {reinterpret_cast<i16*>(storage), bytes / 2}).error == OpusDecodeError::invalid_input &&
                  bytes_equal(saved, storage, bytes) && output[0] == 12345 && output[11521] == 12345,
              "Opus preflight: busy/capacity/empty/oversized/aliased inputs leave real state and PCM unchanged");
        check(opus_decoder_decode(&decoder, {missing_count, 1}, {output, 11520}).error == OpusDecodeError::packet &&
                  opus_decoder_decode(&decoder, {over_duration, 2}, {output, 11520}).error == OpusDecodeError::packet &&
                  bytes_equal(saved, storage, bytes),
              "Opus packets: missing frame count and greater-than120ms duration refuse before decode");
        const auto decoded = opus_decoder_decode(&decoder, {silence, 3}, {output, 960 * channels});
        bool silent = decoded.ok() && decoded.frames == 960;
        for (u32 i = 0; i < 960 * channels; ++i) silent = silent && output[i] == 0;
        check(silent && output[960 * channels] == 12345,
              "Opus PCM: real mono/stereo20ms silence decodes exact frames and preserves trailing sentinel");
        copy_bytes(first, output, 960 * channels * static_cast<u32>(sizeof(i16)));
        check(opus_decoder_reset(&decoder).ok(), "Opus reset: real OPUS_RESET_STATE succeeds without replacing caller storage");
        const auto repeated = opus_decoder_decode(&decoder, {silence, 3}, {output, 960 * channels});
        check(repeated.ok() && repeated.frames == 960 && bytes_equal(first, output, 960 * channels * static_cast<u32>(sizeof(i16))),
              "Opus reset: first packet reproduces exact PCM after decoder history is reset");
        for (auto& sample : output) sample = 12345;
        const auto maximum = opus_decoder_decode(&decoder, {longest, sizeof(longest)}, {output, kOpusDecoderMaximumFrames * channels});
        silent = maximum.ok() && maximum.frames == kOpusDecoderMaximumFrames;
        for (u32 i = 0; i < kOpusDecoderMaximumFrames * channels; ++i) silent = silent && output[i] == 0;
        check(silent && output[kOpusDecoderMaximumFrames * channels] == 12345,
              "Opus boundary: six-frame120ms packet decodes all5760 frames per channel without output overflow");
        check(opus_decoder_decode(&decoder, {bad_lengths, 2}, {output, 11520}).error == OpusDecodeError::packet &&
                  opus_decoder_reset(&decoder).ok(),
              "Opus native error: malformed VBR frame lengths reach native packet rejection and reset remains usable");
        const i16 gains[]{-32768, -1536, 1536, 32767};
        for (i16 gain : gains) {
            opus_int32 actual = 0;
            check(opus_decoder_gain(&decoder, gain).ok() &&
                      ::opus_decoder_ctl(reinterpret_cast<::OpusDecoder*>(storage), OPUS_GET_GAIN(&actual)) == OPUS_OK && actual == gain &&
                      opus_decoder_reset(&decoder).ok() &&
                      ::opus_decoder_ctl(reinterpret_cast<::OpusDecoder*>(storage), OPUS_GET_GAIN(&actual)) == OPUS_OK && actual == gain,
                  "Opus gain: native signed Q8dB adjustment including endpoints survives reset exactly");
        }
        check(opus_decoder_gain(&decoder, 0).ok(), "Opus gain: session can restore neutral header gain");
        copy_bytes(saved, storage, bytes);
        opus_decoder_release(&decoder);
        check(!decoder.state && !decoder.initialized && !decoder.state_bytes && !decoder.channels && bytes_equal(saved, storage, bytes) &&
                  opus_decoder_decode(&decoder, {silence, 3}, {output, 11520}).error == OpusDecodeError::invalid_state,
              "Opus release: detach clears only wrapper metadata and never destroys or frees caller state");
    }
    opus_decoder_release(nullptr);
}
} // namespace maicord
