// Windows messages into window events: the one place that decodes wparam, lparam and the MK_ and WM_ numbers
// (include/maicord/platform/window/win32.hpp). Pure but for the keyboard and cursor state it reads, the window state it
// remembers between two WM_WINDOWPOSCHANGED and the point of the hit test before a cursor query.
#include "maicord/platform/window/win32.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord {
namespace {

// The private messages the application posts to its own window (window_post maps WakeCause to them).
constexpr UINT kBeginWebAuthnMessage = WM_APP + 0x31;
constexpr UINT kPhoneVerifyMessage = WM_APP + 0x32;
constexpr UINT kNotificationMessage = WM_APP + 0x33;

constexpr u8 bit_of(PointerButton button) noexcept {
    return static_cast<u8>(1u << static_cast<u8>(button));
}

// Which buttons the MK_ flags of a pointer message say are down.
u8 buttons_of(WPARAM wparam) noexcept {
    u32 buttons = 0;
    if (wparam & MK_LBUTTON)
        buttons |= bit_of(PointerButton::left);
    if (wparam & MK_RBUTTON)
        buttons |= bit_of(PointerButton::right);
    if (wparam & MK_MBUTTON)
        buttons |= bit_of(PointerButton::middle);
    if (wparam & MK_XBUTTON1)
        buttons |= bit_of(PointerButton::back);
    if (wparam & MK_XBUTTON2)
        buttons |= bit_of(PointerButton::forward);
    return static_cast<u8>(buttons);
}

u8 modifiers_of(WPARAM wparam) noexcept {
    return static_cast<u8>(((wparam & MK_CONTROL) ? mod_ctrl : 0) | ((wparam & MK_SHIFT) ? mod_shift : 0));
}

// The physical Ctrl, Alt and Shift as of the message being handled; Ctrl and Alt both read down for AltGr.
u8 keyboard_modifiers() noexcept {
    return static_cast<u8>(((GetKeyState(key::ctrl) & 0x8000) ? mod_ctrl : 0) |
                           ((GetKeyState(key::alt) & 0x8000) ? mod_alt : 0) |
                           ((GetKeyState(key::shift) & 0x8000) ? mod_shift : 0));
}

float client_coordinate(LPARAM lparam, bool vertical) noexcept {
    return static_cast<float>(static_cast<short>(vertical ? HIWORD(lparam) : LOWORD(lparam)));
}

void set_pointer(WindowEvent* event, WPARAM wparam, LPARAM lparam) noexcept {
    event->pointer.x = client_coordinate(lparam, false);
    event->pointer.y = client_coordinate(lparam, true);
    event->pointer.buttons = buttons_of(wparam);
    event->modifiers = modifiers_of(wparam);
}

// A screen point (wheel and context-menu messages carry one) as client pixels. False leaves it as the screen point.
bool set_screen_point(HWND window, WindowEvent* event, LPARAM lparam) noexcept {
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    const bool mapped = ScreenToClient(window, &point) != FALSE;
    event->pointer.x = static_cast<float>(point.x);
    event->pointer.y = static_cast<float>(point.y);
    return mapped;
}

u32 scale_of_dpi(u32 dpi) noexcept {
    return dpi * 120 / 96;
}

bool settings_area_is(LPARAM lparam, const wchar_t* area) noexcept {
    const auto* text = reinterpret_cast<const wchar_t*>(lparam);
    if (!text)
        return false;
    u32 i = 0;
    for (; area[i]; ++i)
        if (text[i] != area[i])
            return false;
    return text[i] == 0;
}

// The state bits the last WM_WINDOWPOSCHANGED reported, to name what changed.
u32 g_window_state;

// The screen point of the last WM_NCHITTEST. The system hit-tests the pointer's position and then sends the
// WM_SETCURSOR for the answer, so the cursor query takes the point from it instead of asking the system again. The
// window is null once a cursor query spent it.
HWND g_hit_window;
LPARAM g_hit_point;

} // namespace

