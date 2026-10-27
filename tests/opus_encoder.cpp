#include "test_output.hpp"
#include "maicord/media/opus_encoder.hpp"
#include "maicord/media/opus_decoder.hpp"
#include "fixtures/opus_voice_signal.hpp"
#include "maicord/core/win32_kernel.hpp"
#include <opus.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u32 kFrame = kOpusEncoderFrameSamples, kMaximumFrames = 50;
struct Scenario {
    u32 channels, bitrate, complexity, fec, loss, dtx, signal, frames, bitrate2, complexity2;
    u64 digest; // Unmodified upstream 1.6.1 (its own CMake, fixed point, float API off; SIMD and scalar agree).
};
// signal: 0 speech, 1 tones, 2 speech then digital silence. Half-way changes apply bitrate2/complexity2.
constexpr Scenario kScenarios[]{
    {1, 24000, 7, 1, 10, 0, 0, 50, 40000, 3, 0xc42d07a88db11b9eull},
    {2, 64000, 10, 1, 5, 0, 0, 25, 0, 0, 0x7d2175e8c525c94full},
    {1, 16000, 2, 0, 0, 1, 2, 50, 0, 0, 0x8bf420d665ef7901ull},
    {1, 96000, 5, 0, 0, 0, 1, 25, 0, 0, 0x9cc72fb99d689a29ull},
    {2, 128000, 8, 1, 20, 1, 1, 25, 0, 0, 0x9ccfd7e830b5ebfbull},
};
alignas(16) u8 encoder_storage[32768], decoder_storage[32768], saved[32768];
i16 input[2 * kFrame * kMaximumFrames], output[2 * kFrame * kMaximumFrames + 2];
u8 packets[kMaximumFrames][kOpusEncoderMaximumPacketBytes + 1];
u32 sizes[kMaximumFrames];
opus_uint32 ranges[kMaximumFrames]; // Encoder final range per packet.

