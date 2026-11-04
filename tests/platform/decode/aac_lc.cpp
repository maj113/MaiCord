// The in-house AAC-LC decoder (src/media/audio/aac_lc.cpp) and the decoder interface over it on Linux
// (src/platform/decode/linux/aac_decoder.cpp).
//
//   conformance   each fixture of tools/generate_aac_fixtures.py (libavcodec's `aac` encoder at several bitrates, mono
//                 and stereo, 22.05, 44.1 and 48 kHz, short blocks, M/S, intensity stereo, noise substitution, TNS,
//                 escape codes) decoded through the decoder interface and compared with libavcodec's own decoder:
//                 largest error at most 0.55 of a PCM16 step and mean square error at most 0.1 steps squared
//                 (rounding to 16 bits alone gives 1/12), which is a PSNR of about 100 dB at full scale
//   containers    the same packets through the MP4 demuxer and through ADTS framing
//   pulse data    a frame built here, with a pulse, against the IMDCT written out as the standard's formula
//   lifecycle     the interface's metadata, drain, flush and error rules (the Windows test's, without Media Foundation)
//   robustness    corrupted and truncated frames: no crash, an error value or finite output, and a decoder that
//                 recovers: the frame after a rejected one decodes exactly as if it had been dropped
#include "../../fixtures/aac/aac_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/audio/aac_lc.hpp"
#include "maicord/media/containers/mp4_audio_demux.hpp"
#include "maicord/media/containers/mpeg_ts_codec.hpp"
#include "maicord/platform/decode/aac_decoder.hpp"

namespace maicord {
namespace tables {
#include "../../../src/media/audio/aac_tables.inc"
} // namespace tables

namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void metric(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}

// ---- Fixtures ----------------------------------------------------------------------------------------------------

u32 load32(const u8* at) noexcept {
    return at[0] | (u32{at[1]} << 8) | (u32{at[2]} << 16) | (u32{at[3]} << 24);
}

constexpr u32 kMaximumFrames = 8;

struct Fixture {
    const char* name;
    u32 rate, channels, asc_bytes, frames;
    const u8* asc;
    const u8* frame[kMaximumFrames];
    u32 size[kMaximumFrames];
    const u8* oracle; // frames * 1024 * channels little endian int16
};

bool parse(const char* name, const u8* data, usize bytes, Fixture* out) noexcept {
    if (bytes < 20 || load32(data) != 0x4341414du)
        return false;
    *out = {};
    out->name = name;
    out->rate = load32(data + 4);
    out->channels = load32(data + 8);
    out->asc_bytes = load32(data + 12);
    out->frames = load32(data + 16);
    if (out->frames > kMaximumFrames || out->asc_bytes > 16)
        return false;
    usize at = 20;
    out->asc = data + at;
    at += (out->asc_bytes + 3) & ~3u;
    for (u32 i = 0; i < out->frames; ++i) {
        out->size[i] = load32(data + at);
        out->frame[i] = data + at + 4;
        at += 4 + ((out->size[i] + 3) & ~3u);
    }
    out->oracle = data + at;
    return at + usize{out->frames} * 1024 * out->channels * 2 == bytes;
}

float oracle_sample(const Fixture& f, u32 index) noexcept {
    const u32 raw = f.oracle[index * 2] | (u32{f.oracle[index * 2 + 1]} << 8);
    return static_cast<float>(static_cast<i16>(static_cast<u16>(raw)));
}

struct Difference {
    float maximum;
    double square_sum;
    u32 count;
};

// Accumulates the difference of `samples` decoded float samples (one frame, channels interleaved) from the reference.
void compare(const Fixture& f, u32 frame, const float* pcm, Difference* diff) noexcept {
    const u32 samples = 1024 * f.channels;
    for (u32 i = 0; i < samples; ++i) {
        const float error = pcm[i] * 32768.0f - oracle_sample(f, frame * samples + i);
        const float magnitude = error < 0 ? -error : error;
        if (magnitude > diff->maximum)
            diff->maximum = magnitude;
        diff->square_sum += static_cast<double>(error) * error;
        ++diff->count;
    }
}

bool within_bounds(const Difference& d) noexcept {
    return d.count && d.maximum <= 0.55f && d.square_sum / d.count <= 0.1;
}

void* aligned_state(usize* bytes) noexcept {
    *bytes = aac_lc_state_bytes();
    return pages_allocate(*bytes);
}

alignas(64) float g_pcm[kAacDecoderMaximumOutputBytes / 4];

// A fixture decoded through the decoder interface.
bool decode_through_interface(const Fixture& f, Difference* diff, bool check_timestamps) noexcept {
    AacDecoder decoder{};
    if (!aac_decoder_initialize(&decoder, {f.asc, f.asc_bytes}, f.rate, f.channels).ok())
        return false;
    bool ok = true;
    for (u32 i = 0; i < f.frames && ok; ++i) {
        const i64 pts = static_cast<i64>(i) * 1024 * 10'000'000 / f.rate;
        ok = aac_decoder_submit(&decoder, {f.frame[i], f.size[i]}, pts, 1024ull * 10'000'000 / f.rate).ok();
        const auto block = ok ? aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)})
                              : AacDecodedBlock{};
        ok = ok && block.ok() && block.frames == 1024 && block.bytes == 1024 * f.channels * 4 &&
             (!check_timestamps || (block.timestamp_valid && block.presentation_time_100ns == pts));
        if (ok)
            compare(f, i, g_pcm, diff);
    }
    ok = ok && aac_decoder_drain(&decoder).ok() &&
         aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}).error ==
             AacDecodeError::end_of_stream &&
         aac_decoder_release(&decoder).ok() && !decoder.runtime;
    return ok;
}

