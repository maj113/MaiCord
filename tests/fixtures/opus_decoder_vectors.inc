// Synthetic Opus decoder coverage streams (generated; do not edit) in opus_demo
// bitstream format: per packet a big-endian length, the encoder's final range
// and the payload; length 0 marks a lost packet. Encoded once by unmodified
// libopus 1.6.1 (fixed point) from opus_voice_signal.hpp audio with forced
// modes, bandwidths, frame sizes, channel coding, FEC and DTX; the multiframe
// streams pack its single frames into code 1, 2 and 3 packets (with padding).
// Digests: FNV-1a of the 48 kHz PCM16 from the unmodified fixed-point decoder,
// mono and stereo output.
#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
struct OpusDecoderVector {
    const char* name;
    const u8* data;
    u32 bytes, packets, lost, recovered;
    u64 digest[2];
};
inline constexpr u8 kOpusDecoderVector0[]{
    0,0,0,8,0,177,60,206,0,130,4,9,162,27,247,181,0,0,0,11,78,107,93,172,
    0,133,229,249,39,233,68,201,178,82,192,0,0,0,11,0,247,204,88,0,189,67,249,169,
    5,118,94,31,137,252,0,0,0,10,10,101,241,145,0,189,60,16,92,217,27,239,94,80,
    0,0,0,10,5,22,246,31,0,189,56,83,226,18,27,239,233,132,0,0,0,0,0,0,
    0,0,0,0,0,10,2,214,228,43,0,189,60,128,13,24,34,205,127,208,0,0,0,11,
    9,54,216,246,0,188,237,55,167,219,62,101,102,35,192,0,0,0,11,29,171,151,192,0,
    189,22,212,35,191,220,188,235,252,192,0,0,0,10,1,134,19,238,0,189,59,202,147,130,
    100,205,32,208,0,0,0,11,0,219,53,192,0,189,55,227,8,37,237,124,239,122,235,0,
    0,0,13,6,92,25,0,0,189,27,187,114,144,128,48,7,128,204,157,64,0,0,0,10,
    92,127,250,0,0,189,56,85,232,211,102,239,7,128,0,0,0,9,117,36,87,0,0,189,
    56,81,115,55,152,28,33,
};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kOpusDecoderVector1[]{
#embed "opus_decoder_vectors/kOpusDecoderVector1.bin"
};
inline constexpr u8 kOpusDecoderVector2[]{
#embed "opus_decoder_vectors/kOpusDecoderVector2.bin"
};
inline constexpr u8 kOpusDecoderVector3[]{
#embed "opus_decoder_vectors/kOpusDecoderVector3.bin"
};
inline constexpr u8 kOpusDecoderVector4[]{
#embed "opus_decoder_vectors/kOpusDecoderVector4.bin"
};
inline constexpr u8 kOpusDecoderVector5[]{
#embed "opus_decoder_vectors/kOpusDecoderVector5.bin"
};
inline constexpr u8 kOpusDecoderVector6[]{
#embed "opus_decoder_vectors/kOpusDecoderVector6.bin"
};
inline constexpr u8 kOpusDecoderVector7[]{
#embed "opus_decoder_vectors/kOpusDecoderVector7.bin"
};
inline constexpr u8 kOpusDecoderVector8[]{
#embed "opus_decoder_vectors/kOpusDecoderVector8.bin"
};
inline constexpr u8 kOpusDecoderVector9[]{
#embed "opus_decoder_vectors/kOpusDecoderVector9.bin"
};
inline constexpr u8 kOpusDecoderVector10[]{
#embed "opus_decoder_vectors/kOpusDecoderVector10.bin"
};
inline constexpr u8 kOpusDecoderVector11[]{
#embed "opus_decoder_vectors/kOpusDecoderVector11.bin"
};
inline constexpr u8 kOpusDecoderVector12[]{
#embed "opus_decoder_vectors/kOpusDecoderVector12.bin"
};
inline constexpr u8 kOpusDecoderVector13[]{
#embed "opus_decoder_vectors/kOpusDecoderVector13.bin"
};
inline constexpr u8 kOpusDecoderVector14[]{
#embed "opus_decoder_vectors/kOpusDecoderVector14.bin"
};
inline constexpr u8 kOpusDecoderVector15[]{
#embed "opus_decoder_vectors/kOpusDecoderVector15.bin"
};
inline constexpr u8 kOpusDecoderVector16[]{
#embed "opus_decoder_vectors/kOpusDecoderVector16.bin"
};
#pragma clang diagnostic pop
inline constexpr OpusDecoderVector kOpusDecoderVectors[]{
    {"silk_nb_10ms_mono", kOpusDecoderVector0, 247, 14, 1, 0, {0x5029a6c3d7ed0d5aull, 0x698d636e5934c3f5ull}},
    {"silk_mb_20ms_stereo", kOpusDecoderVector1, 393, 10, 0, 0, {0x7c4c45ee796193d3ull, 0x5d6c157bd03bf125ull}},
    {"silk_wb_40ms_mono_fec", kOpusDecoderVector2, 597, 8, 2, 1, {0x3d252893f86a95e3ull, 0xcd551f6e56f5071dull}},
    {"silk_wb_60ms_stereo_fec", kOpusDecoderVector3, 1282, 6, 1, 1, {0x9e5d9b1b8e060db8ull, 0x879c4d415e1b452dull}},
    {"silk_wb_20ms_dtx", kOpusDecoderVector4, 508, 24, 1, 0, {0xa10868a673bcb45bull, 0x819e66d206896655ull}},
    {"hybrid_swb_10ms_stereo", kOpusDecoderVector5, 715, 14, 1, 0, {0x5655191e4ba8e71full, 0x8034d2a271b07a9cull}},
    {"hybrid_fb_20ms_mono_fec", kOpusDecoderVector6, 793, 12, 1, 1, {0x272e0b718a02b130ull, 0x3f3d56a723ccef7bull}},
    {"celt_nb_2_5ms_mono", kOpusDecoderVector7, 444, 32, 1, 0, {0x264d2de27dec4dd9ull, 0x98a738a06e294109ull}},
    {"celt_wb_5ms_stereo", kOpusDecoderVector8, 780, 20, 0, 0, {0xe2776aeabb9639e2ull, 0x801554dc7a0939dbull}},
    {"celt_swb_10ms_mono", kOpusDecoderVector9, 764, 14, 2, 0, {0xc757465a02e0cf9eull, 0x98cc6e29a660a9a5ull}},
    {"celt_fb_20ms_stereo", kOpusDecoderVector10, 1683, 10, 1, 0, {0x50a5ac2e34d1cbc5ull, 0xe1a93607d7252cc7ull}},
    {"switching_modes_mono", kOpusDecoderVector11, 1495, 18, 1, 0, {0x251b385be09644d5ull, 0x1d92edf932463681ull}},
    {"switching_channels_stereo", kOpusDecoderVector12, 1415, 15, 0, 0, {0xd29fd7b64f3dcce0ull, 0x82753ec55292e375ull}},
    {"multiframe_celt_fb_20ms_mono", kOpusDecoderVector13, 2723, 10, 0, 0, {0xc4757a8b426f4217ull, 0x728babc6a1322919ull}},
    {"multiframe_hybrid_swb_20ms_stereo", kOpusDecoderVector14, 2750, 10, 0, 0, {0x7316134741c74a9bull, 0xa95fb7063df04b42ull}},
    {"multiframe_silk_wb_10ms_mono", kOpusDecoderVector15, 1065, 8, 0, 0, {0x39d98966271b7f70ull, 0x322020a8dac34595ull}},
    {"multiframe_celt_wb_2_5ms_stereo", kOpusDecoderVector16, 912, 5, 0, 0, {0x88dfab3c0de4e347ull, 0xd98d622bf5e18e5bull}},
};
} // namespace maicord
