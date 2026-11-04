#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/platform/window/wayland/xkb.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "wayland_protocols.hpp"

// A second connection to the compositor that plays a pointer and a keyboard (Linux, tests only): the wlroots
// virtual-pointer and virtual-keyboard protocols, which sway and the other wlroots compositors serve. The window
// under test is a different client on its own connection, so its events come back through the compositor exactly as
// a real device's do. Everything is synchronous: a call returns after the compositor has taken the request.

namespace maicord::inject {

using namespace wayland;

struct Injector {
    wl_display* display;
    wl_registry* registry;
    wl_seat* seat;
    zwlr_virtual_pointer_manager_v1* pointer_manager;
    zwp_virtual_keyboard_manager_v1* keyboard_manager;
    zwlr_virtual_pointer_v1* pointer;
    zwp_virtual_keyboard_v1* keyboard;
    u32 extent_x, extent_y;  // the compositor's layout size, for absolute pointer motion
    u32 time_ms;
    XkbContext* context;  // the keymap the compositor was given, and the state its keys have produced
    XkbState* state;
};

namespace detail {

inline bool same(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

inline Injector* g_target;

inline void on_global(void*, wl_registry* registry, u32 name, const char* interface, u32 version) noexcept {
    Injector& injector = *g_target;
    if (same(interface, "wl_seat") && !injector.seat)
        injector.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    else if (same(interface, "zwlr_virtual_pointer_manager_v1") && !injector.pointer_manager)
        injector.pointer_manager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2));
    else if (same(interface, "zwp_virtual_keyboard_manager_v1") && !injector.keyboard_manager)
        injector.keyboard_manager = static_cast<zwp_virtual_keyboard_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
}

inline void on_global_remove(void*, wl_registry*, u32) noexcept {}

inline const wl_registry_listener kListener = {on_global, on_global_remove};

} // namespace detail

// Connects and creates the devices; false when the compositor serves neither protocol (Weston, WSLg's Weston).
inline bool open(Injector* injector, u32 extent_x, u32 extent_y) noexcept {
    clear_bytes(injector, static_cast<u32>(sizeof(*injector)));
    if (!client_load())
        return false;
    injector->display = g_client.display_connect(nullptr);
    if (!injector->display)
        return false;
    injector->extent_x = extent_x;
    injector->extent_y = extent_y;
    injector->registry = wl_display_get_registry(injector->display);
    detail::g_target = injector;
    wl_registry_add_listener(injector->registry, &detail::kListener, nullptr);
    g_client.display_roundtrip(injector->display);
    if (!injector->seat || !injector->pointer_manager || !injector->keyboard_manager)
        return false;
    injector->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(injector->pointer_manager, injector->seat);
    injector->keyboard =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(injector->keyboard_manager, injector->seat);
    g_client.display_roundtrip(injector->display);
    return injector->pointer && injector->keyboard;
}

inline void sync(Injector* injector) noexcept {
    g_client.display_roundtrip(injector->display);
}

inline u32 now(Injector* injector) noexcept {
    // One tick per request keeps the compositor's event times strictly ordered even inside one millisecond.
    const u32 clock = static_cast<u32>(clock_monotonic_milliseconds());
    injector->time_ms = clock > injector->time_ms ? clock : injector->time_ms + 1;
    return injector->time_ms;
}

inline void close(Injector* injector) noexcept {
    if (injector->pointer)
        zwlr_virtual_pointer_v1_destroy(injector->pointer);
    if (injector->keyboard)
        zwp_virtual_keyboard_v1_destroy(injector->keyboard);
    if (injector->pointer_manager)
        zwlr_virtual_pointer_manager_v1_destroy(injector->pointer_manager);
    if (injector->keyboard_manager)
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(injector->keyboard_manager));
    if (injector->seat)
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(injector->seat));
    if (injector->registry)
        wl_registry_destroy(injector->registry);
    if (const XkbApi* api = xkb_api()) {
        if (injector->state)
            api->state_unref(injector->state);
        if (injector->context)
            api->context_unref(injector->context);
    }
    if (injector->display) {
        g_client.display_roundtrip(injector->display);
        g_client.display_disconnect(injector->display);
    }
    clear_bytes(injector, static_cast<u32>(sizeof(*injector)));
}

// The pointer, in layout pixels.
inline void pointer_move_to(Injector* injector, u32 x, u32 y) noexcept {
    zwlr_virtual_pointer_v1_motion_absolute(injector->pointer, now(injector), x, y, injector->extent_x,
                                            injector->extent_y);
    zwlr_virtual_pointer_v1_frame(injector->pointer);
    sync(injector);
}

