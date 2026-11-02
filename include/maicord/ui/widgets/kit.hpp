#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/widgets/icon.hpp"

// Shared settings and store controls: stateless geometry, painting, hit-testing and key handling over the
// settings tokens (ui/controls/settings_tokens.hpp). Widgets never emit text: they return label rectangles
// and the style words below; scenes emit through their own text pens. No allocation: arrays are the
// caller's. See local://prog/uikit-api.md for the contract with the settings scene and the store pages.
namespace maicord {
struct PopupItem;
struct UiTextMeasureCache;

// ---------------------------------------------------------------------------
// Text ramp (UiTextCommand::style words) and measuring.
inline constexpr u32 kUiKitTextBody = text_style_none;                                  // 16/20
inline constexpr u32 kUiKitTextBodyBold = text_style_bold;                              // 16/20 bold
inline constexpr u32 kUiKitTextSmall = text_style_subtext;                              // 13/18
inline constexpr u32 kUiKitTextSmallBold = text_style_subtext | text_style_bold;        // 13/18 bold
inline constexpr u32 kUiKitTextLabel = kTextStyleSizeCodeSm;                            // 14/18
inline constexpr u32 kUiKitTextLabelBold = kTextStyleSizeCodeSm | text_style_bold;      // 14/18 bold
inline constexpr u32 kUiKitTextEyebrow = kTextStyleSizeCodeXs | text_style_bold;        // 12/16 bold
inline constexpr u32 kUiKitTextHeading = text_style_heading_2 | text_style_bold;        // 20/24 bold
inline constexpr u32 kUiKitTextTitle = text_style_heading_1 | text_style_bold;          // 24/30 bold
inline constexpr u32 kUiKitTextMono = text_style_subtext | text_style_monospace;        // 13 mono

// Optional measuring hooks over the caller's text cache; null members use the per-character estimate the
// settings rows share (ui_settings_text_width).
struct UiKitMeasure {
    float (*width)(void* context, const Utf8View&, u32 style) noexcept;
    float (*height)(void* context, const Utf8View&, u32 style, float width) noexcept;
    // Wrapped height of text carrying style spans (a markdown block); null estimates it.
    float (*styled_height)(void* context, const Utf8View&, ConstSlice<TextStyleSpan>, float width) noexcept;
    void* context;
};
float ui_kit_style_em(u32 style) noexcept;
float ui_kit_line_height(u32 style) noexcept;
float ui_kit_text_width(const UiKitMeasure*, const Utf8View&, u32 style) noexcept;
// Wrapped height: lines * line height (zero for empty text).
float ui_kit_text_height(const UiKitMeasure*, const Utf8View&, u32 style, float width) noexcept;
// A measure over the host's text cache (null cache: every hook null, the estimates apply). A styled height the
// cache cannot measure is negative: callers fall back to their estimate.
void ui_kit_measure_cache(UiKitMeasure* out, UiTextMeasureCache* cache) noexcept;

// Keys the widgets understand (the host translates its key codes).
enum class UiKitKey : u8 {
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    ctrl_home,
    ctrl_end,
    enter,
    space,
    escape,
    alt_down
};

// Fixed store metrics that are not theme tokens (the spec's component sizes).
inline constexpr float kUiKitChipHeight = 24, kUiKitFilterChipHeight = 32, kUiKitChipPadding = 8,
                       kUiKitFilterChipPadding = 12, kUiKitChipGap = 8, kUiKitBadgeHeight = 18,
                       kUiKitSearchIcon = 16, kUiKitTabHeight = 40, kUiKitCardMinimum = 232, kUiKitColumnsMaximum = 3;

// ---------------------------------------------------------------------------
// SearchField: icon, text, "Ctrl F" hint (empty) or clear button (with text).
struct UiSearchFieldGeometry {
    UiRect field, icon, text, hint, clear;
};
UiSearchFieldGeometry ui_search_field_geometry(const UiSettingsLayoutTokens&, const UiRect& field, bool has_text,
                                               float hint_width) noexcept;
bool ui_search_field_paint(PaintStream*, const UiSettingsTokens&, const UiSearchFieldGeometry&, bool has_text,
                           UiControlState field, UiControlState clear) noexcept;
bool ui_search_field_hit_clear(const UiSearchFieldGeometry&, float x, float y) noexcept;
enum class UiSearchKey : u8 { none, focus, clear, leave };
// Ctrl+F, or '/' while no field has focus, focuses; Esc clears text, then leaves the field.
UiSearchKey ui_search_field_key(u32 virtual_key, bool ctrl, bool alt, bool field_focused, bool has_text) noexcept;

// ---------------------------------------------------------------------------
// SegmentedControl: equal measured items in a bordered track (40 high, 32 compact).
inline constexpr u32 kUiSegmentedMaximum = 5;
float ui_segmented_item_width(const UiSettingsLayoutTokens&, const float* label_widths, u32 count) noexcept;
float ui_segmented_width(const UiSettingsLayoutTokens&, float item_width, u32 count) noexcept;
float ui_segmented_height(const UiSettingsLayoutTokens&, bool compact) noexcept;
UiRect ui_segmented_option(const UiSettingsLayoutTokens&, const UiRect& track, u32 count, u32 index) noexcept;
// ~0u outside every option.
u32 ui_segmented_hit(const UiSettingsLayoutTokens&, const UiRect& track, u32 count, float x, float y) noexcept;
bool ui_segmented_paint(PaintStream*, const UiSettingsTokens&, const UiRect& track, u32 count, u32 selected,
                        u32 hovered, UiControlState) noexcept;
// Selection after a key: left/up and right/down step, home/end jump; unchanged for other keys.
u32 ui_segmented_key(u32 count, u32 selected, UiKitKey) noexcept;
// A track wider than 60% of its row does not fit beside the label.
bool ui_segmented_fits(float track_width, float row_width) noexcept;

// ---------------------------------------------------------------------------
// Dropdown: the select shell with an optional "Sort" label before the value. Opening is the themed context menu.
struct UiDropdownGeometry {
    UiRect field, label, value, chevron;
};
UiDropdownGeometry ui_dropdown_geometry(const UiSettingsLayoutTokens&, const UiRect& field, float label_width) noexcept;
float ui_dropdown_width(const UiSettingsLayoutTokens&, float label_width, float widest_value_width,
                        float maximum_width) noexcept;
bool ui_dropdown_paint(PaintStream*, const UiSettingsTokens&, const UiDropdownGeometry&, bool open,
                       UiControlState) noexcept;
// Menu rows for the popup scene: one per label, the selected one checked, action = first_action + index.
u32 ui_dropdown_menu_items(const Utf8View* labels, u32 count, u32 selected, u32 first_action,
                           Slice<PopupItem> output) noexcept;
bool ui_dropdown_menu_choose(u32 count, u32 first_action, u32 action, u32* selected) noexcept;
// Up/down step the value, enter/space/alt+down request the menu (*open).
bool ui_dropdown_key(u32 count, u32* selected, UiKitKey, bool* open) noexcept;

// ---------------------------------------------------------------------------
// Row anatomy: title and description at the start, one trailing control, optional lead tile.
struct UiRowSpec {
    Utf8View title, description;
    float control_width, control_height;
    bool lead, stacked;
};
struct UiRowGeometry {
    UiRect row, lead, title, description, control;
    float height;
};
UiRowGeometry ui_row_geometry(const UiSettingsLayoutTokens&, const UiRowSpec&, float left, float top, float width,
                              const UiKitMeasure*) noexcept;

// ---------------------------------------------------------------------------
// Slider with its value: the track at the start of the cell, the value column at the end.
struct UiSliderValueGeometry {
    UiSliderGeometry slider;
    UiRect value;
};
UiSliderValueGeometry ui_slider_value_geometry(const UiSettingsLayoutTokens&, const UiRect& cell, u32 count,
                                               u32 index) noexcept;

// ---------------------------------------------------------------------------
// Cards, hairlines and the card grid.
bool ui_card_paint(PaintStream*, const UiSettingsTokens&, const UiRect& card, UiControlState) noexcept;
bool ui_card_hairline_paint(PaintStream*, const UiSettingsTokens&, const UiRect& row) noexcept;
UiRect ui_card_content(const UiSettingsLayoutTokens&, const UiRect& card) noexcept;
struct UiCardGrid {
    u32 columns, count, rows;
    float card_width, card_height, gap, height;
};
UiCardGrid ui_card_grid(const UiSettingsLayoutTokens&, float width, u32 count, float card_height) noexcept;
UiRect ui_card_grid_rect(const UiCardGrid&, float left, float top, u32 index) noexcept;
// Cards of the rows in view and one row either side.
void ui_card_grid_visible(const UiCardGrid&, float view_top, float view_height, u32* first, u32* end) noexcept;
// Relative to the grid's origin; ~0u in a gap or past the last card.
u32 ui_card_grid_hit(const UiCardGrid&, float x, float y) noexcept;
u32 ui_card_grid_move(const UiCardGrid&, u32 index, UiKitKey, u32 page_rows) noexcept;

// ---------------------------------------------------------------------------
// Chips (passive, 24 high) and filter chips (interactive, 32 high).
enum class UiChipTone : u8 { neutral, subtle, risk_low, risk_medium, risk_high, positive, warning, dev, brand, mono };
float ui_chip_width(const UiSettingsLayoutTokens&, float label_width, UiChipTone, bool icon) noexcept;
bool ui_chip_paint(PaintStream*, const UiSettingsTokens&, const UiRect& chip, UiChipTone, UiIcon icon) noexcept;
UiRect ui_chip_label_rect(const UiSettingsLayoutTokens&, const UiRect& chip, UiChipTone, bool icon) noexcept;
Color ui_chip_ink(const UiSettingsTokens&, UiChipTone) noexcept;
float ui_filter_chip_width(const UiSettingsLayoutTokens&, float label_width, float count_width, bool icon,
                           bool selected) noexcept;
bool ui_filter_chip_paint(PaintStream*, const UiSettingsTokens&, const UiRect& chip, bool selected,
                          UiControlState) noexcept;
UiRect ui_filter_chip_label_rect(const UiSettingsLayoutTokens&, const UiRect& chip, bool icon, bool selected) noexcept;
// Flows chips left to right: wrapping onto new rows, or one row shifted by `scroll`. out[i] is the rect of
// chip i; dividers[i] the 1 DIP rule before chip i when it starts a new group on the same row (else empty).
// Returns the strip's height. Never truncates a chip.
float ui_chip_strip_place(const UiSettingsLayoutTokens&, const float* widths, const u8* groups, u32 count, float width,
                          bool wrap, float scroll, UiRect* out, UiRect* dividers) noexcept;
bool ui_chip_divider_paint(PaintStream*, const UiSettingsTokens&, const UiRect& divider) noexcept;
u32 ui_chip_strip_hit(const UiRect* rects, u32 count, float x, float y) noexcept;
u32 ui_chip_strip_move(const UiRect* rects, u32 count, u32 index, UiKitKey) noexcept;
// The scroll offset that brings chip `index` into a one-row strip of `width`.
float ui_chip_strip_reveal(const UiRect* rects, u32 index, float width, float scroll) noexcept;
// Bit i of a mask is chip i on; no bit on is "All". Choosing an All chip clears its group.
u64 ui_chip_toggle(u64 mask, const u8* groups, u32 count, u32 index, bool is_all) noexcept;
// Within a group the selected chips are alternatives, across groups they all apply.
bool ui_chip_filter_matches(u64 selected, u64 entry, const u8* groups, u32 count) noexcept;
float ui_count_badge_width(const UiSettingsLayoutTokens&, float digits_width) noexcept;
bool ui_count_badge_paint(PaintStream*, const UiSettingsTokens&, const UiRect&, bool attention) noexcept;

// ---------------------------------------------------------------------------
// Tabs: the bar's rule and the selected tab's underline.
float ui_tab_width(const UiSettingsLayoutTokens&, float label_width, float badge_width) noexcept;
bool ui_tab_bar_paint(PaintStream*, const UiSettingsTokens&, const UiRect& bar, const UiRect& selected_tab,
                      bool focused) noexcept;

// ---------------------------------------------------------------------------
// Tiles, previews and pills.
enum class UiTileTone : u8 { selected, neutral, dev };
bool ui_glyph_tile_paint(PaintStream*, const UiSettingsTokens&, const UiRect& tile, UiIcon, UiTileTone) noexcept;
enum class UiPreviewAspect : u8 { r16x10, r16x9 };
UiRect ui_preview_rect(const UiRect& area, UiPreviewAspect) noexcept;
enum class UiPreviewState : u8 { empty, loading, ready, failed };
// The plate (with the failed glyph) and the rule under it; a ready preview paints nothing here: the renderer
// draws the decoded image over the plate. top_radius rounds the plate's top corners (a card's).
bool ui_preview_paint(PaintStream*, const UiSettingsTokens&, const UiRect& tile, float top_radius,
                      UiPreviewState) noexcept;
bool ui_pill_paint(PaintStream*, const UiSettingsTokens&, const UiRect& pill) noexcept;
// Lazy decode planning: decoding is the host's, this decides who and when.
inline constexpr u32 kUiPreviewResident = 24, kUiPreviewRequests = 16;
struct UiPreviewResidency {
    u64 keys[kUiPreviewResident];
    u32 stamps[kUiPreviewResident];
    u32 count, clock;
};
struct UiPreviewPlan {
    u64 request[kUiPreviewRequests];
    u64 evict[kUiPreviewResident];
    u32 request_count, evict_count;
};
// Visible keys then one row ahead: keys not resident are requested (visible first), the least recently seen
// resident keys beyond the cap are evicted, never a visible or ahead key.
void ui_preview_plan(UiPreviewResidency*, const u64* visible, u32 visible_count, const u64* ahead, u32 ahead_count,
                     UiPreviewPlan*) noexcept;
// A decode finished (`ready`) or failed (dropped from the plan's resident set).
void ui_preview_resident(UiPreviewResidency*, u64 key, bool ready) noexcept;
// Animated previews: playing starts 300 ms after hover, at once on keyboard focus, never under reduced motion
// unless the play pill was pressed (manual).
inline constexpr u64 kUiPreviewLoopDelayMs = 300;
struct UiPreviewLoop {
    u64 key, since_ms;
    bool playing, manual, reduced;
};
bool ui_preview_loop_update(UiPreviewLoop*, u64 hovered_key, u64 focused_key, bool reduced_motion, u64 now_ms) noexcept;
// The deadline at which a pending hover starts playing, or 0.
u64 ui_preview_loop_next_wake(const UiPreviewLoop&) noexcept;
void ui_preview_loop_toggle(UiPreviewLoop*, u64 key) noexcept;

// ---------------------------------------------------------------------------
// Skeleton loading cards: bars cross-fading between mod_normal and mod_strong.
inline constexpr u64 kUiSkeletonPeriodMs = 1600;
float ui_skeleton_pulse(u64 now_ms, bool reduced_motion) noexcept;
u64 ui_skeleton_next_wake(u64 now_ms, bool reduced_motion) noexcept;
bool ui_skeleton_bar_paint(PaintStream*, const UiSettingsTokens&, const UiRect&, float pulse, bool strong) noexcept;
enum class UiSkeletonCard : u8 { plugin, theme, effect };
float ui_skeleton_card_height(const UiSettingsLayoutTokens&, UiSkeletonCard, float card_width) noexcept;
bool ui_skeleton_card_paint(PaintStream*, const UiSettingsTokens&, const UiRect& card, UiSkeletonCard, float pulse) noexcept;

// ---------------------------------------------------------------------------
// Permission rows (grouped by risk; the sentence table belongs to the consent flow).
enum class UiRisk : u8 { low, medium, high };
Color ui_risk_color(const UiSettingsTokens&, UiRisk) noexcept;
struct UiPermissionRow {
    Utf8View sentence, name;
    bool critical;
};
struct UiPermissionGeometry {
    UiRect row, tile, sentence, name, hosts;
    float height;
};
// host_chip_rows: wrapped rows of host chips under the sentence (0 for none).
UiPermissionGeometry ui_permission_geometry(const UiSettingsLayoutTokens&, const UiPermissionRow&, float left, float top,
                                            float width, u32 host_chip_rows, const UiKitMeasure*) noexcept;
bool ui_permission_paint(PaintStream*, const UiSettingsTokens&, const UiPermissionGeometry&, UiIcon glyph,
                         bool last_in_group) noexcept;

// ---------------------------------------------------------------------------
// Knob rows (effect knobs and the universal performance block).
enum class UiKnobKind : u8 { slider, toggle, color, segments, dropdown };
struct UiKnob {
    UiKnobKind kind;
    Utf8View label, hint, value_text;
    u32 count, index, default_index;
    bool checked, overridden, enabled;
    Color swatch;
};
struct UiKnobGeometry {
    UiRect row, label, value, control, hint, dot, reset;
    float height;
};
UiKnobGeometry ui_knob_geometry(const UiSettingsLayoutTokens&, const UiKnob&, float left, float top, float width,
                                float segments_width, const UiKitMeasure*) noexcept;
// The override dot, the reset button when overridden, and the control itself (segments need their labels'
// text from the caller's pen at ui_segmented_option; dropdowns their value text at ui_dropdown_geometry).
bool ui_knob_paint(PaintStream*, const UiSettingsTokens&, const UiKnob&, const UiKnobGeometry&, UiControlState control,
                   UiControlState reset, u32 hovered_option) noexcept;
// Knob cards sit side by side from 640 DIP of width (gap card_gap) and stack below that.
bool ui_knob_beside(float width) noexcept;
float ui_knob_column_width(const UiSettingsLayoutTokens&, float width) noexcept;

} // namespace maicord
