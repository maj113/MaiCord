#pragma once

#include "maicord/media/vp8.hpp"
#include "maicord/media/webp_alpha.hpp"

namespace maicord {

enum class LossyWebpError : u8 {
    none,
    invalid_input,
    container,
    unsupported_codec,
    vp8_envelope,
    vp8_control,
    vp8_modes,
    vp8_coefficients,
    alpha,
};

struct LossyWebpDecodeResult {
    WebpParseResult parse;
    WebpAlphaDecodeResult alpha;
    Vp8ParseResult envelope;
    Vp8ControlParseResult control;
    Vp8ModeParseResult modes;
    Vp8CoefficientParseResult coefficients;
    u32 alpha_bytes;
    LossyWebpError error;
    u8 reserved[3];

    constexpr bool ok() const noexcept {
        return error == LossyWebpError::none;
    }
};

struct LossyWebpWorkspace {
    Vp8CoefficientWorkspace coefficients;
    Vp8ModeWorkspace modes;
    Vp8ProbabilityWorkspace probabilities;
};

// Parses one complete still WebP document, entropy-decodes its VP8 keyframe
// into caller-owned mode/coefficient records, and decodes an optional external
// ALPH plane. The VP8 pixel reconstruction and color conversion remain GPU
// work; no software RGBA fallback is produced here.
LossyWebpDecodeResult lossy_webp_decode_keyframe(
    ConstSlice<u8> encoded, WebpImage* image, Vp8Keyframe* frame,
    Vp8ControlHeader* header, Vp8ProbabilityWorkspace* probabilities,
    Vp8ModeWorkspace* modes, Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace, Slice<u8> alpha_rgba_scratch,
    Slice<u8> alpha_output) noexcept;

// Decodes the already container-validated VP8/optional ALPH child payloads
// of one ANMF frame. Expected dimensions bind both bitstreams to the frame
// control rectangle; pixel reconstruction remains GPU-only.
LossyWebpDecodeResult lossy_webp_decode_frame_keyframe(
    ConstSlice<u8> vp8_payload, ConstSlice<u8> alpha_payload,
    u32 expected_width, u32 expected_height, Vp8Keyframe* frame,
    Vp8ControlHeader* header, Vp8ProbabilityWorkspace* probabilities,
    Vp8ModeWorkspace* modes, Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace, Slice<u8> alpha_rgba_scratch,
    Slice<u8> alpha_output) noexcept;

static_assert(sizeof(LossyWebpDecodeResult) == 48);
static_assert(sizeof(LossyWebpWorkspace) == 810'624);

} // namespace maicord
