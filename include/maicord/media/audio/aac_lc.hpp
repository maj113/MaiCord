#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {

// An AAC-LC decoder (ISO/IEC 14496-3 object type 2) for the raw_data_block of one frame: mono or stereo, 1024 lines
// per frame, any sampling frequency of the standard's table up to 96 kHz. No operating system, no allocation, no
// libm: the caller gives it memory. The pieces: section, scalefactor and spectral data with the eleven Huffman
// codebooks and escape values, pulse data, M/S, intensity stereo, perceptual noise substitution, TNS, the sine and
// Kaiser-Bessel-derived windows, long, start, short and stop blocks and overlap-add. SBR and PS (HE-AAC) are not
// decoded: the core stream of such a configuration is not the PCM the container promises.
//
// The platform decoder interface (include/maicord/platform/decode/aac_decoder.hpp) is implemented over it on Linux
// (src/platform/decode/linux/aac_decoder.cpp); Windows decodes through Media Foundation.

struct AacLc;

enum class AacLcError : u8 {
    none,
    invalid_input,  // bad arguments: a null pointer, an unsupported rate or channel count
    truncated,      // the frame ends inside an element
    unsupported,    // a valid stream the decoder does not cover (prediction, gain control, CCE, LFE, PCE)
    invalid_stream  // a syntax element out of range: reserved codebook, scalefactor, band or pulse position
};

inline constexpr u32 kAacLcFrameSamples = 1024;
inline constexpr u32 kAacLcMaximumChannels = 2;

// Bytes of decoder state and the alignment of the memory for it.
usize aac_lc_state_bytes() noexcept;
inline constexpr usize kAacLcStateAlignment = 64;

// True when the core decodes `sample_rate` (one of the 13 rates of the standard) in 1 or 2 channels.
bool aac_lc_supported(u32 sample_rate, u32 channels) noexcept;

// Prepares `memory` (aac_lc_state_bytes() bytes, aligned to kAacLcStateAlignment, any content) as a decoder for the
// configuration; null when it is not supported or the memory is not valid. Fills the window and transform tables.
AacLc* aac_lc_initialize(void* memory, u32 sample_rate, u32 channels) noexcept;

// Decodes one raw frame into 1024 * channels interleaved samples scaled to [-1, 1] (a full-scale PCM16 sample is
// 1/32768; the sum of loud channels may exceed 1). Nothing is written and the decoder state is unchanged on error, so
// the next frame decodes as if this one were missing; a decoder that lost frames can call aac_lc_reset. Output is
// always finite.
AacLcError aac_lc_decode(AacLc* decoder, ConstSlice<u8> frame, float* pcm) noexcept;

// Forgets the overlap of the previous frame, as at a seek. The noise generator starts again, as at the stream start.
void aac_lc_reset(AacLc* decoder) noexcept;

} // namespace maicord
