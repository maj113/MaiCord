#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_color_math.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
namespace maicord {
namespace {
u32 at(UiThemeEditorElement e) noexcept {
    return static_cast<u32>(e);
}
u32 effect_index(UiThemeEditorElement e) noexcept {
    return e >= UiThemeEditorElement::effect_none && e <= UiThemeEditorElement::effect_both
               ? at(e) - at(UiThemeEditorElement::effect_none)
               : kBackgroundEffectCount;
}
// Same primitive and tokens as the Settings buttons; Apply and the selected
// segment use the primary (emphasis) variant.
bool editor_primary(const UiThemeEditorScene& s, UiThemeEditorElement e) noexcept {
    return e == UiThemeEditorElement::apply || (e == UiThemeEditorElement::dark && !s.light_mode) ||
           (e == UiThemeEditorElement::light && s.light_mode) ||
           (effect_index(e) < kBackgroundEffectCount && effect_index(e) == static_cast<u32>(s.draft.effect));
}
UiControlState editor_button_state(const UiThemeEditorScene& s, UiWidgetHandle handle) noexcept {
    const bool enabled = ui_widget_manager_input_enabled(s.widgets, handle);
    const bool hovered = enabled && s.widgets.hovered.slot == handle.slot &&
                         s.widgets.hovered.generation == handle.generation;
    const bool pressed = hovered && s.widgets.captured.slot == handle.slot &&
                         s.widgets.captured.generation == handle.generation;
    // The focus ring is painted once, last, outside each clipped button.
    return {enabled, hovered, pressed, false};
}
float clamp(float v, float lo = 0, float hi = 1) noexcept {
    return maximum(lo, minimum(v, hi));
}
bool unit(float v) noexcept {
    return v >= 0 && v <= 1;
}
bool range(UiThemeEditorElement e) noexcept {
    return e == UiThemeEditorElement::sv || e == UiThemeEditorElement::hue ||
           e == UiThemeEditorElement::direction || e == UiThemeEditorElement::intensity ||
           e == UiThemeEditorElement::speed || e == UiThemeEditorElement::strength ||
           e == UiThemeEditorElement::density;
}
bool control(UiThemeEditorElement e) noexcept {
    return at(e) >= at(UiThemeEditorElement::close) && e != UiThemeEditorElement::effect_label;
}
bool slider(UiThemeEditorElement e) noexcept {
    return range(e) && e != UiThemeEditorElement::sv && e != UiThemeEditorElement::hue;
}
void rgb_hsv(UiThemeEditorScene& s, u32 rgb) noexcept {
    const float r = ((rgb >> 16) & 255) / 255.f, g = ((rgb >> 8) & 255) / 255.f, b = (rgb & 255) / 255.f;
    const float high = maximum(r, maximum(g, b)), low = minimum(r, minimum(g, b)), d = high - low;
    s.value = high;
    s.saturation = high ? d / high : 0;
    if (d) {
        float h = high == r ? (g - b) / d : high == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h /= 6;
        if (h < 0)
            h += 1;
        s.hue = h;
    }
}
void numbers(UiThemeEditorScene& s) noexcept {
    constexpr char digits[] = "0123456789ABCDEF";
    s.hex[0] = '#';
    const u32 rgb = s.draft.colors[s.selected_stop];
    for (u32 i = 0; i < 6; ++i)
        s.hex[i + 1] = digits[(rgb >> (20 - i * 4)) & 15];
    s.hex[7] = 0;
    auto number = [](char* dst, u32 v) noexcept -> u8 {
        return static_cast<u8>(decimal_write(dst, v));
    };
    s.direction_bytes = number(s.direction_text, s.draft.angle_degrees);
    s.intensity_bytes = number(s.intensity_text, s.draft.intensity);
    s.intensity_text[s.intensity_bytes++] = '%';
    s.speed_bytes = number(s.speed_text, s.draft.speed);
    s.speed_text[s.speed_bytes++] = '%';
    s.strength_bytes = number(s.strength_text, s.draft.strength);
    s.strength_text[s.strength_bytes++] = '%';
    s.density_bytes = number(s.density_text, s.draft.density);
    s.density_text[s.density_bytes++] = '%';
}
bool style_valid(const UiThemeEditorStyle& s) noexcept {
    return s.width >= 300 && s.width <= 640 && s.height >= 360 && s.height <= 900 && s.padding >= 12 &&
           s.padding <= 32 && s.gap >= 8 && s.gap <= 24 && s.radius >= 0 && s.radius <= 24 &&
           s.control_radius >= 0 && s.control_radius <= 12;
}
bool tree(UiThemeEditorScene& s) noexcept {
    if (!s.storage.arena.base &&
        (!ui_fixed_widget_storage_initialize(&s.storage, kUiThemeEditorElements) ||
         !ui_widget_manager_initialize(&s.widgets, s.storage.widgets, s.storage.layout_nodes)))
        return false;
    ui_widget_manager_reset(&s.widgets);
    const float w = minimum(s.style.width, ui_rect_width(s.viewport) - 24),
                h = minimum(s.style.height, ui_rect_height(s.viewport) - 24);
    if (w < 260 || h < 220)
        return false;
    s.panel = {s.viewport.right - 12 - w, s.viewport.top + 12, s.viewport.right - 12,
               s.viewport.top + 12 + h};
    const float p = s.style.padding, footer_width = w - 2 * p, bw = footer_width - 12;
    UiRect rectangles[kUiThemeEditorElements]{};
    rectangles[at(UiThemeEditorElement::root)] = {0, 0, w, h};
    rectangles[at(UiThemeEditorElement::panel)] = {0, 0, w, h};
    rectangles[at(UiThemeEditorElement::title)] = {p, 14, w - 64, 48};
    rectangles[at(UiThemeEditorElement::close)] = {w - 48, 14, w - 16, 46};
    rectangles[at(UiThemeEditorElement::body)] = {p, 60, w - p, h - 64};
    rectangles[at(UiThemeEditorElement::dark)] = {0, 0, bw * .5f - 4, 36};
    rectangles[at(UiThemeEditorElement::light)] = {bw * .5f + 4, 0, bw, 36};
    rectangles[at(UiThemeEditorElement::gradient)] = {0, 0, bw, 30};
    const float sw = (bw - 92) / 5;
    for (u32 i = 0; i < 5; ++i)
        rectangles[at(UiThemeEditorElement::stop0) + i] = {i * sw, 42, (i + 1) * sw - 8, 74};
    rectangles[at(UiThemeEditorElement::add)] = {bw - 84, 42, bw - 48, 74};
    rectangles[at(UiThemeEditorElement::remove)] = {bw - 40, 42, bw, 74};
    rectangles[at(UiThemeEditorElement::sv)] = {0, 90, bw, 260};
    rectangles[at(UiThemeEditorElement::hue)] = {0, 272, bw, 292};
    rectangles[at(UiThemeEditorElement::hex)] = {0, 304, bw, 340};
    rectangles[at(UiThemeEditorElement::direction)] = {0, 352, bw, 402};
    rectangles[at(UiThemeEditorElement::intensity)] = {0, 414, bw, 464};
    rectangles[at(UiThemeEditorElement::surprise)] = {p, h - 50, p + (footer_width - 16) * .43f, h - 14};
    rectangles[at(UiThemeEditorElement::reset)] = {p + (footer_width - 16) * .43f + 8, h - 50,
                                                   p + (footer_width - 16) * .7f + 8, h - 14};
    rectangles[at(UiThemeEditorElement::apply)] = {p + (footer_width - 16) * .7f + 16, h - 50, w - p,
                                                   h - 14};
    rectangles[at(UiThemeEditorElement::scrollbar)] = {w - p - 12, 60, w - p, h - 64};
    for (u32 i = at(UiThemeEditorElement::gradient); i <= at(UiThemeEditorElement::intensity); ++i) {
        rectangles[i].top += 52;
        rectangles[i].bottom += 52;
    }
    rectangles[at(UiThemeEditorElement::effect_label)] = {0, 528, bw, 550};
    for (u32 i = 0; i < kBackgroundEffectCount; ++i) {
        const float x = static_cast<float>(i % 2) * (bw + 8) * .5f;
        const float y = 562 + static_cast<float>(i / 2) * 40.f;
        const auto element = static_cast<UiThemeEditorElement>(at(UiThemeEditorElement::effect_none) + i);
        rectangles[at(element)] = {x, y, x + (bw - 8) * .5f, y + 32};
    }
    // The fixed offsets below leave room for three rows of two effect cards.
    const float effect_extra_height = (static_cast<float>((kBackgroundEffectCount + 1) / 2) - 3.f) * 40.f;
    rectangles[at(UiThemeEditorElement::speed)] = {0, 690 + effect_extra_height, bw,
                                                   740 + effect_extra_height};
    rectangles[at(UiThemeEditorElement::strength)] = {0, 752 + effect_extra_height, bw,
                                                      802 + effect_extra_height};
    rectangles[at(UiThemeEditorElement::density)] = {0, 814 + effect_extra_height, bw,
                                                     864 + effect_extra_height};
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto e = static_cast<UiThemeEditorElement>(i);
        const auto r = rectangles[i];
        auto style = ui_layout_style();
        style.width = ui_layout_fixed(ui_rect_width(r));
        style.height = ui_layout_fixed(ui_rect_height(r));
        style.margin.left = r.left;
        style.margin.top = r.top;
        style.flags |= ui_layout_node_allow_overlap | ui_layout_node_clip_children;
        const bool body_child =
            (i >= at(UiThemeEditorElement::dark) && i <= at(UiThemeEditorElement::intensity)) ||
            (i >= at(UiThemeEditorElement::effect_label) && e != UiThemeEditorElement::scrollbar);
        UiWidgetHandle parent =
            i == 0 ? ui_widget_invalid_handle()
                   : s.elements[at(body_child ? UiThemeEditorElement::body : UiThemeEditorElement::panel)];
        if (e == UiThemeEditorElement::panel)
            parent = s.elements[0];
        if (e == UiThemeEditorElement::body)
            style.flags |= ui_layout_node_scroll_vertical;
        // Hex opens the scoped native text form. It has no inline edit
        // document, so its launcher uses the same button dispatch as actions.
        // Icon-only buttons carry their action as the widget name.
        const Utf8View name = e == UiThemeEditorElement::close    ? utf8_literal("Close theme editor")
                              : e == UiThemeEditorElement::add    ? utf8_literal("Add color stop")
                              : e == UiThemeEditorElement::remove ? utf8_literal("Remove color stop")
                                                                  : utf8_literal("ThemeEditor");
        const auto added = ui_widget_manager_add_prevalidated(
            &s.widgets, parent,
            {i + 1,
             e == UiThemeEditorElement::body ? UiWidgetKind::scroll_view
             : control(e)                    ? UiWidgetKind::button
                                             : UiWidgetKind::container,
             static_cast<u16>(control(e) ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                         : ui_widget_none),
             {name, style, {}, 0, i + 1}});
        if (!added.ok())
            return false;
        s.elements[i] = added.handle;
    }
    return ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok();
}
void enabled(UiThemeEditorScene& s) noexcept {
    const auto direction = s.elements[at(UiThemeEditorElement::direction)];
    if (auto* widget = ui_widget_manager_resolve(&s.widgets, direction)) {
        widget->flags = static_cast<u16>(s.draft.color_count > 1
                                             ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                             : ui_widget_none);
        if (s.draft.color_count == 1 && s.widgets.focused.slot == direction.slot &&
            s.widgets.focused.generation == direction.generation)
            ui_widget_manager_focus(&s.widgets, s.elements[at(UiThemeEditorElement::stop0)]);
    }
    if (auto* scrollbar =
            ui_widget_manager_resolve(&s.widgets, s.elements[at(UiThemeEditorElement::scrollbar)]))
        scrollbar->flags = ui_widget_enabled | ui_widget_hit_test;
    constexpr UiThemeEditorElement effects[]{UiThemeEditorElement::speed, UiThemeEditorElement::strength,
                                             UiThemeEditorElement::density};
    for (const auto e : effects) {
        auto* w = ui_widget_manager_resolve(&s.widgets, s.elements[at(e)]);
        if (w)
            w->flags = static_cast<u16>(s.draft.effect != BackgroundEffect::none
                                            ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                            : ui_widget_none);
    }
    for (u32 i = 0; i < 5; ++i) {
        auto* w = ui_widget_manager_resolve(&s.widgets, s.elements[at(UiThemeEditorElement::stop0) + i]);
        if (w)
            w->flags = static_cast<u16>(i < s.draft.color_count
                                            ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                            : ui_widget_none);
    }
    constexpr UiThemeEditorElement actions[]{UiThemeEditorElement::add, UiThemeEditorElement::remove};
    for (const auto e : actions) {
        auto* w = ui_widget_manager_resolve(&s.widgets, s.elements[at(e)]);
        if (w)
            w->flags = static_cast<u16>(
                (e == UiThemeEditorElement::add ? s.draft.color_count < 5 : s.draft.color_count > 1)
                    ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                    : ui_widget_none);
    }
}
UiRect track(UiRect r) noexcept {
    return {r.left + 8, r.bottom - 18, r.right - 8, r.bottom - 10};
}
bool publish(UiThemeEditorScene& s, BackgroundTheme next, BackgroundTheme* output) noexcept {
    if (!background_theme_valid(next) || background_theme_equal(next, s.draft))
        return false;
    s.draft = next;
    const u32 profile = background_effect_profile_index(next.effect);
    if (profile < kBackgroundEffectProfileCount)
        s.effect_profiles[profile] = background_effect_settings(next);
    numbers(s);
    enabled(s);
    ++s.revision;
    *output = next;
    return true;
}
} // namespace
u32 ui_theme_editor_hsv_rgb(float h, float s, float v) noexcept {
    if (!unit(h) || !unit(s) || !unit(v))
        return 0;
    const float sector = (h == 1 ? 0 : h) * 6;
    const u32 i = static_cast<u32>(sector);
    const float f = sector - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r = v, g = t, b = p;
    switch (i) {
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    default:
        break;
    }
    return static_cast<u32>(r * 255 + .5f) << 16 | static_cast<u32>(g * 255 + .5f) << 8 |
           static_cast<u32>(b * 255 + .5f);
}
bool ui_theme_editor_current(const UiThemeEditorScene& s) noexcept {
    return s.active && background_theme_valid(s.draft) &&
           ui_widget_manager_layout_node(s.widgets, s.elements[at(UiThemeEditorElement::panel)]);
}
bool ui_theme_editor_update(UiThemeEditorScene* s, const BackgroundTheme& draft, u8 selected,
                            UiRect viewport, const UiThemeEditorStyle* override_style,
                            const BackgroundEffectSettings* profiles) noexcept {
    const auto style = override_style ? *override_style : ui_theme_editor_style();
    if (!s || !background_theme_valid(draft) || selected >= draft.color_count || !ui_rect_valid(viewport) ||
        !style_valid(style))
        return false;
    const bool opening = !s->active;
    if (profiles)
        for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
            if (!background_effect_settings_valid(profiles[i]))
                return false;
    bool profiles_changed = false;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        const auto settings =
            profiles  ? profiles[i]
            : opening ? background_effect_settings_defaults(static_cast<BackgroundEffect>(i + 1))
                      : s->effect_profiles[i];
        profiles_changed =
            profiles_changed || !bytes_equal(&s->effect_profiles[i], &settings, sizeof(settings));
        s->effect_profiles[i] = settings;
    }
    const bool layout =
        opening || !ui_rect_equal(s->viewport, viewport) || !bytes_equal(&s->style, &style, sizeof(style));
    const bool selected_changed =
        !s->active || s->selected_stop != selected || s->draft.colors[selected] != draft.colors[selected];
    const bool changed = layout || profiles_changed || !background_theme_equal(s->draft, draft) ||
                         s->selected_stop != selected;
    s->viewport = viewport;
    s->style = style;
    s->draft = draft;
    const u32 profile = background_effect_profile_index(draft.effect);
    if (profile < kBackgroundEffectProfileCount)
        s->effect_profiles[profile] = background_effect_settings(draft);
    s->selected_stop = selected;
    if (selected_changed)
        rgb_hsv(*s, draft.colors[selected]);
    numbers(*s);
    if (layout && !tree(*s)) {
        ui_theme_editor_close(s);
        return false;
    }
    enabled(*s);
    s->active = true;
    if (opening)
        ++s->generation;
    if (changed)
        ++s->revision;
    return true;
}
void ui_theme_editor_close(UiThemeEditorScene* s) noexcept {
    if (!s)
        return;
    if (s->active)
        ++s->revision;
    s->active = false;
    ui_widget_manager_reset(&s->widgets);
}
void ui_theme_editor_release(UiThemeEditorScene* s) noexcept {
    if (!s)
        return;
    ui_theme_editor_close(s);
    ui_fixed_widget_storage_release(&s->storage);
    *s = {};
}
const UiLayoutNode* ui_theme_editor_node(const UiThemeEditorScene& s, UiThemeEditorElement e) noexcept {
    return at(e) < kUiThemeEditorElements ? ui_widget_manager_layout_node(s.widgets, s.elements[at(e)])
                                          : nullptr;
}
UiThemeEditorElement ui_theme_editor_hit_test(const UiThemeEditorScene& s, UiPoint p) noexcept {
    if (!ui_theme_editor_current(s))
        return UiThemeEditorElement::count;
    const auto h = ui_widget_manager_hit_test(s.widgets, p.x, p.y);
    for (u32 i = 0; i < kUiThemeEditorElements; ++i)
        if (h.slot == s.elements[i].slot && h.generation == s.elements[i].generation)
            return static_cast<UiThemeEditorElement>(i);
    return UiThemeEditorElement::count;
}
bool ui_theme_editor_focus(UiThemeEditorScene* s, UiThemeEditorElement e) noexcept {
    if (!s || !ui_theme_editor_current(*s) || at(e) >= kUiThemeEditorElements)
        return false;
    const auto* widget = ui_widget_manager_resolve(s->widgets, s->elements[at(e)]);
    if (!widget || (widget->flags & (ui_widget_enabled | ui_widget_focusable)) !=
                       (ui_widget_enabled | ui_widget_focusable))
        return false;
    const auto result = ui_widget_reveal(&s->widgets, s->elements[at(e)]);
    if (result.scrolled)
        ++s->revision;
    return result.ok() && ui_widget_manager_focus(&s->widgets, s->elements[at(e)]);
}
bool ui_theme_editor_move_focus(UiThemeEditorScene* s, bool reverse) noexcept {
    if (!s || !ui_theme_editor_current(*s))
        return false;
    const auto old = s->widgets.focused;
    constexpr UiThemeEditorElement order[]{UiThemeEditorElement::close,
                                           UiThemeEditorElement::dark,
                                           UiThemeEditorElement::light,
                                           UiThemeEditorElement::gradient,
                                           UiThemeEditorElement::stop0,
                                           UiThemeEditorElement::stop1,
                                           UiThemeEditorElement::stop2,
                                           UiThemeEditorElement::stop3,
                                           UiThemeEditorElement::stop4,
                                           UiThemeEditorElement::add,
                                           UiThemeEditorElement::remove,
                                           UiThemeEditorElement::sv,
                                           UiThemeEditorElement::hue,
                                           UiThemeEditorElement::hex,
                                           UiThemeEditorElement::direction,
                                           UiThemeEditorElement::intensity,
                                           UiThemeEditorElement::effect_none,
                                           UiThemeEditorElement::effect_stars,
                                           UiThemeEditorElement::effect_comets,
                                           UiThemeEditorElement::effect_both,
                                           UiThemeEditorElement::speed,
                                           UiThemeEditorElement::strength,
                                           UiThemeEditorElement::density,
                                           UiThemeEditorElement::surprise,
                                           UiThemeEditorElement::reset,
                                           UiThemeEditorElement::apply};
    constexpr u32 count = sizeof(order) / sizeof(order[0]);
    u32 current = 0;
    for (u32 i = 0; i < count; ++i)
        if (s->elements[at(order[i])].slot == old.slot)
            current = i;
    for (u32 i = 1; i <= count; ++i) {
        const u32 n = reverse ? (current + count - i) % count : (current + i) % count;
        if (ui_theme_editor_focus(s, order[n]))
            return true;
    }
    return false;
}
bool ui_theme_editor_scroll(UiThemeEditorScene* s, float delta) noexcept {
    if (!s || !ui_theme_editor_current(*s) || !paint_coordinate_valid(delta))
        return false;
    UiWidgetScrollGeometry g{};
    const auto h = s->elements[at(UiThemeEditorElement::body)];
    const auto* w = ui_widget_manager_resolve(s->widgets, h);
    if (!w || !ui_widget_scroll_geometry(s->widgets, h, &g))
        return false;
    const float y = clamp(g.offset.y + delta, 0, g.maximum.y);
    if (y == g.offset.y || !ui_layout_engine_set_scroll(&s->widgets.layout, w->layout, {0, y}))
        return false;
    if (!ui_widget_manager_layout_rectangle(&s->widgets, s->panel).ok())
        return false;
    ++s->revision;
    return true;
}
UiScrollbarGeometry ui_theme_editor_scrollbar(const UiThemeEditorScene& s) noexcept {
    const auto* node = ui_theme_editor_node(s, UiThemeEditorElement::body);
    UiWidgetScrollGeometry geometry{};
    if (!node ||
        !ui_widget_scroll_geometry(s.widgets, s.elements[at(UiThemeEditorElement::body)], &geometry))
        return {};
    return ui_scrollbar_geometry(node->rectangle, geometry.offset.y,
                                 ui_rect_height(node->rectangle) + geometry.maximum.y);
}
bool ui_theme_editor_pointer(UiThemeEditorScene* s, UiThemeEditorElement e, UiPoint p,
                             BackgroundTheme* out) noexcept {
    if (!s || !out || !ui_theme_editor_current(*s) || (!range(e) && e != UiThemeEditorElement::gradient) ||
        !paint_coordinate_valid(p.x) || !paint_coordinate_valid(p.y))
        return false;
    if (e == UiThemeEditorElement::direction && s->draft.color_count == 1)
        return false;
    if ((e == UiThemeEditorElement::speed || e == UiThemeEditorElement::strength ||
         e == UiThemeEditorElement::density) &&
        s->draft.effect == BackgroundEffect::none)
        return false;
    const auto* n = ui_theme_editor_node(*s, e);
    if (!n || !ui_rect_has_area(ui_rect_intersection(n->rectangle, n->clip_rectangle)))
        return false;
    auto next = s->draft;
    const auto r = slider(e) ? track(n->rectangle) : n->rectangle;
    const float x = clamp((p.x - r.left) / ui_rect_width(r)), y = clamp((p.y - r.top) / ui_rect_height(r));
    if (e == UiThemeEditorElement::gradient)
        return ui_theme_editor_action(
            s,
            static_cast<UiThemeEditorElement>(at(UiThemeEditorElement::stop0) +
                                              static_cast<u32>(x * (next.color_count - 1) + .5f)),
            0, out);
    const float old_hue = s->hue, old_saturation = s->saturation, old_value = s->value;
    if (e == UiThemeEditorElement::sv) {
        s->saturation = x;
        s->value = 1 - y;
        next.colors[s->selected_stop] = ui_theme_editor_hsv_rgb(s->hue, s->saturation, s->value);
    } else if (e == UiThemeEditorElement::hue) {
        s->hue = x;
        next.colors[s->selected_stop] = ui_theme_editor_hsv_rgb(s->hue, s->saturation, s->value);
    } else if (e == UiThemeEditorElement::direction)
        next.angle_degrees = background_theme_snap_angle(x * 360);
    else if (e == UiThemeEditorElement::intensity)
        next.intensity = static_cast<u8>(x * 100 + .5f);
    else if (e == UiThemeEditorElement::speed)
        next.speed = static_cast<u8>(x * 100 + .5f);
    else if (e == UiThemeEditorElement::strength)
        next.strength = static_cast<u8>(x * 100 + .5f);
    else
        next.density = static_cast<u8>(x * 100 + .5f);
    if (publish(*s, next, out))
        return true;
    if (old_hue != s->hue || old_saturation != s->saturation || old_value != s->value) {
        ++s->revision;
        *out = next;
        return true;
    }
    return false;
}
bool ui_theme_editor_key(UiThemeEditorScene* s, UiThemeEditorElement e, u32 key,
                         BackgroundTheme* out) noexcept {
    if (!s || !range(e))
        return false;
    const auto* n = ui_theme_editor_node(*s, e);
    if (!n)
        return false;
    if (e == UiThemeEditorElement::direction) {
        if (!out || !ui_theme_editor_current(*s) || s->draft.color_count == 1 ||
            !ui_rect_has_area(ui_rect_intersection(n->rectangle, n->clip_rectangle)))
            return false;
        auto next = s->draft;
        if (key == 0x24)
            next.angle_degrees = 0;
        else if (key == 0x23)
            next.angle_degrees = 360;
        else if (key == 0x25)
            next.angle_degrees = next.angle_degrees ? static_cast<u16>(next.angle_degrees - 1) : 0;
        else if (key == 0x27)
            next.angle_degrees = static_cast<u16>(minimum(360u, static_cast<u32>(next.angle_degrees) + 1));
        else
            return false;
        return publish(*s, next, out);
    }
    const auto r = slider(e) ? track(n->rectangle) : n->rectangle;
    float x = e == UiThemeEditorElement::sv          ? s->saturation
              : e == UiThemeEditorElement::hue       ? s->hue
              : e == UiThemeEditorElement::direction ? s->draft.angle_degrees / 360.f
              : e == UiThemeEditorElement::speed     ? s->draft.speed / 100.f
              : e == UiThemeEditorElement::strength  ? s->draft.strength / 100.f
              : e == UiThemeEditorElement::density   ? s->draft.density / 100.f
                                                     : s->draft.intensity / 100.f,
          y = 1 - s->value;
    const float step =
        e == UiThemeEditorElement::direction || e == UiThemeEditorElement::hue ? 1 / 360.f : .01f;
    if (key == 0x24)
        x = 0;
    else if (key == 0x23)
        x = 1;
    else if (key == 0x25)
        x -= step;
    else if (key == 0x27)
        x += step;
    else if (e == UiThemeEditorElement::sv && key == 0x26)
        y -= .01f;
    else if (e == UiThemeEditorElement::sv && key == 0x28)
        y += .01f;
    else
        return false;
    return ui_theme_editor_pointer(
        s, e, {r.left + clamp(x) * ui_rect_width(r), r.top + clamp(y) * ui_rect_height(r)}, out);
}
bool ui_theme_editor_action(UiThemeEditorScene* s, UiThemeEditorElement e, u32 seed,
                            BackgroundTheme* out) noexcept {
    if (!s || !ui_theme_editor_current(*s) || !out)
        return false;
    auto next = s->draft;
    bool profiles_reset = false;
    if (effect_index(e) < kBackgroundEffectCount) {
        const auto effect = static_cast<BackgroundEffect>(effect_index(e));
        if (effect == next.effect)
            return false;
        const u32 previous = background_effect_profile_index(next.effect);
        if (previous < kBackgroundEffectProfileCount)
            s->effect_profiles[previous] = background_effect_settings(next);
        next.effect = effect;
        const u32 profile = background_effect_profile_index(effect);
        const auto settings = profile < kBackgroundEffectProfileCount
                                  ? s->effect_profiles[profile]
                                  : background_effect_settings_defaults(effect);
        next.speed = settings.speed;
        next.strength = settings.strength;
        next.density = settings.density;
        return publish(*s, next, out);
    }
    const u32 selected = s->selected_stop;
    if (e >= UiThemeEditorElement::stop0 && e <= UiThemeEditorElement::stop4) {
        const auto i = at(e) - at(UiThemeEditorElement::stop0);
        if (i >= next.color_count || i == selected)
            return false;
        s->selected_stop = static_cast<u8>(i);
        rgb_hsv(*s, next.colors[i]);
        numbers(*s);
        ++s->revision;
        *out = next;
        return true;
    }
    if (e == UiThemeEditorElement::gradient)
        return false;
    if (e == UiThemeEditorElement::add) {
        BackgroundTheme added{};
        if (!background_theme_add_color(next, &added))
            return false;
        next = added;
        s->selected_stop = static_cast<u8>(next.color_count - 1);
    } else if (e == UiThemeEditorElement::remove) {
        if (next.color_count == 1)
            return false;
        for (u32 i = selected; i + 1 < next.color_count; ++i)
            next.colors[i] = next.colors[i + 1];
        next.colors[--next.color_count] = 0;
        s->selected_stop = static_cast<u8>(minimum(selected, static_cast<u32>(next.color_count - 1)));
    } else if (e == UiThemeEditorElement::reset) {
        next = background_theme_defaults();
        for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
            const auto defaults = background_effect_settings_defaults(static_cast<BackgroundEffect>(i + 1));
            const auto previous = s->effect_profiles[i];
            profiles_reset = profiles_reset || previous.speed != defaults.speed ||
                             previous.strength != defaults.strength || previous.density != defaults.density;
            s->effect_profiles[i] = defaults;
        }
        s->selected_stop = 0;
    } else if (e == UiThemeEditorElement::surprise) {
        BackgroundTheme randomized{};
        if (!background_theme_randomized(next, seed, &randomized))
            return false;
        next = randomized;
        s->selected_stop = static_cast<u8>(minimum(selected, static_cast<u32>(next.color_count - 1)));
    } else
        return false;
    bool changed = publish(*s, next, out);
    if (!changed && (profiles_reset || selected != s->selected_stop)) {
        numbers(*s);
        enabled(*s);
        ++s->revision;
        *out = next;
        changed = true;
    }
    if (changed)
        rgb_hsv(*s, next.colors[s->selected_stop]);
    return changed;
}
bool ui_theme_editor_paint(const UiThemeEditorScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!ui_theme_editor_current(s))
        return true;
    if (!paint)
        return false;
    if (!ui_surface_shadow_paint_isolated(paint, s.panel, theme, UiElevation::dialog, s.style.radius) ||
        !paint_stream_push_clip(paint, s.panel.left, s.panel.top, s.panel.right, s.panel.bottom))
        return false;
    auto tokens = ui_settings_tokens(theme);
    tokens.metric.control_radius = s.style.control_radius;
    bool ok = ui_surface_paint_plate(paint, s.panel, theme, UiElevation::dialog, s.style.radius, &theme.surface,
                                     &theme.interactive);
    for (u32 i = at(UiThemeEditorElement::close); ok && i < kUiThemeEditorElements; ++i) {
        const auto e = static_cast<UiThemeEditorElement>(i);
        if (e == UiThemeEditorElement::effect_label || e == UiThemeEditorElement::scrollbar)
            continue;
        const auto* n = ui_theme_editor_node(s, e);
        if (!n)
            continue;
        const auto clip = ui_rect_intersection(n->rectangle, n->clip_rectangle);
        if (!ui_rect_has_area(clip))
            continue;
        if (!paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom)) {
            ok = false;
            break;
        }
        const auto r = n->rectangle;
        if (e == UiThemeEditorElement::gradient) {
            auto horizontal = s.draft;
            horizontal.angle_degrees = 90;
            ok = paint_stream_push_palette_gradient(paint, r, horizontal, s.style.control_radius);
        } else if (e == UiThemeEditorElement::sv) {
            const float x = clamp(r.left + s.saturation * ui_rect_width(r), r.left + 6, r.right - 6);
            const float y = clamp(r.top + (1 - s.value) * ui_rect_height(r), r.top + 6, r.bottom - 6);
            ok = paint_stream_push_sv_square(paint, r, background_color_rgb24(ui_theme_editor_hsv_rgb(s.hue, 1, 1)),
                                             s.style.control_radius) &&
                 paint_stream_push_rounded_border(paint, x - 5, y - 5, x + 5, y + 5, Color{}, background_color_rgb24(0), 5,
                                                  2) &&
                 paint_stream_push_rounded_border(paint, x - 4, y - 4, x + 4, y + 4, Color{},
                                                  background_color_rgb24(0xffffff), 4, 1.5f);
        } else if (e == UiThemeEditorElement::hue) {
            const float x = clamp(r.left + s.hue * ui_rect_width(r), r.left + 4, r.right - 4);
            ok = paint_stream_push_hue_strip(paint, r, 4) &&
                 paint_stream_push_rounded_border(paint, x - 3, r.top + 1, x + 3, r.bottom - 1, Color{},
                                                  background_color_rgb24(0), 3, 2) &&
                 paint_stream_push_rounded_border(paint, x - 2, r.top + 2, x + 2, r.bottom - 2, Color{},
                                                  background_color_rgb24(0xffffff), 2, 1);
        } else if (slider(e)) {
            const auto t = track(r);
            const float fraction = e == UiThemeEditorElement::direction  ? s.draft.angle_degrees / 360.f
                                   : e == UiThemeEditorElement::speed    ? s.draft.speed / 100.f
                                   : e == UiThemeEditorElement::strength ? s.draft.strength / 100.f
                                   : e == UiThemeEditorElement::density  ? s.draft.density / 100.f
                                                                         : s.draft.intensity / 100.f;
            const bool active =
                (e == UiThemeEditorElement::direction && s.draft.color_count > 1) ||
                e == UiThemeEditorElement::intensity ||
                (e != UiThemeEditorElement::direction && s.draft.effect != BackgroundEffect::none);
            const float x = t.left + fraction * ui_rect_width(t);
            ok = paint_stream_push_rounded(paint, t.left, t.top, t.right, t.bottom, theme.interactive,
                                           theme.scale.radius_sm) &&
                 paint_stream_push_rounded(paint, t.left, t.top, x, t.bottom,
                                           active ? theme.tokens.emphasis.fill : theme.text_secondary, theme.scale.radius_sm) &&
                 paint_stream_push_rounded_border(paint, x - 7, t.top - 3, x + 7, t.bottom + 3,
                                                  active ? theme.text_primary : theme.text_secondary,
                                                  theme.surface, 7, 1);
        } else if (e >= UiThemeEditorElement::stop0 && e <= UiThemeEditorElement::stop4) {
            const auto stop = i - at(UiThemeEditorElement::stop0);
            const auto c = stop < s.draft.color_count ? background_color_rgb24(s.draft.colors[stop]) : theme.base_low;
            ok = paint_stream_push_rounded_border(
                paint, r.left, r.top, r.right, r.bottom, c,
                stop == s.selected_stop ? theme.text_primary : theme.interactive, s.style.control_radius,
                stop == s.selected_stop ? 2.f : 1.f);
        } else {
            const auto variant =
                editor_primary(s, e) ? UiSettingsButtonVariant::primary : UiSettingsButtonVariant::secondary;
            const auto state = editor_button_state(s, s.elements[i]);
            ok = ui_settings_button_paint(paint, tokens, r, variant, state);
            // Close and the stop count buttons are icon buttons (x, plus, minus), not text glyphs.
            if (ok && (e == UiThemeEditorElement::close || e == UiThemeEditorElement::add ||
                       e == UiThemeEditorElement::remove)) {
                const float extent = ui_icon_button_extent(minimum(ui_rect_width(r), ui_rect_height(r)) - 12);
                const float x = (r.left + r.right - extent) * .5f, y = (r.top + r.bottom - extent) * .5f;
                ok = ui_icon_paint(paint,
                                   e == UiThemeEditorElement::close ? UiIcon::close
                                   : e == UiThemeEditorElement::add ? UiIcon::plus
                                                                    : UiIcon::minus,
                                   {x, y, x + extent, y + extent}, ui_settings_button_text(tokens, variant, state));
            }
        }
        const bool popped = paint_stream_pop_clip(paint);
        ok = ok && popped;
    }
    if (ok && s.widgets.focused.generation)
        ok = paint_stream_push_focus(paint, s.widgets, theme.tokens.border.focus);
    if (ok) {
        const auto handle = s.elements[at(UiThemeEditorElement::scrollbar)];
        const bool hovered =
            s.widgets.hovered.slot == handle.slot && s.widgets.hovered.generation == handle.generation;
        const bool pressed =
            s.widgets.captured.slot == handle.slot && s.widgets.captured.generation == handle.generation;
        ok = ui_scrollbar_paint(paint, ui_theme_editor_scrollbar(s), theme,
                                pressed   ? UiScrollbarVisualState::pressed
                                : hovered ? UiScrollbarVisualState::hovered
                                          : UiScrollbarVisualState::normal);
    }
    const bool popped = paint_stream_pop_clip(paint);
    return ok && popped;
}
bool ui_theme_editor_text_output(const UiThemeEditorScene& s, const Theme& theme,
                                 UiTextOutput* output) noexcept {
    if (!ui_theme_editor_current(s))
        return true;
    if (!ui_text_output_valid(output))
        return false;
    const auto tokens = ui_settings_tokens(theme);
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto e = static_cast<UiThemeEditorElement>(i);
        Utf8View text{};
        switch (e) {
        case UiThemeEditorElement::title:
            text = utf8_literal("Customize theme");
            break;
        case UiThemeEditorElement::dark:
            text = utf8_literal("Dark");
            break;
        case UiThemeEditorElement::light:
            text = utf8_literal("Light");
            break;
        case UiThemeEditorElement::hex:
            text = {s.hex, 7};
            break;
        case UiThemeEditorElement::direction:
            text = utf8_literal("Direction");
            break;
        case UiThemeEditorElement::intensity:
            text = utf8_literal("Intensity");
            break;
        case UiThemeEditorElement::effect_label:
            text = s.draft.effect == BackgroundEffect::none
                       ? utf8_literal("Choose an effect to tune motion")
                       : utf8_literal("Background effects");
            break;
        case UiThemeEditorElement::effect_none:
            text = utf8_literal("None");
            break;
        case UiThemeEditorElement::effect_stars:
            text = utf8_literal("Stars");
            break;
        case UiThemeEditorElement::effect_comets:
            text = utf8_literal("Comets");
            break;
        case UiThemeEditorElement::effect_both:
            text = utf8_literal("Stars and comets");
            break;
        case UiThemeEditorElement::speed:
            text = utf8_literal("Speed");
            break;
        case UiThemeEditorElement::strength:
            text = utf8_literal("Strength");
            break;
        case UiThemeEditorElement::density:
            text = utf8_literal("Density");
            break;
        case UiThemeEditorElement::surprise:
            text = utf8_literal("Surprise me");
            break;
        case UiThemeEditorElement::reset:
            text = utf8_literal("Reset");
            break;
        case UiThemeEditorElement::apply:
            text = s.save_failed ? utf8_literal("Retry save") : utf8_literal("Apply");
            break;
        default:
            break;
        }
        if (!text.bytes)
            continue;
        const auto* n = ui_theme_editor_node(s, e);
        if (!n)
            continue;
        UiRect r = n->rectangle, clip = ui_rect_intersection(r, n->clip_rectangle);
        if (!ui_rect_has_area(clip))
            continue;
        const bool field_slider = slider(e);
        const bool button_ink = !field_slider && e != UiThemeEditorElement::title &&
                                e != UiThemeEditorElement::effect_label;
        if (field_slider)
            r.bottom = r.top + 22;
        if (!ui_text_output_push(
                output,
                {text,
                 {&s.widgets, s.elements[i]},
                 r,
                 clip,
                 button_ink ? ui_settings_button_text(tokens,
                                                      editor_primary(s, e) ? UiSettingsButtonVariant::primary
                                                                           : UiSettingsButtonVariant::secondary,
                                                      editor_button_state(s, s.elements[i]))
                 : e == UiThemeEditorElement::effect_label ||
                         (e == UiThemeEditorElement::direction && s.draft.color_count == 1)
                     ? theme.text_secondary
                     : theme.text_primary,
                 e == UiThemeEditorElement::title ? text_style_bold : 0,
                 e == UiThemeEditorElement::title || field_slider || e == UiThemeEditorElement::effect_label
                     ? UiTextAlignment::start
                     : UiTextAlignment::center,
                 UiTextAlignment::center,
                 UiTextWrapping::no_wrap,
                 UiTextOverflow::ellipsis,
                 UiTextFace::normal,
                 {},
                 {}}))
            return false;
        if (field_slider) {
            r.left = r.right - 60;
            const auto label =
                e == UiThemeEditorElement::direction  ? Utf8View{s.direction_text, s.direction_bytes}
                : e == UiThemeEditorElement::speed    ? Utf8View{s.speed_text, s.speed_bytes}
                : e == UiThemeEditorElement::strength ? Utf8View{s.strength_text, s.strength_bytes}
                : e == UiThemeEditorElement::density  ? Utf8View{s.density_text, s.density_bytes}
                                                      : Utf8View{s.intensity_text, s.intensity_bytes};
            if (!ui_text_output_push(output, {label,
                                              {&s.widgets, s.elements[i]},
                                              r,
                                              clip,
                                              theme.text_secondary,
                                              0,
                                              UiTextAlignment::end,
                                              UiTextAlignment::center,
                                              UiTextWrapping::no_wrap,
                                              UiTextOverflow::clip,
                                              UiTextFace::normal,
                                              {},
                                              {}}))
                return false;
        }
    }
    return true;
}
} // namespace maicord
