#include "../../fixtures/opus_decoder_vectors.inc"
#include "../../support/opus_vector_decode.hpp"
#include "../../support/test_output.hpp"
#include "maicord/media/compression/deflate.hpp"

// Decoder coverage streams: synthetic streams that reach every decoder mode, bandwidth class, frame size, frame-count
// code, concealment, FEC and DTX path, decoded mono and stereo. Each must be range-exact against the encoder. Where the
// fixture pins a digest, the PCM must be libopus's bit for bit (the SILK layer). Elsewhere every fourth 10 ms segment
// is compared with libopus's decode (references.bin): the median segment SNR must reach kFloorDb. The median ignores
// the few segments where the decoders differ by design (concealment, and clipping, which libopus's fixed point does
// inside the transform and MaiCord at the output). When the floor was set, the lowest median was 66.7 dB. Planted
// faults gave lower medians: a comb filter tap with the wrong sign 6.0 dB, the IMDCT window one sample off 36.8 dB,
// one band 1.5 dB too loud 26.0 dB, the SILK stereo predictor with the wrong sign 12.1 dB (and a changed digest).
namespace maicord {
namespace {
constexpr u32 kSegment = 480, kStride = 4;
constexpr double kFloorDb = 55;
constexpr u32 kReferenceBytes = 239040;

u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
OpusDecoder decoder;
i16 frame[kOpusDecoderMaximumFrames * 2];
i16 pcm[1u << 16];
alignas(16) u8 references[kReferenceBytes];
DeflateWorkspace workspace;

double decibels(double ratio) noexcept { // 10 log10(ratio), ratio > 0
    i32 exponent = 0;
    while (ratio >= 2)
        ratio *= 0.5, ++exponent;
    while (ratio < 1)
        ratio *= 2, --exponent;
    const double y = (ratio - 1) / (ratio + 1), y2 = y * y;
    const double ln = 2 * y * (1 + y2 * (1.0 / 3 + y2 * (1.0 / 5 + y2 * (1.0 / 7 + y2 / 9))));
    return 3.0102999566398120 * (exponent + ln * 1.4426950408889634);
}

// The median SNR of the stored segments of one decode against the reference at `*cursor`, which it advances. False
// when the decode has another length than libopus's (the reference runs out).
bool segment_median(const i16* decoded, u32 values, u32 channels, u32* cursor, double* median) noexcept {
    double snr[256];
    u32 count = 0;
    const u32 segment = kSegment * channels;
    for (u32 k = 0, at = 0; at + segment <= values; ++k, at += segment) {
        if (k % kStride)
            continue;
        if (*cursor + segment * 2 > kReferenceBytes || count == 256)
            return false;
        double signal = 0, noise = 0;
        for (u32 i = 0; i < segment; ++i) {
            const u8* const r = references + *cursor + 2 * i;
            const double reference = static_cast<i16>(static_cast<u16>(r[0] | r[1] << 8));
            const double error = reference - decoded[at + i];
            signal += reference * reference, noise += error * error;
        }
        *cursor += segment * 2;
        // One PCM16 step of noise per sample: silence matches and rounding does not count.
        double value = decibels((signal + segment) / (noise + segment));
        u32 j = count++;
        for (; j > 0 && snr[j - 1] > value; --j)
            snr[j] = snr[j - 1];
        snr[j] = value;
    }
    if (!count)
        return false;
    *median = snr[count / 2];
    return true;
}

void test_opus_decoder_vectors() noexcept {
    const DeflateInputSpan input{kOpusDecoderReferences, sizeof(kOpusDecoderReferences)};
    const ZlibInflateResult inflated =
        zlib_inflate({&input, 1}, {references, kReferenceBytes}, kReferenceBytes, &workspace);
    check(inflated.ok() && inflated.output_bytes == kReferenceBytes, "Opus decoder vectors: the references inflate");
    u32 cursor = 0;
    for (const OpusDecoderVector& vector : kOpusDecoderVectors) {
        for (u32 channels = 1; channels <= 2; ++channels) {
            opus_decoder_start(&decoder, channels);
            const OpusVectorDecode decoded =
                opus_vector_decode(&decoder, channels, vector.data, vector.bytes, frame, pcm, sizeof(pcm) / 2);
            bool ok = decoded.packets == vector.packets && decoded.lost == vector.lost &&
                      decoded.recovered == vector.recovered && !decoded.range_errors && !decoded.decode_errors &&
                      !decoded.overflow;
            double median = 0;
            if (vector.digest[channels - 1]) {
                ok = ok && decoded.digest == vector.digest[channels - 1];
            } else {
                ok = ok && segment_median(pcm, static_cast<u32>(decoded.samples) * channels, channels, &cursor,
                                          &median) &&
                     median >= kFloorDb;
                test_write_text(vector.name), test_write_text(channels == 1 ? " (mono)" : " (stereo)");
                test_write_metric(": median segment SNR in 0.1 dB ", static_cast<u64>(median > 0 ? median * 10 : 0));
            }
            if (!ok)
                test_write_text(vector.name), test_write_line(channels == 1 ? " (mono): failed" : " (stereo): failed");
            check(ok, "Opus decoder vectors: range-exact against the encoder, and libopus's PCM (pinned digest or "
                      "median segment SNR)");
        }
    }
    check(cursor == kReferenceBytes, "Opus decoder vectors: every reference segment is compared");
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_opus_decoder_vector_tests_entry) {
    maicord::test_opus_decoder_vectors();
    ::maicord::test_process_exit(maicord::failures ? 1 : 0);
}
