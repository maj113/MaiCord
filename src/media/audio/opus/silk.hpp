#pragma once
// SILK layer of Opus (RFC 6716 4.2): linear prediction speech coding at 8, 12 or 16 kHz. The decoder is
// integer arithmetic throughout, as the format defines it. Internal to src/media/audio/opus.
#include "range_coder.hpp"

namespace maicord::opus {

inline constexpr u32 kSilkOrderMaximum = 16;
inline constexpr u32 kSilkSubframesMaximum = 4;
inline constexpr u32 kSilkSubframeMaximum = 80; // 5 ms at 16 kHz
inline constexpr u32 kSilkFrameMaximum = 320;   // 20 ms at 16 kHz
inline constexpr u32 kSilkFramesPerPacket = 3;
inline constexpr u32 kLtpTaps = 5;
inline constexpr u32 kShellBlock = 16; // excitation samples per pulse-count block
enum SilkSignal : u8 {
    kSilkInactive,
    kSilkUnvoiced,
    kSilkVoiced
};
// How a frame's gains and pitch lag relate to the frame before it in the packet.
enum SilkCoding : u8 {
    kSilkIndependent,
    kSilkIndependentNoLtpScale,
    kSilkConditional
};

// ---- Specification data (silk_tables.cpp) ----
extern const u8 kTypeOffsetActiveIcdf[4], kTypeOffsetInactiveIcdf[2];
extern const u8 kGainIcdf[3][8], kGainDeltaIcdf[41];
extern const u8 kNlsfInterpolationIcdf[5], kNlsfExtensionIcdf[7];
extern const u8 kLtpScaleIcdf[3];
extern const i16 kLtpScales[3];
extern const u8 kUniform3Icdf[3], kUniform4Icdf[4], kUniform5Icdf[5], kUniform6Icdf[6], kUniform8Icdf[8];
extern const u8 kLsbIcdf[2], kLbrrFlags2Icdf[3], kLbrrFlags3Icdf[7];
extern const i16 kStereoWeights[16];
extern const u8 kStereoJointIcdf[25], kStereoMidOnlyIcdf[2];
extern const u8 kPitchLagIcdf[32], kPitchDeltaIcdf[21];
extern const u8 kPitchContourIcdf[34], kPitchContourNarrowIcdf[11], kPitchContour10msIcdf[12],
    kPitchContour10msNarrowIcdf[3];
extern const i8 kPitchContourNarrow[4][11], kPitchContour[4][34], kPitchContour10msNarrow[2][3],
    kPitchContour10ms[2][12];
extern const u8 kLtpPeriodicityIcdf[3];
extern const u8 kLtpFilterIcdf0[8], kLtpFilterIcdf1[16], kLtpFilterIcdf2[32];
extern const i8 kLtpFilters0[8][5], kLtpFilters1[16][5], kLtpFilters2[32][5];
extern const u8* const kLtpFilterIcdf[3];
extern const i8* const kLtpFilters[3];
extern const u8 kRateLevelIcdf[2][9], kPulseCountIcdf[10][18];
extern const u8 kShellIcdf0[152], kShellIcdf1[152], kShellIcdf2[152], kShellIcdf3[152], kShellOffsets[17];
extern const u8 kSignIcdf[42];
extern const i16 kLsfCosine[129];
extern const i16 kResamplerFraction[12][8];
extern const i16 kQuantizationOffsets[2][2]; // Q10, [voiced][offset type]
struct NlsfCodebook {
    u16 vectors, order;
    i16 step_q16, inverse_step_q6;
    const u8* base;       // first stage vectors, Q8
    const i16* weights;   // Q9
    const u8* base_icdf;  // [signal type >> 1][vector]
    const u8* prediction; // Q8
    const u8* selectors;
    const u8* residual_icdf;
    const u8* residual_bits; // Q5, for the encoder
    const i16* spacing;      // minimum distance between neighbours, Q15
};
extern const NlsfCodebook kNlsfNarrow, kNlsfWide;

// ---- Fixed-point arithmetic of the format ----
__forceinline i32 shift_left(i32 a, u32 bits) noexcept {
    return static_cast<i32>(static_cast<u32>(a) << bits);
}
__forceinline i32 multiply_16(i32 a, i32 b) noexcept { // low halves
    return static_cast<i16>(a) * static_cast<i16>(b);
}
__forceinline i32 multiply_high_16(i32 a, i32 b) noexcept { // (a * low half of b) >> 16
    return static_cast<i32>((a * static_cast<i64>(static_cast<i16>(b))) >> 16);
}
__forceinline i32 multiply_high(i32 a, i32 b) noexcept { // (a * b) >> 16
    return static_cast<i32>((a * static_cast<i64>(b)) >> 16);
}
__forceinline i32 multiply_top(i32 a, i32 b) noexcept { // (a * b) >> 32
    return static_cast<i32>((a * static_cast<i64>(b)) >> 32);
}
__forceinline i32 add_wrap(i32 a, i32 b) noexcept {
    return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b));
}
__forceinline i32 round_shift(i32 a, u32 bits) noexcept {
    return bits == 1 ? (a >> 1) + (a & 1) : ((a >> (bits - 1)) + 1) >> 1;
}
__forceinline i64 round_shift(i64 a, u32 bits) noexcept {
    return bits == 1 ? (a >> 1) + (a & 1) : ((a >> (bits - 1)) + 1) >> 1;
}
__forceinline i32 saturate_16(i32 a) noexcept {
    return clamp_value(a, -32768, 32767);
}
__forceinline i32 saturate_32(i64 a) noexcept {
    return static_cast<i32>(clamp_value<i64>(a, -2147483647 - 1, 2147483647));
}
__forceinline i32 shift_left_saturate(i32 a, u32 bits) noexcept {
    return shift_left(clamp_value(a, (-2147483647 - 1) >> bits, 2147483647 >> bits), bits);
}
__forceinline i32 leading_zeros(i32 a) noexcept {
    return a ? __builtin_clz(static_cast<u32>(a)) : 32;
}
// (1 << q) / b and (a << q) / b to about 14 and 30 bits, and sqrt(x) for x > 0 to about 1 %.
i32 inverse_q(i32 b, i32 q) noexcept;
i32 divide_q(i32 a, i32 b, i32 q) noexcept;
i32 square_root_approximate(i32 x) noexcept;
// 2^(x / 128) for x in Q7.
i32 exp2_q7(i32 x) noexcept;

