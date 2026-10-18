// Window caption geometry: frame insets for restored, maximized and auto-hide
// taskbar windows, WM_NCHITTEST results across DPIs (the icon is the window
// menu), and the caption bar's layout, items, button and badge states,
// accessible names, tooltips, the backdrop band and corner insets.
#include "test_output.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window_frame.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/background_surface.hpp"
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

// The client's NCCALCSIZE, as the main window applies it while restored.
LRESULT CALLBACK frame_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCALCSIZE) {
        RECT* rectangle = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                                 : reinterpret_cast<RECT*>(lparam);
        const UINT dpi = GetDpiForWindow(window);
        *rectangle = window_frame_client(*rectangle, window_frame_border(dpi ? dpi : 96), false, {}, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// A real (never shown) restored window: the client sits inside the system
// resize frame on the left, right and bottom, symmetric, and reaches the top.
void restored_window_insets() noexcept {
    // The frame metric is the system's own sizing frame at every DPI.
    constexpr u32 kDpis[]{96, 120, 144, 168, 192, 240, 288};
    bool metrics = true;
    for (u32 dpi : kDpis) {
        RECT frame{};
        const i32 border = window_frame_border(dpi);
        metrics = metrics && AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi) &&
                  -frame.left == border && frame.right == border && frame.bottom == border;
    }
    check(metrics, "resize border equals the system sizing frame at every DPI");

    constexpr wchar_t kClass[] = L"MaiCordWindowCaptionInsetsTest";
    constexpr DWORD kExtended = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = frame_window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kClass;
    const bool registered = RegisterClassExW(&type) != 0;
    const HWND window = registered
                            ? CreateWindowExW(kExtended, kClass, kClass, WS_OVERLAPPEDWINDOW, 100, 100, 640,
                                              480, nullptr, nullptr, type.hInstance, nullptr)
                            : nullptr;
    check(window != nullptr, "inset window");
    const auto insets_match = [&]() noexcept {
        RECT outer{}, client{};
        POINT origin{};
        if (!GetWindowRect(window, &outer) || !GetClientRect(window, &client) ||
            !ClientToScreen(window, &origin))
            return false;
        const UINT dpi = GetDpiForWindow(window);
        RECT frame{};
        if (!AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, kExtended, dpi ? dpi : 96))
            return false;
        const LONG left = origin.x - outer.left, top = origin.y - outer.top;
        const LONG right = outer.right - (origin.x + client.right);
        const LONG bottom = outer.bottom - (origin.y + client.bottom);
        return left == right && left == -frame.left && right == frame.right && bottom == frame.bottom &&
               top == 0;
    };
    if (window) {
        check(insets_match(), "created window: client inside the sizing frame, flush with the top");
        SetWindowPos(window, nullptr, 40, 30, 913, 577, SWP_NOZORDER | SWP_NOACTIVATE);
        check(insets_match(), "resized window keeps symmetric insets");
        DestroyWindow(window);
    }
    if (registered)
        UnregisterClassW(kClass, type.hInstance);
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
                                     static_cast<float>(f.caption) / f.scale, f.scale);
    return f;
}

u32 hit(const HitFixture& f, i32 x, i32 y, bool maximized) noexcept {
    WindowFramePart part = WindowFramePart::client;
    if (x >= 0 && y >= 0 && x < f.width && y < f.caption) {
        switch (ui_caption_bar_native(ui_caption_bar_part(f.layout, static_cast<float>(x) / f.scale,
                                                          static_cast<float>(y) / f.scale))) {
        case UiCaptionNative::caption:
            part = WindowFramePart::caption;
            break;
        case UiCaptionNative::system_menu:
            part = WindowFramePart::system_menu;
            break;
        case UiCaptionNative::maximize:
            part = WindowFramePart::maximize;
            break;
        default:
            break;
        }
    }
    return window_frame_hit(x, y, f.width, f.height, f.border, maximized, part);
}

