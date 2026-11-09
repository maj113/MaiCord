#include "maicord/platform/desktop/screen_color.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/gdi.hpp"
#else
#include "maicord/core/ascii.hpp"
#include "maicord/core/log.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/session.hpp"
#endif

namespace maicord {
namespace {

struct Pick {
    ScreenColorState state;
    u32 rgb;
    void (*settled)(void* context) noexcept;
    void* context;
#if defined(_WIN32)
    HHOOK hook;
    WPARAM swallow; // the button release that belongs to the swallowed press
#else
    dbus::PortalRequest request;
    usize serial; // tells a stale reply from the current pick's
#endif
} g_pick;

void settle(ScreenColorState state, u32 rgb = 0) noexcept {
    g_pick.state = state;
    g_pick.rgb = rgb;
    if (const auto settled = g_pick.settled) {
        g_pick.settled = nullptr;
        settled(g_pick.context);
    }
}

#if defined(_WIN32)
// The UI thread's low-level mouse hook: a primary press picks the pixel under it, a secondary press ends the pick;
// both presses and their releases are swallowed. Everything else passes.
LRESULT __stdcall hook(int code, WPARAM message, LPARAM data) noexcept {
    if (code == HC_ACTION) {
        if (g_pick.state == ScreenColorState::picking && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN)) {
            g_pick.swallow = message == WM_LBUTTONDOWN ? WM_LBUTTONUP : WM_RBUTTONUP;
            if (message == WM_RBUTTONDOWN) {
                settle(ScreenColorState::cancelled);
                return 1;
            }
            const POINT point = reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt;
            const HDC screen = GetDC(nullptr);
            const COLORREF pixel = screen ? GetPixel(screen, point.x, point.y) : CLR_INVALID;
            if (screen)
                ReleaseDC(nullptr, screen);
            if (pixel == CLR_INVALID)
                settle(ScreenColorState::failed);
            else
                settle(ScreenColorState::picked, (pixel & 0xFF) << 16 | (pixel & 0xFF00) | (pixel >> 16 & 0xFF));
            return 1;
        }
        if (g_pick.swallow && message == g_pick.swallow) {
            g_pick.swallow = 0;
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
#else
constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";

bool current(void* context) noexcept {
    return g_pick.state == ScreenColorState::picking && reinterpret_cast<usize>(context) == g_pick.serial;
}
// The Response's results: {color: (ddd)}, each channel in [0, 1].
void on_response(void* context, u32 code, dbus::Reader* results) noexcept {
    if (!current(context))
        return;
    if (code == 1) {
        settle(ScreenColorState::cancelled);
        return;
    }
    double rgb[3]{-1, -1, -1};
    if (code == 0 && dbus::reader_enter_array(results)) {
        while (dbus::reader_more(*results)) {
            const char* key = nullptr;
            const char* signature = nullptr;
            if (!dbus::reader_enter_struct(results) || !dbus::reader_string(results, &key, nullptr) ||
                !dbus::reader_enter_variant(results, &signature))
                break;
            if (c_string_equal(key, "color") && c_string_equal(signature, "(ddd)")) {
                // The colour is all the results need.
                if (dbus::reader_enter_struct(results))
                    for (auto& channel : rgb)
                        dbus::reader_double(results, &channel);
                break;
            }
            if (!dbus::reader_leave(results) || !dbus::reader_leave(results))
                break;
        }
    }
    u32 value = 0;
    for (const double channel : rgb) {
        if (!(channel >= 0 && channel <= 1)) {
            settle(ScreenColorState::failed);
            return;
        }
        value = value << 8 | static_cast<u32>(channel * 255 + .5);
    }
    settle(ScreenColorState::picked, value);
}
void on_reply(void* context, dbus::Status status, const dbus::Message* reply) noexcept {
    if (!current(context))
        return;
    if (status != dbus::Status::ok) {
        log_write(LogLevel::trace, "screen_color",
                  status == dbus::Status::error_reply && reply && reply->error_name ? reply->error_name
                                                                                     : "PickColor failed");
        dbus::portal_request_end(&g_pick.request);
        settle(ScreenColorState::failed);
        return;
    }
    dbus::Reader r;
    dbus::message_reader(*reply, &r);
    const char* handle = nullptr;
    if (dbus::reader_string(&r, &handle, nullptr))
        dbus::portal_request_adopt(&g_pick.request, handle);
}
#endif

} // namespace

bool screen_color_pick_start(void (*settled)(void* context) noexcept, void* context) noexcept {
    screen_color_pick_end();
#if defined(_WIN32)
    g_pick.hook = SetWindowsHookExW(WH_MOUSE_LL, hook, GetModuleHandleW(nullptr), 0);
    if (!g_pick.hook)
        return false;
#else
    using namespace dbus;
    Connection* c = session_bus_open() ? session_bus() : nullptr;
    void* serial = reinterpret_cast<void*>(++g_pick.serial);
    if (!c || !portal_request_begin(&g_pick.request, c, on_response, serial))
        return false;
    Builder body;
    builder_init(&body);
    builder_string(&body, portal_parent_window());
    builder_open_array(&body, "{sv}");
    portal_request_append_token(&body, g_pick.request);
    builder_close_array(&body);
    const bool sent = connection_call(c, {kPortalName, kPortalPath, "org.freedesktop.portal.Screenshot", "PickColor"},
                                      &body, 30000, on_reply, serial);
    builder_release(&body);
    if (!sent) {
        portal_request_end(&g_pick.request);
        return false;
    }
#endif
    g_pick.state = ScreenColorState::picking;
    g_pick.settled = settled;
    g_pick.context = context;
    return true;
}

ScreenColorState screen_color_pick_state(u32* rgb) noexcept {
    if (rgb && g_pick.state == ScreenColorState::picked)
        *rgb = g_pick.rgb;
    return g_pick.state;
}

void screen_color_pick_end() noexcept {
#if defined(_WIN32)
    if (g_pick.hook)
        UnhookWindowsHookEx(g_pick.hook);
    g_pick.hook = nullptr;
    g_pick.swallow = 0;
#else
    if (g_pick.state == ScreenColorState::picking)
        dbus::portal_request_close(&g_pick.request);
    dbus::portal_request_end(&g_pick.request);
#endif
    g_pick.state = ScreenColorState::idle;
    g_pick.settled = nullptr;
    g_pick.context = nullptr;
}

} // namespace maicord
