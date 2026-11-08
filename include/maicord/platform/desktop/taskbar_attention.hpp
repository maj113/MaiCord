#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/platform/window/window.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_types.hpp"

struct ITaskbarList3;
#endif

namespace maicord {

// Overlay badge codes: none, a count 1..9, "9+", or a dot for unread without mentions.
inline constexpr u8 kTaskbarBadgeNone = 0;
inline constexpr u8 kTaskbarBadgeOverflow = 10;
inline constexpr u8 kTaskbarBadgeDot = 11;
inline constexpr u32 kTaskbarBadgeMaximumExtent = 64;

u8 taskbar_attention_badge(u32 mentions, bool unread, bool enabled) noexcept;
// Square root shared by the taskbar badge and tray mark renderers; 0 for a value that is not positive.
float taskbar_attention_root(float value) noexcept;

struct TaskbarAttentionInput {
    u32 mentions;        // mentions plus pending friend requests
    u64 notify_serial;   // bumps when a notification is admitted while unfocused
    bool unread, badge_enabled, flash_enabled, focused;
};
struct TaskbarAttentionState {
    u64 flashed_serial;
#if !defined(_WIN32)
    u32 mentions; // the count the launcher entry shows
#endif
    u8 badge;
    bool flashing, badge_applied;
};
struct TaskbarAttentionPlan {
    u8 badge;
    bool set_badge, start_flash, stop_flash;
    u32 mentions; // the count behind the badge: the docks of Linux show it as it is
};
// Pure transition: the host applies the plan, then the state already reflects it.
TaskbarAttentionPlan taskbar_attention_plan(TaskbarAttentionState*, const TaskbarAttentionInput&) noexcept;
// Straight-alpha BGRA, top-down, extent x extent: a red disc with white text.
bool taskbar_attention_render_badge(u8 badge, u32 extent, Slice<u32> pixels) noexcept;

#if defined(_WIN32)
// Win32 executor: ITaskbarList3 overlay plus FlashWindowEx. Waits for the
// TaskbarButtonCreated message before touching the overlay; the taskbar
// object is activated when the first badge is shown.
struct TaskbarAttention {
    ITaskbarList3* list;
    void* icon; // HICON
    u32 created_message;
    u8 icon_badge;
    bool button_ready, com_initialized, list_attempted;
    TaskbarAttentionState state;
};
// Returns true when message is the TaskbarButtonCreated notification.
bool taskbar_attention_window_message(TaskbarAttention*, UINT message) noexcept;
#else
// Linux executor: the launcher entry (platform/dbus/launcher_entry.hpp) for the dock's count and urgent flag, and the
// compositor's attention request for flashing.
struct TaskbarAttention {
    TaskbarAttentionState state;
};
#endif
bool taskbar_attention_initialize(TaskbarAttention*) noexcept;
void taskbar_attention_apply(TaskbarAttention*, Window, const TaskbarAttentionPlan&, u32 dpi) noexcept;
void taskbar_attention_release(TaskbarAttention*, Window) noexcept;

} // namespace maicord
