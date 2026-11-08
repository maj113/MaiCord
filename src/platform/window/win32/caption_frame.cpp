// The Windows side of a client area that extends over the system caption: the frame messages the window procedure
// leaves to the system (WM_NCCALCSIZE keeps the DWM side and bottom frame for resizing, the shadow, rounded corners
// and window animations; WM_NCHITTEST answers the resize borders and the maximize button the snap layouts anchor to).
// What lies in the bar is the application's caption controller's (Win32CaptionOps); everything here is Windows.
#include "maicord/platform/desktop/window_frame.hpp"
#include "maicord/platform/window/win32.hpp"
#include "internal.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {

i32 window_frame_border(u32 dpi) noexcept {
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

RECT window_frame_client(const RECT& window, i32 border, bool maximized, const RECT& work_area,
                         u8 autohide_edges) noexcept {
    RECT client{window.left + border, window.top, window.right - border, window.bottom - border};
    if (maximized) {
        client.top += border;
        if (work_area.right > work_area.left && work_area.bottom > work_area.top) {
            client.left = maximum(client.left, work_area.left);
            client.top = maximum(client.top, work_area.top);
            client.right = minimum(client.right, work_area.right);
            client.bottom = minimum(client.bottom, work_area.bottom);
        }
        if (autohide_edges & window_frame_edge_left)
            client.left += kWindowFrameAutohideReserve;
        if (autohide_edges & window_frame_edge_top)
            client.top += kWindowFrameAutohideReserve;
        if (autohide_edges & window_frame_edge_right)
            client.right -= kWindowFrameAutohideReserve;
        if (autohide_edges & window_frame_edge_bottom)
            client.bottom -= kWindowFrameAutohideReserve;
    }
    if (client.right < client.left)
        client.right = client.left;
    if (client.bottom < client.top)
        client.bottom = client.top;
    return client;
}

namespace win32_detail {
namespace {

// A TME_NONCLIENT leave request is outstanding.
bool g_tracking;

u32 window_dpi(HWND window) noexcept {
    const u32 dpi = GetDpiForWindow(window);
    return dpi ? dpi : 96;
}

LRESULT hit_code(FrameHit hit) noexcept {
    switch (hit) {
    case FrameHit::caption:
        return HTCAPTION;
    case FrameHit::system_menu:
        return HTSYSMENU;
    case FrameHit::minimize:
        return HTMINBUTTON;
    case FrameHit::maximize:
        return HTMAXBUTTON;
    case FrameHit::close:
        return HTCLOSE;
    case FrameHit::left:
        return HTLEFT;
    case FrameHit::right:
        return HTRIGHT;
    case FrameHit::top:
        return HTTOP;
    case FrameHit::bottom:
        return HTBOTTOM;
    case FrameHit::top_left:
        return HTTOPLEFT;
    case FrameHit::top_right:
        return HTTOPRIGHT;
    case FrameHit::bottom_left:
        return HTBOTTOMLEFT;
    case FrameHit::bottom_right:
        return HTBOTTOMRIGHT;
    case FrameHit::nowhere:
        return HTNOWHERE;
    default:
        return HTCLIENT;
    }
}

FrameHit button_part(WPARAM hit) noexcept {
    switch (hit) {
    case HTMINBUTTON:
        return FrameHit::minimize;
    case HTMAXBUTTON:
        return FrameHit::maximize;
    case HTCLOSE:
        return FrameHit::close;
    default:
        return FrameHit::client;
    }
}

LRESULT hit_test(const Win32CaptionOps& ops, HWND window, LPARAM lparam) noexcept {
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (!ScreenToClient(window, &point))
        return HTNOWHERE;
    return hit_code(ops.hit(ops.context, static_cast<float>(point.x), static_cast<float>(point.y)));
}

// Monitor edges with an auto-hide appbar (the taskbar).
u8 autohide_edges(const RECT& monitor) noexcept {
    u8 edges = 0;
    static constexpr UINT kEdges[]{ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM};
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

void calculate_client(const Win32CaptionOps& ops, HWND window, RECT* rectangle) noexcept {
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
    *rectangle = window_frame_client(*rectangle, window_frame_border(window_dpi(window)), maximized, work, edges);
    ops.maximized(ops.context, maximized);
}

void track_leave(HWND window) noexcept {
    if (g_tracking)
        return;
    TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE | TME_NONCLIENT, window, 0};
    g_tracking = TrackMouseEvent(&tracking) != FALSE;
}

RECT screen_rect(const POINT& origin, const i32 (&rectangle)[4]) noexcept {
    return {origin.x + rectangle[0], origin.y + rectangle[1], origin.x + rectangle[2], origin.y + rectangle[3]};
}

} // namespace

bool caption_frame_message(const Win32CaptionOps& ops, HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                           LRESULT* result) noexcept {
    switch (message) {
    case WM_NCCALCSIZE: {
        if (ops.hidden(ops.context) || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION))
            return false;
        // Both forms begin with the proposed window rectangle; the creation-time FALSE form applies the frame without
        // a later SWP_FRAMECHANGED.
        RECT* rectangle = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                                 : reinterpret_cast<RECT*>(lparam);
        calculate_client(ops, window, rectangle);
        // Prepare the composition surface before Windows commits the new origin, as for the system frame
        // (left-edge drags): the sink gets the client the frame leaves.
        if (wparam)
            deliver_provisional_resize(window, *rectangle);
        *result = 0;
        return true;
    }
    case WM_NCHITTEST:
        if (!ops.height(ops.context))
            return false;
        *result = hit_test(ops, window, lparam);
        return true;
    case WM_NCACTIVATE:
        ops.active(ops.context, wparam != FALSE);
        // -1: the system frame has no caption of its own to repaint.
        *result = DefWindowProcW(window, message, wparam, ops.height(ops.context) ? -1 : lparam);
        return true;
    case WM_NCMOUSEMOVE: {
        const FrameHit part = button_part(wparam);
        ops.hover(ops.context, part);
        if (part == FrameHit::client)
            return false;
        track_leave(window);
        *result = 0;
        return true;
    }
    case WM_NCMOUSEHOVER:
        // No system tooltip over the maximize button; the bar shows its own.
        if (button_part(wparam) == FrameHit::client)
            return false;
        *result = 0;
        return true;
    case WM_NCMOUSELEAVE:
        g_tracking = false;
        ops.leave(ops.context);
        return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK: {
        const FrameHit part = button_part(wparam);
        if (part == FrameHit::client)
            return false;
        // The system's own button tracking would run a modal loop for a caption it does not draw.
        track_leave(window);
        ops.press(ops.context, part);
        *result = 0;
        return true;
    }
    case WM_NCLBUTTONUP:
        if (!ops.release(ops.context, button_part(wparam)))
            return false;
        *result = 0;
        return true;
    case WM_CONTEXTMENU:
        // A right click on the caption opens the window menu, as on a system caption.
        if (lparam == -1 || hit_test(ops, window, lparam) == HTCLIENT)
            return false;
        *result = DefWindowProcW(window, message, wparam, lparam);
        return true;
    case WM_GETTITLEBARINFOEX: {
        *result = DefWindowProcW(window, message, wparam, lparam);
        auto* info = reinterpret_cast<TITLEBARINFOEX*>(lparam);
        FrameBarRects bar{};
        if (!info || info->cbSize < sizeof(TITLEBARINFOEX) || !ops.height(ops.context) ||
            !ops.title_bar(ops.context, &bar))
            return true;
        // Windows anchors the snap layouts flyout to the maximize rectangle.
        POINT origin{};
        ClientToScreen(window, &origin);
        info->rcTitleBar = screen_rect(origin, {0, 0, bar.bar_right, bar.bar_bottom});
        info->rgrect[0] = info->rcTitleBar;
        static constexpr u32 kSlots[3]{2, 3, 5};
        for (u32 i = 0; i < 3; ++i) {
            info->rgrect[kSlots[i]] = screen_rect(origin, bar.button[i]);
            info->rgstate[kSlots[i]] = 0;
        }
        return true;
    }
    default:
        return false;
    }
}

// The outer size of the window for a client of this size: the frame the glue keeps, or the system's.
void frame_outer_size(const Win32CaptionOps* ops, HWND window, u32 client_width, u32 client_height, u32* width,
                      u32* height) noexcept {
    const u32 dpi = window_dpi(window);
    if (!ops || ops->hidden(ops->context)) {
        RECT frame{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        // The main HWND is created without a menu and never attaches one.
        if (AdjustWindowRectExForDpi(&frame, style, FALSE, extended, dpi)) {
            *width = static_cast<u32>(frame.right - frame.left);
            *height = static_cast<u32>(frame.bottom - frame.top);
            return;
        }
        *width = client_width;
        *height = client_height;
        return;
    }
    const u32 border = static_cast<u32>(window_frame_border(dpi));
    *width = client_width + 2 * border;
    *height = client_height + border;
}

} // namespace win32_detail
} // namespace maicord
