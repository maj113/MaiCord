#include "maicord/core/win32_image.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/pump.hpp"
#include "maicord/platform/window/win32.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "../../support/test_output.hpp"

// The Windows backend's events: win32_translate on every message the application models, and the window
// procedure of win32_window_create (hooks in order, the sink's answer, the default procedure, the post-actions the
// backend owns). Real windows, never shown.
namespace maicord {
namespace {

u32 failures;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

constexpr u8 bit_of(PointerButton button) noexcept {
    return static_cast<u8>(1u << static_cast<u8>(button));
}

WindowEvent translate(HWND window, UINT message, WPARAM wparam = 0, LPARAM lparam = 0, bool* modeled = nullptr) noexcept {
    WindowEvent event{};
    const bool result = win32_translate(window, message, wparam, lparam, &event);
    if (modeled)
        *modeled = result;
    return event;
}

HWND make_static(int x, int y, int width, int height) noexcept {
    return CreateWindowExW(0, L"STATIC", L"MaiCord window events fixture", WS_POPUP, x, y, width, height, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

void set_keys(bool control, bool alt, bool shift) noexcept {
    BYTE keys[256]{};
    if (control)
        keys[key::ctrl] = 0x80;
    if (alt)
        keys[key::alt] = 0x80;
    if (shift)
        keys[key::shift] = 0x80;
    SetKeyboardState(keys);
}

void translations(HWND window) noexcept {
    bool modeled = false;
    set_keys(false, false, false);

    // Pointer: client pixels from the packed signed words; buttons and modifiers from the MK_ flags.
    WindowEvent e = translate(window, WM_MOUSEMOVE, MK_LBUTTON | MK_SHIFT, MAKELPARAM(30, 0xFFFB), &modeled);
    check(modeled && e.type == EventType::pointer_move && e.pointer.x == 30.0f && e.pointer.y == -5.0f &&
              e.pointer.buttons == bit_of(PointerButton::left) && e.modifiers == mod_shift &&
              e.window == window,
          "translate: WM_MOUSEMOVE is pointer_move with signed client pixels, buttons and shift, for its window");
    e = translate(window, WM_RBUTTONDOWN, MK_RBUTTON | MK_CONTROL, MAKELPARAM(7, 9));
    check(e.type == EventType::pointer_down && e.button == static_cast<u8>(PointerButton::right) && e.flags == 0 &&
              e.pointer.buttons == bit_of(PointerButton::right) && e.modifiers == mod_ctrl,
          "translate: a right press is pointer_down with the right button and control");
    e = translate(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(1, 2));
    check(e.type == EventType::pointer_down && e.button == static_cast<u8>(PointerButton::left) &&
              (e.flags & ev_double_click) != 0,
          "translate: WM_LBUTTONDBLCLK is a left pointer_down flagged as a double click");
    e = translate(window, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(1, 2));
    check(e.button == static_cast<u8>(PointerButton::middle), "translate: the middle press");
    // The X button number is the high word of wparam.
    e = translate(window, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), MAKELPARAM(1, 2));
    check(e.type == EventType::pointer_down && e.button == static_cast<u8>(PointerButton::back) &&
              e.pointer.buttons == bit_of(PointerButton::back),
          "translate: X1 is the back button");
    e = translate(window, WM_XBUTTONDBLCLK, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), MAKELPARAM(1, 2));
    check(e.type == EventType::pointer_down && e.button == static_cast<u8>(PointerButton::forward) &&
              (e.flags & ev_double_click) != 0,
          "translate: an X2 double click is a forward pointer_down flagged as a double click");
    e = translate(window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), MAKELPARAM(1, 2));
    check(e.type == EventType::pointer_up && e.button == static_cast<u8>(PointerButton::forward) &&
              e.pointer.buttons == 0,
          "translate: releasing X2 is pointer_up of the forward button with nothing held");
    e = translate(window, WM_LBUTTONUP, 0, MAKELPARAM(4, 5));
    check(e.type == EventType::pointer_up && e.button == static_cast<u8>(PointerButton::left), "translate: left release");
    e = translate(window, WM_MOUSELEAVE);
    check(e.type == EventType::pointer_leave, "translate: WM_MOUSELEAVE is pointer_leave");

