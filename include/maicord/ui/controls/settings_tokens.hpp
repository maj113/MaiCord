#pragma once

#include "maicord/ui/theme.hpp"

namespace maicord {

// Resolved settings tokens: UiThemeSettings overrides applied over palette
// derived defaults. Plain values, recomputed on demand; no references kept.
struct UiSettingsColors {
    Color text_strong, text_default, text_subtle, nav_header, border_subtle, border_normal;
    Color mod_subtle, mod_normal, mod_strong, input_background, input_border;
    Color brand, brand_hover, brand_active, on_brand;
    Color critical, critical_hover, critical_text, critical_tint, positive, focus_ring, scrim, button_border;
    Color switch_track, switch_border, switch_border_hover, switch_border_on, thumb, slider_track;
    Color radio_border, radio_border_hover, modal_background, sidebar_background;
    // Selected navigation rows/tabs: mod_strong on flat themes, the emphasis
    // selected tint on gradient themes (accent treatment).
    Color nav_selected;
};

struct UiSettingsMetrics {
    float modal_max_width, modal_viewport_ratio, modal_ratio_threshold, modal_inset_top, modal_inset_side,
        modal_inset_bottom, modal_radius, modal_compact_width;
    float sidebar_width, sidebar_padding, sidebar_top, search_height, nav_header_height, nav_item_height,
        nav_item_radius, nav_item_gap, nav_item_padding, nav_icon, separator_margin;
    float panel_padding_x, panel_padding_top, panel_padding_bottom, panel_max_width, title_gap, category_gap,
        category_heading_gap, row_padding, label_gap, control_gap, field_gap;
    float switch_width, switch_height, switch_thumb, slider_bar, slider_grabber, slider_mark_height,
        radio_size, radio_gap, radio_spacing, control_height, control_radius, button_small, button_medium,
        button_padding, swatch, swatch_gap, close_size, close_caption_gap, notice_padding;
    // Administration rows: segmented options, colour grid, chips and row leads.
    float segment_size, segment_gap, color_cell, color_gap, chip_height, chip_gap, chip_padding, avatar_size,
        grip_width, accent_dot, lead_gap;
    // List rows: the lead glyph, the count column and a trailing icon button;
    // the tri-state switch's segment width.
    float list_icon, list_column, list_action, tristate_segment;
    // Rings around a control: keyboard focus, and the selection of a swatch
    // or colour cell. Each stroke's inner edge sits its gap outside the shape.
    float focus_gap, focus_width, ring_gap, ring_width;
    // Inner padding of text fields and selects (the theme's form field padding).
    float field_padding;
    // How much a hovered, dragged or focused slider's grabber and bar grow,
    // whatever their resting size.
    float slider_grabber_grow, slider_bar_grow;
};

struct UiSettingsDurations {
    u32 switch_ms, switch_reduced_ms, modal_enter_ms, flash_ms, flash_fade_ms;
};

struct UiSettingsLayoutTokens {
    UiSettingsMetrics metric;
    UiSettingsDurations duration;
    // Opacity of every disabled control.
    float disabled_alpha;
};

struct UiSettingsTokens : UiSettingsLayoutTokens {
    UiSettingsColors color;
    bool light;
};
static_assert(sizeof(UiSettingsTokens) ==
              sizeof(UiSettingsLayoutTokens) + sizeof(UiSettingsColors) + sizeof(float));

UiSettingsTokens ui_settings_tokens(const Theme& theme) noexcept;
Color ui_settings_focus_color(const Theme& theme) noexcept;
// Geometry and motion queries do not construct a paint palette.
UiSettingsLayoutTokens ui_settings_layout_tokens(const UiResolvedThemeStyles* styles) noexcept;
// Entrance progress (0..1) of every modal and dialog elapsed_ms after it
// opened: a spring (tension 1000, friction 48) over modal_enter_ms, so Settings
// and each dialog open on one timeline.
float ui_settings_modal_entrance(const UiSettingsDurations& duration, u64 elapsed_ms) noexcept;
Color ui_settings_color_unpack(u32 rgba) noexcept;
Color ui_settings_color_mix(const Color& a, const Color& b, float t) noexcept;
Color ui_settings_color_alpha(const Color& color, float alpha) noexcept;

} // namespace maicord