bool win32_translate(HWND window, UINT message, WPARAM wparam, LPARAM lparam, WindowEvent* out) noexcept {
    WindowEvent& event = *out;
    event = {};
    event.window = window;
    switch (message) {
    case WM_MOUSEMOVE:
        event.type = EventType::pointer_move;
        set_pointer(&event, wparam, lparam);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        event.type = EventType::pointer_down;
        set_pointer(&event, wparam, lparam);
        event.button = static_cast<u8>(message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK   ? PointerButton::left
                                       : message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK ? PointerButton::right
                                       : message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK ? PointerButton::middle
                                       : HIWORD(wparam) == XBUTTON1 ? PointerButton::back
                                                                    : PointerButton::forward);
        if (message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK ||
            message == WM_XBUTTONDBLCLK)
            event.flags = ev_double_click;
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        event.type = EventType::pointer_up;
        set_pointer(&event, wparam, lparam);
        event.button = static_cast<u8>(message == WM_LBUTTONUP   ? PointerButton::left
                                       : message == WM_RBUTTONUP ? PointerButton::right
                                       : message == WM_MBUTTONUP ? PointerButton::middle
                                       : HIWORD(wparam) == XBUTTON1 ? PointerButton::back
                                                                    : PointerButton::forward);
        break;
    case WM_MOUSELEAVE:
        event.type = EventType::pointer_leave;
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // A screen point the window cannot map is not a notch over the client: the default path takes it.
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        if (!ScreenToClient(window, &point))
            break;
        event.type = EventType::wheel;
        event.modifiers = modifiers_of(wparam);
        event.wheel.x = static_cast<float>(point.x);
        event.wheel.y = static_cast<float>(point.y);
        const auto delta = static_cast<i32>(static_cast<short>(HIWORD(wparam)));
        if (message == WM_MOUSEWHEEL) {
            event.wheel.dy120 = delta;
            event.wheel.dy_px = static_cast<float>(delta);
        } else {
            event.wheel.dx120 = delta;
            event.wheel.dx_px = static_cast<float>(delta);
        }
        break;
    }
    case WM_CAPTURECHANGED:
        event.type = EventType::capture_lost;
        event.focus.other = reinterpret_cast<NativeWindow>(lparam);
        break;
    case WM_CANCELMODE:
        event.type = EventType::cancel_input;
        break;
    case WM_NCHITTEST:
        g_hit_window = window;
        g_hit_point = lparam;
        break;
    case WM_SETCURSOR: {
        // The hit test's point is spent by the query that follows it; one sent without a hit test asks the system.
        POINT point{static_cast<short>(LOWORD(g_hit_point)), static_cast<short>(HIWORD(g_hit_point))};
        const bool hit = g_hit_window == window;
        g_hit_window = nullptr;
        // Only the client area is the application's; the frame's cursors stay the system's.
        if (LOWORD(lparam) != HTCLIENT)
            break;
        event.type = EventType::cursor_query;
        if ((hit || GetCursorPos(&point)) && ScreenToClient(window, &point)) {
            event.pointer.x = static_cast<float>(point.x);
            event.pointer.y = static_cast<float>(point.y);
        } else {
            // The system cannot tell where the pointer is: no position (NaN), which no rectangle contains.
            event.pointer.x = event.pointer.y = __builtin_nanf("");
        }
        break;
    }
    case WM_CONTEXTMENU:
        event.type = EventType::context_menu;
        if (static_cast<short>(LOWORD(lparam)) == -1 && static_cast<short>(HIWORD(lparam)) == -1) {
            event.flags = ev_keyboard;
            event.pointer.x = event.pointer.y = -1.0f;
        } else if (!set_screen_point(window, &event, lparam)) {
            event.type = EventType::none;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        event.type = message == WM_KEYDOWN || message == WM_SYSKEYDOWN ? EventType::key_down : EventType::key_up;
        event.key.key = static_cast<u32>(wparam);
        event.key.scancode = (static_cast<u32>(lparam >> 16) & 0xFF) | ((lparam & (1ll << 24)) ? 0x100u : 0u);
        event.modifiers = keyboard_modifiers();
        if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
            event.flags |= ev_system;
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && (lparam & (1ll << 30)))
            event.flags |= ev_repeat;
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        event.type = EventType::character;
        event.character.unit = static_cast<u32>(wparam);
        if (message == WM_SYSCHAR)
            event.flags = ev_system;
        break;
    case WM_UNICHAR:
        // A BMP scalar from a text-injecting tool is the character it names; the rest takes the default path
        // (DefWindowProc turns it into WM_CHARs).
        if (wparam == 0 || wparam >= UNICODE_NOCHAR)
            break;
        event.type = EventType::character;
        event.character.unit = static_cast<u32>(wparam);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        event.type = EventType::focus_changed;
        event.focus.gained = message == WM_SETFOCUS;
        event.focus.other = reinterpret_cast<NativeWindow>(wparam);
        if (message == WM_KILLFOCUS && wparam && GetWindow(reinterpret_cast<HWND>(wparam), GW_OWNER) == window)
            event.flags |= ev_owned_focus;
        break;
    case WM_ACTIVATE:
        event.type = EventType::activation_changed;
        event.focus.gained = LOWORD(wparam) != WA_INACTIVE;
        event.focus.other = reinterpret_cast<NativeWindow>(lparam);
        if (HIWORD(wparam))
            event.flags = ev_minimized;
        break;
    case WM_WINDOWPOSCHANGED: {
        event.type = EventType::state_changed;
        u32 state = 0;
        if (IsIconic(window))
            state |= state_minimized;
        if (IsWindowVisible(window))
            state |= state_visible;
        if (IsZoomed(window))
            state |= state_maximized;
        u32 changed = state ^ g_window_state;
        g_window_state = state;
        const auto* position = reinterpret_cast<const WINDOWPOS*>(lparam);
        if (position && ((position->flags & SWP_NOSIZE) == 0 || (position->flags & SWP_FRAMECHANGED) != 0))
            changed |= state_geometry;
        event.window_state.changed = changed;
        event.window_state.state = state | (changed & state_geometry);
        break;
    }
    case WM_SIZE:
        event.type = EventType::resize;
        event.resize.width_px = LOWORD(lparam);
        event.resize.height_px = HIWORD(lparam);
        event.resize.scale_120 = scale_of_dpi(GetDpiForWindow(window));
        break;
    case WM_ENTERSIZEMOVE:
        event.type = EventType::resize_begin;
        break;
    case WM_EXITSIZEMOVE:
        event.type = EventType::resize_end;
        break;
    case WM_DPICHANGED: {
        event.type = EventType::scale_changed;
        event.resize.scale_120 = scale_of_dpi(HIWORD(wparam));
        if (const auto* suggested = reinterpret_cast<const RECT*>(lparam)) {
            event.resize.width_px = static_cast<u32>(suggested->right - suggested->left);
            event.resize.height_px = static_cast<u32>(suggested->bottom - suggested->top);
        }
        break;
    }
    case WM_GETMINMAXINFO:
        event.type = EventType::size_limits;
        break;
    case WM_PAINT:
        event.type = EventType::paint;
        break;
    case WM_CLOSE:
        event.type = EventType::close_requested;
        break;
    case WM_DESTROY:
        event.type = EventType::destroyed;
        break;
    case WM_DROPFILES:
        event.type = EventType::drop_files;
        event.drop.files = reinterpret_cast<DroppedFiles*>(wparam);
        break;
    case WM_SETTINGCHANGE:
        event.type = EventType::settings_changed;
        // The colour scheme names itself; every other setting may touch the pointer, caret and motion ones.
        event.settings.areas = settings_area_is(lparam, L"ImmersiveColorSet")
                                   ? static_cast<u32>(settings_scheme)
                                   : static_cast<u32>(settings_pointer | settings_caret | settings_motion);
        break;
    case WM_NULL:
        event.type = EventType::wake;
        event.wake.cause = wake_poke;
        break;
    case kBeginWebAuthnMessage:
    case kPhoneVerifyMessage:
    case kNotificationMessage:
        event.type = EventType::wake;
        event.wake.cause = message == kBeginWebAuthnMessage ? wake_begin_webauthn
                           : message == kPhoneVerifyMessage ? wake_phone_verify
                                                            : wake_notifications;
        event.wake.a = wparam;
        event.wake.b = static_cast<u64>(lparam);
        break;
    default:
        break;
    }
    // The press and release times (one system call each) count multi-clicks; no other event's time is read.
    if (event.type == EventType::pointer_down || event.type == EventType::pointer_up)
        event.time_ms = static_cast<u32>(GetMessageTime());
    return event.type != EventType::none;
}

} // namespace maicord
