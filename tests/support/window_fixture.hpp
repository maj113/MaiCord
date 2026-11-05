#pragma once

// The window the application's handlers see in tests, without a desktop: the headless backend
// (include/maicord/platform/window/null.hpp) behind the window service, plus builders for the events a handler
// takes. Link maicord_window_null before maicord_platform.
//
// The null window keeps what the system keeps (capture, focus, the cursor shape, key state, the pointer) and
// reports a lost capture to the sink from inside the call that lost it, as Windows does; the sink here counts them.
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/null.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord::window_test {

// What the fixture's sink saw.
struct Observed {
    u32 events;
    u32 capture_lost;  // events of type capture_lost (a release or a take-over)
    u32 capture_lost_self;  // of those, naming the window itself as the new owner
    EventType last;
};
inline Observed g_observed{};
inline Window g_window{};

inline void sink(void*, const WindowEvent& event, WindowEventResult*) noexcept {
    ++g_observed.events;
    g_observed.last = event.type;
    if (event.type == EventType::capture_lost) {
        ++g_observed.capture_lost;
        if (event.focus.other == g_window)
            ++g_observed.capture_lost_self;
    }
}

// Opens the headless window: key, pointer and capture state start empty. The handlers under test see it through
// g_app.window (assign it) and the service. `routing` replaces the counting sink for tests that route events the
// way the client's window procedure does.
inline Window open(WindowSink routing = sink, void* context = nullptr) noexcept {
    null_window_reset();
    g_observed = {};
    g_window = {};
    window_create({}, routing, context, &g_window);
    return g_window;
}

inline void close() noexcept {
    if (g_window)
        window_destroy(g_window);
    g_window = {};
}

// Holds a modifier key (the generic code and its left variant, as a keyboard reports it) in the key table.
inline void hold(Key key_code) noexcept {
    null_window_set_key(key_code, true);
    if (key_code == key::ctrl)
        null_window_set_key(key::left_ctrl, true);
    else if (key_code == key::shift)
        null_window_set_key(key::left_shift, true);
    else if (key_code == key::alt)
        null_window_set_key(key::left_alt, true);
}

// A client point packed into one value (two signed words), for tests that still keep their positions that way.
inline i64 pack(float x, float y) noexcept {
    return static_cast<i64>(static_cast<u32>(static_cast<u16>(static_cast<i16>(x))) |
                            (static_cast<u32>(static_cast<u16>(static_cast<i16>(y))) << 16));
}

// The signed words of a packed client lparam, for tests that still compute their positions that way.
inline float x_of(i64 packed) noexcept {
    return static_cast<float>(static_cast<i16>(static_cast<u16>(static_cast<u64>(packed) & 0xFFFF)));
}
inline float y_of(i64 packed) noexcept {
    return static_cast<float>(static_cast<i16>(static_cast<u16>((static_cast<u64>(packed) >> 16) & 0xFFFF)));
}

constexpr u8 button_bit(PointerButton button) noexcept {
    return static_cast<u8>(1u << static_cast<u8>(button));
}

// Pointer events. Positions are content pixels, as handlers receive them; `buttons` are the held ones.
inline WindowEvent pointer_move(float x, float y, u8 buttons = 0, u8 modifiers = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::pointer_move;
    event.pointer = {x, y, buttons};
    event.modifiers = modifiers;
    return event;
}

inline WindowEvent pointer_down(float x, float y, PointerButton button = PointerButton::left, u8 flags = 0,
                                u8 modifiers = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::pointer_down;
    event.button = static_cast<u8>(button);
    event.flags = flags;
    event.modifiers = modifiers;
    event.pointer = {x, y, button_bit(button)};
    return event;
}

// The second press of a quick pair, as Windows delivers it.
inline WindowEvent pointer_double(float x, float y, PointerButton button = PointerButton::left,
                                  u8 modifiers = 0) noexcept {
    return pointer_down(x, y, button, ev_double_click, modifiers);
}

inline WindowEvent pointer_up(float x, float y, PointerButton button = PointerButton::left, u8 modifiers = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::pointer_up;
    event.button = static_cast<u8>(button);
    event.modifiers = modifiers;
    event.pointer = {x, y, 0};
    return event;
}

inline WindowEvent pointer_leave() noexcept {
    WindowEvent event{};
    event.type = EventType::pointer_leave;
    return event;
}

// One vertical notch is +-120; a horizontal one fills dx120 only.
inline WindowEvent wheel(float x, float y, i32 dy120, i32 dx120 = 0, u8 modifiers = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::wheel;
    event.modifiers = modifiers;
    event.wheel.x = x;
    event.wheel.y = y;
    event.wheel.dy120 = dy120;
    event.wheel.dx120 = dx120;
    return event;
}

inline WindowEvent cursor_query(float x, float y) noexcept {
    WindowEvent event{};
    event.type = EventType::cursor_query;
    event.pointer = {x, y, 0};
    return event;
}

inline WindowEvent context_menu(float x, float y) noexcept {
    WindowEvent event{};
    event.type = EventType::context_menu;
    event.pointer = {x, y, 0};
    return event;
}

inline WindowEvent context_menu_key() noexcept {
    WindowEvent event{};
    event.type = EventType::context_menu;
    event.flags = ev_keyboard;
    event.pointer = {-1.0f, -1.0f, 0};
    return event;
}

// Capture and focus. `other` is the window taking the capture or the focus (null when none does).
inline WindowEvent capture_lost(NativeWindow other = nullptr) noexcept {
    WindowEvent event{};
    event.type = EventType::capture_lost;
    event.focus.other = other;
    return event;
}

inline WindowEvent cancel_input() noexcept {
    WindowEvent event{};
    event.type = EventType::cancel_input;
    return event;
}

inline WindowEvent focus_changed(bool gained, NativeWindow other = nullptr) noexcept {
    WindowEvent event{};
    event.type = EventType::focus_changed;
    event.focus.gained = gained;
    event.focus.other = other;
    return event;
}

inline WindowEvent activation_changed(bool gained, bool minimized = false) noexcept {
    WindowEvent event{};
    event.type = EventType::activation_changed;
    event.focus.gained = gained;
    event.flags = minimized ? ev_minimized : 0;
    return event;
}

// Keys: `flags` is ev_repeat and/or ev_system; modifiers are what the key event carried.
inline WindowEvent key_down(u32 key_code, u8 flags = 0, u8 modifiers = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::key_down;
    event.key.key = key_code;
    event.flags = flags;
    event.modifiers = modifiers;
    return event;
}

inline WindowEvent key_up(u32 key_code, u8 flags = 0, u8 modifiers = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::key_up;
    event.key.key = key_code;
    event.flags = flags;
    event.modifiers = modifiers;
    return event;
}

inline WindowEvent character(u32 unit, u8 flags = 0) noexcept {
    WindowEvent event{};
    event.type = EventType::character;
    event.character.unit = unit;
    event.flags = flags;
    return event;
}

// Data. `files` is the platform's drop handle (an HDROP on Windows), which the handler takes over.
inline WindowEvent drop_files(void* files) noexcept {
    WindowEvent event{};
    event.type = EventType::drop_files;
    event.drop.files = static_cast<DroppedFiles*>(files);
    return event;
}

// Size and state.
inline WindowEvent resize(u32 width_px, u32 height_px, u32 scale_120 = 120) noexcept {
    WindowEvent event{};
    event.type = EventType::resize;
    event.resize = {width_px, height_px, scale_120};
    return event;
}

inline WindowEvent scale_changed(u32 scale_120) noexcept {
    WindowEvent event{};
    event.type = EventType::scale_changed;
    event.resize.scale_120 = scale_120;
    return event;
}

inline WindowEvent plain(EventType type) noexcept {
    WindowEvent event{};
    event.type = type;
    return event;
}

} // namespace maicord::window_test
