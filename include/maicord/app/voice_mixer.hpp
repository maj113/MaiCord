#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kVoiceChannels = 2;
inline constexpr u32 kVoiceGainDefaultPercent = 100, kVoiceGainMaximumPercent = 200;
// Linear below this magnitude; above it the mix bends smoothly toward full scale.
inline constexpr i32 kVoiceSoftClipKnee = 24576;
// Adds gain-scaled (0-200 %) interleaved PCM16 into a 32-bit accumulator of the same length.
void voice_mixer_accumulate(Slice<i32> mix, ConstSlice<i16> pcm, u32 gain_percent) noexcept;
i16 voice_mixer_soft_clip(i32 sample) noexcept;
void voice_mixer_finish(ConstSlice<i32> mix, Slice<i16> output) noexcept;
// Mean absolute amplitude, for speaking detection.
u32 voice_pcm_level(ConstSlice<i16> pcm) noexcept;
} // namespace maicord
