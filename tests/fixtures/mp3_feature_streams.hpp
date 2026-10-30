#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// Five raw Layer III streams (no container), each from a stream start: an
// original 1.2 s signal (harmonic tones, transients, noise) encoded with
// libmp3lame through FFmpeg 7.1, then edited to reach decoder paths no common
// encoder emits. In order:
//   MPEG-1 44.1 kHz joint stereo 96 kbps without reservoir, frames 10-15,
//     rewritten with a CRC-16 word (from stuffing bits): M/S, long, start,
//     short and stop blocks;
//   MPEG-1 44.1 kHz 64 kbps frames 0-5 with the intensity bit set in every
//     mode extension (M/S and intensity);
//   the same frames with mixed_block_flag set on every short-block granule;
//   MPEG-2 22.05 kHz 24 kbps frames 0-7 with the intensity bit set (MPEG-2
//     intensity positions, including slen 0 partitions);
//   MPEG-2.5 8 kHz 16 kbps joint stereo frames 0-5 (short blocks).
// Reference samples come from the independent libmad 0.15.1 fixed-point
// decoder (ISO intensity and mixed-block rules); hashes are FNV-1a 64 over
// MaiCord's float PCM bits.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kMp3FeatureStreams[]{
#embed "mp3_feature_streams.bin"
};
#pragma clang diagnostic pop
struct Mp3FeatureSample {
    u32 index; // interleaved sample
    float value;
};
struct Mp3FeatureStream {
    u32 offset, bytes, sample_rate, channels, frames;
    u64 pcm_hash;
    Mp3FeatureSample reference[8];
};
inline constexpr Mp3FeatureStream kMp3FeatureStreamList[] = {
    {0, 1881, 44100, 2, 6912, 0x764ebf9b0ed611e0ull,
     {{4030, 3.6217641e-02f}, {8512, -3.9158154e-02f}, {9024, 5.6324292e-02f},
      {9770, 2.6089415e-02f}, {10983, -7.7331021e-02f}, {11747, -2.3565937e-02f},
      {12551, -4.7580589e-02f}, {13076, 3.0461855e-02f}}},
    {1881, 1253, 44100, 2, 6912, 0x1584fc75766e2753ull,
     {{3601, -6.1232522e-02f}, {3722, -6.2383860e-02f}, {5687, 3.4626029e-02f},
      {6178, 2.7362559e-02f}, {7664, -7.5357810e-02f}, {11482, 2.0028662e-02f},
      {11703, -5.8884375e-02f}, {11746, -5.1359065e-02f}}},
    {3134, 1253, 44100, 2, 6912, 0xe86572cf3bd1d65aull,
     {{7888, 3.3575006e-02f}, {8442, -9.9575035e-02f}, {8747, 1.2290444e-02f},
      {9264, -8.4276780e-02f}, {10189, 2.9823035e-02f}, {11271, 5.3456083e-02f},
      {11522, 1.4944915e-02f}, {13763, 1.2257855e-02f}}},
    {4387, 627, 22050, 2, 4608, 0x3865e55d806566e8ull,
     {{2438, 1.7241389e-02f}, {2502, -1.0158433e-01f}, {3002, 7.9268262e-02f},
      {3184, 2.5011878e-02f}, {5320, -1.4354891e-01f}, {5811, 7.0379391e-02f},
      {6468, 4.1908558e-02f}, {9016, 4.4294521e-02f}}},
    {5014, 864, 8000, 2, 3456, 0xd0479a5de388a5fbull,
     {{2250, 4.6292525e-02f}, {2644, 2.9951986e-02f}, {3095, 1.6108233e-02f},
      {3363, -2.9150523e-02f}, {3450, 1.0482363e-02f}, {3986, 2.8837893e-02f},
      {4543, 2.0709794e-02f}, {6880, 1.7183863e-02f}}},
};
} // namespace maicord
