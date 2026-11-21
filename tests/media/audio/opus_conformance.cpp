#include "../../support/opus_vector_decode.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/sys/simd.hpp"

// Opus decoder conformance against the RFC 8251 test vectors: the directory that MAICORD_OPUS_VECTORS names holds
// testvector01.bit to testvector12.bit with their .dec and m.dec reference outputs (opus_testvectors-rfc8251.tar.gz,
// opus_newvectors/). Without the variable the test is skipped (77). Every vector is decoded to mono and to stereo
// at 48 kHz as upstream's run_vectors.sh drives opus_demo -d (concealment for empty packets, FEC for the last lost
// one), and then:
//   - the decoder's final range equals the encoder's after every packet that is not next to a loss;
//   - upstream's opus_compare quality is 0 or more against the .dec or the m.dec reference (mono against the
//     references' mix of the two channels), as the RFC requires;
//   - the PSNR against the nearer reference reaches kFloorDb. When the floor was set, the lowest was 98.7 dB, and a
//     comb filter tap with the wrong sign gave 26.3 dB (and failed opus_compare on 12 of the 24 decodes).
namespace maicord {
namespace {
constexpr u32 kVectors = 12;
constexpr double kFloorDb = 80;

// ---- upstream src/opus_compare.c at 48 kHz: pseudo-NMR over the CELT bands with frequency and temporal masking ----
constexpr double kPi = 3.14159265358979323846;
double cosine(double x) noexcept {
    while (x > kPi)
        x -= 2 * kPi;
    while (x < -kPi)
        x += 2 * kPi;
    const double x2 = x * x;
    double term = 1, sum = 1;
    for (i32 k = 1; k < 15; ++k)
        term *= -x2 / ((2 * k - 1) * (2 * k)), sum += term;
    return sum;
}
double logarithm(double x) noexcept { // x > 0, normal
    u64 bits = __builtin_bit_cast(u64, x);
    i32 exponent = static_cast<i32>((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m = __builtin_bit_cast(double, bits);
    if (m > 1.4142135623730951)
        m *= 0.5, ++exponent;
    const double y = (m - 1) / (m + 1), y2 = y * y;
    double term = y, sum = 0;
    for (i32 k = 1; k < 30; k += 2)
        sum += term / k, term *= y2;
    return exponent * 0.6931471805599453 + 2 * sum;
}
double square_root(double x) noexcept {
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
}

constexpr u32 kBands = 21, kWindow = 480, kStep = 120, kBins = 200;
constexpr u32 kBandEdges[kBands + 1]{0,  2,  4,  6,  8,  10, 12, 14, 16,  20,  24,
                                     28, 32, 40, 48, 56, 68, 80, 96, 120, 156, 200};
float window_table[kWindow];
float cosines[kBins][kWindow], sines[kBins][kWindow];

void compare_tables() noexcept {
    const float pi = 3.14159265f;
    for (u32 k = 0; k < kWindow; ++k)
        window_table[k] = 0.5f - 0.5f * static_cast<float>(cosine((2 * pi / (kWindow - 1)) * static_cast<float>(k)));
    float c[kWindow], s[kWindow];
    for (u32 k = 0; k < kWindow; ++k) {
        const float angle = (2 * pi / kWindow) * static_cast<float>(k);
        c[k] = static_cast<float>(cosine(angle)), s[k] = static_cast<float>(cosine(angle - kPi / 2));
    }
    for (u32 j = 0; j < kBins; ++j)
        for (u32 k = 0; k < kWindow; ++k)
            cosines[j][k] = c[(j * k) % kWindow], sines[j][k] = s[(j * k) % kWindow];
}

struct Spectrum {
    float bins[kBins][2];
    float bands[kBands][2];
};
// `pcm` has `pcm_channels` interleaved channels; a stereo reference is mixed to mono when `channels` is 1.
void spectrum(Spectrum* out, const i16* pcm, u32 pcm_channels, u32 channels, u32 frame) noexcept {
    float x[2][kWindow];
    for (u32 k = 0; k < kWindow; ++k) {
        const i16* sample = pcm + static_cast<usize>(frame * kStep + k) * pcm_channels;
        if (pcm_channels == 2 && channels == 1)
            x[0][k] = window_table[k] *
                      static_cast<float>(0.5 * (static_cast<float>(sample[0]) + static_cast<float>(sample[1])));
        else
            for (u32 c = 0; c < channels; ++c)
                x[c][k] = window_table[k] * static_cast<float>(sample[c]);
    }
    for (u32 b = 0, j = 0; b < kBands; ++b) {
        float p[2]{};
        for (; j < kBandEdges[b + 1]; ++j)
            for (u32 c = 0; c < channels; ++c) {
                // The sums in any order (vectorized): the metric is no output that must keep its rounding.
                #pragma clang fp reassociate(on)
                float re = 0, im = 0;
                for (u32 k = 0; k < kWindow; ++k)
                    re += cosines[j][k] * x[c][k], im -= sines[j][k] * x[c][k];
                out->bins[j][c] = re * re + im * im + 100000;
                p[c] += out->bins[j][c];
            }
        for (u32 c = 0; c < channels; ++c)
            out->bands[b][c] = p[c] / static_cast<float>(kBandEdges[b + 1] - kBandEdges[b]);
    }
}

struct CompareState {
    Spectrum previous_x, previous_y;
    float previous_bands[kBands][2];
    double error;
};
void compare_frame(CompareState* state, Spectrum* x, Spectrum* y, u32 channels, u32 frame) noexcept {
    for (u32 b = 1; b < kBands; ++b)
        for (u32 c = 0; c < channels; ++c)
            x->bands[b][c] += 0.1f * x->bands[b - 1][c];
    for (u32 b = kBands - 1; b-- > 0;)
        for (u32 c = 0; c < channels; ++c)
            x->bands[b][c] += 0.03f * x->bands[b + 1][c];
    if (frame)
        for (u32 b = 0; b < kBands; ++b)
            for (u32 c = 0; c < channels; ++c)
                x->bands[b][c] += 0.5f * state->previous_bands[b][c];
    if (channels == 2)
        for (u32 b = 0; b < kBands; ++b) {
            const float l = x->bands[b][0], r = x->bands[b][1];
            x->bands[b][0] += 0.01f * r, x->bands[b][1] += 0.01f * l;
        }
    for (u32 b = 0; b < kBands; ++b)
        for (u32 j = kBandEdges[b]; j < kBandEdges[b + 1]; ++j)
            for (u32 c = 0; c < channels; ++c)
                x->bins[j][c] += 0.1f * x->bands[b][c], y->bins[j][c] += 0.1f * x->bands[b][c];
    double frame_error = 0;
    for (u32 b = 0; b < kBands; ++b) {
        double band_error = 0;
        for (u32 j = kBandEdges[b]; j < kBandEdges[b + 1]; ++j)
            for (u32 c = 0; c < channels; ++c) {
                // Consecutive frames are summed (upstream's "average"), with the values before the sum.
                const float xs = x->bins[j][c] + (frame ? state->previous_x.bins[j][c] : 0);
                const float ys = y->bins[j][c] + (frame ? state->previous_y.bins[j][c] : 0);
                const float re = ys / xs;
                float im = static_cast<float>(re - logarithm(re) - 1);
                if (j >= 79 && j <= 81)
                    im *= 0.1f;
                if (j == 80)
                    im *= 0.1f;
                band_error += im;
            }
        band_error /= (kBandEdges[b + 1] - kBandEdges[b]) * channels;
        frame_error += band_error * band_error;
    }
    frame_error /= kBands;
    frame_error *= frame_error;
    state->error += frame_error * frame_error;
    state->previous_x = *x, state->previous_y = *y;
    for (u32 b = 0; b < kBands; ++b)
        for (u32 c = 0; c < channels; ++c)
            state->previous_bands[b][c] = x->bands[b][c];
}
double quality(double error, u32 frames) noexcept {
    const double e = square_root(square_root(square_root(square_root(error / frames))));
    return 100 * (1 - 0.5 * logarithm(1 + e) / logarithm(1.13));
}

// ---- The vectors ----
struct File {
    u8* data;
    u32 bytes;
};
struct Job {
    u32 vector, channels;
    File bitstream, reference[2]; // .dec, m.dec: stereo PCM16
    OpusVectorDecode decoded;
    double q[2], psnr[2];
    bool ok;
};
Job jobs[kVectors * 2];
volatile LONG next_job;

// The PSNR of `pcm` against a stereo reference (its mix of the two channels for mono).
double psnr(const i16* pcm, const i16* reference, u32 frames, u32 channels) noexcept {
    double noise = 0;
    for (u32 i = 0; i < frames; ++i)
        for (u32 c = 0; c < channels; ++c) {
            const double r = channels == 2 ? reference[2 * i + c] : 0.5 * (reference[2 * i] + reference[2 * i + 1]);
            const double e = r - pcm[i * channels + c];
            noise += e * e;
        }
    return noise ? 10 * logarithm(32768.0 * 32768.0 * frames * channels / noise) / logarithm(10.0) : 999;
}

void run_job(Job* job) noexcept {
    const u32 channels = job->channels, reference_frames = job->reference[0].bytes / 4;
    const usize capacity = (static_cast<usize>(reference_frames) + kOpusDecoderMaximumFrames) * channels;
    auto* pcm = static_cast<i16*>(pages_allocate(capacity * 2 + kOpusDecoderMaximumFrames * 4 + sizeof(OpusDecoder)));
    if (!pcm)
        return;
    i16* const frame = pcm + capacity;
    auto* decoder = reinterpret_cast<OpusDecoder*>(frame + kOpusDecoderMaximumFrames * 2);
    opus_decoder_start(decoder, channels);
    job->decoded =
        opus_vector_decode(decoder, channels, job->bitstream.data, job->bitstream.bytes, frame, pcm, capacity);
    const bool lengths = !job->decoded.overflow && job->decoded.samples == reference_frames &&
                         job->reference[1].bytes == job->reference[0].bytes && reference_frames >= kWindow;
    for (u32 r = 0; r < 2; ++r)
        job->q[r] = job->psnr[r] = -1000;
    if (lengths) {
        const u32 frames = (reference_frames - kWindow + kStep) / kStep;
        CompareState* const state = static_cast<CompareState*>(pages_allocate(sizeof(CompareState)));
        // The m.dec reference only when the .dec one fails, as passing one is enough.
        for (u32 r = 0; state && r < 2 && job->q[0] < 0; ++r) {
            *state = {};
            Spectrum x, y;
            for (u32 f = 0; f < frames; ++f) {
                spectrum(&y, pcm, channels, channels, f);
                spectrum(&x, reinterpret_cast<const i16*>(job->reference[r].data), 2, channels, f);
                compare_frame(state, &x, &y, channels, f);
            }
            job->q[r] = quality(state->error, frames);
        }
        for (u32 r = 0; r < 2; ++r)
            job->psnr[r] = psnr(pcm, reinterpret_cast<const i16*>(job->reference[r].data), reference_frames, channels);
        if (state)
            pages_release(state);
    }
    job->ok = lengths && !job->decoded.range_errors && !job->decoded.decode_errors &&
              (job->q[0] >= 0 || job->q[1] >= 0) && maximum(job->psnr[0], job->psnr[1]) >= kFloorDb;
    pages_release(pcm);
}

u32 job_thread(void*) noexcept {
    for (;;) {
        const LONG index = InterlockedIncrement(&next_job) - 1;
        if (index >= static_cast<LONG>(kVectors * 2))
            return 0;
        run_job(&jobs[index]);
    }
}

bool read_file(const wchar_t* directory, u32 length, u32 vector, const wchar_t* suffix, File* out) noexcept {
    wchar_t path[1100];
    copy_bytes(path, directory, length * 2);
    const wchar_t name[] = L"/testvector00";
    copy_bytes(path + length, name, sizeof(name));
    path[length + 11] = static_cast<wchar_t>(L'0' + vector / 10);
    path[length + 12] = static_cast<wchar_t>(L'0' + vector % 10);
    u32 at = length + 13;
    while (*suffix)
        path[at++] = *suffix++;
    path[at] = 0;
    const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, kFileSequential});
    u64 size = 0;
    if (file == INVALID_HANDLE_VALUE || !file_size(file, &size) || size > (64u << 20)) {
        file_close(file);
        return false;
    }
    out->bytes = static_cast<u32>(size);
    out->data = static_cast<u8*>(pages_allocate(size + 16));
    u32 read = 0;
    const bool ok = out->data && file_read(file, out->data, out->bytes, &read) && read == out->bytes;
    file_close(file);
    return ok;
}

void tenths(double value) noexcept {
    if (value < 0)
        test_write_text("-"), value = -value;
    const u64 scaled = static_cast<u64>(value * 10 + 0.5);
    test_write_u64(scaled / 10), test_write_text("."), test_write_u64(scaled % 10);
}

u32 run() noexcept {
    wchar_t directory[1024];
    const u32 length = environment_variable(L"MAICORD_OPUS_VECTORS", directory, 1024);
    if (!length || length >= 1024) {
        test_write_line("Opus conformance: MAICORD_OPUS_VECTORS names no directory; skipped");
        return 77;
    }
    for (u32 v = 0; v < kVectors; ++v) {
        File bitstream{}, reference[2]{};
        if (!read_file(directory, length, v + 1, L".bit", &bitstream) ||
            !read_file(directory, length, v + 1, L".dec", &reference[0]) ||
            !read_file(directory, length, v + 1, L"m.dec", &reference[1])) {
            test_write_line("Opus conformance: a vector or reference file is missing or unreadable");
            return 1;
        }
        for (u32 c = 0; c < 2; ++c)
            jobs[2 * v + c] = {v, c + 1, bitstream, {reference[0], reference[1]}, {}, {}, {}, false};
    }
    compare_tables();
    Thread* threads[8];
    u32 started = 0;
    for (; started < 8; ++started)
        if (!(threads[started] = thread_start(job_thread, nullptr, 1u << 20, nullptr)))
            break;
    if (!started)
        job_thread(nullptr);
    threads_wait_all(threads, started, ~0u);
    for (u32 t = 0; t < started; ++t)
        thread_close(threads[t]);
    u32 failed = 0;
    double lowest = 1000;
    for (const Job& job : jobs) {
        test_write_text("testvector"), test_write_u64((job.vector + 1) / 10), test_write_u64((job.vector + 1) % 10);
        test_write_text(job.channels == 1 ? " mono: packets " : " stereo: packets ");
        test_write_u64(job.decoded.packets);
        test_write_text(" range errors "), test_write_u64(job.decoded.range_errors);
        test_write_text(", quality .dec "), tenths(job.q[0]), test_write_text(" m.dec ");
        if (job.q[1] > -1000)
            tenths(job.q[1]);
        else
            test_write_text("(not needed)");
        test_write_text(", PSNR .dec "), tenths(job.psnr[0]), test_write_text(" m.dec "), tenths(job.psnr[1]);
        test_write_line(job.ok ? " dB: pass" : " dB: FAIL");
        failed += !job.ok;
        lowest = minimum(lowest, maximum(job.psnr[0], job.psnr[1]));
    }
    test_write_text("Opus conformance: lowest PSNR "), tenths(lowest), test_write_text(" dB, floor "), tenths(kFloorDb);
    test_write_line(failed ? " dB: FAIL" : " dB: all 24 pass");
    return failed ? 1 : 0;
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_opus_conformance_entry) {
    ::maicord::test_process_exit(maicord::run());
}