    // Wheel: a screen point mapped to the client, the delta in 120ths on the vertical or horizontal axis.
    RECT outer{};
    GetWindowRect(window, &outer);
    e = translate(window, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, 0xFF88),
                  MAKELPARAM(outer.left + 50, outer.top + 60), &modeled);
    check(modeled && e.type == EventType::wheel && e.wheel.x == 50.0f && e.wheel.y == 60.0f && e.wheel.dy120 == -120 &&
              e.wheel.dx120 == 0 && e.modifiers == mod_shift,
          "translate: WM_MOUSEWHEEL is a wheel at the client point with dy120 = the signed delta");
    e = translate(window, WM_MOUSEHWHEEL, MAKEWPARAM(0, 240), MAKELPARAM(outer.left + 1, outer.top + 2));
    check(e.type == EventType::wheel && e.wheel.dx120 == 240 && e.wheel.dy120 == 0, "translate: WM_MOUSEHWHEEL is dx120");

    e = translate(window, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(window));
    check(e.type == EventType::capture_lost && e.focus.other == window, "translate: WM_CAPTURECHANGED names the new owner");
    e = translate(window, WM_CANCELMODE);
    check(e.type == EventType::cancel_input, "translate: WM_CANCELMODE is cancel_input");

    // Cursor: the client area only.
    e = translate(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), &modeled);
    check(modeled && e.type == EventType::cursor_query, "translate: WM_SETCURSOR over the client is cursor_query");
    translate(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCAPTION, WM_MOUSEMOVE), &modeled);
    check(!modeled, "translate: WM_SETCURSOR over the frame is not modeled");

    // Context menu: -1 is the keyboard, anything else a screen point.
    e = translate(window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), static_cast<LPARAM>(-1));
    check(e.type == EventType::context_menu && (e.flags & ev_keyboard) != 0, "translate: lparam -1 is a keyboard context menu");
    e = translate(window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), MAKELPARAM(outer.left + 11, outer.top + 12));
    check(e.type == EventType::context_menu && (e.flags & ev_keyboard) == 0 && e.pointer.x == 11.0f && e.pointer.y == 12.0f,
          "translate: a pointer context menu carries the client point");

    // Keys: the message names the key, the repeat bit the auto-repeat, SYS the system bit; modifiers are a snapshot.
    set_keys(true, false, true);
    e = translate(window, WM_KEYDOWN, key::enter, 0x00000001 | (0x1Cu << 16) | (1u << 30));
    check(e.type == EventType::key_down && e.key.key == key::enter && e.key.scancode == 0x1C && (e.flags & ev_repeat) != 0 &&
              (e.flags & ev_system) == 0 && e.modifiers == (mod_ctrl | mod_shift),
          "translate: WM_KEYDOWN is key_down with scan code, the repeat flag and the ctrl+shift snapshot");
    e = translate(window, WM_KEYDOWN, key::enter, 0x00000001);
    check((e.flags & ev_repeat) == 0, "translate: a first press is not a repeat");
    e = translate(window, WM_KEYUP, key::enter, 0xC0000001u | (1u << 30));
    check(e.type == EventType::key_up && (e.flags & ev_repeat) == 0, "translate: a release is key_up and never a repeat");
    // The system flag is the message's, whatever the modifier state: Windows decides what an Alt chord is
    // (win32_behaviour.cpp documents what could be captured of it).
    set_keys(true, true, false);
    e = translate(window, WM_KEYDOWN, 'Q', 1);
    check(e.type == EventType::key_down && (e.flags & ev_system) == 0 && e.modifiers == (mod_ctrl | mod_alt),
          "translate: Ctrl+Alt (AltGr) arriving as WM_KEYDOWN is not a system key");
    e = translate(window, WM_SYSKEYDOWN, key::f4, 1 | (1u << 29));
    check(e.type == EventType::key_down && (e.flags & ev_system) != 0, "translate: WM_SYSKEYDOWN carries ev_system");
    e = translate(window, WM_SYSKEYUP, key::f4, 0xC0000001u);
    check(e.type == EventType::key_up && (e.flags & ev_system) != 0, "translate: WM_SYSKEYUP is a system key_up");
    set_keys(false, false, false);
    e = translate(window, WM_KEYDOWN, key::menu, 1 | (0x5Du << 16) | (1u << 24));
    check(e.key.scancode == (0x5Du | 0x100u), "translate: the extended bit lifts the scan code by 0x100");

    // Characters: UTF-16 units; the system flag follows WM_SYSCHAR; WM_UNICHAR only for a BMP scalar.
    e = translate(window, WM_CHAR, 0x00E9);
    check(e.type == EventType::character && e.character.unit == 0xE9 && e.flags == 0, "translate: WM_CHAR is character");
    e = translate(window, WM_SYSCHAR, ' ');
    check(e.type == EventType::character && (e.flags & ev_system) != 0, "translate: WM_SYSCHAR is a system character");
    translate(window, WM_UNICHAR, 0x1F600, 0, &modeled);
    check(!modeled, "translate: a WM_UNICHAR beyond the BMP takes the default path");
    translate(window, WM_UNICHAR, UNICODE_NOCHAR, 0, &modeled);
    check(!modeled, "translate: the UNICODE_NOCHAR query takes the default path");
    e = translate(window, WM_UNICHAR, 0x263A);
    check(e.type == EventType::character && e.character.unit == 0x263A, "translate: a BMP WM_UNICHAR is that character");

    // Focus and activation.
    e = translate(window, WM_SETFOCUS, reinterpret_cast<WPARAM>(window));
    check(e.type == EventType::focus_changed && e.focus.gained && e.focus.other == window, "translate: WM_SETFOCUS gains focus");
    e = translate(window, WM_KILLFOCUS, reinterpret_cast<WPARAM>(window));
    check(e.type == EventType::focus_changed && !e.focus.gained, "translate: WM_KILLFOCUS loses it");
    e = translate(window, WM_ACTIVATE, MAKEWPARAM(1, 1), 0);
    check(e.type == EventType::activation_changed && e.focus.gained && (e.flags & ev_minimized) != 0,
          "translate: WM_ACTIVATE gained while minimized");
    e = translate(window, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
    check(e.type == EventType::activation_changed && !e.focus.gained && (e.flags & ev_minimized) == 0,
          "translate: WM_ACTIVATE inactive");

    // State and geometry.
    WINDOWPOS position{};
    position.flags = SWP_NOSIZE;
    e = translate(window, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&position));
    check(e.type == EventType::state_changed && (e.window_state.state & state_geometry) == 0 &&
              (e.window_state.state & state_visible) == 0,
          "translate: a move without a resize is a state_changed without geometry");
    position.flags = 0;
    e = translate(window, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&position));
    check((e.window_state.state & state_geometry) != 0 && (e.window_state.changed & state_geometry) != 0,
          "translate: a resize carries the geometry bit");
    position.flags = SWP_NOSIZE | SWP_FRAMECHANGED;
    e = translate(window, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&position));
    check((e.window_state.state & state_geometry) != 0, "translate: a frame change carries it too");
    e = translate(window, WM_SIZE, 0, MAKELPARAM(640, 480));
    check(e.type == EventType::resize && e.resize.width_px == 640 && e.resize.height_px == 480 && e.resize.scale_120 != 0,
          "translate: WM_SIZE is resize with the client size and a scale");
    e = translate(window, WM_ENTERSIZEMOVE);
    check(e.type == EventType::resize_begin, "translate: WM_ENTERSIZEMOVE");
    e = translate(window, WM_EXITSIZEMOVE);
    check(e.type == EventType::resize_end, "translate: WM_EXITSIZEMOVE");
    RECT suggested{10, 20, 1010, 520};
    e = translate(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
    check(e.type == EventType::scale_changed && e.resize.scale_120 == 180 && e.resize.width_px == 1000 &&
              e.resize.height_px == 500,
          "translate: WM_DPICHANGED is scale_changed with scale_120 = dpi * 120 / 96 and the suggested size");
    e = translate(window, WM_GETMINMAXINFO);
    check(e.type == EventType::size_limits, "translate: WM_GETMINMAXINFO is a size_limits query");
    e = translate(window, WM_PAINT);
    check(e.type == EventType::paint, "translate: WM_PAINT");
    e = translate(window, WM_CLOSE);
    check(e.type == EventType::close_requested, "translate: WM_CLOSE");
    e = translate(window, WM_DESTROY);
    check(e.type == EventType::destroyed, "translate: WM_DESTROY");
    e = translate(window, WM_DROPFILES, 0x1234);
    check(e.type == EventType::drop_files && e.drop.files == reinterpret_cast<DroppedFiles*>(0x1234),
          "translate: WM_DROPFILES carries the drop handle");

    // Settings: the colour scheme names itself; anything else may touch the pointer, caret and motion values.
    e = translate(window, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
    check(e.type == EventType::settings_changed && e.settings.areas == settings_scheme,
          "translate: ImmersiveColorSet is the colour scheme");
    e = translate(window, WM_SETTINGCHANGE, 0, 0);
    check(e.settings.areas == (settings_pointer | settings_caret | settings_motion),
          "translate: any other setting change re-reads pointer, caret and motion");
    e = translate(window, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSetX"));
    check(e.settings.areas == (settings_pointer | settings_caret | settings_motion),
          "translate: the scheme name must match exactly");

    // Wakes.
    e = translate(window, WM_NULL);
    check(e.type == EventType::wake && e.wake.cause == wake_poke, "translate: WM_NULL is a poke");
    e = translate(window, WM_APP + 0x31, 7, 9);
    check(e.type == EventType::wake && e.wake.cause == wake_begin_webauthn && e.wake.a == 7 && e.wake.b == 9,
          "translate: the web authentication message is a wake with both words");
    e = translate(window, WM_APP + 0x32, 1, 2);
    check(e.wake.cause == wake_phone_verify, "translate: the phone verification message");
    e = translate(window, WM_APP + 0x33);
    check(e.wake.cause == wake_notifications, "translate: the notification message");

    // What the application never models stays unmodeled.
    e = translate(window, WM_COMMAND, 5, 6, &modeled);
    check(!modeled && e.type == EventType::none, "translate: an unmodeled message is not an event");
}

// --- the window procedure --------------------------------------------------------------------------------------

constexpr u32 kLogCapacity = 64;
struct Seen {
    char what;  // 's' shell, 'k' sink
    EventType type;
    UINT message;
};
Seen g_seen[kLogCapacity];
u32 g_seen_count;
WindowEvent g_last_sink_event;
float g_cursor_x, g_cursor_y;
bool g_cursor_from_sink;
bool g_veto_close, g_blocked, g_sink_handles;
WindowEventResult g_answer;
Window g_window;

void note(char what, EventType type, UINT message) noexcept {
    if (g_seen_count < kLogCapacity)
        g_seen[g_seen_count++] = {what, type, message};
}

bool shell(void*, HWND, UINT message, WPARAM, LPARAM, LRESULT* result) noexcept {
    note('s', EventType::none, message);
    if (message == WM_USER + 8) {
        *result = 43;
        return true;
    }
    return false;
}

bool blocked(void*) noexcept {
    return g_blocked;
}

void sink(void*, const WindowEvent& event, WindowEventResult* result) noexcept {
    note('k', event.type, 0u);
    g_last_sink_event = event;
    if (event.type == EventType::cursor_query) {
        float x = 0, y = 0;
        g_cursor_from_sink = window_pointer_position(g_window, &x, &y);
        g_cursor_x = x;
        g_cursor_y = y;
    }
    *result = g_answer;
    if (event.type == EventType::close_requested)
        result->handled = g_veto_close;
    else if (event.type != EventType::size_limits && event.type != EventType::wake)
        result->handled = g_sink_handles;
}

bool order_is(const char* expected, u32 count) noexcept {
    if (g_seen_count != count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (g_seen[i].what != expected[i])
            return false;
    return true;
}

void procedure() noexcept {
    const Win32Hooks hooks{nullptr, shell, blocked};
    const char* reason = "";
    Window window{};
    check(win32_window_create({utf8_literal("MaiCord window events"), {}, 400, 300, false}, {0, &hooks}, sink, nullptr,
                              &window, &reason),
          "proc: the backend creates the main window");
    if (!window)
        return;
    g_window = window;
    const HWND hwnd = win32_hwnd(window);
    g_seen_count = 0;
    g_blocked = g_veto_close = g_sink_handles = false;
    g_answer = {};
    const auto reset = [] { g_seen_count = 0; };

    // Order: shell, sink; an unmodeled message skips the sink.
    LRESULT answer = SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(3, 4));
    check(order_is("sk", 2) && g_seen[1].type == EventType::pointer_down && answer == 0,
          "proc: a modeled message runs the shell hook and the sink, then the default");
    reset();
    answer = SendMessageW(hwnd, WM_USER + 1, 0, 0);
    check(order_is("s", 1) && answer == 0, "proc: an unmodeled message skips the sink");
    reset();
    answer = SendMessageW(hwnd, WM_USER + 8, 0, 0);
    check(order_is("s", 1) && answer == 43, "proc: a shell hook that takes a message ends the dispatch with its result");

    // The sink's answer: handled is 0 (TRUE for a cursor query), unhandled falls to the default procedure.
    reset();
    g_sink_handles = true;
    answer = SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(3, 4));
    check(answer == 0, "proc: a handled event answers 0");
    answer = SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    check(answer == TRUE, "proc: a handled cursor query answers TRUE");
    g_sink_handles = false;
    answer = SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(3, 4));
    check(answer == 0, "proc: an unhandled event is the default procedure's (0 for a button release)");
    g_answer = {};

    // Backend rules: erase-background answers 1 without the sink; paint is always 0.
    reset();
    answer = SendMessageW(hwnd, WM_ERASEBKGND, 0, 0);
    check(answer == 1 && !order_is("k", 1), "proc: WM_ERASEBKGND answers 1 for the backend");
    g_sink_handles = false;
    reset();
    answer = SendMessageW(hwnd, WM_PAINT, 0, 0);
    check(answer == 0 && g_seen_count != 0 && g_seen[g_seen_count - 1].type == EventType::paint, "proc: WM_PAINT reaches the sink");

    // A modal blocks the input classes the sink does not model; others still take the default.
    g_blocked = true;
    reset();
    answer = SendMessageW(hwnd, WM_IME_COMPOSITION, 0, 0);
    check(answer == 0 && order_is("s", 1), "proc: a blocked IME message is swallowed (0)");
    answer = SendMessageW(hwnd, WM_COMMAND, 0, 0);
    check(answer == 0, "proc: a blocked command is swallowed");
    g_blocked = false;

    // Cursor: the point is resolved once for the query; the sink reads the same through the service.
    reset();
    g_cursor_from_sink = false;
    SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    check(g_cursor_from_sink && g_cursor_x == g_last_sink_event.pointer.x && g_cursor_y == g_last_sink_event.pointer.y,
          "proc: window_pointer_position answers with the query's own point during cursor_query");
    reset();
    SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCAPTION, WM_MOUSEMOVE));
    check(order_is("s", 1), "proc: a frame cursor never reaches the sink");

    // Capture: the notification is the system's, delivered while window_release_capture runs.
    window_capture_pointer(window);
    check(window_pointer_captured(window), "proc: the window holds the capture it took");
    reset();
    window_release_capture(window);
    check(!window_pointer_captured(window) && order_is("sk", 2) && g_seen[1].type == EventType::capture_lost,
          "proc: releasing the capture reaches the sink as capture_lost before the call returns");

    // Size limits: the sink's answer raises the minimum track size.
    MINMAXINFO limits{};
    limits.ptMinTrackSize = {100, 5000};
    g_answer = {};
    g_answer.handled = true;
    g_answer.width_px = 640;
    g_answer.height_px = 480;
    reset();
    answer = SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    // The sink answers the smallest client: the track size is the window around it.
    RECT outer{0, 0, 640, 480};
    AdjustWindowRectExForDpi(&outer, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
    check(answer == 0 && limits.ptMinTrackSize.x == outer.right - outer.left && limits.ptMinTrackSize.y == 5000,
          "proc: size_limits raises each minimum to the larger of the system's and the window around the sink's client");
    g_answer = {};

    // DPI change: the sink first, then the window moves to the suggested rectangle.
    RECT suggested{20, 30, 520, 430};
    reset();
    answer = SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(120, 120), reinterpret_cast<LPARAM>(&suggested));
    RECT now{};
    GetWindowRect(hwnd, &now);
    bool scaled = false;
    for (u32 i = 0; i < g_seen_count; ++i)
        scaled = scaled || (g_seen[i].what == 'k' && g_seen[i].type == EventType::scale_changed);
    check(answer == 0 && scaled && now.left == 20 && now.top == 30 && now.right == 520 && now.bottom == 430,
          "proc: after the sink, a DPI change applies the suggested rectangle");

    // Provisional size while the frame is computed.
    NCCALCSIZE_PARAMS params{};
    params.rgrc[0] = {0, 0, 800, 600};
    reset();
    SendMessageW(hwnd, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&params));
    bool provisional = false;
    for (u32 i = 0; i < g_seen_count; ++i)
        provisional = provisional || (g_seen[i].what == 'k' && g_seen[i].type == EventType::resize);
    check(provisional && (g_last_sink_event.flags & ev_provisional) != 0 &&
              g_last_sink_event.resize.width_px == static_cast<u32>(params.rgrc[0].right - params.rgrc[0].left) &&
              g_last_sink_event.resize.height_px == static_cast<u32>(params.rgrc[0].bottom - params.rgrc[0].top),
          "proc: WM_NCCALCSIZE reaches the sink as a provisional resize of the computed client");

    // Close: the sink may veto; otherwise the default procedure destroys the window and the backend ends the loop.
    g_veto_close = true;
    reset();
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(IsWindow(hwnd) != FALSE, "proc: a handled close_requested keeps the window");
    g_veto_close = false;
    reset();
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(IsWindow(hwnd) == FALSE, "proc: an unhandled close destroys the window");
    bool destroyed = false;
    for (u32 i = 0; i < g_seen_count; ++i)
        destroyed = destroyed || (g_seen[i].what == 'k' && g_seen[i].type == EventType::destroyed);
    MSG quit{};
    check(destroyed && PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE,
          "proc: destruction reaches the sink and posts the quit message");
}

