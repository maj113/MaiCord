#include "application_caption.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "app_icon.hpp"
#include "application_frame_presentation.hpp"
#include "application_friends.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_tooltip.hpp"
#include "application_updates.hpp"
#include "settings_catalog.hpp"

#include "maicord/core/time.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window_frame.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/ui/tooltip.hpp"

namespace maicord::app_detail {
namespace {

struct CaptionState {
    UiCaptionPart hovered;
    UiCaptionPart pressed;
    bool enabled;
    bool hidden;
    bool active;
    bool maximized;
    // A TME_NONCLIENT leave request is outstanding.
    bool tracking;
    // A client leave request for the bar's own items is outstanding.
    bool client_tracking;
    // The app icon's pixel size on the renderer, zero until decoded.
    u32 icon_extent;
    // Visible frame pixels beside the client beyond those above it; stale
    // after a size change until the next frame measures it.
    u32 side_frame;
    bool side_frame_stale;
    // When the hovered part's tooltip is due; zero while none is pending or
    // after a press, until the pointer moves to another part.
    u64 tooltip_at_ms;
    // When a shown tooltip last hid.
    u64 tooltip_hidden_ms;
    bool tooltip_shown;
} g_caption;

inline constexpr u64 kCaptionTooltipSweepMs = 400;

using GetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, void*, DWORD);
GetWindowAttribute g_get_window_attribute;
bool g_get_window_attribute_resolved;

// The DWM border of a restored window lies outside the client at the sides
// but over the client's top row; none while maximized.
void measure_side_frame(HWND window) noexcept {
    if (!g_get_window_attribute_resolved) {
        g_get_window_attribute_resolved = true;
        if (const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            g_get_window_attribute =
                reinterpret_cast<GetWindowAttribute>(GetProcAddress(dwm, "DwmGetWindowAttribute"));
    }
    constexpr DWORD kExtendedFrameBounds = 9;
    RECT visible{}, client{};
    POINT origin{};
    if (!g_get_window_attribute || IsIconic(window) || !IsWindowVisible(window) ||
        !GetClientRect(window, &client) || !ClientToScreen(window, &origin) ||
        FAILED(g_get_window_attribute(window, kExtendedFrameBounds, &visible, sizeof(visible))))
        return;
    g_caption.side_frame_stale = false;
    const LONG side = minimum(origin.x - visible.left, visible.right - origin.x - client.right);
    const LONG frame = side - (origin.y - visible.top);
    const u32 pixels = frame > 0 && frame < 8 ? static_cast<u32>(frame) : 0;
    if (pixels != g_caption.side_frame) {
        g_caption.side_frame = pixels;
        application_request_frame();
    }
}

u32 window_dpi(HWND window) noexcept {
    const UINT dpi = window ? GetDpiForWindow(window) : 0;
    return dpi ? dpi : 96;
}

u32 caption_pixels(u32 dpi) noexcept {
    if (!g_caption.enabled || g_caption.hidden)
        return 0;
    return static_cast<u32>(ui_caption_bar_height(application_theme()) * static_cast<float>(dpi) / 96.0f +
                            .5f);
}

float to_dips(i32 pixels, u32 dpi) noexcept {
    return static_cast<float>(pixels) * 96.0f / static_cast<float>(dpi);
}

UiCaptionBarLayout layout_for(LONG client_width, u32 dpi) noexcept {
    return ui_caption_bar_layout(application_theme(), to_dips(client_width, dpi),
                                 application_caption_state(),
                                 to_dips(static_cast<i32>(g_app.renderer.caption_height), dpi),
                                 static_cast<float>(dpi) / 96.0f);
}

WindowFramePart frame_part(UiCaptionPart part) noexcept {
    switch (ui_caption_bar_native(part)) {
    case UiCaptionNative::caption:
        return WindowFramePart::caption;
    case UiCaptionNative::system_menu:
        return WindowFramePart::system_menu;
    case UiCaptionNative::maximize:
        return WindowFramePart::maximize;
    default:
        return WindowFramePart::client;
    }
}

UiCaptionPart button_part(WPARAM hit) noexcept {
    switch (hit) {
    case HTMINBUTTON:
        return UiCaptionPart::minimize;
    case HTMAXBUTTON:
        return UiCaptionPart::maximize;
    case HTCLOSE:
        return UiCaptionPart::close;
    default:
        return UiCaptionPart::none;
    }
}

LRESULT hit_test(HWND window, LPARAM lparam) noexcept {
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    RECT client{};
    if (!ScreenToClient(window, &point) || !GetClientRect(window, &client))
        return HTNOWHERE;
    const u32 dpi = window_dpi(window);
    const auto caption = static_cast<LONG>(g_app.renderer.caption_height);
    UiCaptionPart part = UiCaptionPart::none;
    if (point.y >= 0 && point.y < caption && point.x >= 0 && point.x < client.right)
        part = ui_caption_bar_part(layout_for(client.right, dpi), to_dips(point.x, dpi),
                                   to_dips(point.y, dpi));
    return window_frame_hit(point.x, point.y, client.right, client.bottom, window_frame_border(dpi),
                            IsZoomed(window) != FALSE, frame_part(part));
}

// Monitor edges with an auto-hide appbar (the taskbar).
u8 autohide_edges(const RECT& monitor) noexcept {
    u8 edges = 0;
    constexpr UINT kEdges[]{ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM};
    for (u32 i = 0; i < 4; ++i) {
        APPBARDATA bar{};
        bar.cbSize = sizeof(bar);
        bar.uEdge = kEdges[i];
        bar.rc = monitor;
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar))
            edges = static_cast<u8>(edges | (1u << i));
    }
    return edges;
}