void hit_testing() noexcept {
    constexpr u32 kDpis[]{96, 120, 144, 192};
    // Only Maximize is a system button (snap layouts); Minimize and Close are
    // client controls without the system's caption tooltip.
    constexpr u32 kButtons[]{HTCLIENT, HTMAXBUTTON, HTCLIENT};
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
        check(hit(f, f.width - 1, 0, true) == HTCLIENT &&
                  ui_caption_bar_part(f.layout, (f.width - 1) / f.scale, 0) == UiCaptionPart::close,
              "maximized corner is the close control");
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
    check(ui_caption_bar_native(UiCaptionPart::maximize) == UiCaptionNative::maximize &&
              ui_caption_bar_native(UiCaptionPart::caption) == UiCaptionNative::caption &&
              ui_caption_bar_native(UiCaptionPart::icon) == UiCaptionNative::system_menu &&
              ui_caption_bar_native(UiCaptionPart::minimize) == UiCaptionNative::client &&
              ui_caption_bar_native(UiCaptionPart::close) == UiCaptionNative::client,
          "only Maximize is a system button");
    check(ui_caption_bar_part_tooltip(UiCaptionPart::minimize) && ui_caption_bar_part_tooltip(UiCaptionPart::maximize) &&
              ui_caption_bar_part_tooltip(UiCaptionPart::close) && ui_caption_bar_part_tooltip(UiCaptionPart::update) &&
              ui_caption_bar_part_tooltip(UiCaptionPart::settings) &&
              !ui_caption_bar_part_tooltip(UiCaptionPart::icon) && !ui_caption_bar_part_tooltip(UiCaptionPart::caption),
          "buttons, badge and Settings have tooltips");
}

// A gradient theme's band samples the shell's global gradient at the app
// frame's tier, as the guild rail beside it does.
void backdrop_band(PaintStream& paint) noexcept {
    Theme theme = kDefaultTheme;
    theme.background = background_theme_defaults();
    theme.background.effect = BackgroundEffect::stars;
    const UiCaptionBarLayout layout = ui_caption_bar_layout(theme, 800, {});
    PaintStream rail{};
    const bool painted =
        paint_stream_reset(&paint, layout.width, layout.height) &&
        ui_caption_bar_paint(&paint, theme, layout, {UiCaptionPart::close, UiCaptionPart::none, true}, 1) &&
        paint_stream_initialize(&rail, 2) && paint_stream_reset(&rail, 800, 600) &&
        ui_background_paint_canvas(&rail, {0, 0, 72, 600}, theme, theme.base_lowest, UiBackgroundTier::app_frame);
    const PaintQuad& band = paint.quads[0];
    check(painted && ui_background_canvas_quad(band) && band.border_width == -8 &&
              near(band.reserved_y, rail.quads[0].reserved_y) && near(band.left, 0) && near(band.top, 0) &&
              near(band.right, 800) && near(band.bottom, layout.height),
          "gradient band is the app frame's canvas across the bar");
    bool overlays = true;
    for (u32 i = 1; i < paint.count; ++i)
        overlays = overlays && !ui_background_canvas_quad(paint.quads[i]) &&
                   !(near(paint.quads[i].left, 0) && near(paint.quads[i].right, 800));
    check(overlays, "hover and glyphs are overlays, never a second band");
    paint_stream_release(&rail);
}