// --- what each path asks the system ------------------------------------------------------------------------------
// This executable's import slots of the calls below point at counting forwarders while the paths run: an idle wake,
// a pointer move, a press, a key, a resize step. The sink asks the window service what the client's sink asks for
// the same events (shell/window_dispatch.cpp, shell/window_state.hpp).

template <typename T, T... I>
struct Indices {};

struct Counted {
    const char* name;
    u8 arguments;  // 4: up to four, all in registers; else the function's own count
    bool system;   // enters the kernel on every call
};
constexpr Counted kCounted[]{
    {"GetMessageTime", 4, true},       {"GetCursorPos", 4, true},
    {"GetKeyState", 4, false},         {"SetCursor", 4, true},
    {"TrackMouseEvent", 4, true},      {"SetFocus", 4, true},
    {"TranslateMessage", 4, true},     {"PeekMessageW", 5, true},
    {"MsgWaitForMultipleObjectsEx", 5, true}, {"SetWaitableTimerEx", 7, true},
    {"CancelWaitableTimer", 4, true},  {"ValidateRect", 4, true},
    {"GetFocus", 4, false},            {"ScreenToClient", 4, false},
    {"IsIconic", 4, false},            {"IsZoomed", 4, false},
    {"IsWindowVisible", 4, false},     {"GetClientRect", 4, false},
    {"GetDpiForWindow", 4, false},     {"GetSystemMetricsForDpi", 4, false},
    {"GetWindowLongPtrW", 4, false},   {"DispatchMessageW", 4, false},
    {"DefWindowProcW", 4, false},
};
constexpr u32 kCountedTotal = sizeof(kCounted) / sizeof(kCounted[0]);