AacLcStats decode_statistics(const Fixture& f) noexcept {
    usize bytes = 0;
    void* memory = aligned_state(&bytes);
    AacLc* decoder = memory ? aac_lc_initialize(memory, f.rate, f.channels) : nullptr;
    AacLcStats stats{};
    if (decoder) {
        for (u32 i = 0; i < f.frames; ++i)
            aac_lc_decode(decoder, {f.frame[i], f.size[i]}, g_pcm);
        stats = aac_lc_stats(decoder);
    }
    if (memory)
        pages_release(memory, bytes);
    return stats;
}

void print_stats(const char* name, const AacLcStats& s) noexcept {
    char line[160];
    u32 used = 0;
    auto put = [&](const char* text) {
        for (; *text && used + 1 < sizeof(line); ++text)
            line[used++] = *text;
    };
    auto number = [&](u32 value) {
        char digits[12];
        u32 n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (n)
            line[used++] = digits[--n];
    };
    put("  ");
    put(name);
    put(": long ");
    number(s.long_frames);
    put(" start ");
    number(s.start_frames);
    put(" short ");
    number(s.short_frames);
    put(" stop ");
    number(s.stop_frames);
    put(" kbd ");
    number(s.kbd_windows);
    put(" ms ");
    number(s.mid_side_bands);
    put(" is ");
    number(s.intensity_bands);
    put(" pns ");
    number(s.noise_bands);
    put(" tns ");
    number(s.tns_filters);
    put(" esc ");
    number(s.escapes);
    put(" cpe ");
    number(s.pair_elements);
    line[used++] = '\n';
    test_write_bytes(line, used);
}

Fixture g_fixtures[8];
u32 g_fixture_count;