void calculate_client(HWND window, RECT* rectangle) noexcept {
    const bool maximized = IsZoomed(window) != FALSE;
    RECT work{};
    u8 edges = 0;
    if (maximized) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromRect(rectangle, MONITOR_DEFAULTTONEAREST), &monitor)) {
            work = monitor.rcWork;
            edges = autohide_edges(monitor.rcMonitor);
        }
    }
    *rectangle =
        window_frame_client(*rectangle, window_frame_border(window_dpi(window)), maximized, work, edges);
    if (g_caption.maximized != maximized) {
        g_caption.maximized = maximized;
        application_request_frame();
    }
}

void set_hover(UiCaptionPart hovered, UiCaptionPart pressed) noexcept {
    if (g_caption.hovered == hovered && g_caption.pressed == pressed)
        return;
    if (g_caption.hovered != hovered) {
        // A content tooltip gives way to the bar's.
        if (g_caption.hovered == UiCaptionPart::none && ui_caption_bar_part_tooltip(hovered))
            application_tooltip_clear();
        // Sweeping from one shown tooltip to the next part skips the delay.
        const u64 now = clock_monotonic_milliseconds();
        if (g_caption.tooltip_shown)
            g_caption.tooltip_hidden_ms = now;
        const bool sweep = g_caption.tooltip_hidden_ms && now - g_caption.tooltip_hidden_ms < kCaptionTooltipSweepMs;
        g_caption.tooltip_shown = false;
        g_caption.tooltip_at_ms = !ui_caption_bar_part_tooltip(hovered) ? 0
                                  : sweep                                ? maximum<u64>(1, now)
                                                                         : now + kUiTooltipPointerDelayMs;
    }
    if (pressed != UiCaptionPart::none) {
        g_caption.tooltip_shown = false;
        g_caption.tooltip_at_ms = 0;
    }
    g_caption.hovered = hovered;
    g_caption.pressed = pressed;
    application_request_frame();
}

void track_leave(HWND window) noexcept {
    if (g_caption.tracking)
        return;
    TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE | TME_NONCLIENT, window,
                             0};
    g_caption.tracking = TrackMouseEvent(&tracking) != FALSE;
}

RECT screen_rect(HWND window, const UiRect& rectangle, u32 dpi) noexcept {
    const float scale = static_cast<float>(dpi) / 96.0f;
    POINT origin{};
    ClientToScreen(window, &origin);
    return {origin.x + static_cast<LONG>(rectangle.left * scale + .5f),
            origin.y + static_cast<LONG>(rectangle.top * scale + .5f),
            origin.x + static_cast<LONG>(rectangle.right * scale + .5f),
            origin.y + static_cast<LONG>(rectangle.bottom * scale + .5f)};
}

// The icon at the bar's DPI, decoded once per size.
void synchronize_icon(u32 dpi) noexcept {
    const u32 extent =
        static_cast<u32>(application_theme().scale.caption_icon * static_cast<float>(dpi) / 96.0f + .5f);
    if (extent == g_caption.icon_extent || !g_app.renderer_ready)
        return;
    ScratchPages<u32[kAppIconLargestExtent * kAppIconLargestExtent]> pixels;
    if (pixels.data && app_icon_decode(g_app.instance, extent, *pixels.data) &&
        renderer_set_caption_icon(&g_app.renderer, *pixels.data, extent))
        g_caption.icon_extent = extent;
}

