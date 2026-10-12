#pragma once

#include "maicord/ui/geometry.hpp"

namespace maicord {

inline constexpr float kUiTextFlowItemLayoutHeight = 4'096.0f;
inline constexpr float kUiTextFlowOverflowTolerance = 1.0f / 64.0f;

enum class UiTextFlowError : u8 {
    none,
    invalid_input,
    invalid_state,
    invalid_item,
    coordinate_overflow,
};

struct UiTextFlowLayout {
    UiRect content_rectangle;
    UiRect clip_rectangle;
    float start_y;
    float cursor_y;
    u32 item_count;
    u32 spacer_count;
    UiTextFlowError last_error;
    bool item_open;
    bool overflow;
    bool active;
};

bool ui_text_flow_initialize(UiTextFlowLayout* flow, UiRect content_rectangle,
                             UiRect clip_rectangle) noexcept;
bool ui_text_flow_begin_item(UiTextFlowLayout* flow, float left_inset, float right_inset,
                             UiRect* rectangle) noexcept;
bool ui_text_flow_commit_item(UiTextFlowLayout* flow, float measured_height, float minimum_height,
                              float gap_after) noexcept;
bool ui_text_flow_advance(UiTextFlowLayout* flow, float extent) noexcept;
bool ui_text_flow_ensure_extent(UiTextFlowLayout* flow, float minimum_extent) noexcept;
float ui_text_flow_required_height(const UiTextFlowLayout& flow) noexcept;

} // namespace maicord
