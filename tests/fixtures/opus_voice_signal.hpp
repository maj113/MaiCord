#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// Integer-only 48 kHz synthetic test audio, so reference packet digests are
// independent of floating-point code generation.
struct OpusVoiceSignal {
    i32 sine[257]; // Q15, one period
    u32 f0_phase, vibrato_phase, noise;
    u64 sample;
};
inline void opus_voice_signal_initialize(OpusVoiceSignal* signal) noexcept {
    // Q30 rotation by 2*pi/256.
    constexpr i64 c = 1073418434, s = 26350943;
    i64 x = i64{1} << 30, y = 0;
    for (u32 i = 0; i < 257; ++i) {
        signal->sine[i] = static_cast<i32>((y + (i64{1} << 14)) >> 15);
        const i64 nx = (x * c - y * s) >> 30, ny = (x * s + y * c) >> 30;
        x = nx, y = ny;
    }
    signal->f0_phase = signal->vibrato_phase = 0;
    signal->noise = 0x2545f491u;
    signal->sample = 0;
}
inline i32 opus_voice_sine(const OpusVoiceSignal* signal, u32 phase) noexcept {
    const u32 index = phase >> 24;
    const i32 fraction = static_cast<i32>((phase >> 8) & 0xffffu), a = signal->sine[index];
    return a + static_cast<i32>((static_cast<i64>(signal->sine[index + 1] - a) * fraction) >> 16);
}
inline u32 opus_voice_increment(u32 hertz_q8) noexcept {
    return static_cast<u32>((static_cast<u64>(hertz_q8) << 24) / 48000u);
}
inline i32 opus_voice_noise(OpusVoiceSignal* signal) noexcept {
    signal->noise = signal->noise * 1664525u + 1013904223u;
    return static_cast<i32>(signal->noise >> 16) - 32768;
}
// Two alternating vowels: 250 ms syllables with ramps and a 50 ms breathy gap,
// harmonic source with vibrato around 140 Hz and formant-shaped harmonics.
inline i16 opus_voice_speech_sample(OpusVoiceSignal* signal) noexcept {
    // Formant envelopes in 250 Hz bins up to 8 kHz (Q15).
    static constexpr u16 vowels[2][32]{
        {9000, 22000, 30000, 26000, 14000, 9000, 7000, 8000, 12000, 15000, 11000, 6000, 4000, 3000, 2600, 2200,
         2000, 1700, 1400, 1200, 1000, 800, 700, 600, 500, 420, 360, 300, 250, 200, 160, 120},
        {8000, 26000, 18000, 7000, 4000, 3500, 4000, 6000, 9000, 14000, 20000, 16000, 9000, 5000, 3500, 3000,
         2800, 2300, 1800, 1400, 1100, 900, 750, 600, 500, 420, 360, 300, 250, 200, 160, 120}};
    const u64 n = signal->sample++;
    const u32 position = static_cast<u32>(n % 12000u), syllable = static_cast<u32>((n / 12000u) & 1u);
    signal->vibrato_phase += opus_voice_increment(3u << 8);
    const i32 vibrato = opus_voice_sine(signal, signal->vibrato_phase); // Q15
    const u32 f0_q8 = static_cast<u32>((140 << 8) + ((vibrato * 25) >> 7)); // 140 +- 25 Hz
    const u32 increment = opus_voice_increment(f0_q8);
    signal->f0_phase += increment;
    i32 envelope = 0; // Q15
    if (position < 960) envelope = static_cast<i32>(position * 32767u / 960u);
    else if (position < 8640) envelope = 32767;
    else if (position < 9600) envelope = static_cast<i32>((9600u - position) * 32767u / 960u);
    i64 voiced = 0;
    const u32 harmonics = (8000u << 8) / f0_q8;
    for (u32 k = 1; k <= harmonics; ++k) {
        const u32 bin = ((f0_q8 * k) >> 8) / 250u;
        voiced += static_cast<i64>(opus_voice_sine(signal, signal->f0_phase * k)) * vowels[syllable][bin < 32 ? bin : 31];
    }
    // Harmonic sum peaks near 3.5x a single full-scale harmonic; keep ~-6 dBFS peaks.
    i32 value = static_cast<i32>((voiced >> 15) * envelope >> 18);
    value += opus_voice_noise(signal) >> (envelope ? 8 : 6);
    return static_cast<i16>(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
}
inline i16 opus_voice_tone_sample(OpusVoiceSignal* signal, u32* phase, u32 hertz, i32 amplitude) noexcept {
    *phase += opus_voice_increment(hertz << 8);
    return static_cast<i16>((static_cast<i64>(opus_voice_sine(signal, *phase)) * amplitude) >> 15);
}
} // namespace maicord
