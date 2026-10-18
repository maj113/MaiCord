// Window caption geometry: frame insets for restored, maximized and auto-hide
// taskbar windows, WM_NCHITTEST results across DPIs (the icon is the window
// menu), and the caption bar's layout, items, button and badge states,
// accessible names and pixel-snapped glyphs.
#include "test_output.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window_frame.hpp"
#include "maicord/ui/caption_bar.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

u32 failures;
void check(bool passed, const char* name) noexcept {
    test_check(&failures, passed, name);
}

bool same(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool near(float a, float b) noexcept {
    return a - b < .001f && b - a < .001f;
}

bool same_color(const Color& a, const Color& b) noexcept {
    return near(a.red, b.red) && near(a.green, b.green) && near(a.blue, b.blue) && near(a.alpha, b.alpha);
}

void frame_insets() noexcept {
    const i32 border96 = window_frame_border(96), border192 = window_frame_border(192);
    test_write_metric("resize border px at 96 dpi: ", static_cast<u64>(border96));
    test_write_metric("resize border px at 192 dpi: ", static_cast<u64>(border192));
    check(border96 > 0 && border192 > border96, "resize border grows with DPI");
    constexpr i32 b = 8;
    const RECT restored = window_frame_client({100, 50, 1100, 850}, b, false, {}, 0);
    check(same(restored, {100 + b, 50, 1100 - b, 850 - b}),
          "restored client keeps side and bottom frame, no top");

    const RECT work{0, 0, 1920, 1040};
    const RECT maximized = window_frame_client({-b, -b, 1920 + b, 1040 + b}, b, true, work, 0);
    check(same(maximized, work), "maximized client is the work area");
    // A frame metric smaller than the actual overhang still never overhangs.
    const RECT overhang = window_frame_client({-12, -12, 1932, 1052}, b, true, work, 0);
    check(same(overhang, work), "maximized client is clamped to the work area");

    const RECT monitor{0, 0, 1920, 1080};
    const RECT whole{-b, -b, 1920 + b, 1080 + b};
    const RECT bottom = window_frame_client(whole, b, true, monitor, window_frame_edge_bottom);
    check(same(bottom, {0, 0, 1920, 1080 - kWindowFrameAutohideReserve}),
          "auto-hide bottom taskbar keeps a reveal strip");
    const RECT every = window_frame_client(whole, b, true, monitor,
                                           window_frame_edge_left | window_frame_edge_top |
                                               window_frame_edge_right | window_frame_edge_bottom);
    check(same(every, {kWindowFrameAutohideReserve, kWindowFrameAutohideReserve,
                       1920 - kWindowFrameAutohideReserve, 1080 - kWindowFrameAutohideReserve}),
          "auto-hide strip on every appbar edge");
    const RECT tiny = window_frame_client({0, 0, 10, 10}, 8, false, {}, 0);
    check(tiny.right >= tiny.left && tiny.bottom >= tiny.top, "tiny window keeps an ordered client");
}

struct HitFixture {
    UiCaptionBarLayout layout;
    i32 width, height, caption, border;
    float scale;
};

HitFixture fixture(u32 dpi) noexcept {
    HitFixture f{};
    f.scale = static_cast<float>(dpi) / 96.0f;
    f.width = static_cast<i32>(1280 * f.scale);
    f.height = static_cast<i32>(800 * f.scale);
    f.caption = static_cast<i32>(ui_caption_bar_height(kDefaultTheme) * f.scale + .5f);
    f.border = window_frame_border(dpi);
    // The update badge and Settings are client controls inside the bar.
    const UiCaptionBarState state{
        UiCaptionPart::none, UiCaptionPart::none, true, false, UiCaptionUpdate::ready, true, 1};
    f.layout = ui_caption_bar_layout(kDefaultTheme, static_cast<float>(f.width) / f.scale, state,
                                     static_cast<float>(f.caption) / f.scale);
    return f;
}

u32 hit(const HitFixture& f, i32 x, i32 y, bool maximized) noexcept {
    WindowFramePart part = WindowFramePart::client;
    if (x >= 0 && y >= 0 && x < f.width && y < f.caption) {
        switch (ui_caption_bar_part(f.layout, static_cast<float>(x) / f.scale,
                                    static_cast<float>(y) / f.scale)) {
        case UiCaptionPart::caption:
            part = WindowFramePart::caption;
            break;
        case UiCaptionPart::icon:
            part = WindowFramePart::system_menu;
            break;
        case UiCaptionPart::minimize:
            part = WindowFramePart::minimize;
            break;
        case UiCaptionPart::maximize:
            part = WindowFramePart::maximize;
            break;
        case UiCaptionPart::close:
            part = WindowFramePart::close;
            break;
        default:
            break;
        }
    }
    return window_frame_hit(x, y, f.width, f.height, f.border, maximized, part);
}

void hit_testing() noexcept {
    constexpr u32 kDpis[]{96, 120, 144, 192};
    constexpr u32 kButtons[]{HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
    for (u32 dpi : kDpis) {
        const HitFixture f = fixture(dpi);
        check(f.caption > f.border, "caption is taller than the top resize band");
        const i32 band = (f.border + f.caption) / 2;
        check(hit(f, f.width / 3, band, false) == HTCAPTION, "caption drags");
        for (u32 i = 0; i < kUiCaptionButtonCount; ++i) {
            const i32 x =
                static_cast<i32>((f.layout.buttons[i].left + f.layout.buttons[i].right) * .5f * f.scale);
            check(hit(f, x, f.caption - 1, false) == kButtons[i], "button below the resize band");
            check(hit(f, x, 0, true) == kButtons[i], "maximized button reaches the screen edge");
            const float width = (f.layout.buttons[i].right - f.layout.buttons[i].left) * f.scale;
            check(near(width, kDefaultTheme.scale.caption_button_width * f.scale), "button width scales");
        }
        check(hit(f, f.width / 2, 0, false) == HTTOP, "top edge resizes");
        check(hit(f, 0, 0, false) == HTTOPLEFT && hit(f, f.border - 1, f.border - 1, false) == HTTOPLEFT,
              "top-left corner resizes");
        check(hit(f, f.width - 1, 0, false) == HTTOPRIGHT, "top-right corner wins over close");
        check(hit(f, f.width - 1, 0, true) == HTCLOSE, "maximized corner is close");
        check(hit(f, f.width / 2, 0, true) == HTCAPTION, "maximized top edge drags");
        check(hit(f, f.width / 2, f.caption, false) == HTCLIENT, "content starts below the caption");
        check(hit(f, f.width / 2, f.height - 1, false) == HTCLIENT, "content reaches the bottom frame");
        check(hit(f, -1, f.height / 2, false) == HTLEFT && hit(f, f.width, f.height / 2, false) == HTRIGHT,
              "side frames resize");
        check(hit(f, f.width / 2, f.height, false) == HTBOTTOM, "bottom frame resizes");
        check(hit(f, -1, f.height, false) == HTBOTTOMLEFT &&
                  hit(f, f.width + 2, f.height + 2, false) == HTBOTTOMRIGHT,
              "bottom corners resize");
        check(hit(f, -1, 1, false) == HTTOPLEFT && hit(f, f.width, f.height - 1, false) == HTBOTTOMRIGHT,
              "corners extend along the side frames");
        check(hit(f, -1, f.height / 2, true) == HTNOWHERE, "maximized overhang is inert");
        check(hit(f, f.width / 2, -1, false) == HTTOP, "above the client resizes");
        const auto center = [&](const UiRect& r, i32* x, i32* y) noexcept {
            *x = static_cast<i32>((r.left + r.right) * .5f * f.scale);
            *y = static_cast<i32>((r.top + r.bottom) * .5f * f.scale);
        };
        i32 x = 0, y = 0;
        center(f.layout.icon, &x, &y);
        check(hit(f, x, y, false) == HTSYSMENU && hit(f, x, f.caption - 1, true) == HTSYSMENU,
              "app icon is the window menu");
        check(hit(f, 1, f.caption - 1, false) == HTSYSMENU, "window menu reaches the leading edge");
        check(hit(f, x, 0, false) == HTTOPLEFT || hit(f, x, 0, false) == HTTOP,
              "resize band above the icon");
        center(f.layout.update, &x, &y);
        check(hit(f, x, f.caption - 1, false) == HTCLIENT, "update badge is a client control");
        center(f.layout.settings, &x, &y);
        check(hit(f, x, f.caption - 1, false) == HTCLIENT, "Settings is a client control");
    }
}

u32 count_color(const PaintStream& paint, const Color& color) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < paint.count; ++i)
        count +=
            same_color(paint.quads[i].fill, color) || same_color(paint.quads[i].border, color) ? 1u : 0u;
    return count;
}