void conformance() noexcept {
    struct Entry {
        const char* name;
        const u8* data;
        usize bytes;
    };
    const Entry entries[]{
        {"stereo44_128k", kAacStereo44, sizeof(kAacStereo44)},
        {"mono44_64k", kAacMono44, sizeof(kAacMono44)},
        {"stereo48_64k", kAacStereo48, sizeof(kAacStereo48)},
        {"mono48_24k_noise", kAacMono48Noise, sizeof(kAacMono48Noise)},
        {"stereo44_128k_transient", kAacStereo44Transient, sizeof(kAacStereo44Transient)},
        {"stereo44_40k_joint", kAacStereo44Joint, sizeof(kAacStereo44Joint)},
        {"stereo44_256k_loud", kAacStereo44Loud, sizeof(kAacStereo44Loud)},
        {"mono22_32k", kAacMono22, sizeof(kAacMono22)},
    };
    for (const auto& entry : entries) {
        Fixture& f = g_fixtures[g_fixture_count];
        if (!parse(entry.name, entry.data, entry.bytes, &f)) {
            check(false, "AAC fixture: the file parses");
            continue;
        }
        ++g_fixture_count;
        Difference diff{};
        const bool ok = decode_through_interface(f, &diff, true);
        char label[96];
        u32 used = 0;
        for (const char* p = "AAC conformance: "; *p; ++p)
            label[used++] = *p;
        for (const char* p = entry.name; *p && used + 40 < sizeof(label); ++p)
            label[used++] = *p;
        for (const char* p = " within half a PCM16 step of the reference"; *p; ++p)
            label[used++] = *p;
        label[used] = 0;
        check(ok && within_bounds(diff), label);
        metric("  max error x1000 of a step: ", static_cast<u64>(diff.maximum * 1000.0f));
        metric("  mean square error x1e6: ", static_cast<u64>(diff.square_sum / (diff.count ? diff.count : 1) * 1e6));
        print_stats(entry.name, decode_statistics(f));
    }
    const Fixture& transient = g_fixtures[4];
    const AacLcStats stats = decode_statistics(transient);
    check(stats.short_frames >= 1 && stats.start_frames >= 1 && stats.stop_frames >= 1,
          "AAC coverage: the transient fixture has start, eight-short and stop blocks");
    const AacLcStats joint = decode_statistics(g_fixtures[5]);
    check(joint.noise_bands > 0 || g_fixtures[3].channels == 1, "AAC coverage: noise substitution appears");
    check(decode_statistics(g_fixtures[6]).escapes > 0, "AAC coverage: the loud fixture needs escape codes");
}

// ---- Containers --------------------------------------------------------------------------------------------------

Mp4AudioDemuxer g_demuxer;

void containers() noexcept {
    const Fixture& f = g_fixtures[0];
    Difference diff{};
    check(Mp4DemuxError::none == mp4_audio_demux_initialize(&g_demuxer, {kAacStereo44Mp4, sizeof(kAacStereo44Mp4)}) &&
              g_demuxer.sample_rate == 44100 && g_demuxer.channels == 2 && g_demuxer.frames_per_packet == 1024,
          "AAC MP4: the demuxer reads the muxed track as AAC-LC, 44.1 kHz stereo, 1024-frame packets");
    const auto asc = mp4_audio_demux_asc(g_demuxer);
    check(asc.count == f.asc_bytes && bytes_equal(asc.data, f.asc, asc.count),
          "AAC MP4: the demuxed audio specific config is the encoder's");
    AacDecoder decoder{};
    u32 frames = 0;
    bool ok = aac_decoder_initialize(&decoder, asc, g_demuxer.sample_rate, g_demuxer.channels).ok();
    for (;; ++frames) {
        const auto unit = mp4_audio_demux_next(&g_demuxer);
        if (!unit.ok())
            break;
        ok = ok && frames < f.frames && unit.bytes.count == f.size[frames] &&
             bytes_equal(unit.bytes.data, f.frame[frames], unit.bytes.count) &&
             aac_decoder_submit(&decoder, unit.bytes, unit.presentation_time_us_signed * 10, 1).ok();
        const auto block = aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)});
        ok = ok && block.ok();
        if (ok)
            compare(f, frames, g_pcm, &diff);
    }
    aac_decoder_release(&decoder);
    check(ok && frames == f.frames && within_bounds(diff),
          "AAC MP4: every packet the demuxer yields decodes to the reference");

    // ADTS: frame after frame, the way HLS packed audio and transport streams carry it.
    Difference adts_diff{};
    u32 at = 0, adts_frames = 0;
    bool adts_ok = true;
    AacDecoder adts_decoder{};
    while (at < sizeof(kAacStereo44Adts) && adts_ok) {
        MpegTsAacFrame frame{};
        adts_ok = mpeg_ts_aac_frame({kAacStereo44Adts + at, static_cast<u32>(sizeof(kAacStereo44Adts)) - at}, &frame) ==
                      MpegTsError::none &&
                  adts_frames < f.frames && frame.payload.count == f.size[adts_frames] &&
                  bytes_equal(frame.payload.data, f.frame[adts_frames], frame.payload.count);
        if (adts_ok && !adts_decoder.initialized)
            adts_ok = bytes_equal(frame.configuration, f.asc, 2) &&
                      aac_decoder_initialize(&adts_decoder, {frame.configuration, 2}, frame.sample_rate, frame.channels).ok();
        adts_ok = adts_ok && aac_decoder_submit(&adts_decoder, frame.payload, adts_frames * 232199, 232199).ok();
        const auto block = adts_ok ? aac_decoder_read(&adts_decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)})
                                   : AacDecodedBlock{};
        adts_ok = adts_ok && block.ok();
        if (adts_ok)
            compare(f, adts_frames, g_pcm, &adts_diff);
        at += frame.bytes;
        ++adts_frames;
    }
    aac_decoder_release(&adts_decoder);
    check(adts_ok && adts_frames == f.frames && within_bounds(adts_diff),
          "AAC ADTS: every frame of the stream decodes to the reference");
}

