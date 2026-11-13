#include "../../support/test_output.hpp"
#include "../../fixtures/opus_voice_signal.hpp"
#include "maicord/media/audio/opus_decoder.hpp"
#include "maicord/media/audio/opus_encoder.hpp"
#include "maicord/os/thread.hpp"

// The encoder through the decoder: every packet must decode to 20 ms and leave the decoder's range coder
// where the encoder's ended (the format's own check that both read the same symbols), rates must follow the
// profile, the decoded audio must resemble the input, and the packets are pinned, so that both systems code
// the same bytes.
namespace maicord {
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
constexpr u32 kFrame = kOpusEncoderFrameSamples, kMaximumFrames = 50;
// signal: 0 speech, 1 tones, 2 speech then digital silence, 3 two high tones mostly on the left, 4 digital
// silence.
struct Scenario {
    u32 channels, bitrate, complexity, loss, signal, frames;
    // FNV-1a of the packets (with their lengths), and of the range coder's final state after each packet.
    u64 digest, ranges;
};
constexpr Scenario kScenarios[]{
    {2, 64000, 10, 5, 0, 25, 0xaa7f46872d3cd10full, 0x69c2e66ca33e629eull},
    {1, 16000, 2, 0, 2, 50, 0xd77c8a441ace0d71ull, 0x56bc051107bc56a3ull},
    {2, 128000, 8, 20, 1, 25, 0x2830c100c03f1c8dull, 0x0d7bff35e0445af6ull},
    {1, 40000, 3, 10, 0, 50, 0x9150c4b20dc8dda8ull, 0xa7d9764d1e6d7407ull},
    {1, 64000, 7, 10, 0, 50, 0xca0f880d5f0f1906ull, 0xedf1e9bd5015c561ull},  // voice send
    {2, 128000, 7, 10, 0, 50, 0x652b46c7735fba7bull, 0x53b49e0720130aaaull}, // Go Live
    {1, 32000, 10, 0, 0, 50, 0x7fd619d625705ca9ull, 0x080bcf2c0850cf55ull},  // voice message
    {2, 32000, 10, 0, 1, 25, 0x4af09f3a0a5f9cceull, 0xc29702e5b253e991ull},
};
OpusEncoder encoder;
OpusDecoder decoder;
i16 input[2 * kFrame * kMaximumFrames], output[2 * kFrame * kMaximumFrames + 2];
u8 packets[kMaximumFrames][kOpusEncoderMaximumPacketBytes + 1];
u32 sizes[kMaximumFrames];

void fill(const Scenario& s) noexcept {
    OpusVoiceSignal signal;
    opus_voice_signal_initialize(&signal);
    u32 left_phase = 0, right_phase = 0;
    for (u32 i = 0; i < s.frames * kFrame; ++i) {
        i16 left = 0, right = 0;
        if (s.signal == 1) {
            left = opus_voice_tone_sample(&signal, &left_phase, 1000, 10000);
            right = opus_voice_tone_sample(&signal, &right_phase, 660, 8000);
        } else if (s.signal == 3) {
            left = static_cast<i16>(opus_voice_tone_sample(&signal, &left_phase, 10000, 6000) +
                                    opus_voice_tone_sample(&signal, &right_phase, 13000, 4000));
            right = static_cast<i16>(left * 3 / 10);
        } else if (s.signal != 4) {
            left = opus_voice_speech_sample(&signal);
            if (s.signal == 2 && i >= s.frames * kFrame / 2)
                left = 0;
            right =
                static_cast<i16>(left * 3 / 4 + opus_voice_tone_sample(&signal, &right_phase, 220, 2000));
        }
        if (s.channels == 1)
            input[i] = left;
        else
            input[2 * i] = left, input[2 * i + 1] = right;
    }
}
OpusEncoderProfile profile_of(const Scenario& s) noexcept {
    OpusEncoderProfile profile;
    profile.bitrate = s.bitrate;
    profile.channels = static_cast<u8>(s.channels);
    profile.complexity = static_cast<u8>(s.complexity);
    profile.loss_percent = static_cast<u8>(s.loss);
    return profile;
}
u64 hash_bytes(u64 hash, const u8* bytes, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}
u64 hash_value(u64 hash, u32 value, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i)
        hash = (hash ^ ((value >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    return hash;
}
// Encodes the scenario from `input`. False when a packet leaves its bounds: 2 to 1275 bytes behind a hybrid
// full-band 20 ms table-of-contents byte, and nothing written past them.
bool encode_all(const OpusEncoderProfile& profile, u32 frames, u64* digest,
                u64* ranges = nullptr) noexcept {
    const u32 channels = profile.channels == 2 ? 2 : 1;
    u64 hash = 0xcbf29ce484222325ull, range_hash = hash;
    opus_encoder_start(&encoder, profile);
    for (u32 f = 0; f < frames; ++f) {
        packets[f][kOpusEncoderMaximumPacketBytes] = 0xa5;
        const u32 bytes = opus_encode(&encoder, input + f * kFrame * channels, packets[f]);
        if (bytes < 2 || bytes > kOpusEncoderMaximumPacketBytes ||
            packets[f][kOpusEncoderMaximumPacketBytes] != 0xa5 ||
            (packets[f][0] != 0x78 && (channels != 2 || packets[f][0] != 0x7c)))
            return false;
        sizes[f] = bytes;
        hash = hash_bytes(hash_value(hash, bytes, 2), packets[f], bytes);
        range_hash = hash_value(range_hash, opus_encoder_final_range(&encoder), 4);
    }
    *digest = hash;
    if (ranges)
        *ranges = range_hash;
    return true;
}
// Every packet decodes 20 ms into `output`; `ranges` hashes the decoder's final range after each packet.
bool decode_all(u32 channels, u32 frames, u64* ranges = nullptr) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    opus_decoder_start(&decoder, channels);
    for (u32 f = 0; f < frames; ++f) {
        if (opus_decode(&decoder, {packets[f], sizes[f]},
                        {output + f * kFrame * channels, kFrame * channels}) != kFrame)
            return false;
        hash = hash_value(hash, opus_decoder_final_range(&decoder), 4);
    }
    if (ranges)
        *ranges = hash;
    return true;
}
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
double energy_ratio_db(const i16* reference, const i16* test, u32 samples) noexcept {
    double signal = 0, noise = 0;
    for (u32 i = 0; i < samples; ++i) {
        const double a = reference[i], d = a - test[i];
        signal += a * a, noise += d * d;
    }
    return decibels((signal + 1) / (noise + 1));
}
// Best-aligned SNR over frames [skip, frames - 1) around the codec's delay of kOpusEncoderLookahead
// samples.
struct Quality {
    double snr, segmental;
    u32 lag;
};
Quality quality(u32 channels, u32 frames, u32 skip) noexcept {
    Quality best{-1000, 0, 0};
    const u32 first = skip * kFrame, last = (frames - 1) * kFrame;
    for (u32 lag = kOpusEncoderLookahead - 32; lag <= kOpusEncoderLookahead + 32; ++lag) {
        const double snr = energy_ratio_db(input + first * channels, output + (first + lag) * channels,
                                           (last - first) * channels);
        if (snr > best.snr)
            best = {snr, 0, lag};
    }
    double sum = 0;
    u32 segments = 0;
    for (u32 f = skip; f + 1 < frames; ++f) {
        const double value = energy_ratio_db(
            input + f * kFrame * channels, output + (f * kFrame + best.lag) * channels, kFrame * channels);
        sum += value < -10 ? -10 : value > 35 ? 35 : value;
        ++segments;
    }
    best.segmental = segments ? sum / segments : 0;
    return best;
}
void write_hex(u64 value) noexcept {
    char text[19]{'0', 'x'};
    for (u32 i = 0; i < 16; ++i)
        text[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    test_write_text(text);
}

// The heaviest paths of both directions on the smallest stack a thread of either client can have: stereo at
// the highest complexity with redundant frames, first at a rate that codes mono, then at the highest; every
// packet decoded, and a loss both recovered from the next packet and concealed. `passed` is a bool the
// worker sets.
u32 small_stack_worker(void* passed) noexcept {
    static OpusDecoder lossy;
    static i16 pcm[2 * kFrame];
    const Scenario stack_scenarios[]{{2, 24000, 10, 20, 0, 8, 0, 0}, {2, 510000, 10, 20, 0, 8, 0, 0}};
    for (const Scenario& s : stack_scenarios) {
        u64 digest = 0;
        if (!encode_all(profile_of(s), s.frames, &digest) || !decode_all(2, s.frames))
            return 0;
        opus_decoder_start(&lossy, 2);
        for (u32 f = 0; f + 2 < s.frames; f += 2) {
            if (opus_decode(&lossy, {packets[f], sizes[f]}, {pcm, 2 * kFrame}) != kFrame)
                return 0;
            // Frame f + 1 is lost: from frame f + 2's redundant copy when it has one, else concealed.
            const ConstSlice<u8> next =
                (f & 2) ? ConstSlice<u8>{packets[f + 2], sizes[f + 2]} : ConstSlice<u8>{};
            if (opus_decode_lost(&lossy, next, kFrame, {pcm, 2 * kFrame}) != kFrame)
                return 0;
        }
    }
    *static_cast<bool*>(passed) = true;
    return 0;
}

void test_opus_encoder() noexcept {
    // Profile values outside their ranges act as the nearest valid ones.
    {
        const Scenario s{1, 64000, 7, 10, 0, 6, 0, 0};
        fill(s);
        const auto same = [&](OpusEncoderProfile odd, OpusEncoderProfile valid) noexcept {
            u64 a = 0, b = 1;
            return encode_all(odd, s.frames, &a) && encode_all(valid, s.frames, &b) && a == b;
        };
        OpusEncoderProfile low{1, 3, 0, 0}, low_valid{6000, 1, 1, 0};
        OpusEncoderProfile high{4000000000u, 1, 200, 255}, high_valid{510000, 1, 10, 100};
        u64 implicit = 0, written = 1;
        check(same(low, low_valid) && same(high, high_valid),
              "Opus encoder profile: bitrate, complexity, loss and channel count outside their ranges are "
              "clamped");
        check(
            encode_all({}, s.frames, &implicit) && encode_all(profile_of(s), s.frames, &written) &&
                implicit == written,
            "Opus encoder profile: the defaults are mono, 64 kbit/s, complexity 7 and 10 % expected loss");
    }

    // Pinned packets; the decoder ends every one of them where the encoder did.
    for (const Scenario& s : kScenarios) {
        fill(s);
        u64 digest = 0, ranges = 0, decoded_ranges = 1;
        const bool encoded = encode_all(profile_of(s), s.frames, &digest, &ranges);
        const bool decoded = encoded && decode_all(s.channels, s.frames, &decoded_ranges);
        check(decoded && ranges == decoded_ranges,
              "Opus encoder bitstream: every packet decodes 20 ms to the encoder's final range");
        if (digest != s.digest || ranges != s.ranges) {
            test_write_text("    {"), write_hex(digest), test_write_text("ull, "), write_hex(ranges),
                test_write_line("ull}");
        }
        check(encoded && digest == s.digest && ranges == s.ranges,
              "Opus encoder pinned output: the packets equal the pinned digests");
    }

    // Decodability and rate control across the supported bitrate range.
    const u32 rates[]{6000, 12000, 24000, 40000, 64000, 128000, 510000};
    for (u32 channels = 1; channels <= 2; ++channels) {
        Scenario s{channels, 0, 7, 10, 0, 12, 0, 0};
        fill(s);
        for (u32 rate : rates) {
            s.bitrate = rate;
            u64 digest = 0;
            const bool ok = encode_all(profile_of(s), s.frames, &digest) && decode_all(channels, s.frames);
            u32 total = 0;
            for (u32 f = 2; f < s.frames; ++f)
                total += sizes[f];
            const u32 average = total * 8 * 50 / (s.frames - 2);
            check(ok && average <= rate + rate / 4 + 4000 && average >= (rate < 128000 ? rate / 3 : 24000),
                  "Opus encoder rates: 6-510 kbit/s mono and stereo packets decode and follow the "
                  "requested bitrate");
        }
    }

    // Waveform fidelity through the decoder.
    struct Fidelity {
        Scenario scenario;
        double snr, segmental;
        const char* message;
    };
    const Fidelity fidelity[]{
        {{1, 32000, 7, 10, 0, 50, 0, 0},
         9,
         7,
         "Opus encoder fidelity: mono speech at 32 kbit/s keeps SNR and segmental SNR"},
        {{2, 64000, 7, 10, 0, 25, 0, 0},
         9,
         8,
         "Opus encoder fidelity: stereo speech at 64 kbit/s keeps SNR and segmental SNR"},
        {{1, 64000, 7, 0, 1, 25, 0, 0},
         22,
         22,
         "Opus encoder fidelity: a 1 kHz tone at 64 kbit/s keeps SNR"},
    };
    for (const Fidelity& item : fidelity) {
        const Scenario& s = item.scenario;
        fill(s);
        u64 digest = 0;
        const bool ok = encode_all(profile_of(s), s.frames, &digest) && decode_all(s.channels, s.frames);
        const Quality measured = ok ? quality(s.channels, s.frames, 5) : Quality{};
        check(ok && measured.snr >= item.snr && measured.segmental >= item.segmental, item.message);
    }

    // The stereo image above 8 kHz, where the two channels are coded as mid and side: tones 10 dB louder on
    // the left must come out louder on the left by as much (a side with the wrong sign would swap the
    // channels).
    {
        const Scenario s{2, 256000, 7, 10, 3, 25, 0, 0};
        fill(s);
        u64 digest = 0;
        const bool ok = encode_all(profile_of(s), s.frames, &digest) && decode_all(2, s.frames);
        double left = 1, right = 1, left_in = 1, right_in = 1;
        for (u32 i = 5 * kFrame; i < (s.frames - 1) * kFrame; ++i) {
            left += static_cast<double>(output[2 * i]) * output[2 * i],
                right += static_cast<double>(output[2 * i + 1]) * output[2 * i + 1];
            left_in += static_cast<double>(input[2 * i]) * input[2 * i],
                right_in += static_cast<double>(input[2 * i + 1]) * input[2 * i + 1];
        }
        const double balance = decibels(left / right), wanted = decibels(left_in / right_in),
                     level = decibels(left / left_in);
        check(ok && packets[10][0] == 0x7c && balance >= wanted - 2 && balance <= wanted + 2 &&
                  level >= -3 && level <= 3,
              "Opus encoder stereo: high tones keep their side and their level");
    }

    // Digital silence takes a fraction of the rate (there is no discontinuous transmission) and decodes to
    // silence.
    {
        const Scenario s{1, 64000, 7, 10, 4, 25, 0, 0};
        fill(s);
        u64 digest = 0;
        const bool ok = encode_all(profile_of(s), s.frames, &digest) && decode_all(1, s.frames);
        u32 largest = 0;
        i32 peak = 0;
        for (u32 f = 5; f < s.frames; ++f)
            largest = sizes[f] > largest ? sizes[f] : largest;
        for (u32 i = 5 * kFrame; i < s.frames * kFrame; ++i)
            peak = output[i] > peak ? output[i] : -output[i] > peak ? -output[i] : peak;
        check(ok && largest <= 64 && peak <= 8,
              "Opus encoder silence: silent input makes small packets that decode to silence");
    }

    // In-band FEC: the next packet's redundant copy restores a lost voiced frame far better than
    // concealment.
    {
        const Scenario s{1, 32000, 7, 20, 0, 24, 0, 0};
        fill(s);
        u64 digest = 0;
        bool ok = encode_all(profile_of(s), s.frames, &digest) && decode_all(1, s.frames);
        u32 redundant = 0;
        for (u32 f = 2; f < s.frames; ++f)
            redundant += opus_packet_has_fec({packets[f], sizes[f]});
        constexpr u32 lost = 20;
        static i16 prefix[kFrame], recovered[kFrame], concealed[kFrame];
        for (u32 pass = 0; ok && pass < 2; ++pass) {
            static OpusDecoder lossy;
            opus_decoder_start(&lossy, 1);
            for (u32 f = 0; ok && f < lost; ++f)
                ok = opus_decode(&lossy, {packets[f], sizes[f]}, {prefix, kFrame}) == kFrame;
            const ConstSlice<u8> next =
                pass ? ConstSlice<u8>{} : ConstSlice<u8>{packets[lost + 1], sizes[lost + 1]};
            ok = ok &&
                 opus_decode_lost(&lossy, next, kFrame, {pass ? concealed : recovered, kFrame}) == kFrame;
        }
        const double fec_db = energy_ratio_db(output + lost * kFrame, recovered, kFrame);
        const double plc_db = energy_ratio_db(output + lost * kFrame, concealed, kFrame);
        check(ok && redundant >= s.frames - 4 && fec_db >= 10 && fec_db >= plc_db + 10,
              "Opus encoder FEC: every warm packet carries a redundant copy that recovers a lost frame "
              "well above concealment");
        OpusEncoderProfile no_loss = profile_of(s);
        no_loss.loss_percent = 0;
        redundant = 0;
        ok = encode_all(no_loss, s.frames, &digest);
        for (u32 f = 0; f < s.frames; ++f)
            redundant += opus_packet_has_fec({packets[f], sizes[f]});
        check(ok && !redundant, "Opus encoder FEC: no redundant copies when no loss is expected");
    }

    // Neither direction needs caller scratch: the state is the struct and the rest fits a small thread's
    // stack.
    {
        bool passed = false;
        Thread* const thread = thread_start(small_stack_worker, &passed, 64u * 1024u, nullptr);
        if (thread) {
            thread_wait(thread, INFINITE);
            thread_close(thread);
        }
        check(passed, "Opus stack: stereo complexity 10 encoding, decoding, FEC recovery and concealment "
                      "run on a 64 KB thread stack");
    }
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_opus_encoder_tests_entry) {
    maicord::test_opus_encoder();
    ::maicord::test_process_exit(maicord::failures ? 1 : 0);
}
