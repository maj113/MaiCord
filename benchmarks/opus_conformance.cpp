// Opus decoder conformance against the official RFC 6716/8251 test vectors
// (tools/opus_conformance.ps1). Reads opus_testvectors-rfc8251.tar.gz from
// MAICORD_OPUS_VECTORS as untrusted data: pinned SHA-256, in-memory gunzip and
// tar parsing, no extraction. Every vector is decoded mono and stereo at 48 kHz
// the way upstream's run_vectors.sh drives opus_demo -d (concealment for empty
// packets, FEC for the last lost packet), and then:
//   - the decoder's final range must equal the encoder's for every packet;
//   - quality must pass upstream's opus_compare metric against either the
//     original (.dec) or the RFC 8251 (m.dec) reference output;
//   - the decoded PCM must hash to the pinned value of the fixed-point build,
//     so any change to the codec has to be bit-exact.
#include "benchmark_line.hpp"
#include "maicord/media/deflate.hpp"
#include "opus_vector_decode.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <emmintrin.h>

namespace maicord {
namespace {
constexpr u8 kArchiveSha256[32]{0x6b, 0x26, 0xa2, 0x2f, 0x9b, 0xa8, 0x7b, 0x2b, 0x83, 0x69, 0x06, 0xa9, 0xbb, 0x7a, 0xfe, 0xc5,
                                0xf8, 0xe5, 0x4d, 0x49, 0x55, 0x3b, 0x12, 0x00, 0x38, 0x25, 0x20, 0xee, 0x6f, 0xed, 0xfa, 0x55};
constexpr u32 kVectors = 12;
// FNV-1a of the PCM decoded by unmodified libopus 1.6.1 (fixed point), [vector][mono, stereo].
constexpr u64 kPinned[kVectors][2]{
    {0x42261282ad8626a1ull, 0x0302d7690368764aull},
    {0x15f959f97260e086ull, 0x5eac01cccc93301cull},
    {0xa97a51247684aa2aull, 0x9c7f7e103fca8b5full},
    {0x60e5f0434845cc9dull, 0xf24e9c3a21a8902dull},
    {0x5072419e87286935ull, 0xea5b53c828374244ull},
    {0x9143e22a20e275b1ull, 0xf165114f2eefcf53ull},
    {0xcd9a9dcbf038cb9dull, 0x11795ea66da2a042ull},
    {0x5be7eb8e0f5d2287ull, 0x9b6aa219dc26689dull},
    {0x166498a87dca8553ull, 0xca981f98a0dedee4ull},
    {0xbf58ee061ddae274ull, 0x6f9cdf643a99031full},
    {0x6abb4582f0eec50full, 0xf39e5464a08ba234ull},
    {0x2a281c70d5a80b77ull, 0x4e168c3987b94351ull},
};

// One decimal, sign kept.
void tenths(Line& line, const char* key, double value) noexcept {
    line.put(" "), line.put(key), line.put("=");
    if (value < 0) line.put("-"), value = -value;
    const u64 scaled = static_cast<u64>(value * 10 + 0.5);
    line.number(scaled / 10), line.put("."), line.number(scaled % 10);
}

// Double-precision helpers for the compare metric (no CRT math).
constexpr double kPi = 3.14159265358979323846;
double cosine(double x) noexcept {
    while (x > kPi) x -= 2 * kPi;
    while (x < -kPi) x += 2 * kPi;
    const double x2 = x * x;
    double term = 1, sum = 1;
    for (i32 k = 1; k < 15; ++k) term *= -x2 / ((2 * k - 1) * (2 * k)), sum += term;
    return sum;
}
double logarithm(double x) noexcept { // x > 0, normal
    u64 bits = __builtin_bit_cast(u64, x);
    i32 exponent = static_cast<i32>((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m = __builtin_bit_cast(double, bits);
    if (m > 1.4142135623730951) m *= 0.5, ++exponent;
    const double y = (m - 1) / (m + 1), y2 = y * y;
    double term = y, sum = 0;
    for (i32 k = 1; k < 30; k += 2) sum += term / k, term *= y2;
    return exponent * 0.6931471805599453 + 2 * sum;
}
double square_root(double x) noexcept { return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x))); }

// Port of upstream src/opus_compare.c (48 kHz): pseudo-NMR over the CELT bands
// with frequency and temporal masking, streamed one 120-sample step at a time.
constexpr u32 kBands = 21, kWindow = 480, kStep = 120, kBins = 200;
constexpr u32 kBandEdges[kBands + 1]{0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 68, 80, 96, 120, 156, 200};
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
        for (u32 k = 0; k < kWindow; ++k) cosines[j][k] = c[(j * k) % kWindow], sines[j][k] = s[(j * k) % kWindow];
}

