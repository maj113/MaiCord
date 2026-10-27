#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// Original 4096-sample 440 Hz, amplitude 0.125 mono tone at 44.1 kHz.
// Encoded entirely in memory with libmp3lame through PyAV 19.0.1.
// Reference samples use the independent FFmpeg mp3float decoder.
// Raw Layer-III frames deliberately have no container or borrowed artwork.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kMp3OriginalTone[]{
#embed "mp3_original_tone.bin"
};
#pragma clang diagnostic pop
inline constexpr u32 kMp3OriginalToneFrames = 5760;
struct Mp3OriginalToneReference {
    u32 frame;
    float value;
};
inline constexpr Mp3OriginalToneReference kMp3OriginalToneReference[] = {
    {0, 0.000000000e+00f},     {128, 2.211373413e-07f},   {384, 6.533881242e-05f},
    {768, 3.147712559e-04f},   {1024, -1.565231651e-04f}, {1152, 2.280843817e-02f},
    {1400, -4.153765365e-02f}, {1800, -4.770708829e-02f}, {2200, -5.380200967e-02f},
    {2600, -5.973963439e-02f}, {3000, -6.548272818e-02f}, {3400, -7.103934884e-02f},
    {3800, -7.633619010e-02f}, {4200, -8.138200641e-02f}, {4600, -8.608220518e-02f},
    {5000, -9.085793793e-02f}, {5500, 1.133309561e-04f},
};
} // namespace maicord
