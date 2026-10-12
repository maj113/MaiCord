#include "maicord/media/h264_references.hpp"
#include "maicord/media/h264_output_queue.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
H264Sps parameters(u8 type = 0) noexcept {
    H264Sps s{};
    s.frame_num_bits = 4; s.poc_bits = 4; s.poc_type = type; s.max_references = 4;
    s.frame_only = true;
    return s;
}
H264Slice picture(u16 frame, u16 lsb, bool reference = true) noexcept {
    H264Slice p{};
    p.frame_num = frame; p.poc_lsb = lsb; p.reference = reference ? 3 : 0;
    p.type = reference ? H264SliceType::p : H264SliceType::b;
    return p;
}
H264Slice idr() noexcept {
    H264Slice p = picture(0, 0); p.idr = true; p.type = H264SliceType::i; return p;
}
bool accept(H264PocState* state, const H264Sps& sps, const H264Slice& slice, i32 top, i32 bottom) noexcept {
    H264PictureOrder order{};
    return h264_picture_order(*state, sps, slice, &order) == H264Error::none &&
        order.top == top && order.bottom == bottom &&
        h264_picture_order_commit(state, slice, &order) == H264Error::none;
}
bool reference_accept(H264ReferenceState* state, const H264Sps& sps, const H264Slice& slice, u8 surface) noexcept {
    H264ReferencePlan plan{};
    if (h264_reference_plan(*state, sps, slice, surface, &plan) != H264Error::none) return false;
    *state = plan.after; return true;
}
}