struct Spectrum {
    float bins[kBins][2];
    float bands[kBands][2];
};
// reference: interleaved stereo PCM16 (downmixed when channels == 1); else test PCM with `channels`.
void spectrum(Spectrum* out, const i16* pcm, u32 pcm_channels, u32 channels, u32 frame) noexcept {
    float x[2][kWindow];
    for (u32 k = 0; k < kWindow; ++k) {
        const i16* sample = pcm + static_cast<usize>(frame * kStep + k) * pcm_channels;
        if (pcm_channels == 2 && channels == 1)
            x[0][k] = window_table[k] * static_cast<float>(0.5 * (static_cast<float>(sample[0]) + static_cast<float>(sample[1])));
        else
            for (u32 c = 0; c < channels; ++c) x[c][k] = window_table[k] * static_cast<float>(sample[c]);
    }
    for (u32 b = 0, j = 0; b < kBands; ++b) {
        float p[2]{};
        for (; j < kBandEdges[b + 1]; ++j)
            for (u32 c = 0; c < channels; ++c) {
                float re = 0, im = 0;
                for (u32 k = 0; k < kWindow; ++k) re += cosines[j][k] * x[c][k], im -= sines[j][k] * x[c][k];
                out->bins[j][c] = re * re + im * im + 100000;
                p[c] += out->bins[j][c];
            }
        for (u32 c = 0; c < channels; ++c) out->bands[b][c] = p[c] / static_cast<float>(kBandEdges[b + 1] - kBandEdges[b]);
    }
}

