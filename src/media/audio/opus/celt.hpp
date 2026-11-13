#pragma once
// CELT layer of Opus at 48 kHz (RFC 6716 4.3): the 21-band mode with 2.5 to 20 ms frames, in single
// precision. Internal to src/media/audio/opus.
#include "range_coder.hpp"

namespace maicord::opus {

inline constexpr u32 kBands = 21;
inline constexpr u32 kShortBlock = 120; // samples of a 2.5 ms block
inline constexpr u32 kOverlap = 120;    // window overlap between frames
inline constexpr u32 kFrameMaximum = 960;
inline constexpr u32 kHistory = 2048; // decoded samples kept for the post-filter and loss concealment
inline constexpr u32 kPeriodMinimum = 15, kPeriodMaximum = 1024;
inline constexpr u32 kConcealOrder = 24; // LPC order of the pitch-based concealment
inline constexpr u32 kFineBitsMaximum = 8;

// A table built at compile time (a constexpr function cannot return an array).
template <typename T, u32 Count> struct Table {
    T value[Count];
    constexpr const T& operator[](u32 index) const noexcept {
        return value[index];
    }
};

// Band b covers MDCT bins kBandEdges[b] << LM up to kBandEdges[b + 1] << LM.
extern const u8 kBandEdges[kBands + 1];
extern const u8 kLogN[kBands];            // log2 of the band width, 1/8 bit
extern const u8 kAllocation[11 * kBands]; // 1/32 bit per sample for each quality step
extern const i16 kPulseCacheIndex[105];
extern const u8 kPulseCacheBits[392];
extern const u8 kPulseCacheCaps[168];
extern const u8 kEnergyModel[4][2][42];
extern const float kEnergyMeans[kBands];
extern const Table<float, kOverlap> kWindow;
extern const i8 kTfSelect[4][8];

enum Spread : u32 {
    kSpreadNone,
    kSpreadLight,
    kSpreadNormal,
    kSpreadAggressive
};

// The instructions themselves: the library names would set errno where the build keeps it.
inline float square_root(float value) noexcept {
    return __builtin_elementwise_sqrt(value);
}
inline double square_root(double value) noexcept {
    return __builtin_elementwise_sqrt(value);
}
inline float floor_value(float value) noexcept {
    return __builtin_elementwise_floor(value);
}
// 2^x for x below 127, within 2 ulp; log2 of a positive x, within 1e-6.
float exp2_approximate(float x) noexcept;
float log2_approximate(float x) noexcept;
// atan(y / x) in radians for y and x at least 0 and not both 0, within 2e-5.
inline float arc_tangent(float y, float x) noexcept {
    const bool steep = y > x;
    const float z = steep ? x / y : y / x, s = z * z;
    const float angle =
        z *
        (0.99997726f + s * (-0.33262347f +
                            s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f + s * -0.01172120f)))));
    return steep ? 1.57079632679f - angle : angle;
}

// Inverse MDCT of the 960 >> shift bins at spectrum (which it keeps): writes that many samples at
// out + kOverlap / 2 and folds the first kOverlap / 2 of them with the previous block's tail at
// out[0, kOverlap / 2) through the window (time-domain alias cancellation), leaving the new tail raw for
// the next block.
void celt_imdct(const float* spectrum, float* out, u32 shift) noexcept;
// Forward MDCT: (960 >> shift) + kOverlap input samples to 960 >> shift bins.
void celt_mdct(const float* in, float* spectrum, u32 shift) noexcept;

// Band allocation of one frame (RFC 6716 4.3.3).
struct BandAllocation {
    i32 pulses[kBands];  // 1/8 bits for the shape of each band
    i32 fine[kBands];    // fine energy bits per channel
    u8 priority[kBands]; // 1: the band gets a final fine bit only in the second pass
    i32 balance;         // 1/8 bits kept for the bands that can use them
    u32 intensity;       // first band coded as intensity stereo
    u32 coded_bands;     // bands that get shape bits; the others fold
    bool dual_stereo;
};
struct AllocationRequest {
    u32 start, end;
    u32 channels, lm;
    i32 total; // 1/8 bits
    u32 trim;
    const i32* boosts; // per-band dynamic allocation, 1/8 bits
    const i32* caps;
    // Encoder choices (ignored when decoding).
    u32 intensity;
    bool dual_stereo;
    u32 previous_coded_bands, signal_bandwidth;
};
void celt_band_caps(i32* caps, u32 lm, u32 channels) noexcept;
void celt_allocate(RangeCoder& coder, bool encode, const AllocationRequest& request,
                   BandAllocation* out) noexcept;