bool bar_item(UiCaptionPart part) noexcept {
    return part == UiCaptionPart::update || part == UiCaptionPart::settings || part == UiCaptionPart::minimize ||
           part == UiCaptionPart::close;
}

// The bar part under a client point, none below the bar.
UiCaptionPart client_part(HWND window, LPARAM lparam) noexcept {
    const i32 x = static_cast<short>(LOWORD(lparam)), y = static_cast<short>(HIWORD(lparam));
    RECT client{};
    if (y < 0 || y >= static_cast<i32>(g_app.renderer.caption_height) || !GetClientRect(window, &client))
        return UiCaptionPart::none;
    const u32 dpi = window_dpi(window);
    return ui_caption_bar_part(layout_for(client.right, dpi), to_dips(x, dpi), to_dips(y, dpi));
}

// Pointer input over the bar's own items (update badge, Settings): they are
// client area, so the bar consumes these before content coordinates apply.
bool item_pointer(HWND window, UINT message, LPARAM lparam) noexcept {
    switch (message) {
    case WM_MOUSEMOVE: {
        // Another owner's captured drag keeps its moves, even over the bar.
        const bool foreign = GetCapture() == window && !bar_item(g_caption.pressed);
        const UiCaptionPart part = foreign ? UiCaptionPart::none : client_part(window, lparam);
        if (bar_item(part)) {
            if (!g_caption.client_tracking) {
                TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, window, 0};
                g_caption.client_tracking = TrackMouseEvent(&tracking) != FALSE;
            }
            set_hover(part, g_caption.pressed);
        } else if (bar_item(g_caption.hovered)) {
            set_hover(UiCaptionPart::none, g_caption.pressed);
        }
        // A press dragged off its item keeps the pointer until release.
        return bar_item(part) || bar_item(g_caption.pressed);
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const UiCaptionPart part = client_part(window, lparam);
        if (!bar_item(part))
            return false;
        SetCapture(window);
        set_hover(part, part);
        return true;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        // The bar's items take no other buttons; content never sees them.
        return bar_item(client_part(window, lparam));
    case WM_LBUTTONUP: {
        const UiCaptionPart pressed = g_caption.pressed;
        if (!bar_item(pressed))
            return false;
        const UiCaptionPart part = client_part(window, lparam);
        set_hover(bar_item(part) ? part : UiCaptionPart::none, UiCaptionPart::none);
        if (GetCapture() == window)
            ReleaseCapture();
        if (part == pressed)
            application_caption_command(part);
        return true;
    }
    case WM_MOUSELEAVE:
        g_caption.client_tracking = false;
        if (bar_item(g_caption.hovered))
            set_hover(UiCaptionPart::none, g_caption.pressed);
        return false;
    case WM_CAPTURECHANGED:
        if (bar_item(g_caption.pressed))
            set_hover(g_caption.hovered, UiCaptionPart::none);
        return false;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point{};
            if (GetCursorPos(&point) && ScreenToClient(window, &point) &&
                point.y < static_cast<LONG>(g_app.renderer.caption_height)) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

// The current place, as the official client's title bar names it.
UiCaptionTitle caption_title() noexcept {
    UiCaptionTitleInput in{};
    const auto* ui = g_app.account_ui;
    in.signed_in = ui && !g_app.login.visible;
    if (in.signed_in) {
        const auto& state = ui->chat_state;
        in.friends = application_friends_active();
        in.home = state.home_selected;
        const ChannelRecord* channel = chat_state_store_find_channel(state, state.selected_channel_id);
        in.private_type = channel && channel_is_private(channel->type) ? channel->type : 0;
        if (const GuildRecord* guild = state.home_selected ? nullptr
                                                           : chat_state_store_find_guild(state, state.selected_guild_id)) {
            in.guild_id = guild->id;
            in.guild_icon_variant = guild->icon_hash_bytes ? guild->icon_variant : 0;
            in.guild_name = chat_state_store_guild_name(state, *guild);
        }
    }
    return ui_caption_title(in);
}

} // namespace

void application_caption_enable() noexcept {
    g_caption.enabled = true;
    g_caption.active = true;
    g_caption.side_frame_stale = true;
}

u32 application_caption_height() noexcept {
    return g_app.renderer.caption_height;
}

LPARAM application_caption_content_point(LPARAM client) noexcept {
    const u32 caption = g_app.renderer.caption_height;
    if (!caption)
        return client;
    const int y = static_cast<short>(HIWORD(client)) - static_cast<int>(caption);
    return MAKELPARAM(LOWORD(client), static_cast<WORD>(static_cast<short>(y)));
}

SIZE application_caption_window_size(HWND window, u32 dpi, LONG content_width,
                                     LONG content_height) noexcept {
    if (!g_caption.enabled || g_caption.hidden) {
        RECT frame{0, 0, content_width, content_height};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        // The main HWND is created without a menu and never attaches one.
        if (!AdjustWindowRectExForDpi(&frame, style, FALSE, extended, dpi))
            return {content_width, content_height};
        return {frame.right - frame.left, frame.bottom - frame.top};
    }
    const i32 border = window_frame_border(dpi);
    return {content_width + 2 * border, content_height + static_cast<LONG>(caption_pixels(dpi)) + border};
}

u64 application_caption_tooltip_wake() noexcept {
    return g_caption.tooltip_shown ? 0 : g_caption.tooltip_at_ms;
}

UiCaptionBarState application_caption_state() noexcept {
    if (!g_caption.tooltip_shown && g_caption.tooltip_at_ms &&
        clock_monotonic_milliseconds() >= g_caption.tooltip_at_ms)
        g_caption.tooltip_shown = true;
    const bool tooltip = g_caption.tooltip_shown && g_caption.pressed == UiCaptionPart::none &&
                         ui_caption_bar_part_tooltip(g_caption.hovered);
    const ApplicationUpdatesView updates = application_updates_view();
    return {g_caption.hovered,
            g_caption.pressed,
            g_caption.active,
            g_caption.maximized,
            application_update_badge(updates),
            g_app.login.visible,
            application_update_progress(updates),
            to_dips(static_cast<i32>(g_caption.side_frame), window_dpi(g_app.window)),
            tooltip ? g_caption.hovered : UiCaptionPart::none};
}

UiCaptionBarLayout application_caption_layout() noexcept {
    const u32 dpi = maximum(g_app.renderer.dpi, 1u);
    UiCaptionBarLayout layout = layout_for(static_cast<LONG>(g_app.renderer.width), dpi);
    // Content coordinates: the caption sits above the content origin.
    const float top = -layout.height;
    const auto lift = [&](UiRect* r) noexcept { *r = {r->left, r->top + top, r->right, r->bottom + top}; };
    for (auto& button : layout.buttons)
        lift(&button);
    lift(&layout.icon);
    lift(&layout.icon_target);
    lift(&layout.update);
    lift(&layout.settings);
    lift(&layout.title);
    return layout;
}

bool application_caption_command(UiCaptionPart part) noexcept {
    const HWND window = g_app.window;
    if (!window)
        return false;
    WPARAM command = 0;
    switch (part) {
    case UiCaptionPart::icon:
        // As Alt+Space: the window menu at the caption.
        return PostMessageW(window, WM_SYSCOMMAND, SC_KEYMENU, VK_SPACE) != FALSE;
    case UiCaptionPart::update:
        if (application_updates_restart())
            return true;
        return application_settings_open_page(
            {kSettingsNativeOwner, static_cast<u64>(SettingsPanel::updates) + 1});
    case UiCaptionPart::settings:
        return application_settings_open();
    case UiCaptionPart::minimize:
        command = SC_MINIMIZE;
        break;
    case UiCaptionPart::maximize:
        command = IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE;
        break;
    case UiCaptionPart::close:
        command = SC_CLOSE;
        break;
    default:
        return false;
    }
    SendMessageW(window, WM_SYSCOMMAND, command, 0);
    return true;
}

void application_caption_set_hidden(bool hidden) noexcept {
    if (!g_caption.enabled || g_caption.hidden == hidden)
        return;
    g_caption.hidden = hidden;
    renderer_set_caption_height(&g_app.renderer, caption_pixels(window_dpi(g_app.window)));
}

void application_caption_synchronize() noexcept {
    if (!g_caption.enabled || !g_app.window)
        return;
    const u32 height = caption_pixels(window_dpi(g_app.window));
    if (height != g_app.renderer.caption_height) {
        // A theme changed the band: the content below it resizes.
        renderer_set_caption_height(&g_app.renderer, height);
        application_resize_client(g_app.window);
    }
    if (g_caption.side_frame_stale)
        measure_side_frame(g_app.window);
    if (height && renderer_set_caption(&g_app.renderer, application_caption_state(), caption_title()))
        synchronize_icon(window_dpi(g_app.window));
}

bool application_caption_window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                        LRESULT* result) noexcept {
    if (!g_caption.enabled)
        return false;
    if (g_app.renderer.caption_height && item_pointer(window, message, lparam)) {
        *result = message == WM_SETCURSOR ? TRUE : 0;
        return true;
    }
    switch (message) {
    case WM_NCCALCSIZE: {
        if (g_caption.hidden || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION))
            return false;
        // Both forms begin with the proposed window rectangle; the creation-time
        // FALSE form applies the frame without a later SWP_FRAMECHANGED.
        RECT* rectangle = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                                 : reinterpret_cast<RECT*>(lparam);
        calculate_client(window, rectangle);
        // Prepare the composition surface before Windows commits the new
        // origin, as for the system frame (left-edge drags).
        const u32 caption = g_app.renderer.caption_height;
        if (wparam && g_app.renderer_ready && rectangle->right > rectangle->left &&
            rectangle->bottom - rectangle->top > static_cast<LONG>(caption))
            application_resize_surface(static_cast<u32>(rectangle->right - rectangle->left),
                                       static_cast<u32>(rectangle->bottom - rectangle->top) - caption);
        *result = 0;
        return true;
    }
    case WM_NCHITTEST:
        if (!g_app.renderer.caption_height)
            return false;
        *result = hit_test(window, lparam);
        return true;
    case WM_NCACTIVATE:
        if (g_caption.active != (wparam != FALSE)) {
            g_caption.active = wparam != FALSE;
            application_request_frame();
        }
        // -1: the system frame has no caption of its own to repaint.
        *result = DefWindowProcW(window, message, wparam, g_app.renderer.caption_height ? -1 : lparam);
        return true;
    case WM_DPICHANGED:
        renderer_set_caption_height(&g_app.renderer, caption_pixels(HIWORD(wparam)));
        return false;
    case WM_SIZE:
        // DWM publishes the new frame bounds after the move completes.
        g_caption.side_frame_stale = true;
        return false;
    case WM_NCMOUSEMOVE: {
        const UiCaptionPart part = button_part(wparam);
        set_hover(part, g_caption.pressed);
        if (part == UiCaptionPart::none)
            return false;
        track_leave(window);
        *result = 0;
        return true;
    }
    case WM_NCMOUSEHOVER:
        // No system tooltip over the maximize button; the bar shows its own.
        if (button_part(wparam) == UiCaptionPart::none)
            return false;
        *result = 0;
        return true;
    case WM_NCMOUSELEAVE:
        g_caption.tracking = false;
        set_hover(UiCaptionPart::none, UiCaptionPart::none);
        return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK: {
        const UiCaptionPart part = button_part(wparam);
        if (part == UiCaptionPart::none)
            return false;
        // The system's own button tracking would run a modal loop for a
        // caption it does not draw.
        track_leave(window);
        set_hover(part, part);
        *result = 0;
        return true;
    }
    case WM_NCLBUTTONUP: {
        const UiCaptionPart part = button_part(wparam);
        const UiCaptionPart pressed = g_caption.pressed;
        if (pressed == UiCaptionPart::none && part == UiCaptionPart::none)
            return false;
        set_hover(part, UiCaptionPart::none);
        if (part != UiCaptionPart::none && part == pressed)
            application_caption_command(part);
        *result = 0;
        return true;
    }
    case WM_CONTEXTMENU:
        // A right click on the caption opens the window menu, as on a system caption.
        if (lparam == -1 || hit_test(window, lparam) == HTCLIENT)
            return false;
        *result = DefWindowProcW(window, message, wparam, lparam);
        return true;
    case WM_GETTITLEBARINFOEX: {
        *result = DefWindowProcW(window, message, wparam, lparam);
        auto* info = reinterpret_cast<TITLEBARINFOEX*>(lparam);
        if (!info || info->cbSize < sizeof(TITLEBARINFOEX) || !g_app.renderer.caption_height)
            return true;
        // Windows anchors the snap layouts flyout to the maximize rectangle.
        RECT client{};
        GetClientRect(window, &client);
        const u32 dpi = window_dpi(window);
        const UiCaptionBarLayout layout = layout_for(client.right, dpi);
        info->rcTitleBar = screen_rect(window, {0, 0, layout.width, layout.height}, dpi);
        info->rgrect[0] = info->rcTitleBar;
        constexpr u32 kSlots[kUiCaptionButtonCount]{2, 3, 5};
        for (u32 i = 0; i < kUiCaptionButtonCount; ++i) {
            info->rgrect[kSlots[i]] = screen_rect(window, layout.buttons[i], dpi);
            info->rgstate[kSlots[i]] = 0;
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace maicord::app_detail
