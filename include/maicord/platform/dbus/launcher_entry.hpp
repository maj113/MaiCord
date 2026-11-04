#pragma once

#include "maicord/core/base.hpp"

// The taskbar badge and attention of Linux docks and task managers, the Unity LauncherEntry signal
// (com.canonical.Unity.LauncherEntry.Update): the count badge of the app's icon and its urgent flag. KDE Plasma's task
// manager, Plank, Dash to Dock, Ubuntu Dock and others listen for it; where nothing does, the signal is harmless. It is the
// analogue of platform/desktop/taskbar_attention.hpp's overlay badge (a count 1..9, "9+", or a dot) and flashing; the
// compositor's own attention request (xdg-activation, window_request_attention) stays as it is.
//
// Mapping from the Windows plan (TaskbarAttentionPlan): badge 1..9 -> count, kTaskbarBadgeOverflow -> the real count (the
// dock draws "99+" itself), kTaskbarBadgeDot -> urgent without a count, kTaskbarBadgeNone -> cleared; start_flash ->
// urgent. UI thread, on the session bus.

namespace maicord::dbus {

struct LauncherEntryState {
    i64 count;
    bool count_visible;
    bool urgent;
};

// Sends the state for the desktop entry `desktop_id` ("io.github.maj113.MaiCord.desktop"). False without a session bus.
// The state is remembered and sent again after a bus reconnect.
bool launcher_entry_update(const char* desktop_id, const LauncherEntryState& state) noexcept;
// Count hidden, not urgent.
bool launcher_entry_clear(const char* desktop_id) noexcept;

} // namespace maicord::dbus
