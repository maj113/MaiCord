#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/key.hpp"

// Push-to-talk and other chords that fire while the window is in the background, through the
// org.freedesktop.portal.GlobalShortcuts portal (KDE Plasma, GNOME 48 and later, Hyprland): CreateSession, BindShortcuts
// and the Activated and Deactivated signals. Where the portal is missing the state is `unavailable`, silently, and the
// app keeps the focused-window key state of platform/window/global_keys.hpp (D19). The window backend feeds an activated
// shortcut into that key state (window_wayland_global_shortcut in platform/window/wayland.hpp).
//
// UI thread; events arrive inside the session bus processing (session.hpp).

namespace maicord::dbus {

struct GlobalShortcut {
    const char* id;                 // "push-to-talk": stable, reported back
    const char* description;        // what the user sees in the compositor's shortcut settings
    const char* preferred_trigger;  // an XDG shortcut string ("CTRL+ALT+p"); null or empty: the user binds it
};

enum class GlobalShortcutsState : u8 {
    unavailable,  // no portal, the user declined, or nothing was started
    starting,     // the session or the binding is being set up
    ready,        // bound: Activated and Deactivated arrive
};

using GlobalShortcutHandler = void (*)(void* context, const char* id, bool active, u64 timestamp_us) noexcept;

// Starts a session with these shortcuts (at most 8; the strings are copied). Restarts from scratch after a bus
// reconnect. False without a session bus.
bool global_shortcuts_start(const GlobalShortcut* shortcuts, u32 count, GlobalShortcutHandler handler,
                            void* context) noexcept;
GlobalShortcutsState global_shortcuts_state() noexcept;
void global_shortcuts_stop() noexcept;

// The XDG shortcut string for a key and the Modifier bits (mod_ctrl, mod_alt, mod_shift, mod_super) of key.hpp:
// "CTRL+ALT+SHIFT+LOGO+<keysym>". False for keys with no trigger form here (mouse buttons, the modifier keys
// themselves, unknown codes); `out` holds "" then.
bool global_shortcut_trigger(Key key, u8 modifiers, char* out, u32 capacity) noexcept;

} // namespace maicord::dbus
