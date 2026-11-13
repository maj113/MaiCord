#pragma once
// SILK encoding of 20 ms wideband frames (16 kHz, four 5 ms subframes), mono or mid/side stereo: internal
// to src/media/audio/opus. The analysis is single precision; the quantizers and the noise-shaping quantizer
// are the integer arithmetic of the decoder, so that the encoder tracks exactly what a decoder will play.
#include "celt.hpp"
#include "silk.hpp"

namespace maicord::opus {

inline constexpr u32 kSilkRate = 16;                       // kHz
inline constexpr u32 kSilkFrame = kSilkFrameMaximum;       // 320 samples
inline constexpr u32 kSilkSubframe = kSilkSubframeMaximum; // 80 samples
inline constexpr u32 kSilkSubframes = kSilkSubframesMaximum;
inline constexpr u32 kSilkHistory = kSilkFrame;       // 20 ms kept for the long-term predictor
inline constexpr u32 kShapeLookahead = 5 * kSilkRate; // the frame coded lags the input by 5 ms
inline constexpr u32 kPitchLookahead = 2 * kSilkRate;
inline constexpr u32 kShapeOrderMaximum = 24;
inline constexpr u32 kDecisionStatesMaximum = 4;
inline constexpr u32 kDecimatorTaps = 120;

inline float sigmoid(float x) noexcept {
    return 1.f / (1.f + exp2_approximate(-1.44269504f * x));
}
inline float energy(const float* x, u32 n) noexcept {
    return celt_inner_product(x, x, n);
}

// What the complexity setting selects.
struct SilkComplexity {
    float pitch_threshold; // first-stage correlation threshold of the pitch search
    u8 pitch_complexity;   // 0 to 2: candidates and contours searched
    u8 pitch_order;        // whitening filter of the pitch search
    u8 shape_order;        // noise shaping filter
    u8 decision_states;    // delayed-decision states of the quantizer
    u8 nlsf_survivors;
    bool interpolate_nlsf;
    bool warped; // warped noise shaping filter
};

struct SilkVad {
    float filter[3][2];
    float high_pass;
    float last_subframe[4]; // energy of each band's last quarter frame
    float noise[4], inverse_noise[4];
    float ratio_smooth[4];
    u32 counter;
};

// The noise-shaping quantizer's memory: what a decoder will hold as its integers, and the shaping filters'
// states (single precision, in the scale of Q14 integers).
struct SilkQuantizerState {
    i16 output[2 * kSilkFrame];         // quantized output signal, for re-whitening
    float ltp_shape[2 * kSilkFrame];    // shaped quantization noise, for harmonic shaping
    i32 lpc_q14[kSilkOrderMaximum];     // short-term synthesis state
    float ar_shape[kShapeOrderMaximum]; // noise shaping filter state
    float lf_ar;
    float difference; // the last sample's quantization error
    i32 previous_lag;
    i32 ltp_shape_index, ltp_index;
    i32 previous_gain_q16;
    i32 rewhite;
};

struct SilkEncoderChannel {
    i16 input[kSilkFrame + 2]; // this frame at 16 kHz after two samples of the one before
    float x[kSilkHistory + kShapeLookahead + kSilkFrame];
    SilkVad vad;
    float speech_activity, input_tilt, input_quality[4];
    bool active; // voice activity flag of the frame
    bool first_frame;
    i32 previous_lag;
    i8 previous_signal;
    float ltp_correlation;
    float harmonic_gain_smooth, tilt_smooth;
    i8 last_gain_index;
    i16 previous_nlsf_q15[kSilkOrderMaximum];
    SilkQuantizerState quantizer;
    SilkIndices indices;
    i8 pulses[kSilkFrame];
    // The redundant low-rate copy of this frame, sent with the next one.
    bool lbrr_pending, lbrr_flag, lbrr_enabled;
    i32 lbrr_gain_increase;
    SilkIndices lbrr_indices;
    i8 lbrr_pulses[kSilkFrame];
    u32 frame_counter;
    i32 snr_db_q7;
    u32 loss_percent;
    float sum_log_gain; // of the long-term predictor, to keep it from running away after a loss
};

struct SilkEncoder {
    SilkEncoderChannel channel[2];
    float decimator[2][kDecimatorTaps]; // 48 kHz input history per channel
    u32 channels;                       // coded in the last frame (1 or 2)
    i32 bits_exceeded;                  // bit reservoir
    i32 lbrr_bits;                      // smoothed size of the redundant frames
    bool previous_mid_only;
    // Stereo: predictors of the side from the mid, smoothed amplitudes, width.
    i16 predictor_q13[2];
    i16 mid_history[2], side_history[2];
    i32 mid_side_amplitude[4];
    i16 smooth_width_q14, previous_width_q14;
    i32 silent_side; // samples for which the side has been left out
    u8 stereo_index[2][3];
    bool mid_only;
    float variable_cutoff_smooth; // log2 of the high-pass cutoff the pitch suggests
};

struct SilkEncodeFrame {
    u32 input_channels, coded_channels;
    i32 bitrate;      // of the SILK layer, bits per second
    i32 maximum_bits; // what the layer may use in this packet
    u32 complexity;
    u32 loss_percent;
    bool fec;     // code redundant copies (LBRR)
    bool to_mono; // the last stereo frame before a switch to mono: fade the width out
    bool silent;  // the input is all zero: no voice activity, whatever the detector's memory says
};
struct SilkEncodeResult {
    u32 signal; // channel 0's signal type and quantization offset (Q10), for the CELT layer
    i32 offset_q10;
    i32 stereo_width_q14;
};
void silk_encoder_reset(SilkEncoder* encoder) noexcept;
// One frame: 960 samples per input channel at 48 kHz (PCM16 scale), written to the range coder.
SilkEncodeResult silk_encode(SilkEncoder* encoder, RangeCoder& coder, const float* const pcm[2],
                             const SilkEncodeFrame& frame) noexcept;

// ---- Analysis (silk_analysis.cpp) ----
SilkComplexity silk_complexity(u32 complexity) noexcept;
// 960 samples at 48 kHz to 320 at 16 kHz; `history` holds the kDecimatorTaps samples before them.
void silk_decimate(float* history, const float* in, float* out) noexcept;
void silk_vad_reset(SilkVad* vad) noexcept;
// Speech activity, spectral tilt and per-band quality of a frame (with one sample of the previous frame in
// front, as at input + 1).
void silk_vad(SilkEncoderChannel* channel, const float* frame) noexcept;

struct SilkPitch {
    i32 lags[kSilkSubframes];
    float prediction_gain; // of the whitening filter
    bool voiced;
};
// Whitens x (history + frame + pitch lookahead) into `residual` and searches the pitch. Sets the indices'
// lag and contour and the channel's long-term correlation.
SilkPitch silk_find_pitch(SilkEncoderChannel* channel, const SilkComplexity& complexity, float* residual,
                          const float* x) noexcept;

struct SilkShape {
    float gains[kSilkSubframes];
    float ar[kSilkSubframes][kShapeOrderMaximum];
    float lf_ma[kSilkSubframes], lf_ar[kSilkSubframes];
    float tilt[kSilkSubframes], harmonic_gain[kSilkSubframes];
    float input_quality, coding_quality;
};
// Noise shaping filters and gains per subframe, and the quantization offset type. `residual` is the
// whitened frame, x the frame in the encoder's buffer.
void silk_noise_shape(SilkEncoderChannel* channel, const SilkComplexity& complexity, const SilkPitch& pitch,
                      SilkShape* shape, const float* residual, const float* x) noexcept;

// ---- Prediction (silk_prediction.cpp) ----
struct SilkPrediction {
    i16 lpc_q12[2][kSilkOrderMaximum]; // first and second half of the frame
    i16 ltp_q14[kSilkSubframes][kLtpTaps];
    i32 ltp_scale_q14;
    float residual_energy[kSilkSubframes]; // of the short- and long-term prediction, with the gains applied
    float ltp_coding_gain;                 // dB
};
// Long-term filters, short-term coefficients (as quantized NLSFs) and what they leave. Sets the indices of
// both.
void silk_find_prediction(SilkEncoderChannel* channel, const SilkComplexity& complexity,
                          const SilkPitch& pitch, const SilkShape& shape, SilkPrediction* out,
                          const float* residual, const float* x) noexcept;

// ---- Quantizers (silk_quantize.cpp) ----
// 128 * log2(x) for x > 0.
i32 log2_q7(i32 x) noexcept;
// Quantizes NLSFs with `weights` (Q2): the path through the codebook into indices, the quantized values
// back.
void silk_nlsf_encode(i8* indices, i16* nlsf_q15, const NlsfCodebook& codebook, const i16* weights,
                      i32 mu_q20, u32 survivors, u32 signal) noexcept;
// Gain indices of a frame coded on its own; the gains (Q16) come back quantized.
void silk_gains_quantize(i8* indices, i32* gains_q16, i8* previous) noexcept;
// What the noise-shaping quantizer needs of one frame: the predictors and gains as the decoder's integers,
// the shaping as analyzed.
struct SilkQuantizeFrame {
    const i16* lpc_q12; // [2][kSilkOrderMaximum]
    const i16* ltp_q14; // [kSilkSubframes][kLtpTaps]
    const SilkShape* shape;
    i32 gains_q16[kSilkSubframes];
    i32 lags[kSilkSubframes];
    i32 lambda_q10, ltp_scale_q14;
    float warping;
    u32 shape_order, states;
};
// Quantizes the frame x (PCM16 scale) into excitation pulses, choosing among `states` delayed decisions the
// one with the least shaped noise for its rate. Updates the seed index.
void silk_quantize(SilkQuantizerState* state, SilkIndices* indices, const i16* x, i8* pulses,
                   const SilkQuantizeFrame& frame) noexcept;

} // namespace maicord::opus
