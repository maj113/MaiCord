#include "maicord/platform/window_frame.hpp"

#include "maicord/core/win32_user.hpp"

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

u32 window_frame_hit(i32 x, i32 y, i32 client_width, i32 client_height, i32 border, bool maximized,
                     WindowFramePart part) noexcept {
    const bool inside = x >= 0 && y >= 0 && x < client_width && y < client_height;
    if (!inside && maximized)
        return HTNOWHERE;
    if (!maximized) {
        // Corners extend one border along each edge, as on the system frame.
        const bool left = x < border, right = x >= client_width - border;
        if (y >= client_height)
            return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        const bool top = y < border, bottom = y >= client_height - border;
        if (x < 0)
            return top ? HTTOPLEFT : bottom ? HTBOTTOMLEFT : HTLEFT;
        if (x >= client_width)
            return top ? HTTOPRIGHT : bottom ? HTBOTTOMRIGHT : HTRIGHT;
        if (y < 0 || top)
            return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
    }
    switch (part) {
    case WindowFramePart::caption:
        return HTCAPTION;
    case WindowFramePart::system_menu:
        return HTSYSMENU;
    case WindowFramePart::minimize:
        return HTMINBUTTON;
    case WindowFramePart::maximize:
        return HTMAXBUTTON;
    case WindowFramePart::close:
        return HTCLOSE;
    default:
        return HTCLIENT;
    }
}

} // namespace maicord
