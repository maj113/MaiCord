#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// A WebM Opus file and the list of its packets, from tools/generate_webm_fixtures.py: libopus over a synthetic
// signal, libavformat's WebM muxer, ffprobe as the oracle. Layout of the list in the generator's header.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kWebmOpusStereo[]{
#embed "opus_stereo.webm"
};
inline constexpr u8 kWebmOpusStereoPackets[]{
#embed "opus_stereo_packets.bin"
};
#pragma clang diagnostic pop
} // namespace maicord
