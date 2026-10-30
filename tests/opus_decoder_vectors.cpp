#include "fixtures/opus_decoder_vectors.inc"
#include "opus_vector_decode.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

// Fast in-suite subset of tools/opus_conformance.ps1: synthetic streams that
// reach every decoder mode, bandwidth class, frame size, frame-count code,
// concealment, FEC and DTX path, decoded mono and stereo. Each must be
// range-exact against the encoder and bit-exact against the unmodified
// fixed-point decoder.
namespace maicord {
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
alignas(16) u8 decoder_storage[65536], scratch[OPUS_SCRATCH_BYTES];
i16 frame[kOpusVectorMaximumFrame * 2];

void test_opus_decoder_vectors() noexcept {
    u32 configs = 0, codes = 0, layouts = 0, lost = 0, recovered = 0;
    for (const OpusDecoderVector& vector : kOpusDecoderVectors) {
        for (u32 offset = 0; offset + 8 <= vector.bytes;) {
            const u32 length = opus_vector_big_endian(vector.data + offset);
            if (length) {
                const u8 toc = vector.data[offset + 8];
                configs |= 1u << (toc >> 3), codes |= 1u << (toc & 3), layouts |= toc & 4 ? 2 : 1;
            }
            offset += 8 + length;
        }
        for (u32 channels = 1; channels <= 2; ++channels) {
            auto* decoder = reinterpret_cast<::OpusDecoder*>(decoder_storage);
            const bool ready = opus_decoder_get_size(static_cast<int>(channels)) <= static_cast<int>(sizeof(decoder_storage)) &&
                               opus_decoder_init(decoder, 48000, static_cast<int>(channels)) == OPUS_OK;
            const OpusVectorDecode decoded =
                opus_vector_decode(decoder, scratch, channels, vector.data, vector.bytes, frame, nullptr, 0);
            const bool ok = ready && decoded.packets == vector.packets && decoded.lost == vector.lost &&
                            decoded.recovered == vector.recovered && !decoded.range_errors && !decoded.decode_errors &&
                            decoded.digest == vector.digest[channels - 1];
            if (!ok) test_write_text(vector.name), test_write_text(channels == 1 ? " (mono): " : " (stereo): ");
            check(ok, "Opus decoder vectors: range-exact against the encoder and PCM bit-exact against the reference decoder");
            if (channels == 1) lost += decoded.lost, recovered += decoded.recovered;
        }
    }
    // SILK NB/MB/WB at 10-60 ms, hybrid SWB/FB, CELT at every bandwidth and 2.5-20 ms.
    constexpr u32 required = 1u << 0 | 1u << 1 | 1u << 5 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 |
                             1u << 13 | 1u << 15 | 1u << 16 | 1u << 20 | 1u << 21 | 1u << 23 | 1u << 26 | 1u << 31;
    check((configs & required) == required && codes == 15 && layouts == 3,
          "Opus decoder vectors: modes, bandwidths, frame sizes, all four frame-count codes, mono and stereo packets");
    check(lost >= 12 && recovered >= 3, "Opus decoder vectors: concealment and LBRR recovery are exercised");
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_opus_decoder_vector_tests_entry() noexcept {
    maicord::test_opus_decoder_vectors();
    ExitProcess(maicord::failures ? 1 : 0);
}
