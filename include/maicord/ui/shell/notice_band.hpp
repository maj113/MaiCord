#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/theme/theme.hpp"

namespace maicord {

struct PaintStream;
struct Theme;
struct UiTextMeasureCache;

// The app-wide notice band: the official client's notice bar across the top of the app (Streamer Mode, Application
// Test Mode, ...). One shows at a time, the host's highest-priority notice; the shell lays out below it
// (UiShellLayoutMetrics::notice_height), so the panes and their hit-tests shift down by its height. It holds a line
// of text with up to kUiNoticeBoldRuns bold runs, centred, an optional outlined action button after the text and,
// when dismissible, an X at the trailing edge.
enum class UiNoticeKind : u8 { info, warning, brand, danger };
inline constexpr u32 kUiNoticeBoldRuns = 2;
inline constexpr float kUiNoticeBandHeight = 36.0f;
struct UiNoticeBand {
    Utf8View text, action;
    TextStyleSpan bold[kUiNoticeBoldRuns];
    u8 bold_count;
    UiNoticeKind kind;
    bool dismissible;
    // The shown height, 0 (none) to kUiNoticeBandHeight while it expands.
    float height;
};
// A source's notice: shown when active and not dismissed; of several, the lowest `priority` wins.
struct UiNoticeRequest {
    u8 priority;
    bool active, dismissed;
};
// The index of the notice to show, or requests.count when none applies.
u32 ui_notice_band_select(ConstSlice<UiNoticeRequest> requests) noexcept;
// Where the band's parts sit inside `root` (the shell's root rectangle): the band along its top, the text centred
// with the button 8 DIPs after it, the X square at the trailing edge. Text and button widths come from `measure`
// (their labels at the band's styles); without a cache they take the space available.
struct UiNoticeBandGeometry {
    UiRect band, text, action, dismiss;
};
UiNoticeBandGeometry ui_notice_band_geometry(const UiNoticeBand& band, const UiRect& root,
                                             UiTextMeasureCache* measure) noexcept;
enum class UiNoticeBandPart : u8 { none, band, action, dismiss };
UiNoticeBandPart ui_notice_band_hit(const UiNoticeBandGeometry& geometry, float x, float y) noexcept;
// The band's fill (the kind's accent, a tint over the app frame except brand's solid fill), the button outline and
// the X, clipped to the shown height.
bool ui_notice_band_paint(PaintStream* stream, const UiNoticeBand& band, const UiNoticeBandGeometry& geometry,
                          const Theme& theme) noexcept;
// The ink of the band's text, button label and X.
Color ui_notice_band_ink(const UiNoticeBand& band, const Theme& theme) noexcept;
// The text style flags of the band's line and button.
inline constexpr u32 kUiNoticeTextStyle = text_style_none;
inline constexpr u32 kUiNoticeActionStyle = text_style_bold;

} // namespace maicord