void fill(const Scenario& s) noexcept {
    OpusVoiceSignal signal;
    opus_voice_signal_initialize(&signal);
    u32 left_phase = 0, right_phase = 0;
    for (u32 i = 0; i < s.frames * kFrame; ++i) {
        i16 left, right;
        if (s.signal == 1) {
            left = opus_voice_tone_sample(&signal, &left_phase, 1000, 10000);
            right = opus_voice_tone_sample(&signal, &right_phase, 660, 8000);
        } else {
            left = opus_voice_speech_sample(&signal);
            if (s.signal == 2 && i >= s.frames * kFrame / 2) left = 0;
            right = static_cast<i16>(left * 3 / 4 + opus_voice_tone_sample(&signal, &right_phase, 220, 2000));
        }
        if (s.channels == 1) input[i] = left;
        else input[2 * i] = left, input[2 * i + 1] = right;
    }
}
bool configure(OpusEncoder* encoder, const Scenario& s) noexcept {
    return opus_encoder_bitrate(encoder, s.bitrate).ok() && opus_encoder_complexity(encoder, s.complexity).ok() &&
           opus_encoder_inband_fec(encoder, s.fec != 0).ok() && opus_encoder_packet_loss(encoder, s.loss).ok() &&
           ::opus_encoder_ctl(reinterpret_cast<::OpusEncoder*>(encoder_storage), OPUS_SET_DTX(s.dtx != 0)) == OPUS_OK;
}
// FNV-1a over little-endian 16-bit lengths and packet bytes.
bool encode_all(OpusEncoder* encoder, const Scenario& s, u64* digest) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    for (u32 f = 0; f < s.frames; ++f) {
        if (f == s.frames / 2 && s.bitrate2 &&
            (!opus_encoder_bitrate(encoder, s.bitrate2).ok() || !opus_encoder_complexity(encoder, s.complexity2).ok()))
            return false;
        const auto encoded = opus_encoder_encode(encoder, {input + f * kFrame * s.channels, kFrame * s.channels},
                                                 {packets[f], kOpusEncoderMaximumPacketBytes});
        if (!encoded.ok()) return false;
        sizes[f] = encoded.bytes;
        ::opus_encoder_ctl(reinterpret_cast<::OpusEncoder*>(encoder->state), OPUS_GET_FINAL_RANGE(&ranges[f]));
        const u8 length[2]{static_cast<u8>(encoded.bytes), static_cast<u8>(encoded.bytes >> 8)};
        for (u8 byte : length) hash = (hash ^ byte) * 0x100000001b3ull;
        for (u32 i = 0; i < encoded.bytes; ++i) hash = (hash ^ packets[f][i]) * 0x100000001b3ull;
    }
    *digest = hash;
    return true;
}
bool decoder_open(OpusDecoder* decoder, u32 channels) noexcept {
    *decoder = {};
    const u32 bytes = opus_decoder_storage_bytes(channels);
    return bytes <= sizeof(decoder_storage) && opus_decoder_initialize(decoder, {decoder_storage, bytes}, channels).ok();
}
// Every packet decodes 20 ms and leaves the decoder's range coder state equal
// to the encoder's (the RFC 6716 conformance check on the bitstream).
bool decode_all(u32 channels, u32 frames) noexcept {
    OpusDecoder decoder;
    if (!decoder_open(&decoder, channels)) return false;
    for (u32 f = 0; f < frames; ++f) {
        const auto decoded = opus_decoder_decode(&decoder, {packets[f], sizes[f]}, {output + f * kFrame * channels, kFrame * channels});
        opus_uint32 range = 0;
        ::opus_decoder_ctl(reinterpret_cast<::OpusDecoder*>(decoder_storage), OPUS_GET_FINAL_RANGE(&range));
        if (!decoded.ok() || decoded.frames != kFrame || range != ranges[f]) return false;
    }
    opus_decoder_release(&decoder);
    return true;
}
double decibels(double ratio) noexcept { // 10 log10(ratio), ratio > 0
    i32 exponent = 0;
    while (ratio >= 2) ratio *= 0.5, ++exponent;
    while (ratio < 1) ratio *= 2, --exponent;
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
// Best-aligned SNR over frames [skip, frames - 1) against the codec's ~312-sample delay.
struct Quality { double snr, segmental; u32 lag; };
Quality quality(u32 channels, u32 frames, u32 skip) noexcept {
    Quality best{-1000, 0, 0};
    const u32 first = skip * kFrame, last = (frames - 1) * kFrame;
    for (u32 lag = 280; lag <= 344; ++lag) {
        const double snr = energy_ratio_db(input + first * channels, output + (first + lag) * channels, (last - first) * channels);
        if (snr > best.snr) best = {snr, 0, lag};
    }
    double sum = 0;
    u32 segments = 0;
    for (u32 f = skip; f + 1 < frames; ++f) {
        const double value = energy_ratio_db(input + f * kFrame * channels, output + (f * kFrame + best.lag) * channels, kFrame * channels);
        sum += value < -10 ? -10 : value > 35 ? 35 : value;
        ++segments;
    }
    best.segmental = segments ? sum / segments : 0;
    return best;
}
constexpr Scenario kStackScenario{2, 24000, 10, 1, 20, 0, 0, 8, 128000, 10, 0}; // SILK/hybrid, then CELT
DWORD WINAPI stack_worker(void* parameter) noexcept {
    u64 digest = 0;
    auto* encoder = static_cast<OpusEncoder*>(parameter);
    return configure(encoder, kStackScenario) && encode_all(encoder, kStackScenario, &digest) ? 0 : 1;
}
} // namespace

