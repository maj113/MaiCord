#include "maicord/app/voice_mixer.hpp"

namespace maicord {
void voice_mixer_accumulate(Slice<i32> mix, ConstSlice<i16> pcm, u32 gain_percent) noexcept {
    const i32 gain = static_cast<i32>(minimum(gain_percent, kVoiceGainMaximumPercent));
    const u32 count = minimum(mix.count, pcm.count);
    if (!gain || !mix.data || !pcm.data)
        return;
    if (gain == 100) {
        for (u32 i = 0; i < count; ++i)
            mix[i] += pcm[i];
        return;
    }
    for (u32 i = 0; i < count; ++i)
        mix[i] += pcm[i] * gain / 100;
}

i16 voice_mixer_soft_clip(i32 sample) noexcept {
    const i64 magnitude = sample < 0 ? -static_cast<i64>(sample) : sample;
    if (magnitude <= kVoiceSoftClipKnee)
        return static_cast<i16>(sample);
    // Rational knee: unit slope at the knee, asymptotic to 32767, monotonic.
    constexpr i64 range = 32767 - kVoiceSoftClipKnee;
    const i64 excess = magnitude - kVoiceSoftClipKnee;
    const i64 bent = kVoiceSoftClipKnee + range * excess / (excess + range);
    return static_cast<i16>(sample < 0 ? -bent : bent);
}

void voice_mixer_finish(ConstSlice<i32> mix, Slice<i16> output) noexcept {
    const u32 count = minimum(mix.count, output.count);
    for (u32 i = 0; i < count; ++i)
        output[i] = voice_mixer_soft_clip(mix[i]);
}

u32 voice_pcm_level(ConstSlice<i16> pcm) noexcept {
    if (!pcm.data || !pcm.count)
        return 0;
    u64 sum = 0;
    for (u32 i = 0; i < pcm.count; ++i)
        sum += static_cast<u32>(pcm[i] < 0 ? -static_cast<i32>(pcm[i]) : pcm[i]);
    return static_cast<u32>(sum / pcm.count);
}
} // namespace maicord
