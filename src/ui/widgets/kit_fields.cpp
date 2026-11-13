#include "maicord/ui/widgets/kit.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
namespace {
constexpr float kBoldFactor = 1.08f, kRegularFactor = 1.04f;
float clamped(float value, float low, float high) noexcept {
    return value < low ? low : value > high ? high : value;
}
float style_em(u32 style) noexcept {
    if (const float ratio = text_style_size_code_ratio(style))
        return 16 * ratio;
    return style & text_style_heading_1 ? 24.f : style & text_style_heading_2 ? 20.f : style & text_style_heading_3 ? 17.6f
           : style & text_style_subtext ? 13.f : 16.f;
}
// Measuring through the host's text cache: the keys carry the style word (and the spans of a styled block).
constexpr u64 kMeasureSource = 0x4B49544D45415355ull;
bool cache_measure(UiTextMeasureCache* cache, const Utf8View& text, u32 style, u16 kind, UiSize constraint,
                   bool single_line, UiTextMeasureValue* value, UiTextFace face = UiTextFace::normal) noexcept {
    const TextStyleSpan span{0, text.bytes, style};
    const UiTextMeasureRequest request{{kMeasureSource, hash64_mix(hash64_bytes(kHash64Seed, text.data, text.bytes), style), 0, kind, 0},
                                       text,
                                       style ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                                       constraint,
                                       single_line,
                                       face};
    return cache && ui_text_measure_cache_measure(cache, request, value).ok();
}
} // namespace

float ui_kit_line_height(u32 style) noexcept {
    const float em = style_em(style);
    return em >= 24 ? 30.f : em >= 20 ? 24.f : em >= 17 ? 22.f : em >= 16 ? 20.f : em >= 13 ? 18.f : 16.f;
}
float ui_kit_text_width(UiTextMeasureCache* cache, const Utf8View& text, u32 style) noexcept {
    if (!text.bytes)
        return 0;
    UiTextMeasureValue value{};
    if (cache_measure(cache, text, style, 0, {4096.f, 64.f}, true, &value))
        return value.size.width;
    return ui_settings_text_width(text, style_em(style)) *
           ((style & text_style_bold) ? kBoldFactor : kRegularFactor);
}
float ui_kit_chat_text_width(UiTextMeasureCache* cache, const Utf8View& text, u32 style, float chat_em) noexcept {
    if (!text.bytes)
        return 0;
    UiTextMeasureValue value{};
    if (cache_measure(cache, text, style, 3, {4096.f, 64.f}, true, &value, ui_text_face_chat(UiTextFace::normal)) &&
        value.size.width >= 0)
        return value.size.width;
    return ui_settings_text_width(text, style_em(style) * chat_em / 16.f) *
           ((style & text_style_bold) ? kBoldFactor : kRegularFactor);
}
float ui_kit_text_height(UiTextMeasureCache* cache, const Utf8View& text, u32 style, float width) noexcept {
    if (!text.bytes)
        return 0;
    UiTextMeasureValue value{};
    if (cache_measure(cache, text, style, 1, {width, 65536.f}, false, &value))
        return value.size.height;
    return static_cast<float>(ui_settings_text_lines(text, style_em(style) * kRegularFactor, width)) *
           ui_kit_line_height(style);
}
float ui_kit_styled_text_height(UiTextMeasureCache* cache, const Utf8View& text, ConstSlice<TextStyleSpan> spans,
                                float width) noexcept {
    UiTextMeasureValue value{};
    const u64 signature = hash64_counted_bytes(hash64_bytes(kHash64Seed, text.data, text.bytes), spans.data,
                                               spans.count * sizeof(TextStyleSpan));
    const UiTextMeasureRequest request{{kMeasureSource, signature, 0, 2, 0}, text, spans, {width, 65536.f}, false};
    return cache && ui_text_measure_cache_measure(cache, request, &value).ok() ? value.size.height : -1.f;
}