// The update badge and Settings: placement, visibility, paint per state and names.
void items_and_badges(const Theme& theme, PaintStream& paint) noexcept {
    const auto& tokens = theme.tokens.caption;
    UiCaptionBarState state{
        UiCaptionPart::none, UiCaptionPart::none, true, false, UiCaptionUpdate::hidden, true, 0};
    UiCaptionBarLayout layout = ui_caption_bar_layout(theme, 1000, state);
    const float height = layout.height;
    check(ui_rect_has_area(layout.settings) && !ui_rect_has_area(layout.update) &&
              near(layout.settings.right, layout.buttons[0].left - theme.scale.space_2) &&
              near(layout.settings.right - layout.settings.left, height),
          "Settings sits before the window buttons");
    check(near(layout.title.right, layout.settings.left - theme.scale.caption_padding),
          "title yields to Settings");
    state.update = UiCaptionUpdate::downloading;
    layout = ui_caption_bar_layout(theme, 1000, state);
    check(near(layout.update.right, layout.settings.left) &&
              near(layout.update.right - layout.update.left, height),
          "update badge sits before Settings");
    check(ui_caption_bar_part(layout, (layout.update.left + layout.update.right) * .5f, 1) ==
                  UiCaptionPart::update &&
              ui_caption_bar_part(layout, (layout.settings.left + layout.settings.right) * .5f, 1) ==
                  UiCaptionPart::settings,
          "items under points");
    state.settings = false;
    layout = ui_caption_bar_layout(theme, 1000, state);
    check(!ui_rect_has_area(layout.settings) &&
              near(layout.update.right, layout.buttons[0].left - theme.scale.space_2),
          "badge alone takes Settings' place");

    const auto draw = [&](const UiCaptionBarState& s) noexcept {
        const UiCaptionBarLayout l = ui_caption_bar_layout(theme, 1000, s);
        return paint_stream_reset(&paint, l.width, l.height) &&
               ui_caption_bar_paint(&paint, theme, l, s, 1) && !paint.rejected;
    };
    const auto progress_fill = [&](float* width) noexcept {
        for (u32 i = 0; i < paint.count; ++i)
            if (same_color(paint.quads[i].fill, tokens.badge_progress)) {
                *width = paint.quads[i].right - paint.quads[i].left;
                return true;
            }
        return false;
    };
    // Hidden: nothing beyond the bare bar (background and four glyph quads).
    state = {UiCaptionPart::none, UiCaptionPart::none, true, false, UiCaptionUpdate::hidden, false, 0};
    check(draw(state) && paint.count == 5, "hidden badge paints nothing");
    float width = 0;
    state.update = UiCaptionUpdate::downloading;
    state.progress = .25f;
    check(draw(state) && count_color(paint, tokens.badge_track) == 1 && progress_fill(&width) &&
              near(width, theme.scale.caption_icon * .25f),
          "download progress fills its share of the track");
    state.progress = 0;
    check(draw(state) && !progress_fill(&width) && count_color(paint, tokens.badge_track) == 1,
          "no fill before the first byte");
    state.update = UiCaptionUpdate::installing;
    check(draw(state) && progress_fill(&width) && near(width, theme.scale.caption_icon),
          "installing fills the track");
    state.update = UiCaptionUpdate::ready;
    check(draw(state) && count_color(paint, tokens.badge_ready_fill) == 1 &&
              count_color(paint, tokens.badge_ready) == 1 && count_color(paint, tokens.badge_track) == 0,
          "ready badge is tinted, without a track");
    state.hovered = UiCaptionPart::update;
    check(draw(state) && count_color(paint, tokens.hover) == 1 &&
              count_color(paint, tokens.badge_ready_fill) == 1,
          "hover layers over the ready tint");
    state.hovered = state.pressed = UiCaptionPart::settings;
    state.settings = true;
    check(draw(state) && count_color(paint, tokens.pressed) == 1, "pressed Settings");
    const auto* settings_fill = [&]() noexcept -> const PaintQuad* {
        for (u32 i = 0; i < paint.count; ++i)
            if (same_color(paint.quads[i].fill, tokens.pressed))
                return &paint.quads[i];
        return nullptr;
    }();
    const float inset = theme.scale.caption_button_inset;
    check(settings_fill && near(settings_fill->radius + inset, theme.scale.window_radius),
          "item fills share the buttons' concentric radius");

    // Accessible names follow the state.
    UiCaptionBarState names{};
    check(utf8_equal(ui_caption_bar_part_name(UiCaptionPart::icon, names), utf8_literal("System menu")) &&
              !ui_caption_bar_part_name(UiCaptionPart::update, names).bytes &&
              !ui_caption_bar_part_name(UiCaptionPart::settings, names).bytes &&
              !ui_caption_bar_part_name(UiCaptionPart::caption, names).bytes,
          "hidden items have no name");
    names.update = UiCaptionUpdate::downloading;
    names.settings = true;
    check(
        utf8_equal(ui_caption_bar_part_name(UiCaptionPart::update, names),
                   utf8_literal("Downloading update")) &&
            utf8_equal(ui_caption_bar_part_name(UiCaptionPart::settings, names), utf8_literal("Settings")),
        "badge and Settings names");
    names.update = UiCaptionUpdate::ready;
    names.maximized = true;
    check(utf8_equal(ui_caption_bar_part_name(UiCaptionPart::update, names),
                     utf8_literal("Restart to update")) &&
              utf8_equal(ui_caption_bar_part_name(UiCaptionPart::maximize, names), utf8_literal("Restore")),
          "ready badge and restore names");
    const UiCaptionBarLayout all = ui_caption_bar_layout(theme, 1000, names);
    check(ui_rect_equal(ui_caption_bar_part_rect(all, UiCaptionPart::update), all.update) &&
              ui_rect_equal(ui_caption_bar_part_rect(all, UiCaptionPart::icon), all.icon_target) &&
              ui_rect_equal(ui_caption_bar_part_rect(all, UiCaptionPart::close), all.buttons[2]) &&
              !ui_rect_has_area(ui_caption_bar_part_rect(all, UiCaptionPart::caption)),
          "part rectangles");
}