// ---- Pulse data against the IMDCT of the standard ------------------------------------------------------------------

struct BitWriter {
    u8 data[64];
    u32 bits;
    void put(u32 value, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i, ++bits) {
            if (!(bits & 7))
                data[bits >> 3] = 0;
            data[bits >> 3] = static_cast<u8>(data[bits >> 3] | (((value >> (count - 1 - i)) & 1u) << (7 - (bits & 7))));
        }
    }
    u32 bytes() const noexcept {
        return (bits + 7) >> 3;
    }
};

double cosine(double x) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    while (x > kPi)
        x -= 2 * kPi;
    while (x < -kPi)
        x += 2 * kPi;
    // Taylor series around the nearest multiple of pi / 2 keeps it short and exact enough.
    double sign = 1.0;
    if (x > kPi / 2) {
        x = kPi - x;
        sign = -1.0;
    } else if (x < -kPi / 2) {
        x = -kPi - x;
        sign = -1.0;
    }
    double term = 1.0, sum = 1.0;
    for (u32 k = 1; k < 14; ++k) {
        term *= -x * x / ((2.0 * k - 1) * (2.0 * k));
        sum += term;
    }
    return sign * sum;
}

double sine(double x) noexcept {
    return cosine(x - 3.14159265358979323846 / 2);
}

double power_of_two(double exponent) noexcept {
    // 2^exponent for exponents that are multiples of 1/4 in a small range.
    double result = 1.0;
    i32 quarters = static_cast<i32>(exponent * 4 + (exponent >= 0 ? 0.5 : -0.5));
    const double root = 1.18920711500272106671; // 2^(1/4)
    for (; quarters > 0; --quarters)
        result *= root;
    for (; quarters < 0; ++quarters)
        result /= root;
    return result;
}

