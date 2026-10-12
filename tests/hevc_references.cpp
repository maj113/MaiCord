#include "maicord/media/hevc_references.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
int hevc_reference_tests() noexcept {
    HevcSps sps{};
    sps.poc_bits = 4;
    sps.max_buffering = 4;
    HevcPicture picture{};
    picture.idr = picture.irap = picture.reference = true;
    picture.nal_type = 19;
    HevcReferenceState state{};
    HevcReferencePlan plan{};
    if (hevc_reference_plan(state, sps, picture, 0, &plan) != HevcError::none || plan.poc ||
        plan.after.count != 1)
        return 1;
    state = plan.after;
    picture = {};
    picture.nal_type = 1;
    picture.reference = true;
    picture.poc_lsb = 4;
    picture.short_term.negative = 1;
    picture.short_term.delta[0] = -4;
    picture.short_term.used = 1;
    if (hevc_reference_plan(state, sps, picture, 1, &plan) != HevcError::none || plan.poc != 4 ||
        plan.before_count != 1 || plan.before[0] != 0 || plan.after.count != 2)
        return 2;
    state = plan.after;
    picture.reference = false;
    picture.poc_lsb = 2;
    picture.short_term.positive = 1;
    picture.short_term.delta[0] = -2;
    picture.short_term.delta[1] = 2;
    picture.short_term.used = 3;
    if (hevc_reference_plan(state, sps, picture, 2, &plan) != HevcError::none || plan.poc != 2 ||
        plan.before_count != 1 || plan.following_count != 1 || plan.following[0] != 1 ||
        plan.after.count != 2)
        return 3;
    const auto saved = plan;
    picture.short_term.delta[1] = 3;
    if (hevc_reference_plan(state, sps, picture, 2, &plan) != HevcError::malformed ||
        !bytes_equal(&saved, &plan, sizeof(plan)))
        return 4;
    picture = {};
    picture.nal_type = 1;
    picture.reference = true;
    picture.poc_lsb = 1;
    state = {};
    state.ready = true;
    state.previous_lsb = 14;
    state.pictures[0] = {14, 0, false};
    state.count = 1;
    picture.short_term.negative = 1;
    picture.short_term.delta[0] = -3;
    picture.short_term.used = 1;
    if (hevc_reference_plan(state, sps, picture, 1, &plan) != HevcError::none || plan.poc != 17 ||
        plan.after.previous_msb != 16)
        return 5;
    picture = {};
    picture.nal_type = 1;
    picture.poc_lsb = 3;
    picture.long_term_count = 1;
    picture.long_term[0] = {1, 14, true, true};
    state.previous_lsb = 1;
    state.previous_msb = 16;
    if (hevc_reference_plan(state, sps, picture, 2, &plan) != HevcError::none || plan.poc != 19 ||
        plan.long_count != 1 || !plan.after.pictures[0].long_term)
        return 6;
    picture.long_term[0].delta_msb_present = false;
    state.pictures[1] = {30, 1, true};
    state.count = 2;
    if (hevc_reference_plan(state, sps, picture, 2, &plan) != HevcError::malformed)
        return 7;
    sps.max_buffering = 16;
    if (hevc_reference_plan(state, sps, picture, 2, &plan) != HevcError::invalid_input)
        return 8;
    return 0;
}
} // namespace maicord
