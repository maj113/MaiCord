#pragma once

#include "maicord/core/base.hpp"

// The notification-area icon of Linux desktops: a StatusNotifierItem (org.kde.StatusNotifierItem) with its menu over
// com.canonical.dbusmenu, registered with org.kde.StatusNotifierWatcher. It maps what the Windows tray exposes
// (platform/desktop/system_tray.hpp): the icon in the state's colours, a tooltip, a context menu of commands (the
// TrayCommand numbers pass through unchanged), a click that opens the window.
//
// Without a watcher (stock GNOME, a bare window manager) nothing is shown and nothing fails: tray_available() is false,
// the item registers when a watcher appears and again when it comes back. Everything here belongs to the UI thread and
// runs inside the session bus processing (session.hpp); events arrive in that.

namespace maicord::dbus {

// An icon in one size: straight-alpha 0xAARRGGBB, top-down, width * height pixels (what tray_icon_render produces).
// Several sizes let the host pick (16, 22, 24, 32, 48, 64).
struct TrayPixmap {
    u32 width;
    u32 height;
    const u32* pixels;
};

struct TrayMenuEntry {
    const char* label;  // UTF-8
    u32 command;        // reported when chosen; never 0
    bool checkable;     // a check mark, set by `checked`
    bool checked;
    bool separator_before;
};

struct TrayState {
    const char* title;    // "MaiCord"
    const char* tooltip;  // the voice or unread state
    const TrayPixmap* icons;
    u32 icon_count;
    bool attention;  // NeedsAttention: hosts may blink or move the icon
    const TrayMenuEntry* menu;
    u32 menu_count;
};

enum class TrayEventKind : u8 {
    activate,            // the primary click
    secondary_activate,  // middle click
    context_menu,        // the host asks us to show the menu (it normally draws the dbusmenu itself)
    command,             // a menu entry was chosen: `command`
};

struct TrayEvent {
    TrayEventKind kind;
    i32 x, y;     // screen position the host gave, 0 when none
    u32 command;  // TrayEventKind::command
};

using TrayHandler = void (*)(void* context, const TrayEvent& event) noexcept;

// Starts the item: owns its bus name, exports /StatusNotifierItem and /MenuBar, watches for the watcher. `id` is the
// application's id for the host ("maicord"). False without a session bus (the item then starts when one appears).
bool tray_start(const char* id, TrayHandler handler, void* context) noexcept;
// The item is registered with a watcher.
bool tray_available() noexcept;
// Replaces what the item shows; cheap when nothing changed. May be called before tray_start's registration finishes.
void tray_update(const TrayState& state) noexcept;
void tray_stop() noexcept;

} // namespace maicord::dbus