struct CompareState {
    Spectrum previous_x, previous_y;
    float previous_bands[kBands][2];
    double error;
};
void compare_frame(CompareState* state, Spectrum* x, Spectrum* y, u32 channels, u32 frame) noexcept {
    for (u32 b = 1; b < kBands; ++b)
        for (u32 c = 0; c < channels; ++c) x->bands[b][c] += 0.1f * x->bands[b - 1][c];
    for (u32 b = kBands - 1; b-- > 0;)
        for (u32 c = 0; c < channels; ++c) x->bands[b][c] += 0.03f * x->bands[b + 1][c];
    if (frame)
        for (u32 b = 0; b < kBands; ++b)
            for (u32 c = 0; c < channels; ++c) x->bands[b][c] += 0.5f * state->previous_bands[b][c];
    if (channels == 2)
        for (u32 b = 0; b < kBands; ++b) {
            const float l = x->bands[b][0], r = x->bands[b][1];
            x->bands[b][0] += 0.01f * r, x->bands[b][1] += 0.01f * l;
        }
    for (u32 b = 0; b < kBands; ++b)
        for (u32 j = kBandEdges[b]; j < kBandEdges[b + 1]; ++j)
            for (u32 c = 0; c < channels; ++c) x->bins[j][c] += 0.1f * x->bands[b][c], y->bins[j][c] += 0.1f * x->bands[b][c];
    double frame_error = 0;
    for (u32 b = 0; b < kBands; ++b) {
        double band_error = 0;
        for (u32 j = kBandEdges[b]; j < kBandEdges[b + 1]; ++j)
            for (u32 c = 0; c < channels; ++c) {
                // Consecutive frames are summed (upstream "average"), using pre-sum values.
                const float xs = x->bins[j][c] + (frame ? state->previous_x.bins[j][c] : 0);
                const float ys = y->bins[j][c] + (frame ? state->previous_y.bins[j][c] : 0);
                const float re = ys / xs;
                float im = static_cast<float>(re - logarithm(re) - 1);
                if (j >= 79 && j <= 81) im *= 0.1f;
                if (j == 80) im *= 0.1f;
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
        for (u32 c = 0; c < channels; ++c) state->previous_bands[b][c] = x->bands[b][c];
}
double quality(double error, u32 frames) noexcept {
    const double e = square_root(square_root(square_root(square_root(error / frames))));
    return 100 * (1 - 0.5 * logarithm(1 + e) / logarithm(1.13));
}

struct Entry {
    const u8* data;
    u32 bytes;
};
struct Job {
    u32 vector, channels;
    Entry bitstream, reference[2]; // .dec, m.dec
    u32 packets, lost, range_errors, decode_errors, samples;
    double q[2];
    u64 digest;
    bool ok;
};
Job jobs[kVectors * 2];
volatile LONG next_job = 0;

void run_job(Job* job) noexcept {
    const u32 channels = job->channels;
    const u32 reference_frames = job->reference[0].bytes / 4;
    const usize capacity = (static_cast<usize>(reference_frames) + kOpusVectorMaximumFrame) * channels;
    auto* pcm = static_cast<i16*>(VirtualAlloc(nullptr, capacity * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* frame = static_cast<i16*>(VirtualAlloc(nullptr, kOpusVectorMaximumFrame * 2 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* decoder = static_cast<OpusDecoder*>(VirtualAlloc(nullptr, static_cast<usize>(opus_decoder_get_size(static_cast<int>(channels))),
                                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    void* scratch = VirtualAlloc(nullptr, OPUS_SCRATCH_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pcm || !frame || !decoder || !scratch || opus_decoder_init(decoder, 48000, static_cast<int>(channels)) != OPUS_OK)
        return;
    const OpusVectorDecode decoded =
        opus_vector_decode(decoder, scratch, channels, job->bitstream.data, job->bitstream.bytes, frame, pcm, capacity);
    job->packets = decoded.packets, job->lost = decoded.lost;
    job->range_errors = decoded.range_errors, job->decode_errors = decoded.decode_errors;
    job->digest = decoded.digest;
    job->samples = static_cast<u32>(decoded.samples);
    const bool lengths = !decoded.overflow && job->samples == reference_frames && job->reference[1].bytes == job->reference[0].bytes &&
                         reference_frames >= kWindow;
    for (u32 r = 0; r < 2; ++r) job->q[r] = -1000;
    if (lengths) {
        const u32 frames = (reference_frames - kWindow + kStep) / kStep;
        auto* state = static_cast<CompareState*>(VirtualAlloc(nullptr, sizeof(CompareState) * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (state) {
            Spectrum x, y;
            for (u32 f = 0; f < frames; ++f) {
                spectrum(&y, pcm, channels, channels, f);
                for (u32 r = 0; r < 2; ++r) {
                    Spectrum yr = y;
                    spectrum(&x, reinterpret_cast<const i16*>(job->reference[r].data), 2, channels, f);
                    compare_frame(&state[r], &x, &yr, channels, f);
                }
            }
            for (u32 r = 0; r < 2; ++r) job->q[r] = quality(state[r].error, frames);
            VirtualFree(state, 0, MEM_RELEASE);
        }
    }
    job->ok = lengths && !job->range_errors && !job->decode_errors && (job->q[0] >= 0 || job->q[1] >= 0) &&
              kPinned[job->vector][channels - 1] == job->digest;
    VirtualFree(pcm, 0, MEM_RELEASE);
    VirtualFree(frame, 0, MEM_RELEASE);
    VirtualFree(decoder, 0, MEM_RELEASE);
    VirtualFree(scratch, 0, MEM_RELEASE);
}

DWORD WINAPI job_thread(void*) noexcept {
    for (;;) {
        const LONG index = InterlockedIncrement(&next_job) - 1;
        if (index >= static_cast<LONG>(kVectors * 2)) return 0;
        run_job(&jobs[index]);
    }
}

u64 octal(const u8* field, u32 width) noexcept {
    u32 i = 0;
    while (i < width && field[i] == ' ') ++i;
    u64 value = 0;
    for (; i < width && field[i] >= '0' && field[i] <= '7'; ++i) value = value * 8 + (field[i] - '0');
    return value;
}
bool fail(const char* reason) noexcept {
    Line line;
    line.put("opus_conformance error="), line.put(reason), line.emit();
    return false;
}
bool name_is(const u8* header, const char* expected) noexcept {
    u32 i = 0;
    for (; expected[i]; ++i)
        if (i >= 100 || header[i] != static_cast<u8>(expected[i])) return false;
    return i == 100 || header[i] == 0;
}

bool run() noexcept {
    Line line;
    wchar_t path[1024];
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_OPUS_VECTORS", path, 1024);
    if (!length || length >= 1024) return fail("MAICORD_OPUS_VECTORS");
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart < 32 || size.QuadPart > (256ll << 20))
        return fail("archive");
    const u32 archive_bytes = static_cast<u32>(size.QuadPart);
    auto* archive = static_cast<u8*>(VirtualAlloc(nullptr, archive_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD read = 0;
    for (u32 done = 0; archive && done < archive_bytes; done += read)
        if (!ReadFile(file, archive + done, archive_bytes - done, &read, nullptr) || !read) return false;
    CloseHandle(file);
    u8 hash[32];
    if (!archive || BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, archive, archive_bytes, hash, 32) != 0) return false;
    for (u32 i = 0; i < 32; ++i)
        if (hash[i] != kArchiveSha256[i]) return fail("sha256");
    const u32 tar_bytes = archive[archive_bytes - 4] | archive[archive_bytes - 3] << 8 | archive[archive_bytes - 2] << 16 |
                          static_cast<u32>(archive[archive_bytes - 1]) << 24;
    if (tar_bytes > (512u << 20)) return false;
    auto* tar = static_cast<u8*>(VirtualAlloc(nullptr, tar_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DeflateWorkspace workspace{};
    if (!tar || !gzip_inflate({archive, archive_bytes}, {tar, tar_bytes}, &workspace).ok())
        return fail("gzip");
    VirtualFree(archive, 0, MEM_RELEASE);
    for (u32 v = 0; v < kVectors; ++v)
        for (u32 c = 0; c < 2; ++c) jobs[2 * v + c].vector = v, jobs[2 * v + c].channels = c + 1;
    for (u64 offset = 0; offset + 512 <= tar_bytes;) {
        const u8* header = tar + offset;
        if (!header[0]) break;
        u32 sum = 0;
        for (u32 i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : header[i];
        const u64 bytes = octal(header + 124, 12);
        if (sum != octal(header + 148, 8) || offset + 512 + bytes > tar_bytes) return fail("tar");
        const Entry entry{header + 512, static_cast<u32>(bytes)};
        for (u32 v = 0; v < kVectors; ++v) {
            char name[48] = "opus_newvectors/testvector00";
            name[26] = static_cast<char>('0' + (v + 1) / 10), name[27] = static_cast<char>('0' + (v + 1) % 10);
            const char* suffixes[]{".bit", ".dec", "m.dec"};
            for (u32 s = 0; s < 3; ++s) {
                u32 n = 28;
                for (const char* p = suffixes[s]; *p; ++p) name[n++] = *p;
                name[n] = 0;
                if ((header[156] != '0' && header[156] != 0) || header[345]) continue;
                if (!name_is(header, name)) continue;
                for (u32 c = 0; c < 2; ++c) (s ? jobs[2 * v + c].reference[s - 1] : jobs[2 * v + c].bitstream) = entry;
            }
        }
        offset += 512 + ((bytes + 511) & ~511ull);
    }
    for (const Job& job : jobs)
        if (!job.bitstream.data || !job.reference[0].data || !job.reference[1].data)
            return fail("missing_vector");
    compare_tables();
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const u32 count = info.dwNumberOfProcessors < 16 ? info.dwNumberOfProcessors : 16;
    HANDLE threads[16];
    for (u32 t = 0; t < count; ++t)
        if (!(threads[t] = CreateThread(nullptr, 1u << 20, job_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)))
            return false;
    WaitForMultipleObjects(count, threads, TRUE, INFINITE);
    bool ok = true;
    for (const Job& job : jobs) {
        char name[16] = "testvector00";
        name[10] = static_cast<char>('0' + (job.vector + 1) / 10), name[11] = static_cast<char>('0' + (job.vector + 1) % 10);
        line.put("opus_vector name="), line.put(name);
        line.field("channels", job.channels), line.field("packets", job.packets), line.field("lost", job.lost);
        line.field("range_errors", job.range_errors), line.field("decode_errors", job.decode_errors);
        line.field("samples", job.samples), tenths(line, "q_dec", job.q[0]), tenths(line, "q_mdec", job.q[1]);
        line.digest("pcm", job.digest);
        line.field("bit_exact", kPinned[job.vector][job.channels - 1] == job.digest ? 1 : 0);
        line.field("pass", job.ok ? 1 : 0);
        line.emit();
        ok = ok && job.ok;
    }
    line.put("opus_conformance result="), line.put(ok ? "pass" : "fail"), line.emit();
    return ok;
}
} // namespace
} // namespace maicord

extern "C" void maicord_opus_conformance_entry() noexcept { ExitProcess(maicord::run() ? 0 : 1); }
