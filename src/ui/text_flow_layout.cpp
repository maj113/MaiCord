#include "maicord/ui/text_flow_layout.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool fail(UiTextFlowLayout* flow, UiTextFlowError error) noexcept {
    if (flow && flow->last_error == UiTextFlowError::none) {
        flow->last_error = error;
    }
    return false;
}

bool advance_cursor(UiTextFlowLayout* flow, float extent) noexcept {
    if (!ui_extent_valid(extent)) {
        return fail(flow, UiTextFlowError::invalid_item);
    }
    const float height = flow->required_height + extent;
    const float next = flow->content_rectangle.top + height;
    if (!ui_coordinate_valid(next) || height < flow->required_height || next < flow->cursor_y) {
        return fail(flow, UiTextFlowError::coordinate_overflow);
    }
    flow->required_height = height;
    flow->cursor_y = next;
    flow->overflow = flow->overflow || next > flow->content_rectangle.bottom + kUiTextFlowOverflowTolerance;
    return true;
}

} // namespace

bool ui_text_flow_initialize(UiTextFlowLayout* flow, UiRect content_rectangle,
                             UiRect clip_rectangle) noexcept {
    if (!flow || !ui_rect_valid(content_rectangle) || !ui_rect_valid(clip_rectangle)) {
        if (flow) {
            reset_value<UiTextFlowLayout>(flow);
            flow->last_error = UiTextFlowError::invalid_input;
        }
        return false;
    }
    reset_value<UiTextFlowLayout>(flow);
    flow->content_rectangle = content_rectangle;
    flow->clip_rectangle = clip_rectangle;
    flow->cursor_y = content_rectangle.top;
    flow->active = true;
    return true;
}

bool ui_text_flow_begin_item(UiTextFlowLayout* flow, float left_inset, float right_inset,
                             UiRect* rectangle) noexcept {
    if (!flow || !rectangle || !flow->active) {
        return fail(flow, UiTextFlowError::invalid_input);
    }
    if (flow->item_open) {
        return fail(flow, UiTextFlowError::invalid_state);
    }
    if (!ui_extent_valid(left_inset) || !ui_extent_valid(right_inset)) {
        return fail(flow, UiTextFlowError::invalid_item);
    }
    const float left = flow->content_rectangle.left + left_inset;
    const float right = flow->content_rectangle.right - right_inset;
    const float bottom = minimum(kMaximumUiCoordinate, flow->cursor_y + kUiTextFlowItemLayoutHeight);
    const UiRect value{left, flow->cursor_y, right, bottom};
    if (!ui_rect_valid(value)) {
        return fail(flow, UiTextFlowError::invalid_item);
    }
    *rectangle = value;
    flow->item_open = true;
    return true;
}

bool ui_text_flow_commit_item(UiTextFlowLayout* flow, float measured_height, float minimum_height,
                              float gap_after) noexcept {
    if (!flow || !flow->active || !flow->item_open) {
        return fail(flow, UiTextFlowError::invalid_state);
    }
    if (!ui_extent_valid(measured_height) || !ui_extent_valid(minimum_height) ||
        !ui_extent_valid(gap_after)) {
        return fail(flow, UiTextFlowError::invalid_item);
    }
    if (!advance_cursor(flow, maximum(measured_height, minimum_height) + gap_after)) {
        return false;
    }
    flow->item_open = false;
    saturating_increment(flow->item_count);
    return true;
}

bool ui_text_flow_advance(UiTextFlowLayout* flow, float extent) noexcept {
    if (!flow || !flow->active || flow->item_open) {
        return fail(flow, UiTextFlowError::invalid_state);
    }
    if (!advance_cursor(flow, extent)) {
        return false;
    }
    saturating_increment_if(extent != 0.0f, flow->spacer_count);
    return true;
}

bool ui_text_flow_ensure_extent(UiTextFlowLayout* flow, float minimum_extent) noexcept {
    if (!flow || !flow->active || flow->item_open) {
        return fail(flow, UiTextFlowError::invalid_state);
    }
    if (!ui_extent_valid(minimum_extent)) {
        return fail(flow, UiTextFlowError::invalid_item);
    }
    const float current = ui_text_flow_required_height(*flow);
    return current >= minimum_extent ? true : ui_text_flow_advance(flow, minimum_extent - current);
}

float ui_text_flow_required_height(const UiTextFlowLayout& flow) noexcept {
    return flow.active ? flow.required_height : 0.0f;
}

} // namespace maicord