int h264_state_tests() noexcept {
    H264Sps sps = parameters();
    H264PocState state{};
    // Decode order I0 P6 B2 B4 P12 B8 B10 P18 (POC LSB wrap).
    if (!accept(&state, sps, idr(), 0, 0) ||
        !accept(&state, sps, picture(1, 6), 6, 6) ||
        !accept(&state, sps, picture(2, 2, false), 2, 2) ||
        !accept(&state, sps, picture(2, 4, false), 4, 4) ||
        !accept(&state, sps, picture(2, 12), 12, 12) ||
        !accept(&state, sps, picture(3, 8, false), 8, 8) ||
        !accept(&state, sps, picture(3, 10, false), 10, 10) ||
        !accept(&state, sps, picture(3, 2), 18, 18)) return 40;
    // Exactly half the modulus wraps in the decreasing direction only.
    state = {0, 8, 0, 1, true};
    if (!accept(&state, sps, picture(2, 0), 16, 16)) return 41;
    state = {0, 0, 0, 1, true};
    if (!accept(&state, sps, picture(2, 8), 8, 8)) return 42;
    state = {};
    sps = parameters(1); sps.cycle_count = 2; sps.cycle_offsets[0] = 2;
    sps.cycle_offsets[1] = 3; sps.cycle_delta = 5; sps.offset_non_reference = -1;
    sps.offset_top_to_bottom = 1;
    if (!accept(&state, sps, idr(), 0, 1) || !accept(&state, sps, picture(1, 0), 2, 3) ||
        !accept(&state, sps, picture(2, 0, false), 1, 2) ||
        !accept(&state, sps, picture(2, 0), 5, 6)) return 43;
    state = {0, 0, 0, 15, true};
    if (!accept(&state, sps, picture(0, 0), 40, 41)) return 44;
    state = {}; sps = parameters(2);
    if (!accept(&state, sps, idr(), 0, 0) || !accept(&state, sps, picture(1, 0), 2, 2) ||
        !accept(&state, sps, picture(2, 0, false), 3, 3)) return 45;
    state = {0, 0, 0, 15, true};
    if (!accept(&state, sps, picture(0, 0), 32, 32)) return 46;

    // MMCO5 renumbers the retained picture and the following frame history.
    sps = parameters(); state = {0, 6, 0, 3, true};
    H264Slice reset = picture(4, 8); reset.delta_bottom = -2;
    reset.adaptive_marking = true; reset.marking_count = 1; reset.marking[0].operation = 5;
    H264PictureOrder order{};
    if (h264_picture_order(state, sps, reset, &order) != H264Error::none || order.top != 8 || order.bottom != 6 ||
        h264_picture_order_commit(&state, reset, &order) != H264Error::none ||
        order.top != 2 || order.bottom || state.frame_num || state.frame_offset || state.reference_lsb != 2 ||
        !accept(&state, sps, picture(1, 4), 4, 4)) return 47;
    // Arithmetic failure must not mutate output or history.
    const H264PocState saved = state;
    H264PictureOrder sentinel{1,2,3,4}; order = sentinel;
    H264Slice overflow = picture(2, 6); overflow.delta_bottom = 2'147'483'647;
    if (h264_picture_order(state, sps, overflow, &order) != H264Error::malformed ||
        !bytes_equal(&sentinel, &order, sizeof(order)) || !bytes_equal(&saved, &state, sizeof(state))) return 48;
    state = {2'147'483'640, 12, 0, 3, true};
    if (h264_picture_order(state, sps, picture(4, 0), &order) != H264Error::malformed) return 49;

    H264ReferenceState refs{};
    sps = parameters(2); sps.max_references = 2;
    if (!reference_accept(&refs, sps, idr(), 0) ||
        !reference_accept(&refs, sps, picture(1, 0), 1) ||
        !reference_accept(&refs, sps, picture(2, 0, false), 2) || refs.count != 2 ||
        !reference_accept(&refs, sps, picture(2, 0), 2) || refs.count != 2 ||
        refs.pictures[0].frame_num != 1 || refs.pictures[1].frame_num != 2) return 50;
    // Long-term conversion/removal and replacement; all six MMCO operations.
    H264Slice adaptive = picture(3, 0); adaptive.adaptive_marking = true;
    adaptive.marking_count = 3;
    adaptive.marking[0] = {0, 2, 4}; // allow indices 0..1
    adaptive.marking[1] = {1, 0, 1}; // remove frame 1
    adaptive.marking[2] = {0, 1, 3}; // frame 2 -> long index 1
    if (!reference_accept(&refs, sps, adaptive, 0) || !refs.pictures[0].long_term() ||
        refs.pictures[0].long_term_index != 1) return 51;
    adaptive = picture(4, 0); adaptive.adaptive_marking = true; adaptive.marking_count = 2;
    adaptive.marking[0] = {0, 1, 2}; // remove long index 1
    adaptive.marking[1] = {0, 0, 6}; // current -> long index 0
    if (!reference_accept(&refs, sps, adaptive, 1) || refs.count != 2 ||
        !refs.pictures[1].long_term() || refs.pictures[1].long_term_index != 0) return 52;
    adaptive = picture(5, 0); adaptive.adaptive_marking = true; adaptive.marking_count = 1;
    adaptive.marking[0] = {0, 0, 5};
    if (!reference_accept(&refs, sps, adaptive, 2) || refs.count != 1 || refs.poc.frame_num ||
        refs.pictures[0].frame_num || refs.max_long_term_plus1 ||
        !reference_accept(&refs, sps, picture(1, 0), 0)) return 53;
    const auto saved_refs = refs;
    H264ReferencePlan plan{}, saved_plan{};
    plan.after.count = saved_plan.after.count = 13;
    if (h264_reference_plan(refs, sps, picture(2, 0), 0, &plan) != H264Error::invalid_input ||
        !bytes_equal(&plan, &saved_plan, sizeof(plan)) || !bytes_equal(&refs, &saved_refs, sizeof(refs))) return 54;
    if (h264_reference_plan(refs, sps, picture(4, 0), 1, &plan) != H264Error::malformed) return 55;
    sps.gaps = true;
    if (h264_reference_plan(refs, sps, picture(4, 0), 1, &plan) != H264Error::unsupported) return 56;
    sps.gaps = false;
    // A bounded sliding window across several frame_num wraps, reusing only
    // surfaces no longer referenced. This does not test display leases.
    refs = {};
    if (!reference_accept(&refs, sps, idr(), 0)) return 57;
    for (u32 n = 1; n < 100; ++n) {
        if (!reference_accept(&refs, sps, picture(static_cast<u16>(n % 16), 0), static_cast<u8>(n % 3)) ||
            refs.count != 2 || refs.pictures[1].top != static_cast<i32>(n * 2)) return 58;
    }
    // A new IDR discards references and may reuse a reference surface only
    // because the old picture is no longer an input to this decode.
    H264Slice start = idr(); start.long_term = true; start.discard_prior = true;
    if (h264_reference_plan(refs, sps, start, refs.pictures[0].surface, &plan) != H264Error::none ||
        !plan.discard_prior || !plan.reset || plan.after.count != 1 ||
        !plan.after.pictures[0].long_term() || plan.after.max_long_term_plus1 != 1) return 59;
    // MMCO3 replaces an earlier array entry: erasing the previous long-term
    // owner must not shift the selected short-term index onto another picture.
    refs = {}; sps.max_references = 4;
    if (!reference_accept(&refs, sps, start, 0) ||
        !reference_accept(&refs, sps, picture(1, 0), 1) ||
        !reference_accept(&refs, sps, picture(2, 0), 2)) return 62;
    adaptive = picture(3, 0); adaptive.adaptive_marking = true; adaptive.marking_count = 1;
    adaptive.marking[0] = {0,0,3};
    if (!reference_accept(&refs, sps, adaptive, 3) || refs.count != 3 ||
        refs.pictures[0].frame_num != 1 || refs.pictures[1].frame_num != 2 ||
        refs.pictures[1].surface != 2 || !refs.pictures[1].long_term()) return 63;
    adaptive = picture(4, 0); adaptive.adaptive_marking = true; adaptive.marking_count = 1;
    adaptive.marking[0] = {0,0,6};
    if (!reference_accept(&refs, sps, adaptive, 0) || refs.count != 3 ||
        refs.pictures[2].frame_num != 4 || !refs.pictures[2].long_term()) return 64;
    adaptive = picture(5, 0); adaptive.adaptive_marking = true; adaptive.marking_count = 1;
    adaptive.marking[0] = {0,0,4};
    if (!reference_accept(&refs, sps, adaptive, 2) || refs.count != 3 || refs.max_long_term_plus1) return 65;
    for (u32 i = 0; i < refs.count; ++i) if (refs.pictures[i].long_term()) return 66;
    H264OutputQueue queue{};
    if (queue.leased != 255 || h264_free_surface(queue, 5, 4) != 1 ||
        !h264_output_push(&queue, {600, 6, 0, 800}) || !h264_output_push(&queue, {200, 2, 1, -200}) ||
        !h264_output_push(&queue, {400, 4, 2, 350}) || !h264_output_ready(queue, 0, 2, 4)) return 70;
    H264Output displayed{};
    if (!h264_output_pop(&queue, &displayed) || displayed.poc != 2 || displayed.duration_us != 200 ||
        displayed.surface != 1 || displayed.presentation_time_us != -200 || h264_free_surface(queue, 0, 4) != 3 ||
        h264_output_pop(&queue, &displayed) || h264_output_push(&queue, {100, 1, 1})) return 71;
    queue.leased = 255; // The renderer enqueued its copy before releasing.
    if (h264_free_surface(queue, 0, 4) != 1 || h264_free_surface(queue, 2, 4) != 3 ||
        h264_output_ready(queue, 0, 2, 4) || !h264_output_ready(queue, 8, 2, 2)) return 72;
    if (!h264_output_pop(&queue, &displayed) || displayed.poc != 4 || displayed.surface != 2 || displayed.presentation_time_us != 350) return 73;
    queue.leased = 255;
    if (!h264_output_pop(&queue, &displayed) || displayed.poc != 6 || displayed.presentation_time_us != 800 || queue.count ||
        h264_free_surface(queue, 0, 1) != 255) return 74; // Last output is still leased.
    queue = {};
    for (u8 i = 0; i < 17; ++i) if (!h264_output_push(&queue, {1, 16 - i, i})) return 75;
    const auto saved_queue = queue;
    if (h264_output_push(&queue, {1, 18, 17}) || !bytes_equal(&queue, &saved_queue, sizeof(queue)) ||
        !h264_output_ready(queue, 0, 16, 16)) return 76;
    for (i32 i = 0; i < 17; ++i) {
        if (!h264_output_pop(&queue, &displayed) || displayed.poc != i) return 77;
        queue.leased = 255;
    }
    return 0;
}

} // namespace maicord