float ui_segmented_item_width(const UiSettingsLayoutTokens&, const float* label_widths, u32 count) noexcept {
    float widest = 0;
    for (u32 i = 0; i < count; ++i)
        widest = maximum(widest, label_widths[i]);
    return maximum(72.f, widest + 28);
}
float ui_segmented_width(const UiSettingsLayoutTokens& t, float item_width, u32 count) noexcept {
    return item_width * static_cast<float>(count) + t.metric.segment_padding * 2;
}
float ui_segmented_height(const UiSettingsLayoutTokens& t, bool compact) noexcept {
    return compact ? t.metric.button_small : t.metric.control_height;
}
UiRect ui_segmented_option(const UiSettingsLayoutTokens& t, const UiRect& track, u32 count, u32 index) noexcept {
    if (index >= count)
        return {};
    const float pad = t.metric.segment_padding, item = (ui_rect_width(track) - pad * 2) / static_cast<float>(count);
    const float left = track.left + pad + item * static_cast<float>(index);
    return {left, track.top + pad, left + item, track.bottom - pad};
}
u32 ui_segmented_hit(const UiSettingsLayoutTokens& t, const UiRect& track, u32 count, float x, float y) noexcept {
    if (!count || !ui_rect_contains(track, x, y))
        return ~0u;
    const float pad = t.metric.segment_padding, item = (ui_rect_width(track) - pad * 2) / static_cast<float>(count);
    const float position = (x - track.left - pad) / item;
    return position <= 0 ? 0u : minimum(static_cast<u32>(position), count - 1);
}
UiRect ui_segmented_cell(const UiSettingsLayoutTokens& t, const UiRect& track, u32 count, u32 index) noexcept {
    UiRect cell = ui_segmented_option(t, track, count, index);
    // Neighbouring plates keep the track's padding between them; the outer edges stay at the padding.
    const float half = t.metric.segment_padding * .5f;
    if (index > 0)
        cell.left += half;
    if (index + 1 < count)
        cell.right -= half;
    return cell;
}
UiRect ui_segmented_plate(const UiSettingsLayoutTokens& t, const UiRect& track, u32 count, u32 selected,
                          const UiSegmentedMotion& motion) noexcept {
    const UiRect to = ui_segmented_cell(t, track, count, selected);
    if (motion.from >= count || motion.from == selected || !(motion.progress < 1))
        return to;
    const UiRect from = ui_segmented_cell(t, track, count, motion.from);
    const float at = motion.progress > 0 ? motion.progress : 0.f;
    return {from.left + (to.left - from.left) * at, to.top, from.right + (to.right - from.right) * at, to.bottom};
}
bool ui_segmented_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& track, u32 count, u32 selected,
                        u32 hovered, UiControlState state, const UiSegmentedMotion& motion) noexcept {
    const auto& c = t.color;
    const float alpha = state.enabled ? 1.f : t.disabled_alpha, radius = t.metric.control_radius;
    bool ok = paint_stream_push_rounded_border(paint, track, ui_settings_color_alpha(c.input_background, alpha),
                                               ui_settings_color_alpha(c.border_subtle, alpha), radius, 1);
    const UiRect plate = ui_segmented_plate(t, track, count, selected, motion);
    // The hover plate never touches the selected plate, also while that one slides past it.
    const UiRect hover = ui_segmented_cell(t, track, count, hovered);
    const float gap = t.metric.segment_padding;
    const bool apart = selected >= count ||
                       (hover.right + gap <= plate.left + .01f || plate.right + gap <= hover.left + .01f);
    if (ok && state.enabled && hovered < count && hovered != selected && apart)
        ok = paint_stream_push_rounded(paint, hover, state.pressed ? c.mod_normal : c.mod_subtle, radius * .5f);
    if (ok && selected < count)
        ok = paint_stream_push_rounded_border(paint, plate, ui_settings_color_alpha(c.mod_strong, alpha),
                                              ui_settings_color_alpha(c.border_normal, alpha), radius * .5f, 1);
    if (ok && state.focused && count)
        ok = ui_settings_focus_paint(paint, t, selected < count ? plate : ui_segmented_cell(t, track, count, 0),
                                     radius * .5f);
    return ok;
}
bool ui_segmented_fits(float track_width, float row_width) noexcept {
    return track_width <= row_width * .6f;
}

float ui_dropdown_width(const UiSettingsLayoutTokens& t, float label_width, float widest_value_width,
                        float maximum_width) noexcept {
    const float wanted = t.metric.field_padding + (label_width > 0 ? label_width + 8 : 0) + widest_value_width +
                         t.metric.control_height;
    return clamped(wanted, t.metric.select_width, maximum(t.metric.select_width, maximum_width));
}

} // namespace maicord
