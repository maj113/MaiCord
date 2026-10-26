#pragma once

#include "maicord/ui/geometry.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {
struct UiSettingsTokens;
struct UiSettingsLayoutTokens;

// Stateless settings controls: geometry, paint and hit-testing over resolved
// tokens. Callers own state, animation clocks and text emission.
struct UiControlState {
    bool enabled{true}, hovered{}, pressed{}, focused{};
};

// Concentric rings: a stroke of `width` whose inner edge sits `gap` outside a
// rounded shape, every corner sharing the shape's centre of curvature (ring
// radius = radius + gap + width). Focus rings use the focus gap and width.
UiRect ui_settings_ring_rect(const UiRect& shape, float gap, float width) noexcept;
bool ui_settings_ring_paint(PaintStream*, const UiRect& shape, float radius, float gap, float width, const Color&) noexcept;
bool ui_settings_focus_paint(PaintStream*, const UiSettingsTokens&, const UiRect& shape, float radius) noexcept;

// Switch: pill track with a white thumb. motion is the normalized time of a
// running toggle animation (1 when settled); the thumb squashes while pressed.
struct UiSwitchThumb {
    UiRect box;
    float radius;
};
UiSwitchThumb ui_switch_thumb(const UiSettingsLayoutTokens&, const UiRect& track, bool checked, float motion,
                              bool reduced_motion, bool pressed) noexcept;
bool ui_switch_paint(PaintStream*, const UiSettingsTokens&, const UiRect& track, bool checked, float motion,
                     bool reduced_motion, UiControlState) noexcept;

// Radio group: one option per line, indicator at the start.
float ui_radio_group_height(const UiSettingsLayoutTokens&, u32 count) noexcept;
UiRect ui_radio_option(const UiSettingsLayoutTokens&, const UiRect& group, u32 index) noexcept;
UiRect ui_radio_indicator(const UiSettingsLayoutTokens&, const UiRect& option) noexcept;
UiRect ui_radio_label(const UiSettingsLayoutTokens&, const UiRect& option) noexcept;
u32 ui_radio_hit(const UiSettingsLayoutTokens&, const UiRect& group, u32 count, float x, float y) noexcept;
bool ui_radio_paint(PaintStream*, const UiSettingsTokens&, const UiRect& indicator, bool selected,
                    UiControlState) noexcept;

// Slider with optional value marks above the bar; the default mark is tinted.
// Hovered, pressed or focused sliders paint a larger grabber and thicker bar
// (by slider_grabber_grow and slider_bar_grow) around the same centres.
struct UiSliderGeometry {
    UiRect bounds, bar, fill, grabber;
    float track_left, track_right;
    bool marks;
};
float ui_slider_height(const UiSettingsLayoutTokens&, bool marks) noexcept;
UiSliderGeometry ui_slider_geometry(const UiSettingsLayoutTokens&, const UiRect& bounds, u32 count, u32 index,
                                    bool marks) noexcept;
float ui_slider_position(const UiSliderGeometry&, u32 count, u32 index) noexcept;
u32 ui_slider_index_at(const UiSliderGeometry&, u32 count, float x) noexcept;
// Page Up/Down step: a tenth of the range, at least one step.
u32 ui_slider_page_step(u32 count) noexcept;
// A press on the grabber keeps its offset from the grabber's centre so the
// value does not jump; a press elsewhere on the track centres it (0).
float ui_slider_grab_offset(const UiRect& grabber, float x, float y) noexcept;
UiRect ui_slider_mark_label(const UiSettingsLayoutTokens&, const UiSliderGeometry&, u32 count, u32 index) noexcept;
// Meter slider (input sensitivity): the level fills the track, faint below the
// thumb and positive past it; no brand fill.
bool ui_slider_meter_paint(PaintStream*, const UiSettingsTokens&, const UiSliderGeometry&, u32 count, u32 level,
                           UiControlState) noexcept;
bool ui_slider_paint(PaintStream*, const UiSettingsTokens&, const UiSliderGeometry&, u32 count,
                     u32 default_index, UiControlState) noexcept;

