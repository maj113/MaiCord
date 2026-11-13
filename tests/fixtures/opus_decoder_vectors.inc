// Synthetic Opus decoder coverage streams (generated; do not edit) in opus_demo
// bitstream format: per packet a big-endian length, the encoder's final range
// and the payload; length 0 marks a lost packet. Encoded once by unmodified
// libopus 1.6.1 (fixed point) from opus_voice_signal.hpp audio with forced
// modes, bandwidths, frame sizes, channel coding, FEC and DTX; the multiframe
// streams pack its single frames into code 1, 2 and 3 packets (with padding).
// Digests: FNV-1a of the 48 kHz PCM16 that MaiCord's decoder produces, mono and
// stereo output, the same on every system. SILK is integer arithmetic the format
// defines, so the SILK-only streams without concealment are also libopus's PCM
// bit for bit; CELT is single precision, where a digest pins this decoder's own
// arithmetic and tools/opus_conformance.ps1 measures the distance to the
// reference.
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
    {"silk_wb_40ms_mono_fec", kOpusDecoderVector2, 597, 8, 2, 1, {0x677862c0ad699b82ull, 0x0dfbda7d1adf2be5ull}},
    {"silk_wb_60ms_stereo_fec", kOpusDecoderVector3, 1282, 6, 1, 1, {0x9e5d9b1b8e060db8ull, 0x879c4d415e1b452dull}},
    {"silk_wb_20ms_dtx", kOpusDecoderVector4, 508, 24, 1, 0, {0xa10868a673bcb45bull, 0x819e66d206896655ull}},
    {"hybrid_swb_10ms_stereo", kOpusDecoderVector5, 715, 14, 1, 0, {0xc17b8530dd710b83ull, 0x30eb21d3563ed3d5ull}},
    {"hybrid_fb_20ms_mono_fec", kOpusDecoderVector6, 793, 12, 1, 1, {0x4a276abf45e821a8ull, 0xbd2ebb65439f77a2ull}},
    {"celt_nb_2_5ms_mono", kOpusDecoderVector7, 444, 32, 1, 0, {0xe386684e2dd13cbaull, 0xa504801e5a75c461ull}},
    {"celt_wb_5ms_stereo", kOpusDecoderVector8, 780, 20, 0, 0, {0x56beb9ed9d0c76adull, 0x7685f49995a4cb50ull}},
    {"celt_swb_10ms_mono", kOpusDecoderVector9, 764, 14, 2, 0, {0xbe3e7ac7508c37e3ull, 0x584924bca58dd391ull}},
    {"celt_fb_20ms_stereo", kOpusDecoderVector10, 1683, 10, 1, 0, {0xd09412327d7437f3ull, 0x588c2fbd3735875eull}},
    {"switching_modes_mono", kOpusDecoderVector11, 1495, 18, 1, 0, {0x4e73eb3a76aa43bcull, 0xdea65acf9e781a29ull}},
    {"switching_channels_stereo", kOpusDecoderVector12, 1415, 15, 0, 0, {0xe7ef1546ea3bb39bull, 0x29a360f068f4cf7cull}},
    {"multiframe_celt_fb_20ms_mono", kOpusDecoderVector13, 2723, 10, 0, 0, {0x56b62eb955f75b21ull, 0x128fb74ec9afe115ull}},
    {"multiframe_hybrid_swb_20ms_stereo", kOpusDecoderVector14, 2750, 10, 0, 0, {0xf7d4f7967a1a12d6ull, 0x2fd4c2bf3dd71943ull}},
    {"multiframe_silk_wb_10ms_mono", kOpusDecoderVector15, 1065, 8, 0, 0, {0x39d98966271b7f70ull, 0x322020a8dac34595ull}},
    {"multiframe_celt_wb_2_5ms_stereo", kOpusDecoderVector16, 912, 5, 0, 0, {0x51ece5cdb4fe8012ull, 0x2a83b150f2b45dc1ull}},
};
} // namespace maicord