void layout_and_states() noexcept {
    const Theme& theme = kDefaultTheme;
    const auto& tokens = theme.tokens.caption;
    const UiCaptionBarLayout layout = ui_caption_bar_layout(theme, 1000, {});
    const float button = theme.scale.caption_button_width;
    check(near(layout.height, theme.scale.caption_height), "height from the theme");
    check(near(layout.buttons[2].right, 1000) && near(layout.buttons[0].right, layout.buttons[1].left) &&
              near(layout.buttons[1].right, layout.buttons[2].left),
          "buttons are contiguous at the trailing edge");
    bool widths = true;
    for (const UiRect& r : layout.buttons)
        widths =
            widths && near(r.right - r.left, button) && near(r.top, 0) && near(r.bottom, layout.height);
    check(widths, "buttons are full height and theme width");
    const float icon = theme.scale.caption_icon;
    check(near(layout.icon.left, theme.scale.caption_padding) &&
              near(layout.icon.right - layout.icon.left, icon) &&
              near(layout.icon.top + layout.icon.bottom, layout.height),
          "icon at the padding, vertically centred");
    check(near(layout.title.left, layout.icon.right + theme.scale.caption_padding) &&
              near(layout.title.right, layout.buttons[0].left - theme.scale.caption_padding),
          "title span between the icon and the buttons");
    check(!ui_rect_has_area(layout.update) && !ui_rect_has_area(layout.settings),
          "items hidden by default");
    const UiCaptionBarLayout narrow = ui_caption_bar_layout(theme, 50, {});
    check(narrow.buttons[0].left >= 0 && narrow.title.right >= narrow.title.left,
          "narrow bar stays ordered");
    check(ui_caption_bar_part(layout, 5, 5) == UiCaptionPart::icon &&
              ui_caption_bar_part(layout, 200, 5) == UiCaptionPart::caption &&
              ui_caption_bar_part(layout, 1000 - button * .5f, 1) == UiCaptionPart::close &&
              ui_caption_bar_part(layout, 1000 - button * 1.5f, 1) == UiCaptionPart::maximize &&
              ui_caption_bar_part(layout, 1000 - button * 2.5f, 1) == UiCaptionPart::minimize &&
              ui_caption_bar_part(layout, 5, layout.height) == UiCaptionPart::none &&
              ui_caption_bar_part(layout, -1, 1) == UiCaptionPart::none,
          "parts under points");

    PaintStream paint{};
    check(paint_stream_initialize(&paint, 16), "paint stream");
    const auto paint_state = [&](UiCaptionPart hovered, UiCaptionPart pressed, bool active, bool maximized,
                                 float scale) noexcept {
        return paint_stream_reset(&paint, layout.width, layout.height) &&
               ui_caption_bar_paint(&paint, theme, layout, {hovered, pressed, active, maximized}, scale) &&
               !paint.rejected;
    };
    // Background, minimize line, maximize square and the two strokes of close.
    check(paint_state(UiCaptionPart::none, UiCaptionPart::none, true, false, 1) && paint.count == 5,
          "idle caption quads");
    check(same_color(paint.quads[0].fill, tokens.background) && paint.quads[0].fill.alpha == 1,
          "opaque background token");
    check(count_color(paint, tokens.icon) == 4 && count_color(paint, tokens.hover) == 0,
          "idle glyph colours");

    check(paint_state(UiCaptionPart::minimize, UiCaptionPart::none, true, false, 1) && paint.count == 6,
          "hover adds one fill");
    const PaintQuad& fill = paint.quads[1];
    const float inset = theme.scale.caption_button_inset;
    check(same_color(fill.fill, tokens.hover) && near(fill.left, layout.buttons[0].left + inset) &&
              near(fill.top, inset) && near(fill.bottom, layout.height - inset),
          "hover fill inset in its button");
    check(near(fill.radius, ui_inner_radius(theme.scale.window_radius, inset)) &&
              near(fill.radius + inset, theme.scale.window_radius),
          "button fill is concentric with the window corner");
    check(count_color(paint, tokens.icon_hover) == 1, "hovered glyph colour");

    check(paint_state(UiCaptionPart::close, UiCaptionPart::none, true, false, 1) &&
              count_color(paint, tokens.close_hover) == 1 && count_color(paint, tokens.close_icon) == 2,
          "close hover is the danger fill");
    check(paint_state(UiCaptionPart::close, UiCaptionPart::close, true, false, 1) &&
              count_color(paint, tokens.close_pressed) == 1,
          "pressed close");
    check(paint_state(UiCaptionPart::maximize, UiCaptionPart::close, true, false, 1) && paint.count == 5,
          "a press dragged off its button shows no fill");
    check(paint_state(UiCaptionPart::maximize, UiCaptionPart::maximize, true, false, 1) &&
              count_color(paint, tokens.pressed) == 1,
          "pressed minimize/maximize");
    check(paint_state(UiCaptionPart::none, UiCaptionPart::none, true, true, 1) && paint.count == 7,
          "maximized shows the restore glyph");
    check(paint_state(UiCaptionPart::none, UiCaptionPart::none, false, false, 1) &&
              count_color(paint, tokens.icon_inactive) == 4 &&
              same_color(ui_caption_bar_title_color(
                             theme, {UiCaptionPart::none, UiCaptionPart::none, false, false}),
                         tokens.text_inactive),
          "inactive colours");
    // Glyph edges land on device pixels at fractional scales.
    constexpr float kScales[]{1.25f, 1.5f, 1.75f};
    for (float scale : kScales) {
        bool snapped = paint_state(UiCaptionPart::none, UiCaptionPart::none, true, true, scale);
        for (u32 i = 1; snapped && i < paint.count; ++i) {
            const PaintQuad& q = paint.quads[i];
            if (q.border_width < 0)
                continue; // anti-aliased diagonal of close
            const float edges[4]{q.left, q.top, q.right, q.bottom};
            for (float edge : edges) {
                const float pixels = edge * scale;
                const float rounded = static_cast<float>(static_cast<i32>(pixels + .5f));
                snapped = snapped && near(pixels, rounded);
            }
        }
        check(snapped, "glyphs snap to device pixels");
    }
    items_and_badges(theme, paint);
    paint_stream_release(&paint);
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_window_caption_tests_entry() noexcept {
    maicord::frame_insets();
    maicord::hit_testing();
    maicord::layout_and_states();
    maicord::test_write_metric("window caption failures: ", maicord::failures);
    ExitProcess(maicord::failures ? 1 : 0);
}