u64 g_real[kCountedTotal];
u64* g_slot[kCountedTotal];
u32 g_calls[kCountedTotal];
u32 g_mark[kCountedTotal];

template <u32 I>
u64 WINAPI forward4(u64 a, u64 b, u64 c, u64 d) noexcept {
    ++g_calls[I];
    return reinterpret_cast<u64(WINAPI*)(u64, u64, u64, u64) noexcept>(g_real[I])(a, b, c, d);
}
template <u32 I>
u64 WINAPI forward5(u64 a, u64 b, u64 c, u64 d, u64 e) noexcept {
    ++g_calls[I];
    return reinterpret_cast<u64(WINAPI*)(u64, u64, u64, u64, u64) noexcept>(g_real[I])(a, b, c, d, e);
}
template <u32 I>
u64 WINAPI forward7(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g) noexcept {
    ++g_calls[I];
    return reinterpret_cast<u64(WINAPI*)(u64, u64, u64, u64, u64, u64, u64) noexcept>(g_real[I])(a, b, c, d, e, f, g);
}
template <u32 I>
u64 forwarder() noexcept {
    if constexpr (kCounted[I].arguments == 7)
        return reinterpret_cast<u64>(&forward7<I>);
    else if constexpr (kCounted[I].arguments == 5)
        return reinterpret_cast<u64>(&forward5<I>);
    else
        return reinterpret_cast<u64>(&forward4<I>);
}
template <u32... I>
void forwarders(u64* out, Indices<u32, I...>) noexcept {
    ((out[I] = forwarder<I>()), ...);
}

