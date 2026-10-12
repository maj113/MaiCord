#pragma once
#include "maicord/core/slice.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

// Launch arguments after argv0. The startup entry passes --startup; the
// discord:// protocol handler passes the official client's shape, --url -- "<link>".
// Unknown arguments (including the updater's reserved family) are ignored.
inline constexpr u32 kDesktopUrlCapacity = 1024;
struct DesktopLaunch {
    bool startup;
    bool url_present;
    u32 url_units;
    wchar_t url[kDesktopUrlCapacity];
};
// False only for a malformed command line (unbalanced quotes, NUL, oversized
// URL). The URL is kept verbatim; desktop_link_parse validates it.
bool desktop_launch_parse(ConstSlice<wchar_t> command_line, DesktopLaunch* launch) noexcept;

// discord:// links routed to the existing navigation. Accepted shapes:
//   discord://, discord://-/, discord:///app        open the window
//   discord://-/invite/<code>                       invite join dialog
//   discord://-/channels/<guild|@me>/<channel>[/<message>]
// The authority may be empty, "-" or one of the service's web hosts. Query and fragment
// are ignored; anything else is refused.
enum class DesktopLinkKind : u8 { none, open, invite, channel, message };
inline constexpr u32 kDesktopInviteCodeMaximumBytes = 32;
struct DesktopLinkTarget {
    DesktopLinkKind kind;
    u8 code_bytes;
    u64 guild_id; // 0 for @me
    u64 channel_id;
    u64 message_id;
    char code[kDesktopInviteCodeMaximumBytes];
};
bool desktop_link_parse(ConstSlice<wchar_t> url, DesktopLinkTarget* target) noexcept;

// Registry locations. The user scope is HKCU's Run key, its Task Manager
// approval key and Software\Classes; tests pass a private key instead.
struct DesktopRegistryScope {
    HKEY root;
    const wchar_t* run_key;
    const wchar_t* approved_key;
    const wchar_t* classes_key;
    const wchar_t* value_name;
    const wchar_t* protocol;
};
DesktopRegistryScope desktop_registry_user_scope() noexcept;
enum class DesktopRegistration : u8 { absent, ours, other };

// Startup entry: "<executable>" --startup. Ours only when it names this
// executable and Task Manager has not disabled it. Enabling rewrites the
// value and clears a disabled approval; disabling deletes both.
DesktopRegistration desktop_startup_query(const DesktopRegistryScope&, ConstSlice<wchar_t> executable) noexcept;
bool desktop_startup_set(const DesktopRegistryScope&, ConstSlice<wchar_t> executable, bool enable) noexcept;

// URL protocol class with shell\open\command "<executable>" --url -- "%1".
// Disabling removes the class only while it is ours, so another client's
// handler is never deleted.
DesktopRegistration desktop_protocol_query(const DesktopRegistryScope&, ConstSlice<wchar_t> executable) noexcept;
bool desktop_protocol_set(const DesktopRegistryScope&, ConstSlice<wchar_t> executable, bool enable) noexcept;

// Single instance. The first process owns a named mutex; a later ordinary
// launch hands its URL (or an empty "show" request) to the first window by
// WM_COPYDATA and exits.
inline constexpr ULONG_PTR kDesktopForwardMagic = 0x4D43464Fu;
struct DesktopForwardMessage {
    bool url_present;
    u32 url_units;
    wchar_t url[kDesktopUrlCapacity];
};
// Returns the mutex, owned by the calling thread when it did not exist;
// *existed reports another instance (the handle is then not owned).
HANDLE desktop_instance_mutex(const wchar_t* name, bool* existed) noexcept;
// Publishes the primary window under a named mapping; keep the handle open
// for the process lifetime. Names are per profile so a test profile never
// reaches the user's client.
HANDLE desktop_instance_publish(const wchar_t* name, HWND window) noexcept;
enum class DesktopForwardResult : u8 { no_window, forwarded, refused };
// Sends the request to the published window of another process. The
// receiver may take the foreground (AllowSetForegroundWindow).
DesktopForwardResult desktop_instance_forward(const wchar_t* name, const DesktopLaunch& launch,
                                              u32 timeout_ms) noexcept;
// "Local\MaiCord.<kind>.<16 hex>" from a hash of the profile directory.
bool desktop_instance_name(const wchar_t* kind, ConstSlice<wchar_t> profile, wchar_t* out, u32 capacity) noexcept;
// Validates a received WM_COPYDATA payload.
bool desktop_instance_receive(const COPYDATASTRUCT* data, DesktopForwardMessage* message) noexcept;
// Brings a hidden, minimized or background window to the front.
void desktop_window_reveal(HWND window) noexcept;

// Close button policy: hide to the notification area unless the user (or an
// update restart) asked to quit, the preference is off or no icon exists.
enum class DesktopCloseAction : u8 { quit, hide };
constexpr DesktopCloseAction desktop_close_action(bool minimize_to_tray, bool tray_visible,
                                                  bool quit_requested) noexcept {
    return minimize_to_tray && tray_visible && !quit_requested ? DesktopCloseAction::hide : DesktopCloseAction::quit;
}
// ShowWindow commands and the WM_ACTIVATE state named below, as <Windows.h>
// defines them (checked in desktop_integration.cpp).
inline constexpr int kDesktopShowHide = 0;              // SW_HIDE
inline constexpr int kDesktopShowMinimizedNoActive = 7; // SW_SHOWMINNOACTIVE
inline constexpr int kDesktopShowDefault = 10;          // SW_SHOWDEFAULT
inline constexpr int kDesktopActivateInactive = 0;      // WA_INACTIVE
// How the first window appears: a startup-entry launch with Start minimized
// goes to the tray (or the taskbar when the tray is off).
constexpr int desktop_initial_show(bool startup_launch, bool start_minimized, bool minimize_to_tray) noexcept {
    return !startup_launch || !start_minimized ? kDesktopShowDefault
           : minimize_to_tray                  ? kDesktopShowHide
                                               : kDesktopShowMinimizedNoActive;
}
// Whether a WM_ACTIVATE may run activation work that makes a cross-apartment
// COM call (the Windows notification setting). The taskbar restores a
// minimized window by activating it while still minimized, then posting
// SC_RESTORE; an outgoing COM call from the STA in that WM_ACTIVATE lets the
// COM modal loop discard the queued WM_SYSCOMMAND with a beep. The restore's
// own (non-minimized) activation follows, so the work waits for that one.
constexpr bool desktop_activation_refresh_allowed(WPARAM activation) noexcept {
    // LOWORD(activation) is the state; a nonzero HIWORD means minimized.
    return static_cast<WORD>(activation & 0xffff) != kDesktopActivateInactive &&
           static_cast<WORD>((activation >> 16) & 0xffff) == 0;
}

} // namespace maicord
