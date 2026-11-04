#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// Bitstreams and reference samples from tools/generate_aac_fixtures.py: libavcodec's native AAC encoder over
// synthetic signals, libavcodec's native AAC decoder as the oracle. Layout in the generator's header.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kAacStereo44[]{
#embed "aac_stereo44_128k.bin"
};
inline constexpr u8 kAacMono44[]{
#embed "aac_mono44_64k.bin"
};
inline constexpr u8 kAacStereo48[]{
#embed "aac_stereo48_64k.bin"
};
inline constexpr u8 kAacMono48Noise[]{
#embed "aac_mono48_24k_noise.bin"
};
inline constexpr u8 kAacStereo44Transient[]{
#embed "aac_stereo44_128k_transient.bin"
};
inline constexpr u8 kAacStereo44Joint[]{
#embed "aac_stereo44_40k_joint.bin"
};
inline constexpr u8 kAacStereo44Loud[]{
#embed "aac_stereo44_256k_loud.bin"
};
inline constexpr u8 kAacMono22[]{
#embed "aac_mono22_32k.bin"
};
// The packets of kAacStereo44 muxed by libavformat, and as an ADTS stream.
inline constexpr u8 kAacStereo44Mp4[]{
#embed "aac_stereo44_128k.mp4"
};
inline constexpr u8 kAacStereo44Adts[]{
#embed "aac_stereo44_128k.adts"
};
#pragma clang diagnostic pop
} // namespace maicord
