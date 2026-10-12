#include "maicord/media/h264_references.hpp"

namespace maicord {
namespace {

i32 number(const H264Reference& picture, u32 frame, u32 modulus) noexcept {
    return static_cast<i32>(picture.frame_num) - (picture.frame_num > frame ? static_cast<i32>(modulus) : 0);
}
void erase(H264ReferenceState* state, u32 index) noexcept {
    for (u32 i = index + 1; i < state->count; ++i) state->pictures[i - 1] = state->pictures[i];
    state->pictures[--state->count] = {};
}
i32 short_index(const H264ReferenceState& state, i32 target, u32 frame, u32 modulus) noexcept {
    for (u32 i = 0; i < state.count; ++i)
        if (!state.pictures[i].long_term() && number(state.pictures[i], frame, modulus) == target)
            return static_cast<i32>(i);
    return -1;
}
i32 long_index(const H264ReferenceState& state, u8 target) noexcept {
    for (u32 i = 0; i < state.count; ++i)
        if (state.pictures[i].long_term() && state.pictures[i].long_term_index == target)
            return static_cast<i32>(i);
    return -1;
}
void clear_references(H264ReferenceState* state) noexcept {
    for (u32 i = 0; i < state->count; ++i) state->pictures[i] = {};
    state->count = 0; state->max_long_term_plus1 = 0;
}

} // namespace

H264Error h264_reference_plan(const H264ReferenceState& state, const H264Sps& sps,
    const H264Slice& slice, u8 surface, H264ReferencePlan* output) noexcept {
    if (!output || surface >= 32 || state.count > 16 || sps.max_references > 16 ||
        slice.marking_count > 32 || (!slice.adaptive_marking && slice.marking_count))
        return H264Error::invalid_input;
    if (!slice.reference && (slice.adaptive_marking || slice.marking_count)) return H264Error::malformed;
    H264ReferencePlan plan{};
    plan.after = state;
    auto& next = plan.after;
    H264Error error = h264_picture_order(state.poc, sps, slice, &plan.decode_order);
    if (error != H264Error::none) return error;
    const u32 modulus = 1u << sps.frame_num_bits;
    const u32 limit = sps.max_references ? sps.max_references : 1;
    if (!slice.idr) {
        if (!state.ready) return H264Error::unsupported;
        const u32 expected = (state.last_reference_frame + 1u) % modulus;
        if (slice.frame_num != state.last_reference_frame && slice.frame_num != expected)
            return sps.gaps ? H264Error::unsupported : H264Error::malformed;
        for (u32 i = 0; i < state.count; ++i)
            if (state.pictures[i].surface == surface) return H264Error::invalid_input;
    }
    bool current_long = false;
    u8 current_long_index = 0;
    if (slice.idr) {
        clear_references(&next);
        current_long = slice.long_term;
        next.max_long_term_plus1 = slice.long_term ? 1 : 0;
        plan.discard_prior = slice.discard_prior;
        plan.reset = true;
    } else if (slice.reference && slice.adaptive_marking) {
        bool saw_reset = false;
        for (u32 i = 0; i < slice.marking_count; ++i) {
            const auto item = slice.marking[i];
            const i32 target = static_cast<i32>(slice.frame_num) - item.difference_minus1 - 1;
            i32 index = -1;
            switch (item.operation) {
            case 1:
            case 3:
                if (item.difference_minus1 >= modulus) return H264Error::malformed;
                index = short_index(next, target, slice.frame_num, modulus);
                if (index < 0) return H264Error::malformed;
                if (item.operation == 1) erase(&next, static_cast<u32>(index));
                else {
                    if (item.long_term >= next.max_long_term_plus1) return H264Error::malformed;
                    const i32 old_long = long_index(next, item.long_term);
                    if (old_long >= 0) {
                        erase(&next, static_cast<u32>(old_long));
                        if (old_long < index) --index;
                    }
                    next.pictures[index].long_term_index = item.long_term;
                }
                break;
            case 2:
                index = long_index(next, item.long_term);
                if (index < 0) return H264Error::malformed;
                erase(&next, static_cast<u32>(index));
                break;
            case 4:
                if (item.long_term > limit) return H264Error::malformed;
                next.max_long_term_plus1 = item.long_term;
                for (u32 j = next.count; j > 0; --j)
                    if (next.pictures[j - 1].long_term() &&
                        next.pictures[j - 1].long_term_index >= item.long_term) erase(&next, j - 1);
                break;
            case 5:
                if (saw_reset) return H264Error::malformed;
                saw_reset = true;
                clear_references(&next); plan.reset = true;
                break;
            case 6:
                if (current_long || item.long_term >= next.max_long_term_plus1) return H264Error::malformed;
                current_long = true; current_long_index = item.long_term;
                index = long_index(next, item.long_term);
                if (index >= 0) erase(&next, static_cast<u32>(index));
                break;
            default: return H264Error::malformed;
            }
        }
    } else if (slice.reference && next.count == limit) {
        i32 oldest = -1, oldest_number = 0;
        for (u32 i = 0; i < next.count; ++i) if (!next.pictures[i].long_term()) {
            const i32 n = number(next.pictures[i], slice.frame_num, modulus);
            if (oldest < 0 || n < oldest_number) { oldest = static_cast<i32>(i); oldest_number = n; }
        }
        if (oldest < 0) return H264Error::malformed;
        erase(&next, static_cast<u32>(oldest));
    }
    plan.retained_order = plan.decode_order;
    error = h264_picture_order_commit(&next.poc, slice, &plan.retained_order);
    if (error != H264Error::none) return error;
    if (slice.reference) {
        if (next.count >= limit || (current_long && current_long_index >= next.max_long_term_plus1))
            return H264Error::malformed;
        const u16 frame = !slice.idr && plan.reset ? 0 : slice.frame_num;
        next.pictures[next.count++] = {plan.retained_order.top, plan.retained_order.bottom, frame,
            surface, current_long ? current_long_index : static_cast<u8>(255)};
        next.last_reference_frame = frame;
    }
    next.ready = true;
    *output = plan;
    return H264Error::none;
}

} // namespace maicord