void pulse_data() noexcept {
    // One single-channel element, one long block, one band of codebook 1 holding the all-zero quad, global gain 110,
    // and a pulse of amplitude 5 at line 2: the spectrum is a single line, 5^(4/3) * 2^(10/4).
    BitWriter w{};
    w.put(0, 3);          // SCE
    w.put(0, 4);          // tag
    w.put(110, 8);        // global gain
    w.put(0, 1);          // reserved
    w.put(0, 2);          // only long
    w.put(0, 1);          // sine window
    w.put(1, 6);          // max_sfb
    w.put(0, 1);          // no predictor
    w.put(1, 4);          // codebook 1
    w.put(1, 5);          // section of one band
    w.put(tables::kAacScalefactorCodes[60], tables::kAacScalefactorBits[60]); // delta 0
    w.put(1, 1);          // pulse data
    w.put(0, 2);          // one pulse
    w.put(0, 6);          // from band 0
    w.put(2, 5);          // at line 2
    w.put(5, 4);          // amplitude 5
    w.put(0, 1);          // no TNS
    w.put(0, 1);          // no gain control
    w.put(tables::kAacSpectralCodes[40], tables::kAacSpectralBits[40]); // the quad (0, 0, 0, 0) of codebook 1
    w.put(7, 3);          // END
    usize bytes = 0;
    void* memory = aligned_state(&bytes);
    AacLc* decoder = memory ? aac_lc_initialize(memory, 44100, 1) : nullptr;
    float pcm[1024];
    check(decoder && aac_lc_decode(decoder, {w.data, w.bytes()}, pcm) == AacLcError::none &&
              aac_lc_stats(decoder).pulses == 1,
          "AAC pulse data: the frame with a pulse decodes");
    if (decoder) {
        constexpr double kPi = 3.14159265358979323846;
        // 5^(4/3): 5 * cbrt(5)
        const double magnitude = 5.0 * 1.7099759466766968 * power_of_two(2.5);
        double worst = 0, peak = 0;
        for (u32 n = 0; n < 1024; ++n) {
            const double kernel = cosine(kPi / 1024.0 * (n + 0.5 + 512.0) * 2.5);
            const double expected = magnitude * kernel / (1024.0 * 32768.0) * sine(kPi * (n + 0.5) / 2048.0);
            const double error = pcm[n] - expected;
            worst = maximum(worst, error < 0 ? -error : error);
            peak = maximum(peak, expected < 0 ? -expected : expected);
        }
        check(peak > 1e-7 && worst <= peak * 2e-4,
              "AAC pulse data: the output is the windowed IMDCT of the single dequantized line, as the standard's formula");
    }
    if (memory)
        pages_release(memory, bytes);
}

// ---- Lifecycle ---------------------------------------------------------------------------------------------------