// A Linux button code (BTN_LEFT 0x110 ...).
inline void pointer_button(Injector* injector, u32 code, bool pressed) noexcept {
    zwlr_virtual_pointer_v1_button(injector->pointer, now(injector), code,
                                   pressed ? wl_pointer_button_state_pressed : wl_pointer_button_state_released);
    zwlr_virtual_pointer_v1_frame(injector->pointer);
    sync(injector);
}

// A wheel notch: `notches` > 0 scrolls down (or right, horizontal), 15 units a notch.
inline void pointer_notch(Injector* injector, bool horizontal, i32 notches) noexcept {
    zwlr_virtual_pointer_v1_axis_source(injector->pointer, wl_pointer_axis_source_wheel);
    zwlr_virtual_pointer_v1_axis_discrete(injector->pointer, now(injector),
                                          horizontal ? wl_pointer_axis_horizontal_scroll : wl_pointer_axis_vertical_scroll,
                                          notches * 15 * 256, notches);
    zwlr_virtual_pointer_v1_frame(injector->pointer);
    sync(injector);
}

// Continuous (touchpad) scrolling by `units` (positive down or right).
inline void pointer_scroll(Injector* injector, bool horizontal, i32 units) noexcept {
    zwlr_virtual_pointer_v1_axis_source(injector->pointer, wl_pointer_axis_source_finger);
    zwlr_virtual_pointer_v1_axis(injector->pointer, now(injector),
                                 horizontal ? wl_pointer_axis_horizontal_scroll : wl_pointer_axis_vertical_scroll,
                                 units * 256);
    zwlr_virtual_pointer_v1_frame(injector->pointer);
    sync(injector);
}

// The keyboard. The compositor serves `text` (an XKB keymap, NUL terminated or not) to every client from now on.
inline bool keyboard_set_keymap(Injector* injector, const char* text, u32 length) noexcept {
    constexpr long kSysMemfdCreate = 319;
    constexpr long kSysFtruncate = 77;
    const long fd = syscall2(kSysMemfdCreate, reinterpret_cast<long>("maicord-keymap"), 1);
    if (syscall_failed(fd))
        return false;
    const u32 size = length + 1;
    bool ok = !syscall_failed(syscall2(kSysFtruncate, fd, size));
    if (ok) {
        const long address = syscall6(kSysMmap, 0, size, kProtectRead | kProtectWrite, 0x01, fd, 0);
        ok = !syscall_failed(address);
        if (ok) {
            copy_bytes(reinterpret_cast<void*>(address), text, length);
            static_cast<char*>(reinterpret_cast<void*>(address))[length] = 0;
            sys_munmap(reinterpret_cast<void*>(address), size);
        }
    }
    if (ok) {
        zwp_virtual_keyboard_v1_keymap(injector->keyboard, wl_keyboard_keymap_format_xkb_v1, static_cast<i32>(fd), size);
        sync(injector);
        // The injector's own copy of the keymap and its state.
        const XkbApi* api = xkb_api();
        if (api) {
            if (!injector->context)
                injector->context = api->context_new(0);
            if (injector->context) {
                api->context_set_log_level(injector->context, kXkbLogLevelCritical);
                XkbKeymap* keymap =
                    api->keymap_new_from_buffer(injector->context, text, length, kXkbKeymapFormatTextV1, 0);
                if (injector->state)
                    api->state_unref(injector->state);
                injector->state = keymap ? api->state_new(keymap) : nullptr;
                if (keymap)
                    api->keymap_unref(keymap);
            }
        }
    }
    sys_close(static_cast<int>(fd));
    return ok;
}

// An evdev key code. A physical keyboard's modifier state follows its keys; a virtual one has to say so, so the
// injector tracks the keymap's state and reports it after every key.
inline void keyboard_key(Injector* injector, u32 evdev, bool pressed) noexcept {
    zwp_virtual_keyboard_v1_key(injector->keyboard, now(injector), evdev,
                                pressed ? wl_keyboard_key_state_pressed : wl_keyboard_key_state_released);
    const XkbApi* api = xkb_api();
    if (api && injector->state) {
        api->state_update_key(injector->state, evdev + kEvdevToXkb, pressed ? kXkbKeyDown : kXkbKeyUp);
        zwp_virtual_keyboard_v1_modifiers(injector->keyboard, api->state_serialize_mods(injector->state, kXkbStateDepressed),
                                          api->state_serialize_mods(injector->state, kXkbStateLatched),
                                          api->state_serialize_mods(injector->state, kXkbStateModsLocked),
                                          api->state_serialize_layout(injector->state, kXkbStateModsLocked));
    }
    sync(injector);
}

} // namespace maicord::inject