// ---- Shared by decoder and encoder (silk_common.cpp) ----
// Prediction coefficients (Q12) of normalized line spectral frequencies (Q15), bandwidth-expanded until
// stable.
void silk_nlsf_to_lpc(i16* lpc_q12, const i16* nlsf_q15, u32 order) noexcept;
// Moves NLSFs apart to at least the codebook's spacing.
void silk_nlsf_stabilize(i16* nlsf_q15, const i16* spacing_q15, u32 order) noexcept;
// The NLSFs of a codebook path: indices[0] the first stage vector, then one residual per coefficient.
void silk_nlsf_decode(i16* nlsf_q15, const i8* indices, const NlsfCodebook& codebook) noexcept;
// Entropy table offsets and predictors of the residuals of first stage vector `vector`.
void silk_nlsf_unpack(i16* table_offsets, u8* prediction_q8, const NlsfCodebook& codebook,
                      u32 vector) noexcept;
// Inverse of the prediction gain of an LPC filter in Q30; 0 when it is unstable.
i32 silk_inverse_prediction_gain(const i16* lpc_q12, u32 order) noexcept;
void silk_bandwidth_expand(i16* lpc, u32 order, i32 chirp_q16) noexcept;
// Linear gains (Q16) of the gain indices of a frame; *previous is the last index of the frame before.
void silk_gains_decode(i32* gains_q16, const i8* indices, i8* previous, bool conditional,
                       u32 subframes) noexcept;