void lifecycle() noexcept {
    const u8 stereo44[]{0x12, 0x10}, mono48[]{0x11, 0x88}, he44[]{0x2b, 0x92, 0x08, 0};
    AacDecoder decoder{};
    check(aac_decoder_initialize(&decoder, {stereo44, 2}, 44100, 2).ok() && decoder.initialized && !decoder.runtime,
          "AAC lifecycle: initialization is copied metadata with no decoder allocation");
    check(aac_decoder_initialize(&decoder, {stereo44, 2}, 44100, 2).error == AacDecodeError::busy &&
              aac_decoder_submit(&decoder, {}, 0, 1024).error == AacDecodeError::invalid_input &&
              aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}).error == AacDecodeError::need_input &&
              aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), 100}).error == AacDecodeError::capacity,
          "AAC lifecycle: busy initialization, an empty packet, a read with no input and a small buffer are refused");
    check(aac_decoder_drain(&decoder).ok() && decoder.ended && decoder.draining && aac_decoder_flush(&decoder).ok() &&
              !decoder.ended && !decoder.draining && aac_decoder_release(&decoder).ok() && !decoder.initialized,
          "AAC lifecycle: metadata drain, flush and release balance with nothing allocated");
    check(aac_decoder_initialize(&decoder, {he44, 4}, 44100, 2).error == AacDecodeError::unsupported &&
              !decoder.initialized && aac_decoder_initialize(&decoder, {stereo44, 2}, 48000, 2).error ==
                                          AacDecodeError::invalid_input &&
              aac_decoder_initialize(&decoder, {mono48, 2}, 48000, 1).ok() && aac_decoder_release(&decoder).ok(),
          "AAC lifecycle: HE-AAC is unsupported, a rate that does not match the config is invalid");

    // Submit and read one real frame: need_output until the block is read, the packet's timestamp comes back,
    // and a flush starts over.
    const Fixture& f = g_fixtures[1];
    check(aac_decoder_initialize(&decoder, {f.asc, f.asc_bytes}, f.rate, f.channels).ok(),
          "AAC lifecycle: a fixture's configuration initializes");
    check(aac_decoder_submit(&decoder, {f.frame[0], f.size[0]}, -5000, 1000).ok() && decoder.runtime &&
              aac_decoder_submit(&decoder, {f.frame[1], f.size[1]}, 0, 1000).error == AacDecodeError::need_output,
          "AAC lifecycle: a second packet waits for the first block to be read");
    const auto block = aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)});
    check(block.ok() && block.presentation_time_100ns == -5000 && block.timestamp_valid && block.frames == 1024 &&
              aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}).error ==
                  AacDecodeError::need_input,
          "AAC lifecycle: the block carries the packet's signed timestamp and is read once");
    check(aac_decoder_submit(&decoder, {f.frame[1], f.size[1]}, 10, 1000).ok() && aac_decoder_flush(&decoder).ok() &&
              aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}).error ==
                  AacDecodeError::need_input && decoder.accepted_frames == 0,
          "AAC lifecycle: flush drops the undelivered block and the counters");
    check(aac_decoder_submit(&decoder, {f.frame[0], f.size[0]}, 0, 1000).ok() && aac_decoder_drain(&decoder).ok() &&
              aac_decoder_submit(&decoder, {f.frame[1], f.size[1]}, 1, 1000).error == AacDecodeError::end_of_stream &&
              aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}).ok() &&
              aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}).error ==
                  AacDecodeError::end_of_stream && decoder.ended,
          "AAC lifecycle: drain delivers the block in flight, then ends the stream");
    check(aac_decoder_release(&decoder).ok() && !decoder.runtime && !decoder.initialized,
          "AAC lifecycle: release frees the decoder");
    u8 bad[]{0x12, 0x10};
    decoder = {};
    check(aac_decoder_initialize(&decoder, {f.asc, f.asc_bytes}, f.rate, f.channels).ok() &&
              aac_decoder_submit(&decoder, {bad, 2}, 0, 1000).error == AacDecodeError::input &&
              decoder.native_status != 0 &&
              aac_decoder_submit(&decoder, {f.frame[0], f.size[0]}, 0, 1000).ok() && aac_decoder_release(&decoder).ok(),
          "AAC lifecycle: a packet the decoder rejects is an input error and the next packet still decodes");
}

// ---- Robustness --------------------------------------------------------------------------------------------------

u32 g_random = 0x2545f491u;
u32 random() noexcept {
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return g_random;
}

bool finite_bounded(const float* pcm, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (!(pcm[i] >= -1024.0f && pcm[i] <= 1024.0f))
            return false;
    return true;
}

alignas(64) u8 g_scratch[8192];

