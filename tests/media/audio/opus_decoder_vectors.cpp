#include "../../fixtures/opus_decoder_vectors.inc"
#include "../../support/opus_vector_decode.hpp"
#include "../../support/test_output.hpp"

// Decoder coverage streams: synthetic streams that
// reach every decoder mode, bandwidth class, frame size, frame-count code,
// concealment, FEC and DTX path, decoded mono and stereo. Each must be
// range-exact against the encoder and produce the pinned PCM.
namespace maicord {
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
OpusDecoder decoder;
i16 frame[kOpusDecoderMaximumFrames * 2];

void test_opus_decoder_vectors() noexcept {
    for (const OpusDecoderVector& vector : kOpusDecoderVectors) {
        for (u32 channels = 1; channels <= 2; ++channels) {
            opus_decoder_start(&decoder, channels);
            const OpusVectorDecode decoded = opus_vector_decode(&decoder, channels, vector.data, vector.bytes, frame, nullptr, 0);
            const bool ok = decoded.packets == vector.packets && decoded.lost == vector.lost &&
                            decoded.recovered == vector.recovered && !decoded.range_errors && !decoded.decode_errors &&
                            decoded.digest == vector.digest[channels - 1];
            if (!ok) test_write_text(vector.name), test_write_text(channels == 1 ? " (mono): " : " (stereo): ");
            check(ok, "Opus decoder vectors: range-exact against the encoder, and the pinned PCM");
        }
    }
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_opus_decoder_vector_tests_entry) {
    maicord::test_opus_decoder_vectors();
    ::maicord::test_process_exit(maicord::failures ? 1 : 0);
}
