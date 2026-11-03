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
float cache_width(void* context, const Utf8View& text, u32 style) noexcept {
    const TextStyleSpan span{0, text.bytes, style};
    UiTextMeasureValue value{};
    const UiTextMeasureRequest request{{kMeasureSource, hash64_mix(hash64_bytes(kHash64Seed, text.data, text.bytes), style), 0, 0, 0},
                                       text,
                                       style ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                                       {4096.f, 64.f},
                                       true};
    return ui_text_measure_cache_measure(static_cast<UiTextMeasureCache*>(context), request, &value).ok()
               ? value.size.width
               : ui_kit_text_width(nullptr, text, style);
}
float cache_height(void* context, const Utf8View& text, u32 style, float width) noexcept {
    const TextStyleSpan span{0, text.bytes, style};
    UiTextMeasureValue value{};
    const UiTextMeasureRequest request{{kMeasureSource, hash64_mix(hash64_bytes(kHash64Seed, text.data, text.bytes), style), 0, 1, 0},
                                       text,
                                       style ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                                       {width, 65536.f},
                                       false};
    return ui_text_measure_cache_measure(static_cast<UiTextMeasureCache*>(context), request, &value).ok()
               ? value.size.height
               : ui_kit_text_height(nullptr, text, style, width);
}
// A negative height asks the caller for its estimate.
float cache_styled_height(void* context, const Utf8View& text, ConstSlice<TextStyleSpan> spans, float width) noexcept {
    UiTextMeasureValue value{};
    const u64 signature = hash64_counted_bytes(hash64_bytes(kHash64Seed, text.data, text.bytes), spans.data,
                                               spans.count * sizeof(TextStyleSpan));
    const UiTextMeasureRequest request{{kMeasureSource, signature, 0, 2, 0}, text, spans, {width, 65536.f}, false};
    return ui_text_measure_cache_measure(static_cast<UiTextMeasureCache*>(context), request, &value).ok() ? value.size.height
                                                                                                          : -1.f;
}
} // namespace

// ---------------------------------------------------------------------------
float ui_kit_line_height(u32 style) noexcept {
    const float em = style_em(style);
    return em >= 24 ? 30.f : em >= 20 ? 24.f : em >= 17 ? 22.f : em >= 16 ? 20.f : em >= 13 ? 18.f : 16.f;
}
float ui_kit_text_width(const UiKitMeasure* measure, const Utf8View& text, u32 style) noexcept {
    if (!text.bytes)
        return 0;
    if (measure && measure->width)
        return measure->width(measure->context, text, style);
    return ui_settings_text_width(text, style_em(style)) *
           ((style & text_style_bold) ? kBoldFactor : kRegularFactor);
}
float ui_kit_text_height(const UiKitMeasure* measure, const Utf8View& text, u32 style, float width) noexcept {
    if (!text.bytes)
        return 0;
    if (measure && measure->height)
        return measure->height(measure->context, text, style, width);
    return static_cast<float>(ui_settings_text_lines(text, style_em(style) * kRegularFactor, width)) *
           ui_kit_line_height(style);
}
void ui_kit_measure_cache(UiKitMeasure* out, UiTextMeasureCache* cache) noexcept {
    *out = cache ? UiKitMeasure{cache_width, cache_height, cache_styled_height, cache} : UiKitMeasure{};
}

// ---------------------------------------------------------------------------
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
bool ui_segmented_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& track, u32 count, u32 selected,
                        u32 hovered, UiControlState state) noexcept {
    const auto& c = t.color;
    const float alpha = state.enabled ? 1.f : t.disabled_alpha, radius = t.metric.control_radius;
    bool ok = paint_stream_push_rounded_border(paint, track, ui_settings_color_alpha(c.input_background, alpha),
                                               ui_settings_color_alpha(c.border_subtle, alpha), radius, 1);
    for (u32 i = 0; ok && i < count; ++i) {
        const bool on = i == selected, hover = state.enabled && i == hovered;
        if (!on && !hover)
            continue;
        const auto option = ui_segmented_option(t, track, count, i);
        ok = on ? paint_stream_push_rounded_border(paint, option, ui_settings_color_alpha(c.mod_strong, alpha),
                                                   ui_settings_color_alpha(c.border_normal, alpha), radius * .5f, 1)
                : paint_stream_push_rounded(paint, option, state.pressed ? c.mod_normal : c.mod_subtle, radius * .5f);
    }
    if (ok && state.focused && count)
        ok = ui_settings_focus_paint(paint, t, ui_segmented_option(t, track, count, selected < count ? selected : 0),
                                     radius * .5f);
    return ok;
}
bool ui_segmented_fits(float track_width, float row_width) noexcept {
    return track_width <= row_width * .6f;
}

// ---------------------------------------------------------------------------
float ui_dropdown_width(const UiSettingsLayoutTokens& t, float label_width, float widest_value_width,
                        float maximum_width) noexcept {
    const float wanted = t.metric.field_padding + (label_width > 0 ? label_width + 8 : 0) + widest_value_width +
                         t.metric.control_height;
    return clamped(wanted, t.metric.select_width, maximum(t.metric.select_width, maximum_width));
}

} // namespace maicord
