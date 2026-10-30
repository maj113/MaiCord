// Opus voice codec CPU benchmark and bit-exactness matrix (tools/benchmark_opus.ps1).
// Timing: the production wrappers encode and decode synthetic 48 kHz speech,
// tones and noise; lines report the minimum per-frame thread cycles and wall
// time over several passes, plus the packet mode mix. Digests (MAICORD_OPUS_BENCH=digests):
// FNV-1a over the packets and decoded PCM (with losses, concealment and FEC) of a
// wide setting matrix and of streams that switch settings every few frames, so
// codec changes can be checked bit-exact against a previous build.
#include "benchmark_line.hpp"
#include "fixtures/opus_voice_signal.hpp"
#include "maicord/media/opus_decoder.hpp"
#include "maicord/media/opus_encoder.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u32 kFrame = kOpusEncoderFrameSamples, kSeconds = 10, kFrames = kSeconds * 50;
constexpr u32 kMaximumPacket = kOpusEncoderMaximumPacketBytes;

u64 fnv(u64 hash, const void* data, usize bytes) noexcept {
    const auto* p = static_cast<const u8*>(data);
    for (usize i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}
constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;

enum Signal : u32 { speech, tones, speech_silence, noise, chord, loud };
void fill(i16* pcm, u32 frames, u32 channels, u32 signal, u32 seed) noexcept {
    OpusVoiceSignal state;
    opus_voice_signal_initialize(&state);
    state.noise ^= seed * 0x9e3779b9u;
    u32 a = seed * 977u, b = seed * 131u, c = 0;
    for (u32 i = 0; i < frames * kFrame; ++i) {
        i32 left = 0, right = 0;
        switch (signal) {
        case tones:
            left = opus_voice_tone_sample(&state, &a, 1000, 10000);
            right = opus_voice_tone_sample(&state, &b, 660, 8000);
            break;
        case noise:
            left = opus_voice_noise(&state) >> 2, right = opus_voice_noise(&state) >> 2;
            break;
        case chord:
            left = opus_voice_tone_sample(&state, &a, 262, 6000) + opus_voice_tone_sample(&state, &b, 330, 5000) +
                   opus_voice_tone_sample(&state, &c, 392, 4000);
            right = left / 2 + (opus_voice_noise(&state) >> 6);
            break;
        default: {
            left = opus_voice_speech_sample(&state);
            if (signal == speech_silence && i >= frames * kFrame / 2) left = 0;
            if (signal == loud) left = left * 5 > 32767 ? 32767 : left * 5 < -32768 ? -32768 : left * 5;
            right = left * 3 / 4 + opus_voice_tone_sample(&state, &b, 220, 2000);
        }
        }
        left = left > 32767 ? 32767 : left < -32768 ? -32768 : left;
        right = right > 32767 ? 32767 : right < -32768 ? -32768 : right;
        if (channels == 1) pcm[i] = static_cast<i16>(left);
        else pcm[2 * i] = static_cast<i16>(left), pcm[2 * i + 1] = static_cast<i16>(right);
    }
}

struct Settings {
    u32 channels, bitrate, complexity, fec, loss, constrained;
};
bool configure(OpusEncoder* encoder, const Settings& s) noexcept {
    return opus_encoder_bitrate(encoder, s.bitrate).ok() && opus_encoder_complexity(encoder, s.complexity).ok() &&
           opus_encoder_inband_fec(encoder, s.fec != 0).ok() && opus_encoder_packet_loss(encoder, s.loss).ok() &&
           opus_encoder_vbr_constraint(encoder, s.constrained != 0).ok();
}

struct Worker {
    u8 encoder_storage[32768]; // First members of page-aligned VirtualAlloc storage.
    u8 decoder_storage[32768];
    OpusScratch scratch;
    i16 input[2 * kFrame * kFrames];
    i16 output[2 * kFrame];
    u8 packets[kFrames][kMaximumPacket];
    u32 sizes[kFrames];
};

// Decodes packets[0..frames) losing every `loss_period`-th packet; a loss is
// recovered from the next packet's FEC when it carries LBRR, else concealed.
bool decode_digest(Worker* w, u32 channels, u32 frames, u32 loss_period, u64* digest) noexcept {
    OpusDecoder decoder{};
    const u32 bytes = opus_decoder_storage_bytes(channels);
    if (!opus_decoder_initialize(&decoder, {w->decoder_storage, bytes}, channels).ok()) return false;
    u64 hash = kFnvBasis;
    for (u32 f = 0; f < frames; ++f) {
        OpusDecodeResult r;
        if (loss_period && f % loss_period == loss_period - 1 && f + 1 < frames) {
            const ConstSlice<u8> next{w->packets[f + 1], w->sizes[f + 1]};
            r = opus_packet_has_fec(next) ? opus_decoder_decode_fec(&decoder, next, kFrame, {w->output, kFrame * channels}, &w->scratch)
                                          : opus_decoder_conceal(&decoder, kFrame, {w->output, kFrame * channels}, &w->scratch);
        } else {
            r = opus_decoder_decode(&decoder, {w->packets[f], w->sizes[f]}, {w->output, kFrame * channels}, &w->scratch);
        }
        if (!r.ok() || r.frames != kFrame) return false;
        hash = fnv(hash, w->output, kFrame * channels * sizeof(i16));
    }
    opus_decoder_release(&decoder);
    *digest = hash;
    return true;
}

// One matrix row: encode with fixed settings, then decode with and without
// losses, and through a decoder with the other channel count.
struct Row {
    Settings settings;
    u32 signal, frames;
    u64 packets, pcm, lossy, crossed;
    bool ok;
};
void run_row(Worker* w, Row* row) noexcept {
    const Settings& s = row->settings;
    fill(w->input, row->frames, s.channels, row->signal, s.bitrate + s.complexity);
    OpusEncoder encoder{};
    row->ok = opus_encoder_initialize(&encoder, {w->encoder_storage, sizeof(w->encoder_storage)}, s.channels).ok() &&
              configure(&encoder, s);
    u64 hash = kFnvBasis;
    for (u32 f = 0; row->ok && f < row->frames; ++f) {
        const auto r = opus_encoder_encode(&encoder, {w->input + f * kFrame * s.channels, kFrame * s.channels},
                                           {w->packets[f], kMaximumPacket}, &w->scratch);
        row->ok = r.ok();
        w->sizes[f] = r.bytes;
        hash = fnv(fnv(hash, &r.bytes, 2), w->packets[f], r.bytes);
    }
    opus_encoder_release(&encoder);
    row->packets = hash;
    row->ok = row->ok && decode_digest(w, s.channels, row->frames, 0, &row->pcm) &&
              decode_digest(w, s.channels, row->frames, 7, &row->lossy) &&
              decode_digest(w, 3 - s.channels, row->frames, 5, &row->crossed);
}

// Streams whose settings change every few frames: bitrate (mode and bandwidth
// switches with redundancy frames), complexity, FEC and VBR constraint.
void run_switching(Worker* w, Row* row, u32 seed) noexcept {
    const Settings& base = row->settings;
    fill(w->input, row->frames, base.channels, row->signal, seed);
    static constexpr u32 rates[]{6000, 8000, 12000, 16000, 24000, 32000, 48000, 64000, 96000, 128000, 256000, 510000};
    OpusEncoder encoder{};
    row->ok = opus_encoder_initialize(&encoder, {w->encoder_storage, sizeof(w->encoder_storage)}, base.channels).ok() &&
              configure(&encoder, base);
    u32 random = seed * 2654435761u + 1;
    u64 hash = kFnvBasis;
    for (u32 f = 0; row->ok && f < row->frames; ++f) {
        if (f % 6 == 5) {
            random = random * 1664525u + 1013904223u;
            const u32 r = random >> 8;
            Settings s{base.channels, rates[r % 12], (r >> 4) % 11, (r >> 8) & 1, (r >> 9) % 40, (r >> 16) & 1};
            row->ok = configure(&encoder, s);
        }
        const auto r = opus_encoder_encode(&encoder, {w->input + f * kFrame * base.channels, kFrame * base.channels},
                                           {w->packets[f], kMaximumPacket}, &w->scratch);
        row->ok = row->ok && r.ok();
        w->sizes[f] = r.bytes;
        hash = fnv(fnv(hash, &r.bytes, 2), w->packets[f], r.bytes);
    }
    opus_encoder_release(&encoder);
    row->packets = hash;
    row->ok = row->ok && decode_digest(w, base.channels, row->frames, 0, &row->pcm) &&
              decode_digest(w, base.channels, row->frames, 4, &row->lossy) &&
              decode_digest(w, 3 - base.channels, row->frames, 9, &row->crossed);
}

constexpr u32 kRates[]{6000, 9000, 12000, 16000, 20000, 24000, 32000, 40000, 48000,
                       64000, 80000, 96000, 128000, 192000, 256000, 384000, 510000};
constexpr u32 kComplexities[]{0, 2, 4, 6, 7, 8, 10};
constexpr u32 kFecLoss[][2]{{0, 0}, {1, 5}, {1, 25}};
constexpr u32 kConstraint[]{1, 0};
constexpr u32 kMatrixRows = 2 * 17 * 7 * 3 * 2, kSwitchingRows = 48, kMatrixFrames = 40, kSwitchingFrames = 300;
Row rows[kMatrixRows + kSwitchingRows];
Worker* workers[16];
volatile LONG next_row = 0;

DWORD WINAPI digest_thread(void* parameter) noexcept {
    Worker* w = static_cast<Worker*>(parameter);
    for (;;) {
        const LONG index = InterlockedIncrement(&next_row) - 1;
        if (index >= static_cast<LONG>(kMatrixRows + kSwitchingRows)) return 0;
        if (index < static_cast<LONG>(kMatrixRows)) run_row(w, &rows[index]);
        else run_switching(w, &rows[index], static_cast<u32>(index));
    }
}

bool digests() noexcept {
    u32 index = 0;
    for (u32 channels = 1; channels <= 2; ++channels)
        for (u32 rate : kRates)
            for (u32 complexity : kComplexities)
                for (const auto& fec : kFecLoss)
                    for (u32 constrained : kConstraint) {
                        Row& row = rows[index];
                        row.settings = {channels, rate, complexity, fec[0], fec[1], constrained};
                        row.signal = index % 6, row.frames = kMatrixFrames;
                        ++index;
                    }
    for (u32 i = 0; i < kSwitchingRows; ++i) {
        Row& row = rows[index++];
        row.settings = {1 + (i & 1), 32000, 7, 1, 10, 1};
        row.signal = (i >> 1) % 6, row.frames = kSwitchingFrames;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const u32 count = info.dwNumberOfProcessors < 16 ? info.dwNumberOfProcessors : 16;
    HANDLE threads[16];
    for (u32 t = 0; t < count; ++t) {
        workers[t] = static_cast<Worker*>(VirtualAlloc(nullptr, sizeof(Worker), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!workers[t]) return false;
        threads[t] = CreateThread(nullptr, 1u << 20, digest_thread, workers[t], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!threads[t]) return false;
    }
    WaitForMultipleObjects(count, threads, TRUE, INFINITE);
    // One line per channel count and bitrate (all complexities, FEC and VBR
    // constraint settings), then the switching streams and an overall digest.
    u64 total = kFnvBasis;
    bool ok = true;
    Line line;
    for (u32 group = 0; group < 2 * 17; ++group) {
        u64 packets = kFnvBasis, pcm = kFnvBasis;
        for (u32 r = group * 42; r < group * 42 + 42; ++r) {
            ok = ok && rows[r].ok;
            packets = fnv(packets, &rows[r].packets, 8);
            pcm = fnv(fnv(fnv(pcm, &rows[r].pcm, 8), &rows[r].lossy, 8), &rows[r].crossed, 8);
        }
        total = fnv(fnv(total, &packets, 8), &pcm, 8);
        line.put("opus_digest kind=matrix");
        line.field("channels", rows[group * 42].settings.channels);
        line.field("bitrate", rows[group * 42].settings.bitrate);
        line.digest("packets", packets), line.digest("pcm", pcm);
        line.emit();
    }
    u64 packets = kFnvBasis, pcm = kFnvBasis;
    for (u32 r = kMatrixRows; r < kMatrixRows + kSwitchingRows; ++r) {
        ok = ok && rows[r].ok;
        packets = fnv(packets, &rows[r].packets, 8);
        pcm = fnv(fnv(fnv(pcm, &rows[r].pcm, 8), &rows[r].lossy, 8), &rows[r].crossed, 8);
    }
    total = fnv(fnv(total, &packets, 8), &pcm, 8);
    line.put("opus_digest kind=switching"), line.digest("packets", packets), line.digest("pcm", pcm), line.emit();
    line.put("opus_digest kind=total"), line.digest("all", total), line.field("ok", ok ? 1 : 0), line.emit();
    return ok;
}

// Timing. Minimum over passes of thread cycles and wall time per 20 ms frame.
u64 thread_cycles() noexcept {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}
struct Timing {
    u64 cycles = ~0ull, nanoseconds = ~0ull;
};
LARGE_INTEGER frequency;
void sample(Timing* t, u64 cycles, i64 begin, i64 end, u32 frames) noexcept {
    const u64 c = cycles / frames, ns = static_cast<u64>(end - begin) * 1000000000ull / frequency.QuadPart / frames;
    if (c < t->cycles) t->cycles = c;
    if (ns < t->nanoseconds) t->nanoseconds = ns;
}
i64 now() noexcept {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
void modes(Line* line, const Worker* w, u32 frames) noexcept {
    u32 silk = 0, hybrid = 0, celt = 0;
    for (u32 f = 0; f < frames; ++f) {
        const u32 config = w->packets[f][0] >> 3;
        if (w->sizes[f] <= 2) continue;
        config < 12 ? ++silk : config < 16 ? ++hybrid : ++celt;
    }
    line->field("silk", silk), line->field("hybrid", hybrid), line->field("celt", celt);
}

struct Case {
    const char* name;
    Settings settings;
    u32 signal, loss_period; // loss_period: decode-side concealment every n-th packet
};
// FEC with a loss hint keeps the encoder in SILK or hybrid mode, so the CELT
// cases turn it off.
constexpr Case kCases[]{
    {"silk_mono_12k", {1, 12000, 7, 1, 10, 1}, speech, 0},
    {"hybrid_mono_32k", {1, 32000, 7, 1, 10, 1}, speech, 0},
    {"default_mono_64k", {1, 64000, 7, 1, 10, 1}, speech, 0},
    {"hybrid_stereo_64k", {2, 64000, 7, 1, 10, 1}, speech, 0},
    {"celt_mono_96k", {1, 96000, 7, 0, 0, 1}, speech, 0},
    {"celt_stereo_128k", {2, 128000, 7, 0, 0, 1}, chord, 0},
    {"silk_mono_12k_c10", {1, 12000, 10, 1, 10, 1}, speech, 0},
    {"silk_plc_12k", {1, 12000, 7, 1, 10, 1}, speech, 5},
    {"celt_plc_96k", {1, 96000, 7, 0, 0, 1}, speech, 5},
};
constexpr u32 kPasses = 7;

bool timing() noexcept {
    Worker* w = static_cast<Worker*>(VirtualAlloc(nullptr, sizeof(Worker), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!w) return false;
    QueryPerformanceFrequency(&frequency);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << (info.dwNumberOfProcessors - 1));
    Line line;
    for (const Case& c : kCases) {
        const Settings& s = c.settings;
        fill(w->input, kFrames, s.channels, c.signal, 1);
        Timing encode, decode;
        u64 packets = 0, pcm = 0;
        for (u32 pass = 0; pass < kPasses; ++pass) {
            OpusEncoder encoder{};
            if (!opus_encoder_initialize(&encoder, {w->encoder_storage, sizeof(w->encoder_storage)}, s.channels).ok() ||
                !configure(&encoder, s))
                return false;
            u64 hash = kFnvBasis;
            u64 cycles = thread_cycles();
            i64 begin = now();
            for (u32 f = 0; f < kFrames; ++f) {
                const auto r = opus_encoder_encode(&encoder, {w->input + f * kFrame * s.channels, kFrame * s.channels},
                                                   {w->packets[f], kMaximumPacket}, &w->scratch);
                if (!r.ok()) return false;
                w->sizes[f] = r.bytes;
            }
            sample(&encode, thread_cycles() - cycles, begin, now(), kFrames);
            opus_encoder_release(&encoder);
            for (u32 f = 0; f < kFrames; ++f) hash = fnv(fnv(hash, &w->sizes[f], 2), w->packets[f], w->sizes[f]);
            packets = hash;

            OpusDecoder decoder{};
            if (!opus_decoder_initialize(&decoder, {w->decoder_storage, opus_decoder_storage_bytes(s.channels)}, s.channels).ok())
                return false;
            hash = kFnvBasis;
            cycles = thread_cycles();
            begin = now();
            for (u32 f = 0; f < kFrames; ++f) {
                const bool lost = c.loss_period && f % c.loss_period == c.loss_period - 1;
                const auto r = lost ? opus_decoder_conceal(&decoder, kFrame, {w->output, kFrame * s.channels}, &w->scratch)
                                    : opus_decoder_decode(&decoder, {w->packets[f], w->sizes[f]}, {w->output, kFrame * s.channels},
                                                          &w->scratch);
                if (!r.ok()) return false;
                if (pass == 0) hash = fnv(hash, w->output, kFrame * s.channels * sizeof(i16));
            }
            sample(&decode, thread_cycles() - cycles, begin, now(), kFrames);
            opus_decoder_release(&decoder);
            if (pass == 0) pcm = hash;
        }
        line.put("opus_timing case="), line.put(c.name);
        line.field("encode_cycles", encode.cycles), line.field("encode_ns", encode.nanoseconds);
        line.field("decode_cycles", decode.cycles), line.field("decode_ns", decode.nanoseconds);
        modes(&line, w, kFrames);
        line.digest("packets", packets), line.digest("pcm", pcm);
        line.emit();
    }
    return true;
}

bool run() noexcept {
    wchar_t mode[16]{};
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_OPUS_BENCH", mode, 16);
    if (length == 7 && mode[0] == L'd') return digests();
    return timing();
}
} // namespace
} // namespace maicord

extern "C" void maicord_opus_benchmark_entry() noexcept { ExitProcess(maicord::run() ? 0 : 1); }
