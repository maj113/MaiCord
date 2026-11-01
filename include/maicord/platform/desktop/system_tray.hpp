#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

// Notification-area icon. The official client's precedence: in a call, deafened > muted
// (self, server or suppressed) > speaking > connected; otherwise unread
// (messages, mentions or friend requests, unless the badge is off) > normal.
enum class TrayIconState : u8 { normal, unread, connected, speaking, muted, deafened };
struct TrayInput {
    u32 mentions;
    bool unread;
    bool connected, speaking;
    bool self_mute, server_mute, suppressed;
    bool self_deaf, server_deaf;
};
TrayIconState tray_icon_state(const TrayInput&) noexcept;

// Retained notification-area state. Pure: the executor applies the plan.
struct TrayModel {
    TrayIconState state;
    u8 badge; // taskbar badge code: none, 1..9, 9+ or a dot (unread state only)
    bool shown;
};
struct TrayPlan {
    TrayIconState state;
    u8 badge;
    bool add, update, remove;
};
// available: a window owns the icon and the shell accepted it. A shell restart
// (TaskbarCreated) resets the model so the next plan adds the icon again.
TrayPlan tray_plan(TrayModel*, const TrayInput&, bool available) noexcept;
void tray_model_reset(TrayModel*) noexcept;

// Context menu: Open MaiCord, Mute, Deafen (checked by self state), Quit MaiCord.
enum TrayCommand : u32 {
    tray_command_none = 0,
    tray_command_open = 1,
    tray_command_mute = 2,
    tray_command_deafen = 3,
    tray_command_quit = 4,
};
struct TrayMenuItem {
    const wchar_t* label;
    u32 command;
    bool checked, separator_before;
};
inline constexpr u32 kTrayMenuItems = 4;
u32 tray_menu_build(const TrayInput&, TrayMenuItem* items, u32 capacity) noexcept;
// Tooltip text: "MaiCord", plus the voice or unread state.
u32 tray_tooltip(const TrayInput&, wchar_t* text, u32 capacity) noexcept;

// Straight-alpha 0xAARRGGBB colours from the active theme's tokens.
struct TrayIconPalette {
    u32 brand, brand_shade, success, danger, glyph;
};
inline constexpr u32 kTrayIconMaximumExtent = 64;
// MaiCord's mark (rounded tile with an "M") plus the state overlay in the
// lower-right corner: green dot (connected), ringed dot (speaking), struck
// disc (muted), crossed disc (deafened), red dot or count (unread).
bool tray_icon_render(TrayIconState state, u8 badge, const TrayIconPalette& palette, u32 extent,
                      Slice<u32> pixels) noexcept;

// Win32 executor around Shell_NotifyIconW (NOTIFYICON_VERSION_4).
struct SystemTray {
    HWND window;
    void* icon; // HICON
    u32 callback_message;
    u32 created_message; // TaskbarCreated
    TrayModel model;
    TrayIconPalette palette;
    bool added;
};
bool system_tray_initialize(SystemTray*, HWND window, u32 callback_message) noexcept;
// Applies a plan (and palette changes). False when the shell refused it.
bool system_tray_apply(SystemTray*, const TrayPlan&, const TrayIconPalette&, const wchar_t* tooltip,
                       u32 dpi) noexcept;
// True for TaskbarCreated: the icon must be added again.
bool system_tray_shell_restarted(SystemTray*, UINT message) noexcept;
// Shows the menu at the cursor and returns the chosen command.
u32 system_tray_menu(SystemTray*, const TrayMenuItem* items, u32 count) noexcept;
void system_tray_release(SystemTray*) noexcept;
// Window/taskbar icon in MaiCord's mark (callers DestroyIcon it).
HICON system_tray_create_icon(TrayIconState state, u8 badge, const TrayIconPalette&, u32 extent) noexcept;

} // namespace maicord
