#include "maicord/ui/caption_bar.hpp"

#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/media/asset.hpp"

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

// Glyphs on the device grid: `extent` and `stroke` are whole pixels.
bool paint_glyph(PaintStream* paint, UiCaptionPart part, bool maximized, const UiRect& button, float extent,
                 float stroke, Color color, float scale) noexcept {
    const float x0 = snap((button.left + button.right - extent) * .5f, scale);
    const float y0 = snap((button.top + button.bottom - extent) * .5f, scale);
    const float x1 = x0 + extent, y1 = y0 + extent;
    switch (part) {
    case UiCaptionPart::minimize: {
        const float y = snap((y0 + y1 - stroke) * .5f, scale);
        return paint_stream_push(paint, x0, y, x1, y + stroke, color);
    }
    case UiCaptionPart::maximize: {
        if (!maximized)
            return paint_stream_push_rounded_border(paint, x0, y0, x1, y1, {}, color, 0, stroke);
        // Restore: a front square and the top and right edges of one behind it.
        const float offset = snap(extent * .2f, scale);
        return paint_stream_push_rounded_border(paint, x0, y0 + offset, x1 - offset, y1, {}, color, 0,
                                                stroke) &&
               paint_stream_push(paint, x0 + offset, y0, x1, y0 + stroke, color) &&
               paint_stream_push(paint, x1 - stroke, y0, x1, y1 - offset, color);
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

UiCaptionTitle ui_caption_title(const UiCaptionTitleInput& in) noexcept {
    if (!in.signed_in)
        return {utf8_literal("MaiCord"), UiIcon::count, {}};
    if (!in.home && !in.friends && in.guild_id && in.guild_name.bytes)
        return {in.guild_name, UiIcon::guild,
                in.guild_icon_variant ? media_asset_guild_icon(in.guild_id, in.guild_icon_variant, false)
                                      : MediaAssetKey{}};
    if (!in.friends && in.home && (in.private_type == 1 || in.private_type == 3))
        return {utf8_literal("Direct Messages"), UiIcon::direct_message, {}};
    if (in.friends)
        return {utf8_literal("Friends"), UiIcon::members, {}};
    return {{}, UiIcon::count, {}};
}
float ui_caption_title_text_limit(const UiCaptionBarLayout& layout, bool icon, float icon_extent) noexcept {
    const float span = minimum(layout.title.right - layout.title.left, layout.width * .5f);
    return maximum(0.0f, span - (icon ? icon_extent + kUiCaptionTitleGap : 0.0f));
}
UiCaptionTitlePlacement ui_caption_title_place(const UiCaptionBarLayout& layout, float text_width, bool icon,
                                               float icon_extent) noexcept {
    const float lead = icon ? icon_extent + kUiCaptionTitleGap : 0.0f;
    const float group = lead + maximum(0.0f, text_width);
    const float left =
        maximum(layout.title.left, minimum((layout.width - group) * .5f, layout.title.right - group));
    UiCaptionTitlePlacement placement{{}, left + lead};
    if (icon) {
        const float top = (layout.height - icon_extent) * .5f;
        placement.icon = {left, top, left + icon_extent, top + icon_extent};
    }
    return placement;
}

float ui_caption_bar_height(const Theme& theme) noexcept {
    return theme.scale.caption_height;
}

UiCaptionBarLayout ui_caption_bar_layout(const Theme& theme, float width, const UiCaptionBarState& state,
                                         float height, float pixel_scale) noexcept {
    UiCaptionBarLayout layout{};
    layout.width = width > 0 ? width : 0;
    if (!(height > 0))
        height = ui_caption_bar_height(theme);
    if (!(pixel_scale > 0))
        pixel_scale = 1;
    layout.height = height;
    // Whole-pixel insets keep the fills centred in the band. The side frame
    // moves every fill outward so the close fill's right inset, seen from the
    // visible window edge, equals its top inset.
    const float inset = maximum(0.0f, snap(theme.scale.caption_button_inset, pixel_scale));
    const float frame = state.frame > 0 ? minimum(snap(state.frame, pixel_scale), inset) : 0.0f;
    const auto fill = [&](const UiRect& r) noexcept {
        return r.right > r.left ? UiRect{r.left + inset + frame, inset, r.right - inset + frame, height - inset}
                                : UiRect{};
    };
    layout.fill_radius = ui_inner_radius(theme.scale.window_radius, inset);
    const float button = theme.scale.caption_button_width;
    float right = layout.width;
    for (u32 i = kUiCaptionButtonCount; i-- > 0;) {
        const float left = maximum(0.0f, right - button);
        layout.buttons[i] = {left, 0, right, height};
        layout.fills[i] = fill(layout.buttons[i]);
        right = left;
    }
    const float padding = theme.scale.caption_padding;
    const float icon = theme.scale.caption_icon;
    // As far from the visible left edge as from the top.
    const float icon_top = (height - icon) * .5f;
    const float icon_left = maximum(0.0f, icon_top - (state.frame > 0 ? state.frame : 0.0f));
    layout.icon = {icon_left, icon_top, icon_left + icon, icon_top + icon};
    layout.icon_target = {0, 0, minimum(layout.icon.right + padding * .5f, right), height};
    // Square trailing items, Settings next to the window buttons.
    if (state.settings || state.update != UiCaptionUpdate::hidden)
        right = maximum(0.0f, right - theme.scale.space_2);
    if (state.settings) {
        layout.settings = {maximum(0.0f, right - height), 0, right, height};
        layout.settings_fill = fill(layout.settings);
        right = layout.settings.left;
    }
    if (state.update != UiCaptionUpdate::hidden) {
        layout.update = {maximum(0.0f, right - height), 0, right, height};
        layout.update_fill = fill(layout.update);
        right = layout.update.left;
    }
    const float title_left = minimum(layout.icon.right + padding, right);
    layout.title = {title_left, 0, maximum(title_left, right - padding), height};
    layout.title_glyph = UiIcon::count;
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

UiCaptionNative ui_caption_bar_native(UiCaptionPart part) noexcept {
    switch (part) {
    case UiCaptionPart::caption:
        return UiCaptionNative::caption;
    case UiCaptionPart::icon:
        return UiCaptionNative::system_menu;
    case UiCaptionPart::maximize:
        return UiCaptionNative::maximize;
    default:
        return UiCaptionNative::client;
    }
}

bool ui_caption_bar_part_tooltip(UiCaptionPart part) noexcept {
    return part >= UiCaptionPart::update && part <= UiCaptionPart::close;
}

Utf8View ui_caption_bar_part_name(UiCaptionPart part, const UiCaptionBarState& state) noexcept {
    switch (part) {
    case UiCaptionPart::icon:
        return utf8_literal("System menu");
    case UiCaptionPart::update:
        return state.update == UiCaptionUpdate::ready         ? utf8_literal("Restart to update")
               : state.update == UiCaptionUpdate::installing  ? utf8_literal("Verifying update")
               : state.update == UiCaptionUpdate::downloading ? utf8_literal("Downloading update")
               : state.update == UiCaptionUpdate::failed      ? utf8_literal("Couldn't update")
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
                          const UiCaptionBarState& state, float pixel_scale, u32* buttons_first) noexcept {
    if (!paint || !(pixel_scale > 0))
        return false;
    const auto& tokens = theme.tokens.caption;
    // The shell's backdrop at the app frame's tier: a gradient and the
    // background effect continue under the bar. Focus changes only its ink.
    if (!ui_background_paint_canvas(paint, {0, 0, layout.width, layout.height}, theme,
                                    opaque_over(tokens.background, theme.base_lowest),
                                    UiBackgroundTier::app_frame))
        return false;
    const float radius = layout.fill_radius;
    const float stroke = maximum(1.0f, snap(1.0f, pixel_scale) * pixel_scale) / pixel_scale;
    const float extent = maximum(stroke * 4, snap(theme.scale.caption_glyph, pixel_scale));
    const auto lit_part = [&](UiCaptionPart part, bool* pressed) noexcept {
        *pressed = state.pressed == part && state.hovered == part;
        return *pressed || (state.hovered == part && state.pressed == UiCaptionPart::none);
    };
    const auto push_fill = [&](const UiRect& r, Color color) noexcept {
        return paint_stream_push_rounded(paint, r, color, radius);
    };
    const auto item_box = [&](const UiRect& item) noexcept {
        const float icon = snap(theme.scale.caption_icon, pixel_scale);
        const float left = snap((item.left + item.right - icon) * .5f, pixel_scale);
        const float top = snap((item.top + item.bottom - icon) * .5f, pixel_scale);
        return UiRect{left, top, left + icon, top + icon};
    };
    const Color idle = state.active ? tokens.icon : tokens.icon_inactive;
    if (layout.title_glyph != UiIcon::count && ui_rect_has_area(layout.title_icon) &&
        !ui_icon_paint(paint, layout.title_glyph, layout.title_icon, ui_caption_bar_title_color(theme, state)))
        return false;
    if (state.settings && layout.settings.right > layout.settings.left) {
        bool pressed = false;
        const bool lit = lit_part(UiCaptionPart::settings, &pressed);
        const UiRect& r = layout.settings_fill;
        if ((lit && !push_fill(r, pressed ? tokens.pressed : tokens.hover)) ||
            !ui_icon_paint(paint, UiIcon::settings, item_box(r), lit ? tokens.icon_hover : idle))
            return false;
    }
    if (state.update != UiCaptionUpdate::hidden && layout.update.right > layout.update.left) {
        bool pressed = false;
        const bool lit = lit_part(UiCaptionPart::update, &pressed);
        const UiRect& r = layout.update_fill;
        const bool ready = state.update == UiCaptionUpdate::ready;
        // A ready update keeps its tint; hover and press layer on top.
        if ((ready && !push_fill(r, tokens.badge_ready_fill)) ||
            (lit && !push_fill(r, pressed ? tokens.pressed : tokens.hover)))
            return false;
        const UiRect box = item_box(r);
        const bool failed = state.update == UiCaptionUpdate::failed;
        if (!ui_icon_paint(paint, failed ? UiIcon::warning : UiIcon::download, box,
                           ready    ? tokens.badge_ready
                           : failed ? theme.tokens.text.danger
                           : lit    ? tokens.icon_hover
                                    : idle))
            return false;
        if (!ready && !failed) {
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
    if (buttons_first)
        *buttons_first = paint->count;
    for (u32 i = 0; i < kUiCaptionButtonCount; ++i) {
        const UiCaptionPart part = button_part(i);
        const UiRect& fill = layout.fills[i];
        if (fill.right <= fill.left)
            continue;
        const bool close = part == UiCaptionPart::close;
        bool pressed = false;
        const bool lit = lit_part(part, &pressed);
        if (lit && !push_fill(fill, close ? (pressed ? tokens.close_pressed : tokens.close_hover)
                                          : (pressed ? tokens.pressed : tokens.hover)))
            return false;
        const Color ink = close && lit   ? tokens.close_icon
                          : lit          ? tokens.icon_hover
                          : state.active ? tokens.icon
                                         : tokens.icon_inactive;
        if (!paint_glyph(paint, part, state.maximized, fill, extent, stroke, ink, pixel_scale))
            return false;
    }
    return true;
}

} // namespace maicord
