#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/text/style.hpp"

// Shared settings controls: stateless geometry, painting and hit-testing over the settings tokens
// (ui/controls/settings_tokens.hpp). Widgets never emit text: they return label rectangles and the style
// words below; scenes emit through their own text pens. No allocation: arrays are the caller's.
namespace maicord {
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

// Optional measuring hooks over the caller's text cache; null members use the per-character estimate the
// settings rows share (ui_settings_text_width).
struct UiKitMeasure {
    float (*width)(void* context, const Utf8View&, u32 style) noexcept;
    float (*height)(void* context, const Utf8View&, u32 style, float width) noexcept;
    // Wrapped height of text carrying style spans (a markdown block); null estimates it.
    float (*styled_height)(void* context, const Utf8View&, ConstSlice<TextStyleSpan>, float width) noexcept;
    void* context;
};
float ui_kit_line_height(u32 style) noexcept;
float ui_kit_text_width(const UiKitMeasure*, const Utf8View&, u32 style) noexcept;
// Wrapped height: lines * line height (zero for empty text).
float ui_kit_text_height(const UiKitMeasure*, const Utf8View&, u32 style, float width) noexcept;
// A measure over the host's text cache (null cache: every hook null, the estimates apply). A styled height the
// cache cannot measure is negative: callers fall back to their estimate.
void ui_kit_measure_cache(UiKitMeasure* out, UiTextMeasureCache* cache) noexcept;

// Fixed chip metrics that are not theme tokens.
inline constexpr float kUiKitChipHeight = 24, kUiKitChipGap = 8;

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
// A track wider than 60% of its row does not fit beside the label.
bool ui_segmented_fits(float track_width, float row_width) noexcept;

// The select shell's width: its padding, an optional label and the widest value, within the row's share.
float ui_dropdown_width(const UiSettingsLayoutTokens&, float label_width, float widest_value_width,
                        float maximum_width) noexcept;

// The neutral hairline between rows of a card.
bool ui_card_hairline_paint(PaintStream*, const UiSettingsTokens&, const UiRect& row) noexcept;

// Passive chips (24 high).
enum class UiChipTone : u8 { neutral, positive, warning, dev, brand };
float ui_chip_width(float label_width) noexcept;
bool ui_chip_paint(PaintStream*, const UiSettingsTokens&, const UiRect& chip, UiChipTone) noexcept;
UiRect ui_chip_label_rect(const UiRect& chip) noexcept;
Color ui_chip_ink(const UiSettingsTokens&, UiChipTone) noexcept;

} // namespace maicord
