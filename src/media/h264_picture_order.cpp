#include "maicord/media/h264_picture_order.hpp"

namespace maicord {
namespace {
constexpr bool fits(i64 n) noexcept { return n >= (-2'147'483'647ll - 1) && n <= 2'147'483'647; }
}

H264Error h264_picture_order(const H264PocState& state, const H264Sps& sps,
    const H264Slice& slice, H264PictureOrder* output) noexcept {
    if (!output) return H264Error::invalid_input;
    if (!state.ready && !slice.idr) return H264Error::unsupported;
    const u32 max_frame = 1u << sps.frame_num_bits;
    if (slice.frame_num >= max_frame || (slice.idr && (slice.frame_num || !slice.reference)))
        return H264Error::malformed;
    i64 frame_offset = slice.idr ? 0 : state.frame_offset;
    if (!slice.idr && state.frame_num > slice.frame_num) frame_offset += max_frame;
    const i64 absolute_frame = frame_offset + slice.frame_num;
    if (frame_offset < 0 || !fits(absolute_frame)) return H264Error::malformed;
    i64 top = 0, bottom = 0, msb = 0;
    if (sps.poc_type == 0) {
        const i32 modulus = 1 << sps.poc_bits;
        if (slice.poc_lsb >= modulus) return H264Error::malformed;
        const i64 previous_lsb = slice.idr ? 0 : state.reference_lsb;
        msb = slice.idr ? 0 : state.reference_msb;
        const i64 delta = static_cast<i64>(slice.poc_lsb) - previous_lsb;
        if (delta < 0 && -delta >= modulus / 2) msb += modulus;
        else if (delta > modulus / 2) msb -= modulus;
        top = msb + slice.poc_lsb;
        bottom = top + slice.delta_bottom;
    } else if (sps.poc_type == 1) {
        i64 absolute = sps.cycle_count ? absolute_frame : 0;
        if (!slice.reference && absolute > 0) --absolute;
        i64 expected = 0;
        if (absolute > 0) {
            // Dividing before multiplying keeps the parsed 255-entry signed
            // cycle sum times frame count within i64 for every valid i32 input.
            expected = ((absolute - 1) / sps.cycle_count) * sps.cycle_delta;
            const u32 end = static_cast<u32>((absolute - 1) % sps.cycle_count);
            for (u32 i = 0; i <= end; ++i) expected += sps.cycle_offsets[i];
        }
        if (!slice.reference) expected += sps.offset_non_reference;
        top = expected + slice.delta_poc0;
        bottom = top + sps.offset_top_to_bottom + slice.delta_poc1;
    } else {
        top = slice.idr ? 0 : 2 * absolute_frame - (slice.reference ? 0 : 1);
        bottom = top;
    }
    if (!fits(top) || !fits(bottom) || !fits(msb)) return H264Error::malformed;
    *output = {static_cast<i32>(top), static_cast<i32>(bottom),
        static_cast<i32>(msb), static_cast<i32>(frame_offset)};
    return H264Error::none;
}

H264Error h264_picture_order_commit(H264PocState* state, const H264Slice& slice,
    H264PictureOrder* picture) noexcept {
    if (!state || !picture || slice.marking_count > 32) return H264Error::invalid_input;
    bool reset = false;
    for (u32 i = 0; i < slice.marking_count; ++i) reset |= slice.marking[i].operation == 5;
    H264PictureOrder order = *picture;
    H264PocState next = *state;
    if (reset) {
        if (!slice.reference || slice.idr || !slice.adaptive_marking) return H264Error::malformed;
        const i64 origin = order.top < order.bottom ? order.top : order.bottom;
        const i64 top = order.top - origin, bottom = order.bottom - origin;
        if (!fits(top) || !fits(bottom)) return H264Error::malformed;
        order.top = static_cast<i32>(top); order.bottom = static_cast<i32>(bottom);
    }
    next.frame_num = reset ? 0 : slice.frame_num;
    next.frame_offset = reset ? 0 : order.frame_offset;
    next.ready = true;
    if (slice.reference) {
        next.reference_msb = reset ? 0 : order.msb;
        next.reference_lsb = reset ? order.top : slice.poc_lsb;
    }
    *state = next; *picture = order;
    return H264Error::none;
}

} // namespace maicord