bool same_name(const char* a, const char* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

bool write_slot(u64* slot, u64 value) noexcept {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous))
        return false;
    *slot = value;
    VirtualProtect(slot, sizeof(*slot), previous, &previous);
    return true;
}

// Points every counted import of this executable at its forwarder; false when one is not imported. The build keeps no
// import name table, so a slot is known by the address the loader wrote into it.
bool count_imports() noexcept {
    u64 forward[kCountedTotal];
    forwarders(forward, __make_integer_seq<Indices, u32, kCountedTotal>{});
    const HMODULE modules[]{GetModuleHandleW(L"user32.dll"), GetModuleHandleW(L"kernel32.dll")};
    for (u32 c = 0; c < kCountedTotal; ++c)
        for (const HMODULE module : modules)
            if (!g_real[c] && module)
                g_real[c] = reinterpret_cast<u64>(GetProcAddress(module, kCounted[c].name));
    auto* base = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name;
         ++entry)
        for (auto* slot = reinterpret_cast<u64*>(base + entry->FirstThunk); *slot; ++slot)
            for (u32 c = 0; c < kCountedTotal; ++c)
                if (!g_slot[c] && g_real[c] && *slot == g_real[c] && write_slot(slot, forward[c]))
                    g_slot[c] = slot;
    bool all = true;
    for (u32 c = 0; c < kCountedTotal; ++c)
        all = all && g_slot[c];
    return all;
}