// Fill insets, centring and concentric corners at common DPIs, flush and with
// the restored window's side border outside the client.
void corner_geometry(PaintStream& paint) noexcept {
    const Theme& theme = kDefaultTheme;
    constexpr u32 kDpis[]{96, 120, 144, 192};
    for (u32 dpi : kDpis) {
        const float scale = static_cast<float>(dpi) / 96.0f;
        const float pixel = 1 / scale;
        const float height =
            static_cast<float>(static_cast<i32>(theme.scale.caption_height * scale + .5f)) / scale;
        const float width = static_cast<float>(static_cast<i32>(1280 * scale)) / scale;
        for (u32 border = 0; border < 2; ++border) {
            UiCaptionBarState state{UiCaptionPart::none, UiCaptionPart::none, true, false,
                                    UiCaptionUpdate::ready, true, 1};
            state.frame = static_cast<float>(border) * pixel;
            const UiCaptionBarLayout layout = ui_caption_bar_layout(theme, width, state, height, scale);
            const UiRect& close = layout.fills[2];
            // Seen from the visible window edge: the side border adds to the right.
            const float right = width - close.right + state.frame;
            check(close.top > 0 && near(close.top, right), "close fill: equal top and right insets");
            check(near(layout.fill_radius, ui_inner_radius(theme.scale.window_radius, close.top)) &&
                      near(layout.fill_radius + close.top, theme.scale.window_radius),
                  "close fill concentric with the window corner");
            const float pixels = close.top * scale;
            check(near(pixels, static_cast<float>(static_cast<i32>(pixels + .5f))), "fill inset on whole pixels");
            bool consistent = true;
            for (u32 i = 0; i < kUiCaptionButtonCount; ++i) {
                const UiRect& f = layout.fills[i];
                consistent = consistent && near(f.top, close.top) && near(f.top + f.bottom, height) &&
                             near(f.right - f.left, close.right - close.left);
                if (i)
                    consistent = consistent && near(f.left - layout.fills[i - 1].right,
                                                    layout.fills[1].left - layout.fills[0].right);
            }
            const UiRect items[]{layout.update_fill, layout.settings_fill};
            for (const UiRect& f : items)
                consistent = consistent && near(f.top, close.top) && near(f.top + f.bottom, height);
            check(consistent, "fills share size, gaps and vertical inset, centred in the band");
            check(near(layout.icon.left + state.frame, layout.icon.top) &&
                      near(layout.icon.top + layout.icon.bottom, height),
                  "icon margin from the visible edge equals its vertical margin");
            // Glyphs centred in their fills to within half a device pixel.
            bool centred = paint_stream_reset(&paint, layout.width, layout.height) &&
                           ui_caption_bar_paint(&paint, theme, layout, state, scale) && !paint.rejected;
            for (u32 i = 0; centred && i < 2; ++i) {
                const UiRect& f = layout.fills[i];
                bool found = false;
                for (u32 q = 1; q < paint.count; ++q) {
                    const PaintQuad& glyph = paint.quads[q];
                    if (glyph.left < f.left || glyph.right > f.right || glyph.border_width < 0)
                        continue;
                    found = true;
                    centred = centred &&
                              (glyph.left + glyph.right - f.left - f.right) * .5f <= pixel * .5f + .001f &&
                              (f.left + f.right - glyph.left - glyph.right) * .5f <= pixel * .5f + .001f;
                    if (i == 1)
                        centred = centred && (glyph.top + glyph.bottom - height) * .5f <= pixel * .5f + .001f &&
                                  (height - glyph.top - glyph.bottom) * .5f <= pixel * .5f + .001f;
                }
                centred = centred && found;
            }
            check(centred, "button glyphs centred in their fills");
        }
    }
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
    check(near(layout.icon.left, layout.icon.top) && near(layout.icon.right - layout.icon.left, icon) &&
              near(layout.icon.top + layout.icon.bottom, layout.height),
          "icon vertically centred, as far from the leading edge as from the top");
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
    // Backdrop, minimize line, maximize square and the two strokes of close.
    check(paint_state(UiCaptionPart::none, UiCaptionPart::none, true, false, 1) && paint.count == 5,
          "idle caption quads");
    check(same_color(paint.quads[0].fill, tokens.background) && paint.quads[0].fill.alpha == 1 &&
              ui_background_canvas_quad(paint.quads[0]),
          "the band is a backdrop canvas on the background token");
    check(count_color(paint, tokens.icon) == 4 && count_color(paint, tokens.hover) == 0,
          "idle glyph colours");
    const PaintQuad active_band = paint.quads[0];
    check(paint_state(UiCaptionPart::none, UiCaptionPart::none, false, false, 1) &&
              bytes_equal(&paint.quads[0], &active_band, sizeof(PaintQuad)),
          "focus changes only the ink, not the band");

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
    backdrop_band(paint);
    corner_geometry(paint);
    paint_stream_release(&paint);
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_window_caption_tests_entry() noexcept {
    maicord::frame_insets();
    maicord::restored_window_insets();
    maicord::hit_testing();
    maicord::layout_and_states();
    maicord::test_write_metric("window caption failures: ", maicord::failures);
    ExitProcess(maicord::failures ? 1 : 0);
}
