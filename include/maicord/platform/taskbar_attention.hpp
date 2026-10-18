#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

struct ITaskbarList3;

namespace maicord {

// Overlay badge codes: none, a count 1..9, "9+", or a dot for unread without mentions.
inline constexpr u8 kTaskbarBadgeNone = 0;
inline constexpr u8 kTaskbarBadgeOverflow = 10;
inline constexpr u8 kTaskbarBadgeDot = 11;
inline constexpr u32 kTaskbarBadgeMaximumExtent = 64;

u8 taskbar_attention_badge(u32 mentions, bool unread, bool enabled) noexcept;

struct TaskbarAttentionInput {
    u32 mentions;        // mentions plus pending friend requests
    u64 notify_serial;   // bumps when a notification is admitted while unfocused
    bool unread, badge_enabled, flash_enabled, focused;
};
struct TaskbarAttentionState {
    u64 flashed_serial;
    u8 badge;
    bool flashing, badge_applied;
};
struct TaskbarAttentionPlan {
    u8 badge;
    bool set_badge, start_flash, stop_flash;
};
// Pure transition: the host applies the plan, then the state already reflects it.
TaskbarAttentionPlan taskbar_attention_plan(TaskbarAttentionState*, const TaskbarAttentionInput&) noexcept;
// Straight-alpha BGRA, top-down, extent x extent: a red disc with white text.
bool taskbar_attention_render_badge(u8 badge, u32 extent, Slice<u32> pixels) noexcept;

// Win32 executor: ITaskbarList3 overlay plus FlashWindowEx. Waits for the
// TaskbarButtonCreated message before touching the overlay.
struct TaskbarAttention {
    ITaskbarList3* list;
    void* icon; // HICON
    u32 created_message;
    u8 icon_badge;
    bool button_ready, com_initialized;
    TaskbarAttentionState state;
};
bool taskbar_attention_initialize(TaskbarAttention*) noexcept;
// Returns true when message is the TaskbarButtonCreated notification.
bool taskbar_attention_window_message(TaskbarAttention*, UINT message) noexcept;
void taskbar_attention_apply(TaskbarAttention*, HWND, const TaskbarAttentionPlan&, u32 dpi) noexcept;
void taskbar_attention_release(TaskbarAttention*, HWND) noexcept;

} // namespace maicord