void restore_imports() noexcept {
    for (u32 c = 0; c < kCountedTotal; ++c)
        if (g_slot[c])
            write_slot(g_slot[c], g_real[c]);
}

void mark() noexcept {
    for (u32 c = 0; c < kCountedTotal; ++c)
        g_mark[c] = g_calls[c];
}

u32 since_mark(const char* name) noexcept {
    for (u32 c = 0; c < kCountedTotal; ++c)
        if (same_name(kCounted[c].name, name))
            return g_calls[c] - g_mark[c];
    return 0;
}

// Writes "<path> <call>=<count>" for each counted call since mark(), and the path's total and kernel entries.
void report(const char* path) noexcept {
    u32 total = 0, system = 0;
    for (u32 c = 0; c < kCountedTotal; ++c) {
        const u32 calls = g_calls[c] - g_mark[c];
        if (!calls)
            continue;
        total += calls;
        if (kCounted[c].system)
            system += calls;
        test_write_text("costs: ");
        test_write_text(path);
        test_write_text(" ");
        test_write_text(kCounted[c].name);
        test_write_metric("=", calls);
    }
    test_write_text("costs: ");
    test_write_text(path);
    test_write_metric(" calls=", total);
    test_write_text("costs: ");
    test_write_text(path);
    test_write_metric(" system calls=", system);
}

