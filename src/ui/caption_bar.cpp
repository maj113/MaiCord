#include "maicord/ui/caption_bar.hpp"

#include "maicord/ui/icon.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {
namespace {

float snap(float value, float scale) noexcept {
    const float pixels = value * scale;
    return static_cast<float>(static_cast<i32>(pixels + (pixels < 0 ? -.5f : .5f))) / scale;
}

// Translucent tokens composite over the palette base so the band stays opaque.
Color opaque_over(Color top, Color base) noexcept {
    const float a = top.alpha;
    return {top.red * a + base.red * (1 - a), top.green * a + base.green * (1 - a),
            top.blue * a + base.blue * (1 - a), 1};
}

UiCaptionPart button_part(u32 index) noexcept {
    return static_cast<UiCaptionPart>(static_cast<u32>(UiCaptionPart::minimize) + index);
}

bool push_box(PaintStream* paint, float left, float top, float right, float bottom, Color color) noexcept {
    return paint_stream_push(paint, left, top, right, bottom, color);
}

// Glyphs on the device grid: `extent` and `stroke` are whole pixels.
bool paint_glyph(PaintStream* paint, UiCaptionPart part, bool maximized, const UiRect& button, float extent,
                 float stroke, Color color, float scale) noexcept {
    const float x0 = snap((button.left + button.right - extent) * .5f, scale);
    const float y0 = snap((button.top + button.bottom - extent) * .5f, scale);
    const float x1 = x0 + extent, y1 = y0 + extent;
    switch (part) {
    case UiCaptionPart::minimize: {
        const float y = snap((y0 + y1 - stroke) * .5f, scale);
        return push_box(paint, x0, y, x1, y + stroke, color);
    }
    case UiCaptionPart::maximize: {
        if (!maximized)
            return paint_stream_push_rounded_border(paint, x0, y0, x1, y1, {}, color, 0, stroke);
        // Restore: a front square and the top and right edges of one behind it.
        const float offset = snap(extent * .2f, scale);
        return paint_stream_push_rounded_border(paint, x0, y0 + offset, x1 - offset, y1, {}, color, 0,
                                                stroke) &&
               push_box(paint, x0 + offset, y0, x1, y0 + stroke, color) &&
               push_box(paint, x1 - stroke, y0, x1, y1 - offset, color);
    }
    case UiCaptionPart::close: {
        const float inset = stroke * .5f;
        return paint_stream_push_line(paint, {x0 + inset, y0 + inset}, {x1 - inset, y1 - inset}, stroke,
                                      color) &&
               paint_stream_push_line(paint, {x1 - inset, y0 + inset}, {x0 + inset, y1 - inset}, stroke,
                                      color);
    }
    default:
        return true;
    }
}

} // namespace

float ui_caption_bar_height(const Theme& theme) noexcept {
    return theme.scale.caption_height;
}

UiCaptionBarLayout ui_caption_bar_layout(const Theme& theme, float width, const UiCaptionBarState& state,
                                         float height) noexcept {
    UiCaptionBarLayout layout{};
    layout.width = width > 0 ? width : 0;
    if (!(height > 0))
        height = ui_caption_bar_height(theme);
    layout.height = height;
    const float button = theme.scale.caption_button_width;
    float right = layout.width;
    for (u32 i = kUiCaptionButtonCount; i-- > 0;) {
        const float left = maximum(0.0f, right - button);
        layout.buttons[i] = {left, 0, right, height};
        right = left;
    }
    const float padding = theme.scale.caption_padding;
    const float icon = theme.scale.caption_icon;
    const float icon_top = (height - icon) * .5f;
    layout.icon = {padding, icon_top, padding + icon, icon_top + icon};
    layout.icon_target = {0, 0, minimum(layout.icon.right + padding * .5f, right), height};
    // Square trailing items, Settings next to the window buttons.
    if (state.settings || state.update != UiCaptionUpdate::hidden)
        right = maximum(0.0f, right - theme.scale.space_2);
    if (state.settings) {
        layout.settings = {maximum(0.0f, right - height), 0, right, height};
        right = layout.settings.left;
    }
    if (state.update != UiCaptionUpdate::hidden) {
        layout.update = {maximum(0.0f, right - height), 0, right, height};
        right = layout.update.left;
    }
    const float title_left = minimum(layout.icon.right + padding, right);
    layout.title = {title_left, 0, maximum(title_left, right - padding), height};
    return layout;
}

