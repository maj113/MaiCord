#include "maicord/media/image/lossy_webp.hpp"
// Once per image, around the entropy decode of vp8.cpp: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

inline LossyWebpDecodeResult failure(LossyWebpDecodeResult result,
                              LossyWebpError error) noexcept {
    result.error = error;
    return result;
}

// The payloads come from webp_parse or animated_webp_parse over the same bytes, so the VP8 payload is in bounds
// and an ALPH payload exists only where the image declares alpha; the media pipeline then passes the lossless
// workspace and the alpha storage.
LossyWebpDecodeResult decode_payloads(
    LossyWebpDecodeResult result, ConstSlice<u8> vp8_payload,
    ConstSlice<u8> alpha_payload, u32 expected_width,
    u32 expected_height, Vp8Keyframe* frame, Vp8ControlHeader* header,
    Vp8ProbabilityWorkspace* probabilities, Vp8ModeWorkspace* modes,
    Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace,
    Slice<u8> alpha_rgba_scratch, Slice<u8> alpha_output) noexcept {
    result.envelope = vp8_parse_keyframe(vp8_payload, frame);
    if (!result.envelope.ok() || frame->width != expected_width ||
        frame->height != expected_height) {
        return failure(result, LossyWebpError::vp8_envelope);
    }
    result.control =
        vp8_parse_keyframe_control(*frame, header, probabilities);
    if (!result.control.ok()) {
        return failure(result, LossyWebpError::vp8_control);
    }
    result.coefficients = vp8_decode_keyframe_macroblocks(
        *frame, *header, *probabilities, modes, coefficients);
    if (!result.coefficients.ok()) {
        return failure(result, LossyWebpError::vp8_coefficients);
    }
    if (alpha_payload.count != 0) {
        result.alpha = webp_alpha_decode(
            alpha_payload, expected_width, expected_height,
            alpha_lossless_workspace, alpha_rgba_scratch, alpha_output);
        if (!result.alpha.ok()) {
            return failure(result, LossyWebpError::alpha);
        }
        result.alpha_bytes = result.alpha.alpha_bytes;
    }
    result.error = LossyWebpError::none;
    return result;
}

} // namespace

// The media pipeline calls this only for a document that webp_parse found lossy (decode_webp_bytes).
LossyWebpDecodeResult lossy_webp_decode_keyframe(
    ConstSlice<u8> encoded, WebpImage* image, Vp8Keyframe* frame,
    Vp8ControlHeader* header, Vp8ProbabilityWorkspace* probabilities,
    Vp8ModeWorkspace* modes, Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace, Slice<u8> alpha_rgba_scratch,
    Slice<u8> alpha_output) noexcept {
    LossyWebpDecodeResult result{};
    result.parse = webp_parse(encoded, image);
    if (!result.parse.ok()) {
        return failure(result, LossyWebpError::container);
    }
    return decode_payloads(
        result, image->bitstream, image->alpha_bitstream,
        image->width, image->height, frame, header, probabilities, modes,
        coefficients, alpha_lossless_workspace, alpha_rgba_scratch,
        alpha_output);
}

LossyWebpDecodeResult lossy_webp_decode_frame_keyframe(
    ConstSlice<u8> vp8_payload, ConstSlice<u8> alpha_payload,
    u32 expected_width, u32 expected_height, Vp8Keyframe* frame,
    Vp8ControlHeader* header, Vp8ProbabilityWorkspace* probabilities,
    Vp8ModeWorkspace* modes, Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace, Slice<u8> alpha_rgba_scratch,
    Slice<u8> alpha_output) noexcept {
    return decode_payloads(
        LossyWebpDecodeResult{}, vp8_payload, alpha_payload,
        expected_width, expected_height, frame, header, probabilities,
        modes, coefficients, alpha_lossless_workspace, alpha_rgba_scratch,
        alpha_output);
}

} // namespace maicord
#pragma clang attribute pop