constexpr u32 kCostCaption = 32;
u32 cost_caption_height(void*) noexcept {
    return kCostCaption;
}
bool cost_caption_hidden(void*) noexcept {
    return false;
}
FrameHit cost_caption_hit(void*, float, float y) noexcept {
    return y < static_cast<float>(kCostCaption) ? FrameHit::caption : FrameHit::client;
}
void cost_caption_flag(void*, bool) noexcept {}
void cost_caption_part(void*, FrameHit) noexcept {}
void cost_caption_leave(void*) noexcept {}
bool cost_caption_release(void*, FrameHit) noexcept {
    return false;
}
bool cost_caption_bar(void*, FrameBarRects*) noexcept {
    return false;
}

Window g_cost_window;

void cost_sink(void*, const WindowEvent& event, WindowEventResult* result) noexcept {
    if (!g_cost_window)
        return;
    switch (event.type) {
    case EventType::state_changed:
        (void)window_state(g_cost_window);
        (void)window_metrics(g_cost_window);
        result->handled = true;
        break;
    case EventType::cursor_query: {
        float x = 0, y = 0;
        g_cursor_from_sink = window_pointer_position(g_cost_window, &x, &y);
        g_cursor_x = x;
        g_cursor_y = y;
        window_set_cursor(g_cost_window, UiCursor::arrow);
        result->handled = true;
        break;
    }
    case EventType::pointer_down:
        window_focus(g_cost_window);
        result->handled = true;
        break;
    case EventType::size_limits:
        result->width_px = 200;
        result->height_px = 150;
        result->handled = true;
        break;
    default:
        break;
    }
}

bool pass_keys(void*, const WindowEvent&) noexcept {
    return false;
}

void drain() noexcept {
    while (window_pump(64, 1'000'000, {}).dispatched)
        ;
}

// The system's order for the pointer over the client: the hit test, the cursor query for its answer, the message.
void pointer_message(HWND hwnd, UINT message, WPARAM wparam, POINT client) noexcept {
    POINT screen = client;
    ClientToScreen(hwnd, &screen);
    const LRESULT hit = SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y));
    SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(static_cast<WORD>(hit), message));
    SendMessageW(hwnd, message, wparam, MAKELPARAM(client.x, client.y));
}