UiCaptionPart ui_caption_bar_part(const UiCaptionBarLayout& layout, float x, float y) noexcept {
    if (!(x >= 0 && y >= 0 && x < layout.width && y < layout.height))
        return UiCaptionPart::none;
    for (u32 i = 0; i < kUiCaptionButtonCount; ++i)
        if (x >= layout.buttons[i].left && x < layout.buttons[i].right)
            return button_part(i);
    if (x >= layout.settings.left && x < layout.settings.right)
        return UiCaptionPart::settings;
    if (x >= layout.update.left && x < layout.update.right)
        return UiCaptionPart::update;
    if (x < layout.icon_target.right)
        return UiCaptionPart::icon;
    return UiCaptionPart::caption;
}

Utf8View ui_caption_bar_part_name(UiCaptionPart part, const UiCaptionBarState& state) noexcept {
    switch (part) {
    case UiCaptionPart::icon:
        return utf8_literal("System menu");
    case UiCaptionPart::update:
        return state.update == UiCaptionUpdate::ready         ? utf8_literal("Restart to update")
               : state.update == UiCaptionUpdate::installing  ? utf8_literal("Installing update")
               : state.update == UiCaptionUpdate::downloading ? utf8_literal("Downloading update")
                                                              : Utf8View{};
    case UiCaptionPart::settings:
        return state.settings ? utf8_literal("Settings") : Utf8View{};
    case UiCaptionPart::minimize:
        return utf8_literal("Minimize");
    case UiCaptionPart::maximize:
        return state.maximized ? utf8_literal("Restore") : utf8_literal("Maximize");
    case UiCaptionPart::close:
        return utf8_literal("Close");
    default:
        return {};
    }
}

UiRect ui_caption_bar_part_rect(const UiCaptionBarLayout& layout, UiCaptionPart part) noexcept {
    switch (part) {
    case UiCaptionPart::icon:
        return layout.icon_target;
    case UiCaptionPart::update:
        return layout.update;
    case UiCaptionPart::settings:
        return layout.settings;
    case UiCaptionPart::minimize:
    case UiCaptionPart::maximize:
    case UiCaptionPart::close:
        return layout.buttons[static_cast<u32>(part) - static_cast<u32>(UiCaptionPart::minimize)];
    default:
        return {};
    }
}

Color ui_caption_bar_title_color(const Theme& theme, const UiCaptionBarState& state) noexcept {
    return state.active ? theme.tokens.caption.text : theme.tokens.caption.text_inactive;
}

