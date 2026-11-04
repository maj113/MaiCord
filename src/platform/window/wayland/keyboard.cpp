// wl_keyboard and libxkbcommon to WindowEvents: the keymap, key codes (the Windows virtual-key numbering, D14),
// characters with compose, client-side repeat on a timerfd, held-key tracking and focus.
#include "maicord/core/memory.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/platform/window/wayland/keymap.hpp"
#include "maicord/sys/linux/process.hpp"
#include "backend.hpp"
#include "fd.hpp"

namespace maicord::wayland {
namespace {

constexpr i32 kDefaultRate = 25;
constexpr i32 kDefaultDelay = 600;
constexpr u32 kMaxEvdev = 768;

bool is_modifier_key(Key key) noexcept {
    return key == key::shift || key == key::ctrl || key == key::alt || key == key::right_alt ||
           key == key::left_super || key == key::right_super || key == key::caps_lock || key == key::num_lock ||
           key == key::scroll_lock;
}

// The locale compose tables are read for: LC_ALL, LC_CTYPE, LANG, else "C".
const char* compose_locale() noexcept {
    const char* const names[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (const char* name : names) {
        const char* value = environment_get(name);
        if (value && *value)
            return value;
    }
    return "C";
}

u8 compute_modifiers() noexcept {
    KeyboardState& keyboard = g.keyboard;
    const XkbApi* api = xkb_api();
    if (!api || !keyboard.state)
        return keyboard.altgr_count ? mod_altgr : 0;
    const auto active = [&](const char* name) {
        return api->state_mod_name_is_active(keyboard.state, name, kXkbStateModsEffective) > 0;
    };
    u8 mods = 0;
    if (active("Control"))
        mods |= mod_ctrl;
    if (active("Mod1"))
        mods |= mod_alt;
    if (active("Shift"))
        mods |= mod_shift;
    if (active("Mod4"))
        mods |= mod_super;
    if (active("Lock"))
        mods |= mod_caps;
    if (active("Mod2"))
        mods |= mod_num;
    if (keyboard.altgr_count)
        mods |= mod_altgr;
    return mods;
}

// The portable key of an evdev key under the active layout and the keysym it produces now.
Key map_key(u32 evdev, u32* active_symbol) noexcept {
    return xkb_key_for(*xkb_api(), g.keyboard.keymap, g.keyboard.state, evdev, active_symbol);
}

// Decodes the first code point of UTF-8 text.
u32 first_code_point(const char* text) noexcept {
    const u8* bytes = reinterpret_cast<const u8*>(text);
    if (bytes[0] < 0x80)
        return bytes[0];
    u32 length = bytes[0] >= 0xF0 ? 4 : bytes[0] >= 0xE0 ? 3 : bytes[0] >= 0xC0 ? 2 : 0;
    if (length == 0)
        return 0;
    u32 value = bytes[0] & (0xFFu >> (length + 1));
    for (u32 i = 1; i < length; ++i) {
        if ((bytes[i] & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (bytes[i] & 0x3Fu);
    }
    return value;
}

u8 event_flags(Key key, u8 mods) noexcept {
    const bool system = ((mods & mod_alt) && !(mods & mod_ctrl)) || key == key::alt || key == key::f10;
    return system ? ev_system : 0;
}

void push_key(EventType type, Key key, u32 evdev, u32 time, u8 extra_flags) noexcept {
    KeyboardState& keyboard = g.keyboard;
    WindowEvent event = make_event(type, time);
    event.modifiers = keyboard.modifiers;
    event.flags = static_cast<u8>(event_flags(key, keyboard.modifiers) | extra_flags);
    event.key.key = key;
    event.key.scancode = evdev;
    ring_push(event);
}

void push_character(u32 code_point, u32 time, u8 flags) noexcept {
    if (!is_message_character(code_point))
        return;
    u16 units[2];
    const u32 count = utf16_units(code_point, units);
    for (u32 i = 0; i < count; ++i) {
        WindowEvent event = make_event(EventType::character, time);
        event.modifiers = g.keyboard.modifiers;
        event.flags = flags;
        event.character.unit = units[i];
        ring_push(event);
    }
}

// `altgr`: the key is an ISO_Level3_Shift (AltGr). xkbcommon keeps the level-three modifier virtual, whose bit a
// state's masks do not carry, so the held AltGr keys are counted and mod_altgr follows them.
void set_down(u32 evdev, Key key, bool down, bool altgr = false) noexcept {
    KeyboardState& keyboard = g.keyboard;
    if (evdev >= kMaxEvdev)
        return;
    Key specific = key::none;
    if (down) {
        keyboard.down_key[evdev] = key;
        specific = (key == key::shift || key == key::ctrl || key == key::alt) ? specific_modifier_key(evdev) : key::none;
        keyboard.down_specific[evdev] = specific;
        keyboard.down_altgr[evdev] = altgr;
        keyboard.altgr_count = static_cast<u8>(keyboard.altgr_count + (altgr ? 1 : 0));
        if (keyboard.held_count[key] != 0xFF)
            ++keyboard.held_count[key];
        if (specific && keyboard.held_count[specific] != 0xFF)
            ++keyboard.held_count[specific];
    } else {
        specific = keyboard.down_specific[evdev];
        keyboard.down_key[evdev] = key::none;
        keyboard.down_specific[evdev] = key::none;
        if (keyboard.down_altgr[evdev] && keyboard.altgr_count)
            --keyboard.altgr_count;
        keyboard.down_altgr[evdev] = false;
        if (keyboard.held_count[key])
            --keyboard.held_count[key];
        if (specific && keyboard.held_count[specific])
            --keyboard.held_count[specific];
    }
    keyboard.modifiers = static_cast<u8>((keyboard.modifiers & ~mod_altgr) | (keyboard.altgr_count ? mod_altgr : 0));
    global_keys_set(key, keyboard.held_count[key] != 0);
    if (specific)
        global_keys_set(specific, keyboard.held_count[specific] != 0);
}

// The characters of a key press: through compose when the keymap's locale has a table, else the keysym's own.
// Returns the code point, or 0 for none; *plain says the press can repeat its text.
u32 press_text(u32 evdev, u32 active_symbol, bool* plain) noexcept {
    KeyboardState& keyboard = g.keyboard;
    const XkbApi* api = xkb_api();
    *plain = true;
    if (keyboard.compose_state && !is_modifier_key(key_from_fixed_keysym(active_symbol))) {
        if (api->compose_state_feed(keyboard.compose_state, active_symbol) == kXkbComposeFeedAccepted) {
            const int status = api->compose_state_get_status(keyboard.compose_state);
            if (status == kXkbComposeStatusComposing) {
                *plain = false;
                return 0;
            }
            if (status == kXkbComposeStatusComposed) {
                char text[16];
                *plain = false;
                const int length = api->compose_state_get_utf8(keyboard.compose_state, text, sizeof(text));
                api->compose_state_reset(keyboard.compose_state);
                return length > 0 ? first_code_point(text) : 0;
            }
            if (status == kXkbComposeStatusCancelled) {
                *plain = false;
                api->compose_state_reset(keyboard.compose_state);
                return 0;
            }
        }
    }
    return api->state_key_get_utf32(keyboard.state, evdev + kEvdevToXkb);
}

void arm_repeat(u32 evdev, u64 now_ms) noexcept {
    KeyboardState& keyboard = g.keyboard;
    const XkbApi* api = xkb_api();
    if (keyboard.repeat_rate <= 0 || !api->keymap_key_repeats(keyboard.keymap, evdev + kEvdevToXkb)) {
        key_repeat_stop(&keyboard.repeat);
        return;
    }
    key_repeat_start(&keyboard.repeat, evdev, now_ms, static_cast<u32>(keyboard.repeat_delay),
                     static_cast<u32>(keyboard.repeat_rate));
}

void on_keymap(void*, wl_keyboard*, u32 format, i32 fd, u32 size) noexcept {
    KeyboardState& keyboard = g.keyboard;
    const XkbApi* api = xkb_api();
    const long address = (api && format == wl_keyboard_keymap_format_xkb_v1 && size > 0)
                             ? fd_map(fd, size, kProtectRead, kMapPrivate)
                             : -1;
    if (!syscall_failed(address)) {
        if (!keyboard.context) {
            keyboard.context = api->context_new(0);
            if (keyboard.context)
                api->context_set_log_level(keyboard.context, kXkbLogLevelCritical);
        }
        const char* text = reinterpret_cast<const char*>(address);
        u32 length = size;
        if (length > 0 && text[length - 1] == 0)
            --length;
        XkbKeymap* keymap = keyboard.context ? api->keymap_new_from_buffer(keyboard.context, text, length,
                                                                          kXkbKeymapFormatTextV1, 0)
                                             : nullptr;
        XkbState* state = keymap ? api->state_new(keymap) : nullptr;
        if (state) {
            // The old layout's keys mean nothing under the new one: stop what repeats and what composes.
            key_repeat_stop(&keyboard.repeat);
            if (keyboard.compose_state)
                api->compose_state_reset(keyboard.compose_state);
            if (keyboard.state)
                api->state_unref(keyboard.state);
            if (keyboard.keymap)
                api->keymap_unref(keyboard.keymap);
            keyboard.keymap = keymap;
            keyboard.state = state;
            keyboard.level_three_modifier = api->keymap_mod_get_index(keymap, "LevelThree");
            if (keyboard.level_three_modifier == kXkbModifierInvalid)
                keyboard.level_three_modifier = api->keymap_mod_get_index(keymap, "Mod5");
            keyboard.modifiers = 0;
            if (!keyboard.compose_tried) {
                keyboard.compose_tried = true;
                keyboard.compose_table = api->compose_table_new_from_locale(keyboard.context, compose_locale(), 0);
                if (keyboard.compose_table)
                    keyboard.compose_state = api->compose_state_new(keyboard.compose_table, 0);
            }
        } else if (keymap) {
            api->keymap_unref(keymap);
        }
        sys_munmap(reinterpret_cast<void*>(address), size);
    }
    sys_close(fd);
}

void on_enter(void*, wl_keyboard*, u32 serial, wl_surface*, Array* keys) noexcept {
    KeyboardState& keyboard = g.keyboard;
    keyboard.focused = true;
    keyboard.serial = serial;
    g.input_serial = serial;
    // Keys already down when focus arrives become held state; they raise no key_down.
    if (keyboard.state && keys && keys->data) {
        const u32* held = static_cast<const u32*>(keys->data);
        for (usize i = 0; i < keys->size / sizeof(u32); ++i) {
            const u32 evdev = held[i];
            if (evdev >= kMaxEvdev || keyboard.down_key[evdev])
                continue;
            u32 active = 0;
            const Key key = map_key(evdev, &active);
            if (key != key::none)
                set_down(evdev, key, true, active == keysym::iso_level3_shift);
        }
    }
    WindowEvent event = make_event(EventType::focus_changed, now_ms32());
    event.focus.gained = true;
    ring_push(event);
}

void on_leave(void*, wl_keyboard*, u32 serial, wl_surface*) noexcept {
    KeyboardState& keyboard = g.keyboard;
    keyboard.serial = serial;
    key_repeat_stop(&keyboard.repeat);
    if (keyboard.compose_state)
        xkb_api()->compose_state_reset(keyboard.compose_state);
    // The compositor sends no releases for keys that were down: the app's held-key tails would never end.
    const u32 time = now_ms32();
    for (u32 evdev = 0; evdev < kMaxEvdev; ++evdev) {
        const Key key = keyboard.down_key[evdev];
        if (!key)
            continue;
        set_down(evdev, key, false);
        push_key(EventType::key_up, key, evdev, time, ev_synthetic);
    }
    global_keys_clear();
    keyboard.focused = false;
    WindowEvent event = make_event(EventType::focus_changed, time);
    event.focus.gained = false;
    ring_push(event);
}

void on_key(void*, wl_keyboard*, u32 serial, u32 time, u32 evdev, u32 state_value) noexcept {
    KeyboardState& keyboard = g.keyboard;
    keyboard.serial = serial;
    g.input_serial = serial;
    if (!keyboard.state || evdev >= kMaxEvdev)
        return;
    if (state_value != wl_keyboard_key_state_pressed) {
        const Key key = keyboard.down_key[evdev];
        if (!key)
            return;
        set_down(evdev, key, false);
        key_repeat_release(&keyboard.repeat, evdev);
        push_key(EventType::key_up, key, evdev, time, 0);
        return;
    }
    u32 active = 0;
    const Key key = map_key(evdev, &active);
    if (keyboard.down_key[evdev])
        return;  // already down from before focus: not a new press
    if (key != key::none) {
        set_down(evdev, key, true, active == keysym::iso_level3_shift);
        push_key(EventType::key_down, key, evdev, time, 0);
    }
    bool plain = true;
    const u32 code_point = press_text(evdev, active, &plain);
    if (code_point != 0 && !(keyboard.modifiers & mod_super))
        push_character(code_point, time, event_flags(key, keyboard.modifiers));
    // A modifier or a lock does not interrupt what repeats; any other key replaces it.
    if (is_modifier_key(key))
        return;
    key_repeat_stop(&keyboard.repeat);
    if (key == key::none || !plain)
        return;
    arm_repeat(evdev, clock_monotonic_milliseconds());
}

void on_modifiers(void*, wl_keyboard*, u32 serial, u32 depressed, u32 latched, u32 locked, u32 group) noexcept {
    KeyboardState& keyboard = g.keyboard;
    keyboard.serial = serial;
    if (!keyboard.state)
        return;
    keyboard.group = group;
    xkb_api()->state_update_mask(keyboard.state, depressed, latched, locked, 0, 0, group);
    keyboard.modifiers = compute_modifiers();
}

void on_repeat_info(void*, wl_keyboard*, i32 rate, i32 delay) noexcept {
    g.keyboard.repeat_rate = rate;
    g.keyboard.repeat_delay = delay;
    if (rate <= 0)
        key_repeat_stop(&g.keyboard.repeat);
}

const wl_keyboard_listener kKeyboardListener = {on_keymap, on_enter, on_leave, on_key, on_modifiers, on_repeat_info};

void release_keyboard(wl_keyboard* keyboard) noexcept {
    if (g.seat_version >= wl_keyboard_release_since)
        wl_keyboard_release(keyboard);
    else
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(keyboard));
}

} // namespace

void keyboard_attach(wl_seat* seat) noexcept {
    KeyboardState& keyboard = g.keyboard;
    if (keyboard.keyboard || !xkb_api())
        return;
    keyboard.keyboard = wl_seat_get_keyboard(seat);
    keyboard.repeat_rate = kDefaultRate;
    keyboard.repeat_delay = kDefaultDelay;
    wl_keyboard_add_listener(keyboard.keyboard, &kKeyboardListener, nullptr);
}

void keyboard_detach() noexcept {
    KeyboardState& keyboard = g.keyboard;
    if (!keyboard.keyboard)
        return;
    release_keyboard(keyboard.keyboard);
    keyboard.keyboard = nullptr;
    key_repeat_stop(&keyboard.repeat);
    keyboard.focused = false;
}

void keyboard_shutdown() noexcept {
    KeyboardState& keyboard = g.keyboard;
    const XkbApi* api = xkb_api();
    keyboard_detach();
    if (api) {
        if (keyboard.compose_state)
            api->compose_state_unref(keyboard.compose_state);
        if (keyboard.compose_table)
            api->compose_table_unref(keyboard.compose_table);
        if (keyboard.state)
            api->state_unref(keyboard.state);
        if (keyboard.keymap)
            api->keymap_unref(keyboard.keymap);
        if (keyboard.context)
            api->context_unref(keyboard.context);
    }
    waitable_close(keyboard.repeat_timer);
    global_keys_clear();
    clear_bytes(&keyboard, static_cast<u32>(sizeof(keyboard)));
}

Waitable keyboard_repeat_waitable() noexcept {
    if (!g.keyboard.repeat_timer)
        g.keyboard.repeat_timer = timer_create_waitable(false, false);
    return g.keyboard.repeat_timer;
}

u32 keyboard_repeat_wait_ms(u64 now_ms) noexcept {
    return key_repeat_wait_ms(g.keyboard.repeat, now_ms);
}

void keyboard_poll_repeat(u64 now_ms) noexcept {
    KeyboardState& keyboard = g.keyboard;
    if (!key_repeat_fire(&keyboard.repeat, now_ms))
        return;
    const u32 evdev = keyboard.repeat.token;
    if (!keyboard.state || evdev >= kMaxEvdev || !keyboard.down_key[evdev]) {
        key_repeat_stop(&keyboard.repeat);
        return;
    }
    // The repeat is a new press as the app sees it, with the key and text the layout gives now.
    const Key key = keyboard.down_key[evdev];
    const u32 time = now_ms32();
    push_key(EventType::key_down, key, evdev, time, ev_repeat);
    const u32 code_point = xkb_api()->state_key_get_utf32(keyboard.state, evdev + kEvdevToXkb);
    if (code_point != 0 && !(keyboard.modifiers & mod_super))
        push_character(code_point, time, static_cast<u8>(event_flags(key, keyboard.modifiers) | ev_repeat));
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

u8 window_modifiers(Window) noexcept {
    return g.keyboard.modifiers;
}

bool window_key_down(Window, Key key_code) noexcept {
    const u8 buttons = g.pointer.app_buttons;
    switch (key_code) {
    case key::lbutton:
        return (buttons & button_bit(PointerButton::left)) != 0;
    case key::rbutton:
        return (buttons & button_bit(PointerButton::right)) != 0;
    case key::mbutton:
        return (buttons & button_bit(PointerButton::middle)) != 0;
    case key::xbutton1:
        return (buttons & button_bit(PointerButton::back)) != 0;
    case key::xbutton2:
        return (buttons & button_bit(PointerButton::forward)) != 0;
    default:
        return g.keyboard.held_count[key_code] != 0;
    }
}

bool window_toggle_state(Window, Key key_code) noexcept {
    const XkbApi* api = xkb_api();
    if (!api || !g.keyboard.state)
        return false;
    const char* name = key_code == key::caps_lock ? "Lock" : key_code == key::num_lock ? "Mod2" : nullptr;
    return name && api->state_mod_name_is_active(g.keyboard.state, name, kXkbStateModsLocked) > 0;
}

bool window_layout_types_text(Window, Key wanted, u8 modifiers) noexcept {
    KeyboardState& keyboard = g.keyboard;
    const XkbApi* api = xkb_api();
    if (!api || !keyboard.keymap || !keyboard.state)
        return false;
    XkbState* probe = api->state_new(keyboard.keymap);
    if (!probe)
        return false;
    u32 mask = 0;
    const auto add = [&](const char* name) {
        const u32 index = api->keymap_mod_get_index(keyboard.keymap, name);
        if (index != kXkbModifierInvalid)
            mask |= 1u << index;
    };
    if (modifiers & mod_shift)
        add("Shift");
    if (modifiers & mod_ctrl)
        add("Control");
    if (modifiers & mod_alt)
        add("Mod1");
    if (modifiers & mod_super)
        add("Mod4");
    if (modifiers & mod_altgr) {
        add("Mod5");  // what the virtual level-three modifier maps to in every standard keymap
        if (keyboard.level_three_modifier != kXkbModifierInvalid)
            mask |= 1u << keyboard.level_three_modifier;
    }
    api->state_update_mask(probe, mask, 0, 0, 0, 0, keyboard.group);
    bool types = false;
    for (u32 evdev = 1; evdev < 256 && !types; ++evdev) {
        if (map_key(evdev, nullptr) != wanted)
            continue;
        const u32 code_point = api->state_key_get_utf32(probe, evdev + kEvdevToXkb);
        types = code_point >= 0x20 && code_point != 0x7F;
    }
    api->state_unref(probe);
    return types;
}

} // namespace maicord