void test_opus_encoder(Check check) noexcept {
    check(!opus_encoder_storage_bytes(0) && !opus_encoder_storage_bytes(3) && opus_encoder_storage_bytes(1) &&
              opus_encoder_storage_bytes(2) > opus_encoder_storage_bytes(1) && opus_encoder_storage_bytes(2) <= sizeof(encoder_storage),
          "Opus encoder storage: native mono/stereo sizes fit caller storage; other channel counts refused");
    OpusEncoder absent{};
    check(opus_encoder_encode(&absent, {input, kFrame}, {packets[0], kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_state &&
              opus_encoder_bitrate(&absent, 64000).error == OpusEncodeError::invalid_state &&
              opus_encoder_bitrate(nullptr, 1).error == OpusEncodeError::invalid_state &&
              opus_encoder_initialize(nullptr, {encoder_storage, sizeof(encoder_storage)}, 1).error == OpusEncodeError::invalid_input,
          "Opus encoder lifecycle: inactive and null callers never reach the native encoder");
    OpusEncoder encoder{};
    const u32 mono = opus_encoder_storage_bytes(1);
    check(opus_encoder_initialize(&encoder, {encoder_storage, mono - 1}, 1).error == OpusEncodeError::capacity &&
              opus_encoder_initialize(&encoder, {encoder_storage + 1, mono}, 1).error == OpusEncodeError::invalid_input &&
              opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, 3).error == OpusEncodeError::invalid_input &&
              !encoder.initialized && !encoder.state,
          "Opus encoder admission: short, misaligned or unsupported storage leaves the handle inactive");
    check(opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, 1).ok() && encoder.state == encoder_storage &&
              encoder.state_bytes == mono && encoder.channels == 1 &&
              opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, 1).error == OpusEncodeError::busy,
          "Opus encoder lifecycle: caller storage binds once at 48 kHz VOIP");
    auto* native = reinterpret_cast<::OpusEncoder*>(encoder_storage);
    opus_int32 bitrate = 0, complexity = 0, fec = 0, loss = 0, dtx = 1, vbr = 0, application = 0;
    opus_uint32 range = 1;
    check(::opus_encoder_ctl(native, OPUS_GET_BITRATE(&bitrate)) == OPUS_OK && bitrate == static_cast<opus_int32>(kOpusEncoderDefaultBitrate) &&
              ::opus_encoder_ctl(native, OPUS_GET_COMPLEXITY(&complexity)) == OPUS_OK && complexity == static_cast<opus_int32>(kOpusEncoderDefaultComplexity) &&
              ::opus_encoder_ctl(native, OPUS_GET_INBAND_FEC(&fec)) == OPUS_OK && fec == 1 &&
              ::opus_encoder_ctl(native, OPUS_GET_PACKET_LOSS_PERC(&loss)) == OPUS_OK && loss == static_cast<opus_int32>(kOpusEncoderDefaultPacketLossPercent) &&
              ::opus_encoder_ctl(native, OPUS_GET_DTX(&dtx)) == OPUS_OK && dtx == 0 &&
              ::opus_encoder_ctl(native, OPUS_GET_FINAL_RANGE(&range)) == OPUS_OK && range == 0,
          "Opus encoder defaults: 64 kbit/s, measured complexity, FEC with a loss hint, DTX off");
    check(::opus_encoder_ctl(native, OPUS_GET_VBR(&vbr)) == OPUS_UNIMPLEMENTED &&
              ::opus_encoder_ctl(native, OPUS_GET_APPLICATION(&application)) == OPUS_UNIMPLEMENTED &&
              ::opus_encoder_ctl(native, OPUS_SET_VBR(0)) == OPUS_UNIMPLEMENTED &&
              ::opus_encoder_ctl(native, OPUS_SET_INBAND_FEC(2)) == OPUS_BAD_ARG &&
              ::opus_encode(native, input, static_cast<int>(kFrame / 2), packets[0], kOpusEncoderMaximumPacketBytes) == OPUS_BAD_ARG &&
              ::opus_encoder_init(reinterpret_cast<::OpusEncoder*>(saved), 24000, 1, OPUS_APPLICATION_VOIP) == OPUS_BAD_ARG &&
              ::opus_encoder_init(reinterpret_cast<::OpusEncoder*>(saved), 48000, 1, OPUS_APPLICATION_AUDIO) == OPUS_BAD_ARG,
          "Opus encoder profile: 48 kHz VOIP, VBR and 20 ms frames are fixed; other settings are refused");
    copy_bytes(saved, encoder_storage, mono);
    check(opus_encoder_bitrate(&encoder, kOpusEncoderMinimumBitrate - 1).error == OpusEncodeError::invalid_input &&
              opus_encoder_bitrate(&encoder, kOpusEncoderMaximumBitrate + 1).error == OpusEncodeError::invalid_input &&
              opus_encoder_complexity(&encoder, 11).error == OpusEncodeError::invalid_input &&
              opus_encoder_packet_loss(&encoder, 101).error == OpusEncodeError::invalid_input &&
              bytes_equal(saved, encoder_storage, mono),
          "Opus encoder settings: out-of-range bitrate, complexity and loss are refused without state change");
    for (u32 i = 0; i < kFrame * 2; ++i) input[i] = static_cast<i16>(i * 37);
    for (auto& byte : packets[0]) byte = 0xa5;
    check(opus_encoder_encode(&encoder, {input, kFrame - 1}, {packets[0], kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_input &&
              opus_encoder_encode(&encoder, {input, kFrame * 2}, {packets[0], kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_input &&
              opus_encoder_encode(&encoder, {nullptr, kFrame}, {packets[0], kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_input &&
              opus_encoder_encode(&encoder, {input, kFrame}, {packets[0], kOpusEncoderMaximumPacketBytes - 1}).error == OpusEncodeError::capacity &&
              opus_encoder_encode(&encoder, {input, kFrame}, {reinterpret_cast<u8*>(input) + 2, kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_input &&
              opus_encoder_encode(&encoder, {reinterpret_cast<const i16*>(encoder_storage), kFrame}, {packets[0], kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_input &&
              opus_encoder_encode(&encoder, {input, kFrame}, {encoder_storage + 64, kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_input &&
              bytes_equal(saved, encoder_storage, mono) && packets[0][0] == 0xa5,
          "Opus encoder preflight: frame size, capacity and aliasing are refused before native encoding");
    const auto first = opus_encoder_encode(&encoder, {input, kFrame}, {packets[0], kOpusEncoderMaximumPacketBytes + 1});
    check(first.ok() && first.bytes > 2 && first.bytes <= kOpusEncoderMaximumPacketBytes &&
              packets[0][kOpusEncoderMaximumPacketBytes] == 0xa5,
          "Opus encoder bound: larger packet buffers are capped at the single-frame maximum");
    opus_encoder_release(&encoder);

    // Bit-exact verification against unmodified upstream, then decodability and DTX of the same packets.
    for (const Scenario& s : kScenarios) {
        fill(s);
        u64 digest = 0;
        encoder = {};
        check(opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, s.channels).ok() &&
                  configure(&encoder, s) && encode_all(&encoder, s, &digest) && digest == s.digest,
              "Opus encoder upstream identity: MaiCord profile packets equal unmodified libopus 1.6.1 packets");
        check(decode_all(s.channels, s.frames), "Opus encoder bitstream: every upstream-identical packet decodes 20 ms");
        if (s.dtx && s.signal == 2) {
            u32 silent = 0;
            for (u32 f = s.frames / 2 + 12; f < s.frames; ++f) silent += sizes[f] <= 2;
            check(silent >= s.frames / 2 - 14, "Opus encoder DTX: digital silence settles to header-only packets");
        }
        opus_encoder_release(&encoder);
    }

    // Decodability and rate control across the supported bitrate range.
    const u32 rates[]{kOpusEncoderMinimumBitrate, 12000, 24000, 40000, 64000, 128000, kOpusEncoderMaximumBitrate};
    for (u32 channels = 1; channels <= 2; ++channels) {
        Scenario s{channels, 0, kOpusEncoderDefaultComplexity, 1, 10, 0, 0, 12, 0, 0, 0};
        fill(s);
        for (u32 rate : rates) {
            s.bitrate = rate;
            u64 digest = 0;
            encoder = {};
            bool ok = opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, channels).ok() &&
                      configure(&encoder, s) && encode_all(&encoder, s, &digest) && decode_all(channels, s.frames);
            u32 total = 0;
            for (u32 f = 2; f < s.frames; ++f) total += sizes[f];
            const u32 average = total * 8 * 50 / (s.frames - 2);
            check(ok && average <= rate + rate / 4 + 4000 && average >= (rate < 128000 ? rate / 3 : 24000),
                  "Opus encoder rates: 6-510 kbit/s mono/stereo packets decode and track the requested bitrate");
            opus_encoder_release(&encoder);
        }
    }

    // Waveform fidelity through the MaiCord decoder; measured 12.3/9.8, 12.0/10.8 and 26.5 dB.
    struct Fidelity { Scenario scenario; double snr, segmental; const char* message; };
    const Fidelity fidelity[]{
        {{1, 32000, kOpusEncoderDefaultComplexity, 1, 10, 0, 0, 50, 0, 0, 0}, 9, 7,
         "Opus encoder fidelity: mono speech at 32 kbit/s keeps SNR and segmental SNR"},
        {{2, 64000, kOpusEncoderDefaultComplexity, 1, 10, 0, 0, 25, 0, 0, 0}, 9, 8,
         "Opus encoder fidelity: stereo speech at 64 kbit/s keeps SNR and segmental SNR"},
        {{1, 64000, kOpusEncoderDefaultComplexity, 0, 0, 0, 1, 25, 0, 0, 0}, 22, 22,
         "Opus encoder fidelity: a 1 kHz tone at 64 kbit/s keeps SNR"},
    };
    for (const Fidelity& item : fidelity) {
        const Scenario& s = item.scenario;
        fill(s);
        u64 digest = 0;
        encoder = {};
        const bool ok = opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, s.channels).ok() &&
                        configure(&encoder, s) && encode_all(&encoder, s, &digest) && decode_all(s.channels, s.frames);
        const Quality measured = ok ? quality(s.channels, s.frames, 5) : Quality{};
        check(ok && measured.snr >= item.snr && measured.segmental >= item.segmental, item.message);
        opus_encoder_release(&encoder);
    }

    // In-band FEC: the next packet's LBRR copy restores a lost voiced frame (measured 18.6 dB, PLC -2.3 dB).
    {
        const Scenario s{1, 32000, kOpusEncoderDefaultComplexity, 1, 20, 0, 0, 24, 0, 0, 0};
        fill(s);
        u64 digest = 0;
        encoder = {};
        bool ok = opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, 1).ok() &&
                  configure(&encoder, s) && encode_all(&encoder, s, &digest) && decode_all(1, s.frames);
        u32 lbrr = 0;
        for (u32 f = 2; f < s.frames; ++f) lbrr += ::opus_packet_has_lbrr(packets[f], static_cast<opus_int32>(sizes[f])) == 1;
        constexpr u32 lost = 20;
        i16 prefix[kFrame], recovered[kFrame], concealed[kFrame];
        for (u32 pass = 0; ok && pass < 2; ++pass) {
            OpusDecoder decoder;
            ok = decoder_open(&decoder, 1);
            for (u32 f = 0; ok && f < lost; ++f)
                ok = opus_decoder_decode(&decoder, {packets[f], sizes[f]}, {prefix, kFrame}).ok();
            auto* state = reinterpret_cast<::OpusDecoder*>(decoder_storage);
            i16* target = pass ? concealed : recovered;
            const int frames = pass ? ::opus_decode(state, nullptr, 0, target, static_cast<int>(kFrame), 0)
                                    : ::opus_decode(state, packets[lost + 1], static_cast<opus_int32>(sizes[lost + 1]), target, static_cast<int>(kFrame), 1);
            ok = ok && frames == static_cast<int>(kFrame);
            opus_decoder_release(&decoder);
        }
        const double fec_db = energy_ratio_db(output + lost * kFrame, recovered, kFrame);
        const double plc_db = energy_ratio_db(output + lost * kFrame, concealed, kFrame);
        check(ok && lbrr >= s.frames - 4 && fec_db >= 10 && fec_db >= plc_db + 10,
              "Opus encoder FEC: LBRR in every warm packet recovers a lost frame above PLC");
        opus_encoder_release(&encoder);
    }

    {
        encoder = {};
        const bool ok = opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, 2).ok();
        copy_bytes(saved, encoder_storage, encoder.state_bytes);
        const u32 bytes = encoder.state_bytes;
        opus_encoder_release(&encoder);
        check(ok && !encoder.state && !encoder.initialized && bytes_equal(saved, encoder_storage, bytes) &&
                  opus_encoder_encode(&encoder, {input, kFrame * 2}, {packets[0], kOpusEncoderMaximumPacketBytes}).error == OpusEncodeError::invalid_state,
              "Opus encoder release: detach clears only wrapper metadata");
    }

    // The documented stack reserve covers the heaviest stereo complexity-10 paths.
    {
        fill(kStackScenario);
        encoder = {};
        DWORD code = 1;
        HANDLE thread = opus_encoder_initialize(&encoder, {encoder_storage, sizeof(encoder_storage)}, 2).ok()
                            ? CreateThread(nullptr, kOpusEncoderStackReserve, stack_worker, &encoder, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)
                            : nullptr;
        if (thread) {
            WaitForSingleObject(thread, INFINITE);
            GetExitCodeThread(thread, &code);
            CloseHandle(thread);
        }
        check(code == 0, "Opus encoder stack: stereo complexity 10 encodes within kOpusEncoderStackReserve");
        opus_encoder_release(&encoder);
    }
    opus_encoder_release(nullptr);
}
} // namespace maicord

namespace maicord {
namespace opus_encoder_entry {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_opus_encoder_tests_entry() noexcept {
    maicord::test_opus_encoder(maicord::opus_encoder_entry::check);
    ExitProcess(maicord::opus_encoder_entry::failures ? 1 : 0);
}
