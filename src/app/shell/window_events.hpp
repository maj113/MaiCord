#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/dropped_files.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/ui/text/secure_text_store.hpp"

namespace maicord::app_detail {

// Window events as the handlers read them. Coordinates of pointer, wheel, cursor and context-menu events reach the
// handlers in content pixels (the dispatcher takes the caption's rows off the client point once).

constexpr u8 input_button_bit(PointerButton button) noexcept {
    return static_cast<u8>(1u << static_cast<u8>(button));
}

// A press of `button`. The second press of a quick pair arrives as a double press instead (input_double), as
// WM_xBUTTONDOWN and WM_xBUTTONDBLCLK did.
inline bool input_press(const WindowEvent& event, PointerButton button = PointerButton::left) noexcept {
    return event.type == EventType::pointer_down && event.button == static_cast<u8>(button) &&
           (event.flags & ev_double_click) == 0;
}
inline bool input_double(const WindowEvent& event, PointerButton button = PointerButton::left) noexcept {
    return event.type == EventType::pointer_down && event.button == static_cast<u8>(button) &&
           (event.flags & ev_double_click) != 0;
}
// Either press: the click, whether or not it is the second of a pair.
inline bool input_down(const WindowEvent& event, PointerButton button = PointerButton::left) noexcept {
    return event.type == EventType::pointer_down && event.button == static_cast<u8>(button);
}
inline bool input_release(const WindowEvent& event, PointerButton button = PointerButton::left) noexcept {
    return event.type == EventType::pointer_up && event.button == static_cast<u8>(button);
}
inline bool input_move(const WindowEvent& event) noexcept {
    return event.type == EventType::pointer_move;
}
// Any pointer button message: moves, presses, releases and wheels, as the range WM_MOUSEFIRST..WM_MOUSELAST was.
inline bool input_pointer(const WindowEvent& event) noexcept {
    return event.type == EventType::pointer_move || event.type == EventType::pointer_down ||
           event.type == EventType::pointer_up || event.type == EventType::wheel;
}
// Whether `button` is held, for moves and presses (the MK_ flags of the message).
inline bool input_held(const WindowEvent& event, PointerButton button = PointerButton::left) noexcept {
    return (event.pointer.buttons & input_button_bit(button)) != 0;
}
inline bool input_shift(const WindowEvent& event) noexcept {
    return (event.modifiers & mod_shift) != 0;
}
inline bool input_control(const WindowEvent& event) noexcept {
    return (event.modifiers & mod_ctrl) != 0;
}
// Vertical and horizontal wheel notches (WM_MOUSEWHEEL, WM_MOUSEHWHEEL): a horizontal one carries no vertical
// delta.
inline bool input_horizontal_wheel(const WindowEvent& event) noexcept {
    return event.type == EventType::wheel && event.wheel.dy120 == 0 && event.wheel.dx120 != 0;
}
inline bool input_vertical_wheel(const WindowEvent& event) noexcept {
    return event.type == EventType::wheel && !input_horizontal_wheel(event);
}
// The left-button step of a pointer event, as modal layers route it: a move, a press (a second press of a pair is a
// press too) or a release; none for anything else.
enum class PointerStep : u8 { none, move, press, release };
inline PointerStep input_pointer_step(const WindowEvent& event) noexcept {
    if (event.type == EventType::pointer_move)
        return PointerStep::move;
    if (input_down(event))
        return PointerStep::press;
    return input_release(event) ? PointerStep::release : PointerStep::none;
}
// A right or middle button press or release (not a second press of a pair): what modal layers swallow.
inline bool input_side_button(const WindowEvent& event) noexcept {
    if (event.button != static_cast<u8>(PointerButton::right) && event.button != static_cast<u8>(PointerButton::middle))
        return false;
    return (event.type == EventType::pointer_down && (event.flags & ev_double_click) == 0) ||
           event.type == EventType::pointer_up;
}
// A press, second press or release of any button but the primary one.
inline bool input_other_button(const WindowEvent& event) noexcept {
    return (event.type == EventType::pointer_down || event.type == EventType::pointer_up) &&
           event.button != static_cast<u8>(PointerButton::left);
}
// The key of a WM_KEYDOWN (not a system key) or WM_SYSKEYDOWN.
inline bool input_key_down(const WindowEvent& event) noexcept {
    return event.type == EventType::key_down;
}
inline bool input_plain_key_down(const WindowEvent& event) noexcept {
    return event.type == EventType::key_down && (event.flags & ev_system) == 0;
}
inline bool input_plain_key_up(const WindowEvent& event) noexcept {
    return event.type == EventType::key_up && (event.flags & ev_system) == 0;
}
// WM_KILLFOCUS and WM_SETFOCUS.
inline bool input_focus_lost(const WindowEvent& event) noexcept {
    return event.type == EventType::focus_changed && !event.focus.gained;
}
inline bool input_repeated(const WindowEvent& event) noexcept {
    return (event.flags & ev_repeat) != 0;
}
// A WM_KEYDOWN or WM_KEYUP (not a system key).
inline bool input_plain_key(const WindowEvent& event) noexcept {
    return input_plain_key_down(event) || input_plain_key_up(event);
}
// The previous-state bit of a key message: set on auto-repeat and on every release.
inline bool input_key_was_down(const WindowEvent& event) noexcept {
    return event.type == EventType::key_up || input_repeated(event);
}

// The position of a pointer event packed as the lparam of a client mouse message, for the click handlers that still
// take one (they move to events with the core switch, and this goes with them).
inline i64 input_lparam(const WindowEvent& event) noexcept {
    return static_cast<i64>(static_cast<u32>(static_cast<u16>(static_cast<i16>(event.pointer.x))) |
                            (static_cast<u32>(static_cast<u16>(static_cast<i16>(event.pointer.y))) << 16));
}
// The DPI a scale_changed event or a resize names (the scale is 120 * dpi / 96, rounded down).
inline u32 input_dpi(const WindowEvent& event) noexcept {
    return (event.resize.scale_120 * 96u + 60u) / 120u;
}
// A cursor query carries a position unless the system could not tell where the pointer is.
inline bool input_has_position(const WindowEvent& event) noexcept {
    return event.pointer.x == event.pointer.x;
}
// Wheel notches of a wheel event; positive scrolls up/away from the user.
inline float input_wheel_notches(const WindowEvent& event) noexcept {
    return static_cast<float>(event.wheel.dy120) / 120.0f;
}

} // namespace maicord::app_detail