// Shape (PVQ) coding of every band (RFC 6716 4.3.4): decodes into x (and y for stereo), 120 << lm
// normalized bins per channel, or encodes them.
struct ShapeRequest {
    u32 start, end;
    u32 lm;
    bool transient; // short blocks
    bool encode;
    bool keep_phase; // mono output: no phase inversion of intensity stereo
    u32 spread;
    const i8* tf;                // per-band time-frequency change
    i32 total_bits;              // 1/8 bits, without the anti-collapse reservation
    const float* band_amplitude; // encoder: linear band energies, [channel][band]
};
void celt_code_bands(RangeCoder& coder, const ShapeRequest& request, const BandAllocation& allocation,
                     float* x, float* y, u8* collapse_masks, u32* seed) noexcept;

inline u32 celt_random(u32 seed) noexcept {
    return 1664525u * seed + 1013904223u;
}
// Scales the N values of x to the norm `gain`.
void celt_renormalize(float* x, u32 n, float gain) noexcept;
float celt_inner_product(const float* a, const float* b, u32 n) noexcept;

struct CeltDecoder {
    u32 channels; // output channels
    // Cleared by celt_decoder_reset.
    u32 range; // final range of the last frame, which also seeds the noise of the next
    u32 last_pitch;
    u32 loss_duration, conceal_duration; // 2.5 ms units
    u8 last_frame;                       // 1 decoded, 2 noise concealment, 3 pitch concealment
    bool skip_pitch_conceal;             // until two frames arrive in a row
    bool fold_pending;                   // the concealed tail still needs pre-filtering and folding
    u32 period, period_old;
    float gain, gain_old;
    u32 tapset, tapset_old;
    float emphasis[2];
    float energy[2][kBands];                      // log2 band energies of the last frame, mean removed
    float energy1[2][kBands], energy2[2][kBands]; // of the two frames before
    float background[2][kBands];
    float lpc[2][kConcealOrder];
    alignas(32) float history[2][kHistory + kOverlap];
};
void celt_decoder_reset(CeltDecoder* decoder) noexcept;
struct CeltFrame {
    u32 samples;         // 120, 240, 480 or 960
    u32 stream_channels; // channels coded in the frame
    u32 start, end;      // band range: start 17 in hybrid frames
    bool accumulate;     // add to pcm (the SILK output) instead of replacing it
    // The output is rounded and saturated after `gain`. Samples [tap_start, tap_start + tap_count) are also
    // stored in `tap` before both (interleaved): what a cross-fade mixes must not have clipped yet.
    // `tap_only` leaves pcm alone.
    float gain;
    float* tap;
    u32 tap_start, tap_count;
    bool tap_only;
};
// Decodes one frame from coder (already past any SILK data), whose bytes count is `bytes`; a null coder
// conceals a lost frame. pcm is interleaved by decoder->channels. False when the frame used more bits than
// it has.
bool celt_decode(CeltDecoder* decoder, RangeCoder* coder, u32 bytes, const CeltFrame& frame,
                 i16* pcm) noexcept;

// The CELT layer of the hybrid encoder: bands 17 to 20 of 20 ms frames.
struct CeltEncoder {
    u32 channels; // input channels
    // Cleared by celt_encoder_reset.
    u32 range;
    float delayed_intra; // how much an intra frame would cost in prediction, accumulated
    u32 last_coded_bands, consecutive_transients, intensity;
    float emphasis[2];
    float overlap[2][kOverlap];    // the end of the previous frame, pre-emphasized
    float energy[2][kBands];       // as the decoder has them
    float energy_error[2][kBands]; // what the last frame's energies missed by
};
void celt_encoder_reset(CeltEncoder* encoder) noexcept;
struct CeltEncodeFrame {
    u32 stream_channels; // 1 codes the mean of two input channels
    i32 bitrate;         // this layer's share, bits per second
    u32 complexity, loss_percent;
    u32 silk_signal; // the SILK frame's signal type (2: voiced) and quantization offset (Q10)
    i32 silk_offset;
};
// Codes 960 samples per channel (PCM16 scale) into `coder`, which holds the SILK layer and may grow to
// maximum_bytes in all, and finishes it. The input is working memory: kOverlap floats before each channel's
// samples are written, the samples overwritten. Returns the frame's size in bytes, 0 when it did not fit.
u32 celt_encode(CeltEncoder* encoder, RangeCoder& coder, float* const pcm[2], u32 maximum_bytes,
                const CeltEncodeFrame& frame) noexcept;

} // namespace maicord::opus
