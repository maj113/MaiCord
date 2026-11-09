#include "store_internal.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/theme/background_surface.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/text/edit_field.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"

namespace maicord {
using namespace store_detail;
namespace {

struct Pass {
    const UiStoreScene& s;
    const Theme& theme;
    const UiSettingsThemeTokens t;
    Pass(const UiStoreScene& scene, const Theme& th) noexcept : s(scene), theme(th), t(th) {}

    Color ink(u8 role) const noexcept {
        const auto& c = t.color;
        switch (static_cast<UiStoreInk>(role)) {
        case UiStoreInk::strong:
            return c.text_strong;
        case UiStoreInk::subtle:
            return c.text_subtle;
        case UiStoreInk::header:
            return c.nav_header;
        case UiStoreInk::link:
            return theme.tokens.text.link;
        case UiStoreInk::warning:
            return theme.tokens.text.warning;
        case UiStoreInk::critical:
            return c.critical_text;
        case UiStoreInk::positive:
            return c.positive;
        case UiStoreInk::brand:
            return c.brand;
        case UiStoreInk::on_brand:
            return c.on_brand;
        case UiStoreInk::on_scrim:
            return theme.tokens.overlay.text;
        default:
            return c.text_default;
        }
    }
    Color fill(u8 role) const noexcept {
        const auto& c = t.color;
        switch (static_cast<UiStoreFill>(role)) {
        case UiStoreFill::subtle:
            return c.mod_subtle;
        case UiStoreFill::normal:
            return c.mod_normal;
        case UiStoreFill::strong:
            return c.mod_strong;
        case UiStoreFill::input:
            return c.input_background;
        case UiStoreFill::selected:
            return theme.tokens.emphasis.selected;
        case UiStoreFill::translucent:
            return theme.tokens.fill.translucent;
        case UiStoreFill::scrim:
            return theme.tokens.overlay.scrim;
        case UiStoreFill::positive_tint:
            return ui_settings_color_alpha(theme.tokens.accent.success, .16f);
        case UiStoreFill::warning_tint:
            return ui_settings_color_alpha(theme.tokens.accent.warning, .10f);
        case UiStoreFill::critical_tint:
            return c.critical_tint;
        case UiStoreFill::brand:
            return c.brand;
        case UiStoreFill::critical:
            return c.critical;
        case UiStoreFill::success:
            return theme.tokens.accent.success;
        case UiStoreFill::caution:
            return theme.tokens.accent.warning;
        case UiStoreFill::danger:
            return theme.tokens.accent.danger;
        default:
            return {};
        }
    }
    Color rgb(u32 value) const noexcept {
        return {((value >> 16) & 255) / 255.f, ((value >> 8) & 255) / 255.f, (value & 255) / 255.f, 1};
    }
    UiControlState state(const UiStoreNode& n) const noexcept {
        const bool on = n.action != 0;
        return {!(n.flags & ui_store_disabled), on && n.action == s.hover_action, on && n.action == s.press_action,
                on && s.focus_visible && n.action == s.focus_action};
    }
    // The focused node's ring waits for the end of its layer, so a neighbour drawn later (the next segment of a
    // track) cannot cover it.
    mutable UiRect ring{}, ring_clip{};
    mutable float ring_radius = 0;
    mutable bool ring_set = false;
    void focus(const UiRect& shape, float radius) const noexcept {
        ring = shape;
        ring_radius = radius;
        ring_set = true;
    }
};

void paint_ring(UiPaint& p, const Pass& k) noexcept {
    if (k.ring_set)
        p.clip(k.ring_clip).use<ui_settings_focus_paint>(k.t, k.ring, k.ring_radius).unclip();
}

struct ChipLook {
    UiStoreFill fill;
    UiStoreInk ink;
};
ChipLook chip_look(UiStoreTone tone) noexcept {
    switch (tone) {
    case UiStoreTone::positive:
        return {UiStoreFill::positive_tint, UiStoreInk::positive};
    case UiStoreTone::warning:
        return {UiStoreFill::warning_tint, UiStoreInk::warning};
    case UiStoreTone::dev:
        return {UiStoreFill::critical_tint, UiStoreInk::critical};
    case UiStoreTone::brand:
        return {UiStoreFill::brand, UiStoreInk::on_brand};
    case UiStoreTone::mono:
        return {UiStoreFill::normal, UiStoreInk::normal};
    case UiStoreTone::risk_low:
    case UiStoreTone::risk_medium:
    case UiStoreTone::risk_high:
        return {UiStoreFill::subtle, UiStoreInk::normal};
    case UiStoreTone::scrim:
        return {UiStoreFill::scrim, UiStoreInk::on_scrim};
    default:
        return {UiStoreFill::subtle, UiStoreInk::subtle};
    }
}

UiSettingsButtonVariant plate_variant(UiStoreButton variant) noexcept {
    switch (variant) {
    case UiStoreButton::primary:
        return UiSettingsButtonVariant::primary;
    case UiStoreButton::critical:
        return UiSettingsButtonVariant::critical;
    default:
        return UiSettingsButtonVariant::secondary;
    }
}
bool plated(UiStoreButton variant) noexcept {
    return variant == UiStoreButton::secondary || variant == UiStoreButton::primary || variant == UiStoreButton::critical;
}

Color button_ink(const Pass& k, const UiStoreNode& n) noexcept {
    const auto variant = static_cast<UiStoreButton>(n.variant);
    const auto state = k.state(n);
    if (plated(variant))
        return ui_settings_button_text(k.t, plate_variant(variant), state);
    if (variant == UiStoreButton::ghost_critical)
        return k.t.color.critical_text;
    if (variant == UiStoreButton::link)
        return k.theme.tokens.text.link;
    return state.hovered ? k.t.color.text_strong : k.t.color.text_default;
}

float disc_radius(const UiRect& r) noexcept {
    return minimum(ui_rect_width(r), ui_rect_height(r)) * .5f;
}

float pulse_mix(const UiStoreScene& s) noexcept {
    if (s.reduced_motion)
        return 0;
    const float phase = static_cast<float>(s.clock_ms % 1600) / 1600.f;
    const float tri = phase < .5f ? phase * 2 : 2 - phase * 2;
    return tri * tri * (3 - 2 * tri);
}
// How far the last activated switch has slid toward its new state (1 for every other switch, and while it has not
// reached that state).
float toggle_motion(const UiStoreScene& s, const UiStoreNode& n) noexcept {
    if (!s.toggle_action || n.action != s.toggle_action || (n.value != 0) != s.toggle_to)
        return 1.f;
    if (!s.toggle_started_ms)
        return 0.f;
    const float elapsed = s.clock_ms > s.toggle_started_ms ? static_cast<float>(s.clock_ms - s.toggle_started_ms) : 0.f;
    return s.toggle_ms ? minimum(1.f, elapsed / static_cast<float>(s.toggle_ms)) : 1.f;
}

void paint_icon(UiPaint& p, u8 icon, const UiRect& box, const Color& color) noexcept {
    if (icon != kNoIcon)
        p.icon(static_cast<UiIcon>(icon), box, color);
}
UiRect centred(const UiRect& r, float size) noexcept {
    const float x = (r.left + r.right - size) * .5f, y = (r.top + r.bottom - size) * .5f;
    return {x, y, x + size, y + size};
}

// A theme's four colours as a small chat window: rail, sidebar, header, messages and composer.
void paint_palette(UiPaint& p, const Pass& k, const UiRect& r, const u32* sw) noexcept {
    const Color bg = k.rgb(sw[0]), surface = k.rgb(sw[1]), text = k.rgb(sw[2]), accent = k.rgb(sw[3]);
    const float mx = ui_rect_width(r) * .06f, my = ui_rect_height(r) * .07f;
    const UiRect w{r.left + mx, r.top + my, r.right - mx, r.bottom - my};
    const float ww = ui_rect_width(w), wh = ui_rect_height(w);
    p.rounded(w, bg, 6);
    p.rounded_border(w, Color{}, ui_settings_color_alpha(text, .14f), 6, 1);
    p.fill({w.left + ww * .07f, w.top + 1, w.left + ww * .27f, w.bottom - 1}, surface);
    p.fill({w.left + ww * .27f, w.top + 1, w.right - 1, w.top + wh * .1f}, surface);
    for (u32 i = 0; i < 4; ++i) {
        const float y = w.top + wh * (.17f + .17f * i), x = w.left + ww * .31f, d = wh * .08f;
        p.rounded({x, y, x + d, y + d}, i % 2 ? ui_settings_color_alpha(text, .4f) : accent, d * .5f);
        p.rounded({x + d * 1.4f, y, x + d * 1.4f + ww * .12f, y + wh * .028f}, i % 2 ? ui_settings_color_alpha(text, .7f) : accent, 2);
        p.rounded({x + d * 1.4f, y + wh * .05f, x + d * 1.4f + ww * (i % 2 ? .3f : .4f), y + wh * .075f}, ui_settings_color_alpha(text, .5f), 2);
    }
    p.rounded({w.left + ww * .3f, w.bottom - wh * .13f, w.right - ww * .03f, w.bottom - wh * .04f}, surface, 4);
    for (u32 i = 0; i < 4; ++i)
        p.rounded({w.left + ww * .02f, w.top + wh * (.1f + .09f * i), w.left + ww * .05f, w.top + wh * (.1f + .09f * i) + wh * .06f},
                  i ? ui_settings_color_alpha(text, .3f) : accent, 3);
}

void paint_node(UiPaint& p, const Pass& k, const UiStoreNode& n, const UiRect& r, u32 layer) noexcept {
    const auto& t = k.t;
    const auto& c = t.color;
    const bool foreground = (n.flags & ui_store_foreground) != 0;
    if (layer < 2 && foreground != (layer == 1))
        return;
    const auto state = k.state(n);
    switch (n.kind) {
    case UiStoreKind::column:
    case UiStoreKind::row:
    case UiStoreKind::flow:
    case UiStoreKind::split:
    case UiStoreKind::grid:
        if (n.kind == UiStoreKind::row && n.variant == 1)
            p.rounded_border(r, c.input_background, c.input_border, t.metric.control_radius, 1);
        else if (n.fill)
            p.fill(r, k.fill(n.fill));
        if (n.fill == static_cast<u8>(UiStoreFill::translucent) && n.kind == UiStoreKind::row)
            p.separator({r.left, r.top, r.right, r.top + 1});
        if (n.flags & ui_store_hairline)
            p.separator({r.left + n.pad_x, r.bottom - 1, r.right - n.pad_x, r.bottom});
        break;
    case UiStoreKind::card: {
        Color fill = n.fill ? k.fill(n.fill) : c.mod_subtle, border = c.border_subtle;
        if (n.action && state.hovered) {
            fill = c.mod_normal;
            border = c.border_normal;
        }
        // Translucent plates (the live preview's mock panels) keep the subtle edge, so they read over a dark effect.
        if (n.fill == static_cast<u8>(UiStoreFill::warning_tint))
            border = ui_settings_color_alpha(k.theme.tokens.accent.warning, .35f);
        else if (n.fill == static_cast<u8>(UiStoreFill::critical_tint))
            border = ui_settings_color_alpha(c.critical, .35f);
        p.rounded_border(r, fill, border, kRadius, border.alpha > 0 ? 1.f : 0.f);
        if (state.focused)
            k.focus(r, kRadius);
        break;
    }
    case UiStoreKind::preview: {
        const bool top_only = (n.flags & ui_store_clip) != 0;
        const bool live = n.variant == static_cast<u8>(UiStoreAspect::live) && !(n.flags & ui_store_critical);
        const float radius = ui_rect_width(r) < 100 ? 6.f : kRadius;
        p.clip(r);
        if (live)
            p.use<ui_background_paint_tile>(r, k.theme.base_lowest, radius);
        else
            p.rounded(top_only ? UiRect{r.left, r.top, r.right, r.bottom + radius} : r, c.mod_subtle, radius);
        if (n.flags & ui_store_critical)
            p.icon(UiIcon::image_off, centred(r, 32), c.text_subtle);
        else if (n.data)
            paint_palette(p, k, r, static_cast<const u32*>(n.data));
        p.unclip();
        if (top_only)
            p.fill({r.left, r.bottom - 1, r.right, r.bottom}, c.border_subtle);
        else
            p.rounded_border(r, Color{}, c.border_subtle, radius, 1);
        break;
    }
    case UiStoreKind::icon:
        paint_icon(p, n.icon, r, k.ink(n.ink));
        break;
    case UiStoreKind::tile: {
        const bool large = ui_rect_width(r) >= 56;
        const float radius = large ? 12 : 8;
        const auto tone = static_cast<UiStoreTone>(n.variant);
        Color fill = k.theme.tokens.emphasis.selected, glyph = k.theme.tokens.emphasis.outline,
              border = ui_settings_color_alpha(k.theme.tokens.emphasis.outline, .25f);
        if (tone == UiStoreTone::neutral) {
            fill = k.theme.tokens.fill.subtle;
            glyph = c.text_default;
            border = {};
        } else if (tone == UiStoreTone::dev) {
            fill = c.critical_tint;
            glyph = c.critical_text;
            border = ui_settings_color_alpha(c.critical, .25f);
        }
        p.rounded_border(r, fill, border, radius, border.alpha > 0 ? 1.f : 0.f);
        paint_icon(p, n.icon, centred(r, ui_rect_width(r) * .5f), glyph);
        break;
    }
    case UiStoreKind::chip: {
        const auto tone = static_cast<UiStoreTone>(n.variant);
        const auto look = chip_look(tone);
        if ((n.flags & ui_store_foreground) && tone != UiStoreTone::scrim)
            p.rounded(r, k.theme.tokens.overlay.scrim, kChipHeight * .5f);
        p.rounded(r, k.fill(static_cast<u8>(look.fill)), kChipHeight * .5f);
        if (tone >= UiStoreTone::risk_low && tone <= UiStoreTone::risk_high) {
            const u8 role = tone == UiStoreTone::risk_low      ? static_cast<u8>(UiStoreFill::success)
                            : tone == UiStoreTone::risk_medium ? static_cast<u8>(UiStoreFill::caution)
                                                               : static_cast<u8>(UiStoreFill::danger);
            const float cy = (r.top + r.bottom) * .5f;
            p.rounded({r.left + 8, cy - 4, r.left + 16, cy + 4}, k.fill(role), 4);
        } else if (n.icon != kNoIcon)
            paint_icon(p, n.icon, {r.left + 7, r.top + 5, r.left + 21, r.top + 19}, k.ink(static_cast<u8>(look.ink)));
        break;
    }
    case UiStoreKind::button: {
        const auto variant = static_cast<UiStoreButton>(n.variant);
        if (plated(variant))
            p.use<ui_settings_button_paint>(t, r, plate_variant(variant), state);
        else if (variant == UiStoreButton::ghost || variant == UiStoreButton::ghost_critical) {
            if (state.hovered || state.pressed)
                p.rounded(r, c.mod_subtle, t.metric.control_radius);
            if (state.focused)
                k.focus(r, t.metric.control_radius);
        } else if (state.focused)
            k.focus(r, 4.f);
        const Color ink = button_ink(k, n);
        const float pad = variant == UiStoreButton::link ? 0 : n.type ? 16 : 12;
        const float content = leaf_text_content(n);
        float x = (n.flags & ui_store_stretch) ? r.left + (ui_rect_width(r) - content) * .5f : r.left + pad;
        const float cy = (r.top + r.bottom) * .5f;
        if (n.flags & ui_store_busy)
            p.spinner({x, cy - 8, x + 16, cy + 8}, ink, k.s.spinner_phase);
        else if (n.icon != kNoIcon)
            p.icon(static_cast<UiIcon>(n.icon), {x, cy - 8, x + 16, cy + 8}, ink);
        if (n.icon2 != kNoIcon) {
            const float right = (n.flags & ui_store_stretch) ? x + content : r.right - pad;
            p.icon(static_cast<UiIcon>(n.icon2), {right - 14, cy - 7, right, cy + 7}, ink);
        }
        break;
    }
    case UiStoreKind::icon_button: {
        const bool plate = n.variant != 0;
        if (plate || state.hovered || state.pressed)
            p.rounded(r, n.variant == 2 ? k.theme.tokens.overlay.scrim : state.pressed ? c.mod_normal : c.mod_subtle,
                      n.variant == 2 ? ui_rect_height(r) * .5f : 8);
        if (state.focused)
            k.focus(r, n.variant == 2 ? ui_rect_height(r) * .5f : 8.f);
        const Color ink = n.ink == static_cast<u8>(UiStoreInk::normal) ? (state.hovered ? c.text_strong : c.text_subtle)
                                                                       : k.ink(n.ink);
        paint_icon(p, n.icon, centred(r, ui_rect_height(r) >= 32 ? 18 : 16), n.variant == 2 ? k.theme.tokens.overlay.text : ink);
        break;
    }
    case UiStoreKind::toggle:
        p.use<ui_switch_paint>(t, r, n.value != 0, toggle_motion(k.s, n), k.s.reduced_motion, state);
        break;
    case UiStoreKind::check: {
        const UiRect box{r.left, r.top + 4, r.left + 16, r.top + 20};
        p.rounded_border(box, n.value ? c.brand : c.input_background, n.value ? c.brand : c.input_border, 4, 1);
        if (n.value)
            p.icon(UiIcon::check, {box.left + 1, box.top + 1, box.right - 1, box.bottom - 1}, c.on_brand);
        if (state.focused)
            k.focus(box, 4.f);
        break;
    }
    case UiStoreKind::filter: {
        const bool selected = (n.flags & ui_store_selected) != 0;
        Color fill = selected ? k.theme.tokens.emphasis.selected : c.mod_subtle;
        Color border = selected ? k.theme.tokens.emphasis.outline : c.border_subtle;
        if (state.hovered && !selected) {
            fill = c.mod_normal;
            border = c.border_normal;
        }
        p.rounded_border(r, fill, border, kFilterHeight * .5f, 1);
        const float cy = (r.top + r.bottom) * .5f;
        if (selected)
            p.icon(UiIcon::check, {r.left + 12, cy - 7, r.left + 26, cy + 7}, c.text_strong);
        else if (n.icon != kNoIcon)
            p.icon(static_cast<UiIcon>(n.icon), {r.left + 12, cy - 7, r.left + 26, cy + 7}, c.text_default);
        if (state.focused)
            k.focus(r, kFilterHeight * .5f);
        break;
    }
    case UiStoreKind::tab: {
        if (n.flags & ui_store_selected)
            p.rounded({r.left, r.bottom - 2, r.right, r.bottom}, c.brand, 1);
        if (n.text2.bytes) {
            const float width = text_width(static_cast<u8>(UiStoreType::body), n.text);
            const UiRect badge = tab_badge(n, r, width);
            const bool attention = n.variant == 1;
            p.rounded(badge, attention ? c.brand : c.mod_strong, kBadgeHeight * .5f);
        }
        if (state.focused)
            k.focus(UiRect{r.left - 4, r.top + 6, r.right + 4, r.bottom - 4}, 6.f);
        break;
    }
    case UiStoreKind::search: {
        const bool editing = k.s.search && k.s.search->action == n.action && k.s.search->view && k.s.search->view->focused;
        const bool has_text = n.value != 0;
        UiControlState field = state;
        field.focused = field.focused || editing;
        p.use<ui_input_shell_paint>(t, r, field, false);
        const float cy = (r.top + r.bottom) * .5f;
        p.icon(UiIcon::search, {r.left + 12, cy - 8, r.left + 28, cy + 8}, c.text_subtle);
        if (has_text) {
            const UiRect clear = search_clear_rect(r);
            p.rounded(clear, state.hovered && k.s.hover_clear ? c.mod_strong : c.mod_normal, 12);
            p.icon(UiIcon::close, centred(clear, 14), c.text_strong);
        } else if (n.text2.bytes) {
            const UiRect hint = search_hint_rect(n, r);
            p.rounded_border(hint, Color{}, c.border_normal, 4, 1);
        }
        if (editing && k.s.search->geometry) {
            const UiRect content = search_text_rect(n, r);
            p.use<ui_edit_field_paint_content>(content, *k.s.search->view, k.s.search->geometry, k.theme, 96u);
        }
        break;
    }
    case UiStoreKind::dropdown:
        p.use<ui_select_paint>(t, r, false, state, false);
        break;
    case UiStoreKind::segment:
        if (n.flags & ui_store_selected)
            p.rounded_border(r, c.mod_strong, c.border_normal, 6, 1);
        else if (state.hovered)
            p.rounded(r, c.mod_subtle, 6);
        if (state.focused)
            k.focus(r, 6.f);
        break;
    case UiStoreKind::slider: {
        const u32 steps = n.count > 1 ? n.count : 2;
        const auto geometry = ui_slider_geometry(t, r, steps, n.value < steps ? n.value : steps - 1, false);
        p.use<ui_slider_paint>(t, geometry, steps, n.aux ? n.aux - 1u : static_cast<u32>(kUiSettingsNoDefault), state);
        break;
    }
    case UiStoreKind::swatch: {
        const Color fill = n.variant == 2 ? c.brand : n.variant == 3 ? k.theme.tokens.accent.danger : n.variant == 4 ? k.theme.tokens.accent.warning
                           : n.variant == 5 ? k.theme.tokens.accent.success : k.rgb(n.value);
        if (n.variant >= 2)
            p.rounded(r, fill, disc_radius(r));
        else if (n.variant == 1)
            p.rounded_border(r, fill, c.border_subtle, 8, 1);
        else
            p.rounded_border(r, fill, c.border_normal, disc_radius(r), 1);
        break;
    }
    case UiStoreKind::bar:
        p.rounded(r, ui_settings_color_mix(n.variant == 2 ? c.mod_strong : c.mod_normal, c.mod_strong, pulse_mix(k.s)),
                  n.variant == 1 ? ui_rect_height(r) * .5f : n.variant == 2 ? 8.f : 4.f);
        break;
    case UiStoreKind::rule:
        p.separator(r);
        break;
    case UiStoreKind::disc:
        p.rounded(r, c.mod_subtle, 32);
        paint_icon(p, n.icon, centred(r, 40), c.text_subtle);
        break;
    case UiStoreKind::spinner:
        p.spinner(r, c.text_subtle, k.s.spinner_phase);
        break;
    default:
        break;
    }
}

u32 page_end(const UiStoreScene& s) noexcept {
    return s.dialog_root != kUiStoreNone ? s.dialog_root : s.count;
}
} // namespace

bool ui_store_scene_paint(const UiStoreScene& s, const Theme& theme, PaintStream* out, u32 layer) noexcept {
    if (!s.active || s.page_root == kUiStoreNone)
        return true;
    g_measure = s.measure;
    const Pass k(s, theme);
    UiPaint p{out, theme};
    if (layer < 2) {
        p.clip(s.panel);
        k.ring_clip = s.panel;
        for (u32 i = 0; p && i < page_end(s); ++i) {
            const auto& n = s.nodes[i];
            const UiRect r = screen_rect(s, static_cast<u16>(i));
            if (!hidden(n) && ui_rect_has_area(r) && ui_rect_intersects(r, s.panel))
                paint_node(p, k, n, r, layer);
        }
        paint_ring(p, k);
        if (layer == 0 && s.content_height > ui_rect_height(s.panel))
            p.scrollbar(ui_scrollbar_geometry({s.panel.left, s.column.top, s.panel.right, s.panel.bottom}, s.scroll,
                                              s.content_height));
        return p.unclip().end();
    }
    if (!s.dialog_open)
        return true;
    if (s.dialog_bar)
        p.popover(s.dialog_rect, 12);
    else
        p.dialog(s.modal, s.dialog_rect);
    for (u32 i = s.dialog_root; p && i < s.count; ++i) {
        const auto& n = s.nodes[i];
        const UiRect r = screen_rect(s, static_cast<u16>(i));
        const UiRect clip = clip_of(s, static_cast<u16>(i));
        if (hidden(n) || !ui_rect_has_area(r) || !ui_rect_intersects(r, clip))
            continue;
        p.clip(clip);
        const bool ringed = k.ring_set;
        paint_node(p, k, n, r, layer);
        p.unclip();
        if (!ringed && k.ring_set)
            k.ring_clip = clip;
    }
    paint_ring(p, k);
    return p.unclip().end();
}

bool ui_store_scene_search_geometry(const UiStoreScene& s, UiRect* text) noexcept {
    if (!s.active || !s.search)
        return false;
    const u16 index = find_action(s, s.search->action);
    if (index == kUiStoreNone)
        return false;
    *text = search_text_rect(s.nodes[index], screen_rect(s, index));
    return ui_rect_has_area(*text);
}

bool ui_store_scene_text_output(const UiStoreScene& s, const Theme& theme, UiTextOutput* out, bool dialog) noexcept {
    if (!s.active || s.page_root == kUiStoreNone || (dialog && !s.dialog_open))
        return true;
    g_measure = s.measure;
    const Pass k(s, theme);
    const auto& c = k.t.color;
    UiTextPen<UiTextClip::region> text{out, s.widgets, dialog ? s.dialog_rect : s.panel};
    const u32 first = dialog ? s.dialog_root : 0, end = dialog ? s.count : page_end(s);
    for (u32 i = first; i < end; ++i) {
        const auto& n = s.nodes[i];
        const UiRect r = screen_rect(s, static_cast<u16>(i));
        const UiRect clip = clip_of(s, static_cast<u16>(i));
        if (hidden(n) || !ui_rect_has_area(r) || !ui_rect_intersects(r, clip))
            continue;
        const auto put = [&](const Utf8View& view, const UiRect& rect, const Color& color, u8 type, u32 mode = 0) noexcept {
            if (type == static_cast<u8>(UiStoreType::mono)) {
                // The code face: the typography's own family, not the fallback of an inline code span.
                if (view.bytes && ui_rect_has_area(rect))
                    text.emit({view, {&s.widgets, s.root}, rect, clip, color, kTextStyleSizeCodeXs, UiTextAlignment::start,
                               UiTextAlignment::center, TextWrapping::no_wrap, TextOverflow::ellipsis, UiTextFace::mono});
                return;
            }
            text.in(clip)(s.root, view, rect, color, type_metrics(type).style, mode);
        };
        switch (n.kind) {
        case UiStoreKind::text:
            put(n.text, r, k.ink(n.ink), n.type,
                ((n.flags & ui_store_wrap) ? ui_text_wrap : 0u) | ((n.flags & ui_store_center) ? ui_text_center : 0u) |
                    ((n.flags & ui_store_end) ? ui_text_end : 0u));
            break;
        case UiStoreKind::chip: {
            const auto tone = static_cast<UiStoreTone>(n.variant);
            const auto look = chip_look(tone);
            const float inset = tone >= UiStoreTone::risk_low && tone <= UiStoreTone::risk_high ? 22 : n.icon != kNoIcon ? 26 : 8;
            put(n.text, {r.left + inset, r.top, r.right - 8, r.bottom}, k.ink(static_cast<u8>(look.ink)),
                static_cast<u8>(tone == UiStoreTone::mono ? UiStoreType::mono : UiStoreType::small));
            break;
        }
        case UiStoreKind::button: {
            const Color ink = button_ink(k, n);
            const float content = leaf_text_content(n);
            const float pad = n.variant == static_cast<u8>(UiStoreButton::link) ? 0 : n.type ? 16 : 12;
            const float x = (n.flags & ui_store_stretch) ? r.left + (ui_rect_width(r) - content) * .5f : r.left + pad;
            const float left = x + (leaf_leading_glyph(n) ? 24 : 0);
            put(n.text, {left, r.top, left + text_width(static_cast<u8>(UiStoreType::label_bold), n.text) + 2, r.bottom}, ink,
                static_cast<u8>(UiStoreType::label_bold));
            break;
        }
        case UiStoreKind::filter: {
            const bool selected = (n.flags & ui_store_selected) != 0;
            const float left = r.left + 12 + (selected || n.icon != kNoIcon ? 20 : 0);
            const float width = text_width(static_cast<u8>(UiStoreType::label_bold), n.text);
            put(n.text, {left, r.top, left + width + 2, r.bottom}, selected ? c.text_strong : c.text_default,
                static_cast<u8>(UiStoreType::label_bold));
            if (n.text2.bytes)
                put(n.text2, {left + width + 5, r.top, r.right - 8, r.bottom}, c.text_subtle,
                    static_cast<u8>(UiStoreType::small));
            break;
        }
        case UiStoreKind::tab: {
            const float width = text_width(static_cast<u8>(UiStoreType::body), n.text);
            put(n.text, {r.left, r.top, r.left + width + 2, r.bottom - 2},
                (n.flags & ui_store_selected) ? c.text_strong : c.text_subtle, static_cast<u8>(UiStoreType::body));
            if (n.text2.bytes)
                put(n.text2, tab_badge(n, r, width), n.variant == 1 ? c.on_brand : c.text_strong,
                    static_cast<u8>(UiStoreType::small_bold), ui_text_center);
            break;
        }
        case UiStoreKind::search: {
            const UiRect content = search_text_rect(n, r);
            if (s.search && s.search->action == n.action && s.search->view) {
                const UiStoreSearch& search = *s.search;
                const UiRect measured = search.geometry && search.view->focused && ui_rect_has_area(search.geometry->text)
                                            ? search.geometry->text
                                            : content;
                text.in(ui_rect_intersection(content, clip))(s.root, search.view->text.bytes ? search.view->text : search.placeholder,
                                                             measured, search.view->text.bytes ? c.text_strong : c.text_subtle,
                                                             type_metrics(static_cast<u8>(UiStoreType::body)).style, 0);
            } else
                put(n.text, content, c.text_subtle, static_cast<u8>(UiStoreType::body));
            if (!n.value && n.text2.bytes) {
                const UiRect hint = search_hint_rect(n, r);
                put(n.text2, hint, c.nav_header, static_cast<u8>(UiStoreType::eyebrow), ui_text_center);
            }
            break;
        }
        case UiStoreKind::dropdown: {
            const float label = text_width(static_cast<u8>(UiStoreType::label), n.text);
            put(n.text, {r.left + 12, r.top, r.left + 12 + label + 2, r.bottom}, c.text_subtle,
                static_cast<u8>(UiStoreType::label));
            put(n.text2, {r.left + 12 + label + 8, r.top, r.right - 32, r.bottom}, c.text_strong,
                static_cast<u8>(UiStoreType::label));
            break;
        }
        case UiStoreKind::segment: {
            const bool selected = (n.flags & ui_store_selected) != 0;
            put(n.text, r, selected ? c.text_strong : c.text_default,
                static_cast<u8>(selected ? UiStoreType::label_bold : UiStoreType::label), ui_text_center);
            break;
        }
        case UiStoreKind::check:
            put(n.text, {r.left + 28, r.top, r.right, r.bottom}, c.text_default, static_cast<u8>(UiStoreType::body));
            break;
        case UiStoreKind::markdown:
            if (n.data) {
                auto* block = const_cast<UiMarkdown*>(static_cast<const UiMarkdown*>(n.data));
                for (u32 part = 0, parts = ui_markdown_part_count(*block); part < parts; ++part)
                    for (u32 marker = 0; marker < 2; ++marker) {
                        const auto command = ui_markdown_command(block, part, marker != 0, nullptr, {&s.widgets, s.root}, r,
                                                                 clip, c.text_default, c.text_subtle);
                        if (command.text.bytes)
                            text.emit(command);
                    }
            } else
                put(n.text, r, c.text_default, static_cast<u8>(UiStoreType::paragraph), ui_text_wrap);
            break;
        default:
            break;
        }
    }
    return text.ok();
}

bool ui_store_scene_next_media(const UiStoreScene& s, u32* cursor, UiStoreMedia* media) noexcept {
    if (!s.active || !s.media_key || s.page_root == kUiStoreNone)
        return false;
    for (; *cursor < page_end(s); ++*cursor) {
        const auto& n = s.nodes[*cursor];
        if (n.kind != UiStoreKind::preview || !n.value || hidden(n) ||
            (n.flags & (ui_store_critical | ui_store_busy)) || n.variant == static_cast<u8>(UiStoreAspect::live))
            continue;
        const UiRect r = screen_rect(s, static_cast<u16>(*cursor));
        if (!ui_rect_intersects(r, s.panel))
            continue;
        UiStoreMedia result{};
        if (!s.media_key(s.media_context, n.value, r, &result.key, &result.domain))
            continue;
        result.slot = n.value;
        result.rect = r;
        result.clip = s.panel;
        result.mask = (n.flags & ui_store_clip) ? UiRect{r.left, r.top, r.right, r.bottom + kRadius} : r;
        result.mask_radius = ui_rect_width(r) < 100 ? 6.f : kRadius;
        *media = result;
        ++*cursor;
        return true;
    }
    return false;
}

bool ui_store_scene_live_visible(const UiStoreScene& s) noexcept {
    if (!s.active || s.page_root == kUiStoreNone || (s.dialog_open && !s.dialog_bar))
        return false;
    for (u32 i = 0; i < page_end(s); ++i) {
        const auto& n = s.nodes[i];
        if (n.kind == UiStoreKind::preview && n.variant == static_cast<u8>(UiStoreAspect::live) && !hidden(n) &&
            !(n.flags & ui_store_critical) && ui_rect_intersects(screen_rect(s, static_cast<u16>(i)), s.panel))
            return true;
    }
    return false;
}

} // namespace maicord