// Prediction residual of in[order..length) into out (out[0..order) is zero).
void silk_lpc_analysis(i16* out, const i16* in, const i16* lpc_q12, u32 length, u32 order) noexcept;
// Energy of x scaled down by 2^*shift so that it fits 32 bits with two bits of headroom.
i32 silk_energy(const i16* x, u32 length, i32* shift) noexcept;
// Pitch lags of the subframes from the lag index and the contour index.
void silk_pitch_lags(i32* lags, i32 lag_index, i32 contour, u32 rate_khz, u32 subframes) noexcept;

// ---- Decoder ----
struct SilkIndices {
    i8 gains[kSilkSubframesMaximum];
    i8 ltp[kSilkSubframesMaximum];
    i8 nlsf[kSilkOrderMaximum + 1];
    i16 lag;
    i8 contour;
    i8 signal;        // SilkSignal
    i8 offset;        // quantization offset type
    i8 interpolation; // NLSF interpolation factor, Q2
    i8 periodicity;
    i8 ltp_scale;
    i8 seed;
};
struct SilkUpsampler { // 8, 12 or 16 kHz to 48 kHz
    i32 allpass[6];
    i16 fir[8];
    i16 delay[16];
    u32 rate_khz;
    u32 input_delay;
    i32 step_q16; // input position advance per output sample, at twice the input rate
};
struct SilkChannel {
    i32 previous_gain_q16;
    i32 excitation[kSilkFrameMaximum]; // Q14
    i32 lpc_state[kSilkOrderMaximum];  // Q14
    i16 history[kSilkFrameMaximum + 2 * kSilkSubframeMaximum];
    i32 previous_lag;
    i8 last_gain_index;
    bool first_frame; // after a reset: no NLSF interpolation
    u32 rate_khz;
    u32 subframes, frame_length, subframe_length, ltp_memory, order;
    i16 previous_nlsf[kSilkOrderMaximum];
    u32 frames_decoded, frames_per_packet;
    i32 coded_signal; // of the last frame read from the bitstream
    i16 coded_lag;
    bool active[kSilkFramesPerPacket];
    bool lbrr, lbrr_frame[kSilkFramesPerPacket];
    SilkUpsampler upsampler;
    SilkIndices indices;
    // Comfort noise.
    i32 noise_excitation[kSilkFrameMaximum];
    i16 noise_nlsf[kSilkOrderMaximum];
    i32 noise_state[kSilkOrderMaximum];
    i32 noise_gain_q16;
    i32 noise_seed;
    u32 noise_rate_khz;
    // Loss concealment.
    u32 loss_count;
    i32 previous_signal;
    i32 conceal_pitch_q8;
    i16 conceal_ltp[kLtpTaps];
    i16 conceal_lpc[kSilkOrderMaximum];
    bool last_frame_lost;
    i32 conceal_seed;
    i16 conceal_noise_scale_q14;
    i32 concealed_energy, concealed_energy_shift;
    i16 conceal_ltp_scale_q14;
    i32 conceal_gains_q16[2];
    u32 conceal_rate_khz, conceal_subframes, conceal_subframe_length;
};
struct SilkDecoder {
    SilkChannel channel[2];
    i16 predictor_q13[2]; // stereo prediction of the frame before
    i16 mid[2], side[2];  // last two samples of each
    u32 channels_out, channels_stream;
    bool previous_mid_only;
};
// Forgets all history (also what a switch from CELT-only frames does).
void silk_decoder_reset(SilkDecoder* decoder) noexcept;
struct SilkFrame {
    u32 channels_out;    // PCM channels
    u32 stream_channels; // channels coded
    u32 rate_khz;        // 8, 12 or 16
    u32 packet_ms;       // 10, 20, 40 or 60: the Opus frame this SILK frame is part of
    u32 lost;            // 0 decode, 1 conceal, 2 decode the redundant copy of the lost packet (LBRR)
    bool first;          // first SILK frame of the Opus frame
};
// Decodes or conceals one SILK frame (10 or 20 ms) into 48 kHz PCM16 interleaved by channels_out; returns
// the samples per channel.
u32 silk_decode(SilkDecoder* decoder, RangeCoder* coder, const SilkFrame& frame, i16* pcm) noexcept;

} // namespace maicord::opus
