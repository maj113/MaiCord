#include "maicord/ui/widgets/kit.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
namespace {
// Windows virtual keys the search shortcuts name.
constexpr u32 kKeyEscape = 0x1B, kKeyF = 0x46, kKeySlash = 0xBF;
constexpr float kBoldFactor = 1.08f, kRegularFactor = 1.04f;
UiRect centered(const UiRect& r, float width, float height) noexcept {
    const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
    return {cx - width * .5f, cy - height * .5f, cx + width * .5f, cy + height * .5f};
}
float clamped(float value, float low, float high) noexcept {
    return value < low ? low : value > high ? high : value;
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
float ui_kit_style_em(u32 style) noexcept {
    if (const float ratio = text_style_size_code_ratio(style))
        return 16 * ratio;
    return style & text_style_heading_1 ? 24.f : style & text_style_heading_2 ? 20.f : style & text_style_heading_3 ? 17.6f
           : style & text_style_subtext ? 13.f : 16.f;
}
float ui_kit_line_height(u32 style) noexcept {
    const float em = ui_kit_style_em(style);
    return em >= 24 ? 30.f : em >= 20 ? 24.f : em >= 17 ? 22.f : em >= 16 ? 20.f : em >= 13 ? 18.f : 16.f;
}
float ui_kit_text_width(const UiKitMeasure* measure, const Utf8View& text, u32 style) noexcept {
    if (!text.bytes)
        return 0;
    if (measure && measure->width)
        return measure->width(measure->context, text, style);
    return ui_settings_text_width(text, ui_kit_style_em(style)) *
           ((style & text_style_bold) ? kBoldFactor : kRegularFactor);
}
float ui_kit_text_height(const UiKitMeasure* measure, const Utf8View& text, u32 style, float width) noexcept {
    if (!text.bytes)
        return 0;
    if (measure && measure->height)
        return measure->height(measure->context, text, style, width);
    return static_cast<float>(ui_settings_text_lines(text, ui_kit_style_em(style) * kRegularFactor, width)) *
           ui_kit_line_height(style);
}
void ui_kit_measure_cache(UiKitMeasure* out, UiTextMeasureCache* cache) noexcept {
    *out = cache ? UiKitMeasure{cache_width, cache_height, cache_styled_height, cache} : UiKitMeasure{};
}

// ---------------------------------------------------------------------------
UiSearchFieldGeometry ui_search_field_geometry(const UiSettingsLayoutTokens&, const UiRect& field, bool has_text,
                                               float hint_width) noexcept {
    const float cy = (field.top + field.bottom) * .5f;
    UiSearchFieldGeometry g{};
    g.field = field;
    g.icon = {field.left + 12, cy - kUiKitSearchIcon * .5f, field.left + 12 + kUiKitSearchIcon,
              cy + kUiKitSearchIcon * .5f};
    if (has_text)
        g.clear = {field.right - 8 - 24, cy - 12, field.right - 8, cy + 12};
    else if (hint_width > 0) {
        const float width = hint_width + 12;
        g.hint = {field.right - 8 - width, cy - 10, field.right - 8, cy + 10};
    }
    const float right = has_text ? g.clear.left - 4 : hint_width > 0 ? g.hint.left - 8 : field.right - 12;
    g.text = {g.icon.right + 8, field.top, maximum(g.icon.right + 9, right), field.bottom};
    return g;
}
bool ui_search_field_paint(PaintStream* paint, const UiSettingsTokens& t, const UiSearchFieldGeometry& g, bool has_text,
                           UiControlState field, UiControlState clear) noexcept {
    const auto& c = t.color;
    const float radius = t.metric.control_radius, alpha = field.enabled ? 1.f : t.disabled_alpha;
    bool ok = paint_stream_push_rounded_border(paint, g.field, ui_settings_color_alpha(c.input_background, alpha),
                                               field.focused ? c.focus_ring
                                               : field.hovered ? c.border_normal
                                                               : c.input_border,
                                               radius, 1);
    if (ok && field.focused)
        ok = ui_settings_ring_paint(paint, g.field, radius, 0, 1, c.focus_ring);
    ok = ok && ui_icon_paint(paint, UiIcon::search, g.icon, ui_settings_color_alpha(c.text_subtle, alpha));
    if (ok && has_text) {
        if (ui_rect_has_area(g.clear))
            ok = paint_stream_push_rounded(paint, g.clear, clear.hovered || clear.pressed ? c.mod_strong : c.mod_normal,
                                           ui_rect_width(g.clear) * .5f) &&
                 ui_icon_paint(paint, UiIcon::close, centered(g.clear, 14, 14), c.text_strong);
        if (ok && clear.focused)
            ok = ui_settings_focus_paint(paint, t, g.clear, ui_rect_width(g.clear) * .5f);
    } else if (ok && ui_rect_has_area(g.hint))
        ok = paint_stream_push_rounded_border(paint, g.hint, {}, c.border_normal, radius * .5f, 1);
    return ok;
}
bool ui_search_field_hit_clear(const UiSearchFieldGeometry& g, float x, float y) noexcept {
    return ui_rect_contains(g.clear, x, y);
}
UiSearchKey ui_search_field_key(u32 virtual_key, bool ctrl, bool alt, bool field_focused, bool has_text) noexcept {
    if (virtual_key == kKeyF && ctrl && !alt)
        return UiSearchKey::focus;
    if (virtual_key == kKeySlash && !ctrl && !alt && !field_focused)
        return UiSearchKey::focus;
    if (virtual_key == kKeyEscape && field_focused)
        return has_text ? UiSearchKey::clear : UiSearchKey::leave;
    return UiSearchKey::none;
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
u32 ui_segmented_key(u32 count, u32 selected, UiKitKey key) noexcept {
    if (!count)
        return selected;
    const bool none = selected >= count;
    switch (key) {
    case UiKitKey::left:
    case UiKitKey::up:
        return none ? count - 1 : selected ? selected - 1 : 0;
    case UiKitKey::right:
    case UiKitKey::down:
        return none ? 0 : minimum(selected + 1, count - 1);
    case UiKitKey::home:
        return 0;
    case UiKitKey::end:
        return count - 1;
    default:
        return selected;
    }
}
bool ui_segmented_fits(float track_width, float row_width) noexcept {
    return track_width <= row_width * .6f;
}

// ---------------------------------------------------------------------------
UiDropdownGeometry ui_dropdown_geometry(const UiSettingsLayoutTokens& t, const UiRect& field, float label_width) noexcept {
    UiDropdownGeometry g{};
    g.field = field;
    g.chevron = {field.right - ui_rect_height(field), field.top, field.right, field.bottom};
    const float left = field.left + t.metric.field_padding;
    if (label_width > 0)
        g.label = {left, field.top, left + label_width, field.bottom};
    const float value_left = label_width > 0 ? g.label.right + 8 : left;
    g.value = {value_left, field.top, maximum(value_left + 1, g.chevron.left), field.bottom};
    return g;
}
float ui_dropdown_width(const UiSettingsLayoutTokens& t, float label_width, float widest_value_width,
                        float maximum_width) noexcept {
    const float wanted = t.metric.field_padding + (label_width > 0 ? label_width + 8 : 0) + widest_value_width +
                         t.metric.control_height;
    return clamped(wanted, t.metric.select_width, maximum(t.metric.select_width, maximum_width));
}
bool ui_dropdown_paint(PaintStream* paint, const UiSettingsTokens& t, const UiDropdownGeometry& g, bool open,
                       UiControlState state) noexcept {
    return ui_select_paint(paint, t, g.field, open, state);
}
u32 ui_dropdown_menu_items(const Utf8View* labels, u32 count, u32 selected, u32 first_action,
                           Slice<PopupItem> output) noexcept {
    if (!labels || !output.data || output.count < count)
        return 0;
    for (u32 i = 0; i < count; ++i)
        output[i] = {labels[i], first_action + i,
                     static_cast<u16>(popup_item_enabled | (i == selected ? popup_item_checked : 0))};
    return count;
}
bool ui_dropdown_menu_choose(u32 count, u32 first_action, u32 action, u32* selected) noexcept {
    if (!selected || action < first_action || action - first_action >= count)
        return false;
    *selected = action - first_action;
    return true;
}
bool ui_dropdown_key(u32 count, u32* selected, UiKitKey key, bool* open) noexcept {
    if (!count || !selected || !open)
        return false;
    switch (key) {
    case UiKitKey::up:
    case UiKitKey::down: {
        const u32 next = key == UiKitKey::up ? (*selected < count && *selected ? *selected - 1 : 0)
                                             : (*selected + 1 < count ? *selected + 1 : count - 1);
        const bool changed = next != *selected;
        *selected = next;
        return changed;
    }
    case UiKitKey::enter:
    case UiKitKey::space:
    case UiKitKey::alt_down:
        *open = true;
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
UiRowGeometry ui_row_geometry(const UiSettingsLayoutTokens& t, const UiRowSpec& spec, float left, float top, float width,
                              const UiKitMeasure* measure) noexcept {
    const auto& m = t.metric;
    constexpr float kLead = 32;
    const float inset = m.card_inset, lead = spec.lead ? kLead + m.lead_gap : 0;
    const bool side = spec.control_width > 0 && !spec.stacked;
    const float text_left = left + inset + lead;
    const float text_width = maximum(1.f, width - inset * 2 - lead - (side ? spec.control_width + m.control_gap : 0));
    const float title = spec.title.bytes ? ui_kit_line_height(kUiKitTextBody) : 0;
    const float description = ui_kit_text_height(measure, spec.description, kUiKitTextSmall, text_width);
    const float text = title + (title > 0 && description > 0 ? m.label_gap : 0) + description;
    const float stack = spec.control_width > 0 && spec.stacked ? spec.control_height + (text > 0 ? m.field_gap : 0) : 0;
    const float inner = maximum(maximum(text + stack, side ? spec.control_height : 0.f), spec.lead ? kLead : 0.f);
    UiRowGeometry g{};
    g.height = maximum(m.row_min_height, inner + m.row_padding * 2);
    // The content block is centred in the row; beside a control or lead the text centres in it too.
    const float content = top + (g.height - inner) * .5f;
    const float text_top = content + (side || spec.lead ? (inner - (text + stack)) * .5f : 0.f);
    g.row = {left, top, left + width, top + g.height};
    if (spec.lead)
        g.lead = {left + inset, top + (g.height - kLead) * .5f, left + inset + kLead, top + (g.height + kLead) * .5f};
    if (title > 0)
        g.title = {text_left, text_top, text_left + text_width, text_top + title};
    if (description > 0) {
        const float at = text_top + title + (title > 0 ? m.label_gap : 0);
        g.description = {text_left, at, text_left + text_width, at + description};
    }
    if (spec.control_width > 0) {
        const float right = left + width - inset;
        g.control = side ? UiRect{right - spec.control_width, top + (g.height - spec.control_height) * .5f, right,
                                  top + (g.height + spec.control_height) * .5f}
                         : UiRect{text_left, text_top + text + (text > 0 ? m.field_gap : 0), right,
                                  text_top + text + (text > 0 ? m.field_gap : 0) + spec.control_height};
    }
    return g;
}

UiSliderValueGeometry ui_slider_value_geometry(const UiSettingsLayoutTokens& t, const UiRect& cell, u32 count,
                                               u32 index) noexcept {
    UiSliderValueGeometry g{};
    g.value = {cell.right - t.metric.value_width, cell.top, cell.right, cell.bottom};
    const UiRect track{cell.left, cell.top, g.value.left - 8, cell.bottom};
    const float cy = (cell.top + cell.bottom) * .5f;
    g.slider = ui_slider_geometry(t, {track.left, cy - 12, maximum(track.left + 1, track.right), cy + 12}, count, index,
                                  false);
    return g;
}

} // namespace maicord
