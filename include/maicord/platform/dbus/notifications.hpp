#pragma once

#include "maicord/core/base.hpp"

// Desktop notifications over the session bus (session.hpp): org.freedesktop.Notifications directly (Notify with
// actions; the ActionInvoked, NotificationClosed and ActivationToken signals) or, in a sandbox, the portal
// (org.freedesktop.portal.Notification: AddNotification, RemoveNotification, ActionInvoked). The backend is the portal
// when the process runs under Flatpak (/.flatpak-info) or Snap (SNAP set) and the daemon directly otherwise;
// MAICORD_NOTIFICATION_BACKEND=portal|direct overrides it.
//
// Everything here belongs to the UI thread; the events arrive inside session_bus_process (the window pump).

namespace maicord::dbus {

enum class NotificationBackend : u8 { none, direct, portal };

struct NotificationAction {
    const char* key;    // reported in the action event
    const char* label;  // the button's text
};

// normal is the default of a zeroed request; the wire values (0 low, 1 normal, 2 critical) are mapped on sending.
enum class NotificationUrgency : u8 { normal = 0, low = 1, critical = 2 };

struct NotificationRequest {
    const char* summary;        // required
    const char* body;           // plain text, may be null (sent as markup, escaped, to a daemon that reads markup)
    const char* icon;           // an icon theme name or an absolute path (the portal gets the file's bytes); may be null
    const char* category;       // "im.received" ...; may be null
    const char* desktop_entry;  // the .desktop file's name without the extension; null for none
    const NotificationAction* actions;  // buttons
    u32 action_count;
    bool default_action;  // clicking the notification itself reports the action "default"
    u32 replaces_id;      // direct backend: replace this notification (0 = a new one)
    i32 timeout_ms;       // -1 the server's default, 0 never expires
    NotificationUrgency urgency;
    bool silent;  // ask the server not to play a sound
    u64 cookie;   // returned in every event of this notification
};

enum class NotificationEventKind : u8 {
    shown,   // the server accepted it: `id` is set
    action,  // a button or the notification was activated: `action_key` (and `activation_token` when given)
    closed,  // `reason`: 1 expired, 2 dismissed by the user, 3 closed by notification_close, 4 other
    failed,  // the server refused it or there is none
};

struct NotificationEvent {
    NotificationEventKind kind;
    u64 cookie;
    u32 id;
    const char* action_key;        // valid during the call
    const char* activation_token;  // an xdg-activation token the server made for the click; null when none
    u32 reason;
};

using NotificationHandler = void (*)(void* context, const NotificationEvent& event) noexcept;
void notifications_set_handler(NotificationHandler handler, void* context) noexcept;

// The backend this process uses (none without a session bus).
NotificationBackend notification_backend() noexcept;

// Sends the notification. False when it could not be sent (no bus, bad arguments); a refusal arrives as a failed event.
bool notification_show(const NotificationRequest& request) noexcept;
// Closes a shown notification (the closed event follows on the direct backend).
bool notification_close(u32 id) noexcept;

} // namespace maicord::dbus