void costs() noexcept {
    UnregisterClassW(L"MaiCordMainWindow", GetModuleHandleW(nullptr));
    static const Win32CaptionOps caption{nullptr,           cost_caption_height,  cost_caption_hidden,
                                         cost_caption_hit,  cost_caption_flag,    cost_caption_flag,
                                         cost_caption_part, cost_caption_leave,   cost_caption_part,
                                         cost_caption_release, cost_caption_bar};
    const char* reason = "";
    Window window{};
    check(win32_window_create({utf8_literal("MaiCord window costs"), {}, 400, 300, false},
                              {0, nullptr, &caption, false}, cost_sink, nullptr, &window, &reason),
          "costs: the backend creates a captioned main window");
    if (!window)
        return;
    g_cost_window = window;
    const HWND hwnd = win32_hwnd(window);
    check(count_imports(), "costs: every counted call is imported by this executable");
    drain();

    // Idle: a wake with nothing to do, in the foreground (exact) and in the background (a tolerant timer).
    mark();
    window_wait(nullptr, 0, 0, 0);
    window_pump(64, 1'000'000, {});
    report("idle wake, exact timeout");
    // A message the system sends meanwhile ends the wait first; the next one waits for the timer again.
    WindowWaitResult expired{};
    for (u32 attempt = 0; attempt < 4 && !expired.timed_out; ++attempt) {
        mark();
        expired = window_wait(nullptr, 0, 1, 1);
        window_pump(64, 1'000'000, {});
    }
    report("idle wake, tolerant timer");
    check(expired.timed_out, "costs: the tolerant timer's expiry reads as the timeout");
    // Input while a tolerant deadline stands: the second wake waits for the same deadline (measured over a stretch the
    // millisecond tick did not move through, so both waits name the same deadline).
    WindowWaitResult woken{};
    bool steady = false;
    for (u32 attempt = 0; attempt < 4 && !steady; ++attempt) {
        const u64 tick = GetTickCount64();
        const u64 deadline = tick + 5'000;
        PostMessageW(hwnd, WM_NULL, 0, 0);
        window_wait(nullptr, 0, static_cast<u32>(deadline - GetTickCount64()), 50);
        window_pump(64, 1'000'000, {});
        mark();
        PostMessageW(hwnd, WM_NULL, 0, 0);
        woken = window_wait(nullptr, 0, static_cast<u32>(deadline - GetTickCount64()), 50);
        window_pump(64, 1'000'000, {});
        steady = GetTickCount64() == tick;
    }
    report("input wake, tolerant deadline unchanged");
    check(woken.window_input, "costs: a posted message wakes a tolerant wait");
    check(!steady || (since_mark("SetWaitableTimerEx") == 0 && since_mark("CancelWaitableTimer") == 0),
          "costs: a wake for something else keeps the timer armed for the unchanged deadline");
    mark();
    window_wait(nullptr, 0, 0, 0);
    report("exact wait after a tolerant one");
    check(since_mark("CancelWaitableTimer") == 1, "costs: an exact wait disarms the timer the tolerant one left");

    // The pointer over the client, once to arm the leave notification, then measured.
    const POINT inside{100, 100};
    pointer_message(hwnd, WM_MOUSEMOVE, 0, inside);
    mark();
    g_cursor_from_sink = false;
    pointer_message(hwnd, WM_MOUSEMOVE, 0, {101, 101});
    report("pointer move");
    check(g_cursor_from_sink && g_cursor_x == 101.0f && g_cursor_y == 101.0f,
          "costs: the cursor query reads the point of the hit test before it");
    check(since_mark("GetCursorPos") == 0 && since_mark("GetMessageTime") == 0,
          "costs: a pointer move reads neither the cursor nor the message time from the system");

    // A press and release on the focused window.
    SetFocus(hwnd);
    mark();
    pointer_message(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, inside);
    pointer_message(hwnd, WM_LBUTTONUP, 0, inside);
    report("press and release");
    check(since_mark("GetCursorPos") == 0 && since_mark("GetMessageTime") == 2 && since_mark("SetFocus") == 0,
          "costs: a press and release read their two times and leave the focused window's focus alone");

    // A key through the loop as the client pumps it: prefilter, TranslateMessage, the character it posts (after the
    // leave notifications the pointer above queued: the pointer is never over the hidden window).
    const WindowPumpHooks keys{nullptr, pass_keys, nullptr, nullptr};
    drain();
    mark();
    PostMessageW(hwnd, WM_KEYDOWN, 'A', 1 | (0x1E << 16));
    window_pump(64, 1'000'000, keys);
    report("key press");
    check(since_mark("GetMessageTime") == 0, "costs: a key press reads no message time");
    mark();
    PostMessageW(hwnd, WM_KEYUP, 'A', static_cast<LPARAM>(0xC0000001u | (0x1Eu << 16)));
    window_pump(64, 1'000'000, keys);
    report("key release");
    check(since_mark("GetMessageTime") == 0, "costs: a key release reads no message time");

    // One step of a resize, as the sizing loop moves an edge.
    SetWindowPos(hwnd, nullptr, 0, 0, 640, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    mark();
    SetWindowPos(hwnd, nullptr, 0, 0, 648, 488, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    report("resize step");
    check(since_mark("GetMessageTime") == 0, "costs: a resize step reads no message time");

    restore_imports();
    g_cost_window = nullptr;
    DestroyWindow(hwnd);
    drain();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_window_win32_events_entry) {
    using namespace maicord;
    BYTE saved[256]{};
    const bool have_saved = GetKeyboardState(saved) != FALSE;
    check(have_saved, "win32 events: thread keyboard state saved");
    // Translation runs against a window at a known place: popups have no frame, so screen = client + origin.
    HWND fixture = make_static(300, 400, 320, 240);
    check(fixture != nullptr, "win32 events: translation fixture window exists");
    if (fixture) {
        translations(fixture);
        DestroyWindow(fixture);
    }
    procedure();
    costs();
    if (have_saved)
        SetKeyboardState(saved);
    test_exit(failures, "window win32 events");
}