void robustness() noexcept {
    // The frame after a rejected one decodes as if the rejected one had been dropped.
    {
        const Fixture& f = g_fixtures[0];
        usize bytes = 0;
        void* memory = aligned_state(&bytes);
        AacLc* decoder = memory ? aac_lc_initialize(memory, f.rate, f.channels) : nullptr;
        Difference diff{};
        bool ok = decoder != nullptr, rejected = false;
        for (u32 i = 0; i < f.frames && ok; ++i) {
            if (i == 2) {
                const u32 half = f.size[i] / 2;
                rejected = aac_lc_decode(decoder, {f.frame[i], half}, g_pcm) != AacLcError::none;
            }
            ok = aac_lc_decode(decoder, {f.frame[i], f.size[i]}, g_pcm) == AacLcError::none;
            if (ok)
                compare(f, i, g_pcm, &diff);
        }
        check(ok && rejected && within_bounds(diff),
              "AAC robustness: a truncated frame is rejected and leaves the decoder as it was");
        if (memory)
            pages_release(memory, bytes);
    }
    // Mutations of every fixture frame, and noise: no crash, an error or finite output.
    u32 decoded = 0, rejected = 0, bad_output = 0, runs = 0;
    for (u32 fixture = 0; fixture < g_fixture_count; ++fixture) {
        const Fixture& f = g_fixtures[fixture];
        usize bytes = 0;
        void* memory = aligned_state(&bytes);
        AacLc* decoder = memory ? aac_lc_initialize(memory, f.rate, f.channels) : nullptr;
        if (!decoder) {
            check(false, "AAC robustness: a decoder for the fixture");
            continue;
        }
        for (u32 round = 0; round < 40; ++round)
            for (u32 i = 0; i < f.frames; ++i) {
                u32 size = f.size[i];
                copy_bytes(g_scratch, f.frame[i], size);
                switch (random() % 5) {
                case 0: // flip a few bits
                    for (u32 k = 0, n = 1 + random() % 8; k < n; ++k)
                        g_scratch[random() % size] ^= static_cast<u8>(1u << (random() % 8));
                    break;
                case 1: // overwrite a run
                    for (u32 k = 0, at = random() % size, n = 1 + random() % 24; k < n && at + k < size; ++k)
                        g_scratch[at + k] = static_cast<u8>(random());
                    break;
                case 2: // truncate
                    size = 1 + random() % size;
                    break;
                case 3: // the header bytes only, mutated
                    g_scratch[random() % (size < 8 ? size : 8)] = static_cast<u8>(random());
                    break;
                default: // noise of the same length
                    for (u32 k = 0; k < size; ++k)
                        g_scratch[k] = static_cast<u8>(random());
                    break;
                }
                // Poison the output: a failure must not write it.
                for (u32 k = 0; k < 1024 * f.channels; ++k)
                    g_pcm[k] = 12345.0f;
                const AacLcError error = aac_lc_decode(decoder, {g_scratch, size}, g_pcm);
                ++runs;
                if (error == AacLcError::none) {
                    ++decoded;
                    bad_output += !finite_bounded(g_pcm, 1024 * f.channels);
                } else {
                    ++rejected;
                    bad_output += g_pcm[0] != 12345.0f || g_pcm[1024 * f.channels - 1] != 12345.0f;
                }
            }
        pages_release(memory, bytes);
    }
    metric("  corrupted frames decoded: ", decoded);
    metric("  corrupted frames rejected: ", rejected);
    check(runs && !bad_output, "AAC robustness: corrupted frames give an error with no output, or finite bounded output");
    check(rejected > 0, "AAC robustness: corruption is detected");
    // Noise at every length, and the degenerate frames.
    usize bytes = 0;
    void* memory = aligned_state(&bytes);
    AacLc* decoder = memory ? aac_lc_initialize(memory, 44100, 2) : nullptr;
    u32 noise_bad = 0;
    for (u32 length = 1; decoder && length <= 700; ++length) {
        for (u32 k = 0; k < length; ++k)
            g_scratch[k] = static_cast<u8>(random());
        const AacLcError error = aac_lc_decode(decoder, {g_scratch, length}, g_pcm);
        noise_bad += error == AacLcError::none && !finite_bounded(g_pcm, 2048);
    }
    u8 zeros[64]{};
    check(decoder && !noise_bad && aac_lc_decode(decoder, {zeros, 64}, g_pcm) != AacLcError::none &&
              aac_lc_decode(decoder, {}, g_pcm) == AacLcError::invalid_input &&
              aac_lc_decode(decoder, {g_scratch, 9000}, g_pcm) == AacLcError::invalid_input,
          "AAC robustness: noise of every length, a zero frame, an empty frame and an oversized frame");
    check(!aac_lc_initialize(nullptr, 44100, 2) && !aac_lc_initialize(g_scratch + 1, 44100, 2) &&
              !(memory && aac_lc_initialize(memory, 11111, 2)) && !(memory && aac_lc_initialize(memory, 44100, 3)) &&
              !(memory && aac_lc_initialize(memory, 44100, 0)),
          "AAC robustness: unsupported or misplaced decoder configurations are refused");
    if (memory)
        pages_release(memory, bytes);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_aac_lc_entry) {
    maicord::conformance();
    maicord::containers();
    maicord::pulse_data();
    maicord::lifecycle();
    maicord::robustness();
    maicord::test_exit(maicord::g_failures, "maicord_aac_lc");
}
