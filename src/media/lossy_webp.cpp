#include "maicord/media/lossy_webp.hpp"

namespace maicord {
namespace {

LossyWebpDecodeResult failure(LossyWebpDecodeResult result,
                              LossyWebpError error) noexcept {
    result.error = error;
    return result;
}

LossyWebpDecodeResult decode_payloads(
    LossyWebpDecodeResult result, ConstSlice<u8> vp8_payload,
    ConstSlice<u8> alpha_payload, u32 expected_width,
    u32 expected_height, Vp8Keyframe* frame, Vp8ControlHeader* header,
    Vp8ProbabilityWorkspace* probabilities, Vp8ModeWorkspace* modes,
    Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace,
    Slice<u8> alpha_rgba_scratch, Slice<u8> alpha_output) noexcept {
    if (!frame || !header || !probabilities || !modes || !coefficients ||
        !vp8_payload.data || vp8_payload.count == 0 ||
        (!alpha_payload.data && alpha_payload.count != 0) ||
        expected_width == 0 || expected_height == 0 ||
        expected_width > kWebpDimensionMaximum ||
        expected_height > kWebpDimensionMaximum) {
        return failure(result, LossyWebpError::invalid_input);
    }
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
    result.modes = vp8_parse_keyframe_modes(*frame, *header, modes);
    if (!result.modes.ok()) {
        return failure(result, LossyWebpError::vp8_modes);
    }
    result.coefficients = vp8_decode_keyframe_coefficients(
        *frame, *header, *probabilities, *modes, coefficients);
    if (!result.coefficients.ok()) {
        return failure(result, LossyWebpError::vp8_coefficients);
    }
    if (alpha_payload.count != 0) {
        if (!alpha_lossless_workspace || !alpha_rgba_scratch.data ||
            !alpha_output.data) {
            return failure(result, LossyWebpError::invalid_input);
        }
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

LossyWebpDecodeResult lossy_webp_decode_keyframe(
    ConstSlice<u8> encoded, WebpImage* image, Vp8Keyframe* frame,
    Vp8ControlHeader* header, Vp8ProbabilityWorkspace* probabilities,
    Vp8ModeWorkspace* modes, Vp8CoefficientWorkspace* coefficients,
    Vp8lWorkspace* alpha_lossless_workspace, Slice<u8> alpha_rgba_scratch,
    Slice<u8> alpha_output) noexcept {
    LossyWebpDecodeResult result{};
    if (!image || !frame || !header || !probabilities || !modes ||
        !coefficients || (!encoded.data && encoded.count != 0)) {
        return failure(result, LossyWebpError::invalid_input);
    }
    result.parse = webp_parse(encoded, image);
    if (!result.parse.ok()) {
        return failure(result, LossyWebpError::container);
    }
    if (image->codec != WebpCodec::vp8_lossy) {
        return failure(result, LossyWebpError::unsupported_codec);
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