// Input shell shared by select, text field and search. Focus (or an open
// select) draws a 2 DIP outline over the border: brand, or critical while the
// field shows an error.
bool ui_input_shell_paint(PaintStream*, const UiSettingsTokens&, const UiRect&, UiControlState,
                          bool error = false) noexcept;
UiRect ui_select_text_rect(const UiSettingsLayoutTokens&, const UiRect& field) noexcept;
bool ui_select_paint(PaintStream*, const UiSettingsTokens&, const UiRect& field, bool open, UiControlState,
                     bool error = false) noexcept;

enum class UiSettingsButtonVariant : u8 {
    secondary,
    primary,
    critical,
    critical_secondary,
    count
};
// Button sizes: sm is 32 DIP, md 40 DIP.
enum class UiSettingsButtonSize : u8 {
    compact,
    regular
};
float ui_settings_button_width(const UiSettingsLayoutTokens&, UiSettingsButtonSize, float label_width) noexcept;
bool ui_settings_button_paint(PaintStream*, const UiSettingsTokens&, const UiRect&, UiSettingsButtonVariant,
                              UiControlState) noexcept;
Color ui_settings_button_text(const UiSettingsTokens&, UiSettingsButtonVariant, UiControlState) noexcept;

// Theme swatch: rounded square with an inset ring; selection adds an outer
// brand ring and a check badge. A null gradient paints the flat fill.
UiRect ui_swatch_rect(const UiSettingsLayoutTokens&, const UiRect& field, u32 index, u32 columns) noexcept;
u32 ui_swatch_columns(const UiSettingsLayoutTokens&, float width) noexcept;
float ui_swatch_label_height() noexcept;
bool ui_swatch_paint(PaintStream*, const UiSettingsTokens&, const UiRect& swatch, const Color& fill,
                     const BackgroundTheme* gradient, bool selected, UiControlState) noexcept;

// Sidebar rows, search field and the circular close button.
bool ui_settings_nav_item_paint(PaintStream*, const UiSettingsTokens&, const UiRect&, bool selected, bool destructive,
                                UiControlState) noexcept;
Color ui_settings_nav_item_text(const UiSettingsTokens&, bool selected, bool destructive,
                                UiControlState) noexcept;
UiRect ui_settings_nav_icon_rect(const UiSettingsLayoutTokens&, const UiRect& item) noexcept;
UiRect ui_settings_nav_label_rect(const UiSettingsLayoutTokens&, const UiRect& item, bool icon) noexcept;
bool ui_settings_separator_paint(PaintStream*, const UiSettingsTokens&, const UiRect& row) noexcept;
bool ui_settings_search_paint(PaintStream*, const UiSettingsTokens&, const UiRect& field, bool has_text,
                              UiControlState) noexcept;
UiRect ui_settings_search_text_rect(const UiSettingsLayoutTokens&, const UiRect& field) noexcept;
UiRect ui_settings_search_clear_rect(const UiSettingsLayoutTokens&, const UiRect& field) noexcept;
UiRect ui_settings_close_rect(const UiSettingsLayoutTokens&, float center_x, float top) noexcept;
UiRect ui_settings_close_caption_rect(const UiSettingsLayoutTokens&, const UiRect& circle) noexcept;
bool ui_settings_close_paint(PaintStream*, const UiSettingsTokens&, const UiRect& circle, UiControlState) noexcept;
// Dialog close: a bare 24 DIP glyph centred in the target, with a hover fill.
bool ui_settings_dialog_close_paint(PaintStream*, const UiSettingsTokens&, const UiRect& target, UiControlState) noexcept;
Color ui_settings_close_color(const UiSettingsTokens&, UiControlState) noexcept;

// Segmented options: square buttons at a row's end.
float ui_segments_width(const UiSettingsLayoutTokens&, u32 count) noexcept;
UiRect ui_segments_option(const UiSettingsLayoutTokens&, const UiRect& group, u32 count, u32 index) noexcept;
u32 ui_segments_hit(const UiSettingsLayoutTokens&, const UiRect& group, u32 count, float x, float y) noexcept;
bool ui_segments_paint(PaintStream*, const UiSettingsTokens&, const UiRect& group, u32 count, u32 selected, u32 hovered,
                       UiControlState) noexcept;

