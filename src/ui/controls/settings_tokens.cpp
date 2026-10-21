#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/transition.hpp"
#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/theme_style_projection.hpp"

namespace maicord {
namespace {
constexpr Color neutral(float alpha) noexcept {
    return {151 / 255.f, 151 / 255.f, 159 / 255.f, alpha};
}
Color scaled(Color color, float factor) noexcept {
    return {color.red * factor, color.green * factor, color.blue * factor, color.alpha};
}
Color pick(u32 packed, Color fallback) noexcept {
    return packed ? ui_settings_color_unpack(packed) : fallback;
}
const UiThemeSettings& settings_style(const UiResolvedThemeStyles* styles) noexcept {
    static constexpr UiThemeSettings defaults{};
    return styles && styles->ready ? styles->settings : defaults;
}
void set_layout_tokens(UiSettingsTokens& t, const UiThemeSettings& s, float space,
                         const UiResolvedThemeStyles* styles) noexcept {
    auto& m = t.metric;
    m.modal_max_width = s.modal_max_width;
    m.modal_viewport_ratio = static_cast<float>(s.modal_viewport_ratio) / 100;
    m.modal_ratio_threshold = s.modal_ratio_threshold;
    m.modal_inset_top = s.modal_inset_top;
    m.modal_inset_side = s.modal_inset_side;
    m.modal_inset_bottom = s.modal_inset_bottom;
    m.modal_radius = s.modal_radius;
    m.modal_compact_width = s.modal_compact_width;
    m.sidebar_width = s.sidebar_width;
    m.sidebar_padding = s.sidebar_padding;
    m.sidebar_top = s.sidebar_top;
    m.search_height = s.search_height;
    m.nav_header_height = s.nav_header_height;
    m.nav_item_height = s.nav_item_height;
    m.nav_item_radius = s.nav_item_radius;
    m.nav_item_gap = s.nav_item_gap;
    m.nav_item_padding = s.nav_item_padding;
    m.nav_icon = s.nav_icon;
    m.separator_margin = s.separator_margin;
    m.panel_padding_x = s.panel_padding_x;
    m.panel_padding_top = s.panel_padding_top;
    m.panel_padding_bottom = s.panel_padding_bottom;
    m.panel_max_width = s.panel_max_width;
    m.title_gap = s.title_gap;
    m.category_gap = s.category_gap;
    m.category_heading_gap = s.category_heading_gap;
    m.row_padding = s.row_padding;
    m.label_gap = s.label_gap;
    m.control_gap = s.control_gap;
    m.field_gap = s.field_gap;
    m.switch_width = s.switch_width;
    m.switch_height = s.switch_height;
    m.switch_thumb = s.switch_thumb;
    m.slider_bar = s.slider_bar;
    m.slider_grabber = s.slider_grabber;
    m.slider_mark_height = s.slider_mark_height;
    m.radio_size = s.radio_size;
    m.radio_gap = s.radio_gap;
    m.radio_spacing = s.radio_spacing;
    m.control_height = s.control_height;
    m.control_radius = s.control_radius;
    m.button_small = s.button_small;
    m.button_medium = s.button_medium;
    m.button_padding = s.button_padding;
    m.swatch = s.swatch;
    m.swatch_gap = s.swatch_gap;
    m.close_size = s.close_size;
    m.close_caption_gap = s.close_caption_gap;
    m.notice_padding = s.notice_padding;
    m.segment_size = 28;
    m.segment_gap = 4;
    m.color_cell = 24;
    m.color_gap = 8;
    m.chip_height = 22;
    m.chip_gap = 4;
    m.chip_padding = 8;
    m.avatar_size = 40;
    m.grip_width = 20;
    m.accent_dot = 12;
    m.lead_gap = 12;
    m.list_icon = 24;
    m.list_column = 96;
    m.list_action = s.button_small;
    m.tristate_segment = 32;
    m.focus_gap = space;
    m.focus_width = space;
    m.ring_gap = space;
    m.ring_width = space;
    m.field_padding = ui_theme_metrics(styles).form_field_padding;
    m.slider_grabber_grow = 4;
    m.slider_bar_grow = 2;
    t.disabled_alpha = .5f;
    t.duration = {s.switch_ms, s.switch_reduced_ms, s.modal_enter_ms, s.flash_ms, s.flash_fade_ms};
}
} // namespace

Color ui_settings_color_unpack(u32 rgba) noexcept {
    return {((rgba >> 24) & 255) / 255.f, ((rgba >> 16) & 255) / 255.f, ((rgba >> 8) & 255) / 255.f,
            (rgba & 255) / 255.f};
}
Color ui_settings_color_mix(const Color& a, const Color& b, float t) noexcept {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return {a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
            a.alpha + (b.alpha - a.alpha) * t};
}
Color ui_settings_color_alpha(const Color& color, float alpha) noexcept {
    return {color.red, color.green, color.blue, color.alpha * (alpha < 0 ? 0 : alpha > 1 ? 1 : alpha)};
}

Color ui_settings_focus_color(const Theme& theme) noexcept {
    return pick(settings_style(theme.styles).focus_ring,
                ui_background_active(theme) ? theme.tokens.border.focus : Color{.416f, .659f, .957f, 1});
}

UiSettingsTokens ui_settings_tokens(const Theme& theme) noexcept {
    const auto& s = settings_style(theme.styles);
    const bool light = theme.appearance == ThemeAppearance::light;
    constexpr Color white{1, 1, 1, 1}, black{0, 0, 0, 1};
    UiSettingsTokens t{};
    t.light = light;
    auto& c = t.color;
    c.text_default = theme.text_primary;
    c.text_strong = pick(s.text_strong, ui_settings_color_mix(theme.text_primary, light ? black : white, .6f));
    c.text_subtle = pick(s.text_subtle, ui_settings_color_mix(theme.text_secondary, theme.text_primary, .2f));
    c.nav_header = pick(s.nav_header, ui_settings_color_mix(theme.text_secondary, theme.base_low, .28f));
    c.border_subtle = pick(s.border_subtle, neutral(light ? .24f : .122f));
    c.border_normal = pick(s.border_normal, neutral(light ? .36f : .2f));
    c.mod_subtle = pick(s.mod_subtle, neutral(light ? .16f : .122f));
    c.mod_normal = pick(s.mod_normal, neutral(light ? .22f : .161f));
    c.mod_strong = pick(s.mod_strong, neutral(light ? .28f : .2f));
    c.input_background = pick(s.input_background, ui_settings_color_alpha(black, light ? .04f : .122f));
    c.input_border = pick(s.input_border, neutral(light ? .36f : .2f));
    // Accent treatment: emphasis tokens follow a gradient theme's palette.
    c.brand = theme.tokens.emphasis.fill;
    c.brand_hover = pick(s.brand_hover, scaled(c.brand, .77f));
    c.brand_active = scaled(c.brand_hover, .86f);
    c.on_brand = ui_theme_contrast_ratio(theme.tokens.emphasis.text, c.brand_hover) >= 4.5f
                     ? theme.tokens.emphasis.text
                     : white;
    c.critical = pick(s.critical, ui_settings_color_mix(theme.notification, black, .13f));
    c.critical_hover = scaled(c.critical, .8f);
    c.critical_text = pick(s.critical_text, ui_settings_color_mix(theme.notification, light ? black : white,
                                                                  light ? .2f : .45f));
    c.critical_tint = pick(s.critical_tint, ui_settings_color_alpha(theme.notification, .1f));
    c.positive = pick(s.positive, theme.tokens.text.positive);
    c.focus_ring = ui_settings_focus_color(theme);
    c.scrim = pick(s.scrim, theme.tokens.overlay.scrim_strong);
    c.button_border = theme.tokens.border.subtle;
    c.switch_track = pick(s.switch_track, ui_settings_color_alpha(black, light ? .16f : .122f));
    c.switch_border = pick(s.switch_border, neutral(light ? .36f : .2f));
    c.switch_border_hover = c.switch_border;
    c.switch_border_hover.alpha = minimum(1.f, c.switch_border.alpha * 2);
    c.switch_border_on = c.border_subtle;
    c.thumb = white;
    c.slider_track =
        pick(s.slider_track, ui_settings_color_mix(theme.base_low, theme.text_secondary, light ? .35f : .25f));
    c.radio_border = pick(s.radio_border, neutral(.64f));
    c.radio_border_hover = neutral(.8f);
    c.modal_background = pick(s.modal_background, light ? theme.surface : theme.base_low);
    c.sidebar_background = pick(s.sidebar_background, theme.base_lower);
    c.nav_selected = ui_background_active(theme) ? theme.tokens.emphasis.selected : c.mod_strong;

    set_layout_tokens(t, s, theme.scale.space_1, theme.styles);
    return t;
}

UiSettingsTokens ui_settings_layout_tokens(const UiResolvedThemeStyles* styles) noexcept {
    UiSettingsTokens t{};
    set_layout_tokens(t, settings_style(styles), kDefaultTheme.scale.space_1, styles);
    return t;
}

float ui_settings_modal_entrance(const UiSettingsDurations& duration, u64 elapsed_ms) noexcept {
    const u64 total = duration.modal_enter_ms ? duration.modal_enter_ms : 1;
    return elapsed_ms >= total ? 1.f
                               : ui_motion_curve_sample(UiMotionCurve::spring,
                                                        static_cast<float>(elapsed_ms) / static_cast<float>(total));
}

} // namespace maicord
