#include "maicord/ui/shell/notice_band.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord {
namespace {
// The official notice: 36 DIPs, the text in 14 px, an outlined 24-DIP button 8 DIPs after it (10 DIPs of padding),
// and the X in a 36-DIP square at the trailing edge, its glyph 16 DIPs.
constexpr float kButtonHeight = 24, kButtonPadding = 10, kButtonGap = 8, kDismissGlyph = 16, kSidePadding = 40;
constexpr u64 kMeasureSource = 0x4e4f'5449'4345'0001ull; // "NOTICE"

float measured_width(UiTextMeasureCache* cache, Utf8View text, ConstSlice<TextStyleSpan> styles, u16 item,
                     float limit) noexcept {
    if (!text.bytes || limit <= 0)
        return 0;
    if (!cache)
        return limit;
    u64 signature = hash64_bytes(kHash64Seed, text.data, text.bytes);
    for (u32 i = 0; i < styles.count; ++i)
        signature = hash64_mix(signature, (static_cast<u64>(styles[i].byte_offset) << 32) | styles[i].byte_count);
    UiTextMeasureValue value{};
    const UiTextMeasureRequest request{{kMeasureSource, signature, 0, item, 0}, text, styles, {limit, 40}, true};
    return ui_text_measure_cache_measure(cache, request, &value).ok() ? minimum(limit, value.size.width) : limit;
}
} // namespace

u32 ui_notice_band_select(ConstSlice<UiNoticeRequest> requests) noexcept {
    u32 chosen = requests.count;
    for (u32 i = 0; i < requests.count; ++i)
        if (requests[i].active && !requests[i].dismissed &&
            (chosen == requests.count || requests[i].priority < requests[chosen].priority))
            chosen = i;
    return chosen;
}
UiNoticeBandGeometry ui_notice_band_geometry(const UiNoticeBand& band, const UiRect& root,
                                             UiTextMeasureCache* measure) noexcept {
    UiNoticeBandGeometry g{};
    if (band.height <= 0 || !band.text.bytes)
        return g;
    // Laid out at the full height and clipped to the shown one while the band expands.
    g.band = {root.left, root.top, root.right, root.top + band.height};
    const float top = root.top + band.height - kUiNoticeBandHeight;
    const float middle = top + kUiNoticeBandHeight * .5f;
    if (band.dismissible)
        g.dismiss = {root.right - kUiNoticeBandHeight, top, root.right, top + kUiNoticeBandHeight};
    const float space = maximum(0.f, ui_rect_width(root) - 2 * kSidePadding);
    const TextStyleSpan action_style{0, band.action.bytes, kUiNoticeActionStyle};
    const float action = band.action.bytes
                             ? measured_width(measure, band.action, {&action_style, 1}, 2, space * .5f) +
                                   2 * kButtonPadding
                             : 0.f;
    const float text = measured_width(measure, band.text, {band.bold, minimum<u32>(band.bold_count, kUiNoticeBoldRuns)},
                                      1, maximum(0.f, space - (action ? action + kButtonGap : 0.f)));
    const float total = text + (action ? kButtonGap + action : 0.f);
    const float left = root.left + kSidePadding + maximum(0.f, (space - total) * .5f);
    g.text = {left, top, left + text, top + kUiNoticeBandHeight};
    if (action)
        g.action = {g.text.right + kButtonGap, middle - kButtonHeight * .5f, g.text.right + kButtonGap + action,
                    middle + kButtonHeight * .5f};
    return g;
}

UiNoticeBandPart ui_notice_band_hit(const UiNoticeBandGeometry& g, float x, float y) noexcept {
    if (!ui_rect_contains(g.band, x, y))
        return UiNoticeBandPart::none;
    if (ui_rect_contains(g.dismiss, x, y))
        return UiNoticeBandPart::dismiss;
    if (ui_rect_contains(g.action, x, y))
        return UiNoticeBandPart::action;
    return UiNoticeBandPart::band;
}

Color ui_notice_band_ink(const UiNoticeBand& band, const Theme& theme) noexcept {
    return band.kind == UiNoticeKind::brand ? theme.tokens.text.on_accent : theme.tokens.text.normal;
}

bool ui_notice_band_paint(PaintStream* stream, const UiNoticeBand& band, const UiNoticeBandGeometry& g,
                          const Theme& theme) noexcept {
    if (!stream || !ui_rect_has_area(g.band))
        return true;
    const auto& accent = theme.tokens.accent;
    const Color tint = band.kind == UiNoticeKind::warning ? accent.warning
                       : band.kind == UiNoticeKind::danger ? accent.danger
                       : band.kind == UiNoticeKind::info   ? accent.app_badge
                                                           : accent.brand;
    const Color ink = ui_notice_band_ink(band, theme);
    // Brand fills solid as the official brand notice; the others tint the app frame, dark on dark themes and light
    // on light ones, so the regular text colour reads on them.
    const bool solid = band.kind == UiNoticeKind::brand;
    if (!paint_stream_push_clip(stream, g.band) || !paint_stream_push(stream, g.band, solid ? tint : theme.base_lowest) ||
        (!solid && !paint_stream_push(stream, g.band, ui_theme_color_alpha(tint, .38f))))
        return false;
    if (ui_rect_has_area(g.action) &&
        !paint_stream_push_rounded_border(stream, g.action, ui_theme_color_alpha(ink, 0), ink,
                                          theme.scale.radius_sm, 1))
        return false;
    if (ui_rect_has_area(g.dismiss)) {
        const float cx = (g.dismiss.left + g.dismiss.right) * .5f, cy = (g.dismiss.top + g.dismiss.bottom) * .5f;
        if (!ui_icon_paint(stream, UiIcon::close,
                           {cx - kDismissGlyph * .5f, cy - kDismissGlyph * .5f, cx + kDismissGlyph * .5f,
                            cy + kDismissGlyph * .5f},
                           ink))
            return false;
    }
    return paint_stream_pop_clip(stream);
}

} // namespace maicord