// Tri-state permission switch: deny / neutral / allow as three joined
// segments in one bordered group (tristate_segment by segment_size each).
// The selected segment fills critical, neutral or positive; the others tint
// their glyph. Every point of the group resolves to a segment.
inline constexpr u32 kUiTristateCount = 3;
float ui_tristate_width(const UiSettingsLayoutTokens&) noexcept;
float ui_tristate_radius(const UiSettingsLayoutTokens&) noexcept;
UiRect ui_tristate_option(const UiSettingsLayoutTokens&, const UiRect& group, u32 index) noexcept;
u32 ui_tristate_hit(const UiSettingsLayoutTokens&, const UiRect& group, float x, float y) noexcept;
bool ui_tristate_paint(PaintStream*, const UiSettingsTokens&, const UiRect& group, u32 selected, u32 hovered,
                       UiControlState) noexcept;

// Compact colour grid: small rounded squares; the selected one shows a check.
u32 ui_color_grid_columns(const UiSettingsLayoutTokens&, float width) noexcept;
float ui_color_grid_height(const UiSettingsLayoutTokens&, u32 count, float width) noexcept;
UiRect ui_color_grid_cell(const UiSettingsLayoutTokens&, const UiRect& field, u32 index) noexcept;
u32 ui_color_grid_hit(const UiSettingsLayoutTokens&, const UiRect& field, u32 count, float x, float y) noexcept;
bool ui_color_grid_paint(PaintStream*, const UiSettingsTokens&, const UiRect& cell, const Color& fill, bool selected,
                         UiControlState) noexcept;

// List row decorations: drag handle, colour dot, initials avatar and chips.
// Chips wrap greedily inside width; widths are estimated like other labels.
// With dots, every chip except a "+N" overflow chip reserves a colour dot.
bool ui_admin_chip_dot(const Utf8View& label, bool dots) noexcept;
float ui_admin_chip_width(const UiSettingsLayoutTokens&, const Utf8View& label, bool dot) noexcept;
u32 ui_admin_chip_rows(const UiSettingsLayoutTokens&, const ConstSlice<Utf8View>& labels, bool dots, float width) noexcept;
float ui_admin_chips_height(const UiSettingsLayoutTokens&, u32 rows) noexcept;
UiRect ui_admin_chip_rect(const UiSettingsLayoutTokens&, const ConstSlice<Utf8View>& labels, bool dots, const UiRect& area,
                          u32 index) noexcept;
bool ui_admin_chip_paint(PaintStream*, const UiSettingsTokens&, const UiRect& chip, const Color& dot, bool has_dot,
                         float alpha) noexcept;
UiRect ui_admin_chip_label_rect(const UiSettingsLayoutTokens&, const UiRect& chip, bool dot) noexcept;
bool ui_admin_grip_paint(PaintStream*, const UiSettingsTokens&, const UiRect& lead, bool hovered, bool enabled) noexcept;
bool ui_admin_dot_paint(PaintStream*, const UiSettingsTokens&, const UiRect& lead, const Color& color, float alpha) noexcept;
bool ui_admin_avatar_paint(PaintStream*, const UiSettingsTokens&, const UiRect& avatar, const Color& fill, float alpha) noexcept;
// Initials of up to two words, uppercase ASCII where possible; output is borrowed from storage.
Utf8View ui_admin_initials(const Utf8View& label, Slice<char> storage) noexcept;

// Layered soft shadow approximated with translucent rounded rectangles.
bool ui_settings_shadow_paint(PaintStream*, const UiRect&, float radius, float spread, float alpha) noexcept;

// Text metrics used by settings layout. Without a shaping backend at layout
// time, widths are estimated from per-character advances of a UI sans face.
inline constexpr float kUiSettingsLabelEm = 16, kUiSettingsLabelLine = 20;
inline constexpr float kUiSettingsDescriptionEm = 13, kUiSettingsDescriptionLine = 18;
inline constexpr float kUiSettingsCaptionLine = 16;
float ui_settings_text_width(const Utf8View& text, float em) noexcept;
u32 ui_settings_text_lines(const Utf8View& text, float em, float width) noexcept;

} // namespace maicord