bool ui_caption_bar_paint(PaintStream* paint, const Theme& theme, const UiCaptionBarLayout& layout,
                          const UiCaptionBarState& state, float pixel_scale) noexcept {
    if (!paint || !(pixel_scale > 0))
        return false;
    const auto& tokens = theme.tokens.caption;
    const Color background =
        opaque_over(state.active ? tokens.background : tokens.background_inactive, theme.base_lowest);
    if (!push_box(paint, 0, 0, layout.width, layout.height, background))
        return false;
    const float inset = theme.scale.caption_button_inset;
    // Concentric with the window's own corner at the trailing edge.
    const float radius = ui_inner_radius(theme.scale.window_radius, inset);
    const float stroke = maximum(1.0f, snap(1.0f, pixel_scale) * pixel_scale) / pixel_scale;
    const float extent = maximum(stroke * 4, snap(theme.scale.caption_glyph, pixel_scale));
    const auto fill_for = [&](UiCaptionPart part, bool* lit) noexcept {
        const bool pressed = state.pressed == part && state.hovered == part;
        *lit = pressed || (state.hovered == part && state.pressed == UiCaptionPart::none);
        return pressed ? tokens.pressed : tokens.hover;
    };
    const auto item_box = [&](const UiRect& item) noexcept {
        const float icon = snap(theme.scale.caption_icon, pixel_scale);
        const float left = snap((item.left + item.right - icon) * .5f, pixel_scale);
        const float top = snap((item.top + item.bottom - icon) * .5f, pixel_scale);
        return UiRect{left, top, left + icon, top + icon};
    };
    const Color idle = state.active ? tokens.icon : tokens.icon_inactive;
    if (state.settings && layout.settings.right > layout.settings.left) {
        bool lit = false;
        const Color fill = fill_for(UiCaptionPart::settings, &lit);
        const UiRect& r = layout.settings;
        if ((lit && !paint_stream_push_rounded(paint, r.left + inset, r.top + inset, r.right - inset,
                                               r.bottom - inset, fill, radius)) ||
            !ui_icon_paint(paint, UiIcon::settings, item_box(r), lit ? tokens.icon_hover : idle))
            return false;
    }
    if (state.update != UiCaptionUpdate::hidden && layout.update.right > layout.update.left) {
        bool lit = false;
        const Color hover = fill_for(UiCaptionPart::update, &lit);
        const UiRect& r = layout.update;
        const bool ready = state.update == UiCaptionUpdate::ready;
        // A ready update keeps its tint; hover and press layer on top.
        if ((ready && !paint_stream_push_rounded(paint, r.left + inset, r.top + inset, r.right - inset,
                                                 r.bottom - inset, tokens.badge_ready_fill, radius)) ||
            (lit && !paint_stream_push_rounded(paint, r.left + inset, r.top + inset, r.right - inset,
                                               r.bottom - inset, hover, radius)))
            return false;
        const UiRect box = item_box(r);
        if (!ui_icon_paint(paint, UiIcon::download, box,
                           ready ? tokens.badge_ready
                           : lit ? tokens.icon_hover
                                 : idle))
            return false;
        if (!ready) {
            // Progress under the icon; installing has no measure and fills it.
            const float gap = theme.scale.space_1;
            const float top = snap(box.bottom + gap, pixel_scale);
            const float bottom = top + maximum(stroke, snap(gap, pixel_scale));
            const float progress = state.update == UiCaptionUpdate::installing ? 1.0f
                                   : state.progress > 1                        ? 1.0f
                                   : state.progress > 0                        ? state.progress
                                                                               : 0.0f;
            const float end = box.left + (box.right - box.left) * progress;
            const float bar_radius = (bottom - top) * .5f;
            if (!paint_stream_push_rounded(paint, box.left, top, box.right, bottom, tokens.badge_track,
                                           bar_radius) ||
                (end > box.left && !paint_stream_push_rounded(paint, box.left, top, end, bottom,
                                                              tokens.badge_progress, bar_radius)))
                return false;
        }
    }
    for (u32 i = 0; i < kUiCaptionButtonCount; ++i) {
        const UiCaptionPart part = button_part(i);
        const UiRect& button = layout.buttons[i];
        if (button.right <= button.left)
            continue;
        const bool close = part == UiCaptionPart::close;
        const bool pressed = state.pressed == part && state.hovered == part;
        const bool hovered = state.hovered == part && (state.pressed == UiCaptionPart::none || pressed);
        if (pressed || hovered) {
            const Color fill = close ? (pressed ? tokens.close_pressed : tokens.close_hover)
                                     : (pressed ? tokens.pressed : tokens.hover);
            if (!paint_stream_push_rounded(paint, button.left + inset, button.top + inset,
                                           button.right - inset, button.bottom - inset, fill, radius))
                return false;
        }
        const Color ink = close && (pressed || hovered) ? tokens.close_icon
                          : hovered || pressed          ? tokens.icon_hover
                          : state.active                ? tokens.icon
                                                        : tokens.icon_inactive;
        if (!paint_glyph(paint, part, state.maximized, button, extent, stroke, ink, pixel_scale))
            return false;
    }
    return true;
}

} // namespace maicord
