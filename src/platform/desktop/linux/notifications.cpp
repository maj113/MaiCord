// platform/desktop/notifications.hpp on Linux: the toasts are org.freedesktop.Notifications messages (or the portal's
// in a sandbox) over platform/dbus/notifications.hpp. The calls keep their Windows meaning: show_once makes at most one
// Notify call, the removals close what is still shown, and a click on a toast becomes an activation the UI thread takes
// after the wake. Everything runs on the UI thread, inside the session bus processing, so nothing here locks. The
// Windows side keeps the history of the Action Center; Linux servers do not expose one, so the shown toasts of this
// process (the last kToasts) are what the removals reach.
#include "maicord/platform/desktop/notifications.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/dbus/notifications.hpp"

namespace maicord {
namespace {

constexpr u32 kToasts = 64;
constexpr u32 kTitleBytes = 1024;
constexpr u32 kBodyBytes = 4096;
constexpr u32 kIconBytes = 1024;

struct Toast {
    u64 cookie; // zero: the slot is free
    DesktopNotificationActivation target;
    u32 id; // the server's, once it said so
    bool close_requested;
};

struct Runtime {
    Toast toasts[kToasts];
    DesktopNotificationActivation activations[kDesktopNotificationActivationCapacity];
    u32 activation_count;
    Waker wake;
    u64 next_cookie;
    bool initialized;
};
Runtime g_runtime;

Runtime* runtime_of(DesktopNotifications* notifications) noexcept {
    return notifications && notifications->runtime == &g_runtime && g_runtime.initialized ? &g_runtime : nullptr;
}

bool same_message(const DesktopNotificationActivation& a, const DesktopNotificationActivation& b) noexcept {
    return a.profile_id == b.profile_id && a.channel_id == b.channel_id && a.message_id == b.message_id;
}

Toast* find(Runtime* runtime, u64 cookie) noexcept {
    for (Toast& toast : runtime->toasts)
        if (cookie && toast.cookie == cookie)
            return &toast;
    return nullptr;
}

// The slot of a new toast: a free one, else the oldest, whose toast stops being tracked (it stays on screen).
Toast* claim(Runtime* runtime) noexcept {
    Toast* oldest = &runtime->toasts[0];
    for (Toast& toast : runtime->toasts) {
        if (!toast.cookie)
            return &toast;
        if (toast.cookie < oldest->cookie)
            oldest = &toast;
    }
    return oldest;
}

void close_toast(Toast* toast) noexcept {
    if (toast->id)
        dbus::notification_close(toast->id);
    else
        toast->close_requested = true; // the server has not named it yet: closed when it does
}

// Closes every toast that `match` accepts. A toast that never got an id is forgotten when it arrives.
template <typename Match> void close_where(Runtime* runtime, Match match) noexcept {
    for (Toast& toast : runtime->toasts)
        if (toast.cookie && match(toast))
            close_toast(&toast);
}

void on_event(void* context, const dbus::NotificationEvent& event) noexcept {
    auto* runtime = static_cast<Runtime*>(context);
    Toast* toast = find(runtime, event.cookie);
    if (!toast)
        return;
    switch (event.kind) {
    case dbus::NotificationEventKind::shown:
        toast->id = event.id;
        if (toast->close_requested)
            dbus::notification_close(event.id);
        break;
    case dbus::NotificationEventKind::action:
        if (runtime->activation_count < kDesktopNotificationActivationCapacity)
            runtime->activations[runtime->activation_count++] = toast->target;
        if (runtime->wake.window)
            window_post(runtime->wake, 0, 0);
        break;
    case dbus::NotificationEventKind::closed:
    case dbus::NotificationEventKind::failed:
        reset_value(toast);
        break;
    }
}

// The path of a file: URI ("file:///a%20b" is "/a b"), into output with its NUL; false for any other target.
bool file_uri_path(Utf8View uri, char* output, u32 capacity) noexcept {
    constexpr char kScheme[] = "file://";
    constexpr u32 kSchemeBytes = sizeof(kScheme) - 1;
    if (!uri.data || uri.bytes <= kSchemeBytes || !bytes_equal(uri.data, kScheme, kSchemeBytes))
        return false;
    u32 length = 0;
    for (u32 i = kSchemeBytes; i < uri.bytes; ++i) {
        char c = uri.data[i];
        if (c == '%' && i + 2 < uri.bytes) {
            const auto digit = [](char h) noexcept { return h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1; };
            const int high = digit(uri.data[i + 1]), low = digit(uri.data[i + 2]);
            if (high < 0 || low < 0)
                return false;
            c = static_cast<char>(high * 16 + low);
            i += 2;
        }
        if (!c || length + 1 >= capacity)
            return false;
        output[length++] = c;
    }
    output[length] = 0;
    return length > 1 && output[0] == '/';
}

bool text_valid(Utf8View text) noexcept {
    if (text.bytes && !text.data)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (!text.data[i])
            return false;
    return utf8_validate(text).valid;
}

} // namespace

DesktopNotificationShowState desktop_notification_show_state(DesktopNotificationResult result) noexcept {
    // A bus that is gone is not retried within a turn: the next toast finds the bus again by itself.
    return result.ok() ? DesktopNotificationShowState::submitted : DesktopNotificationShowState::failed;
}

DesktopNotificationResult desktop_notifications_initialize(DesktopNotifications* notifications,
                                                           const DesktopNotificationConfig& config) noexcept {
    if (!notifications || !config.app_id.count)
        return {DesktopNotificationError::invalid_input, 0};
    reset_value(&g_runtime);
    g_runtime.wake = config.wake;
    g_runtime.initialized = true;
    notifications->runtime = &g_runtime;
    dbus::notifications_set_handler(on_event, &g_runtime);
    return {};
}

DesktopNotificationShowResult desktop_notifications_show_once(DesktopNotifications* notifications,
                                                              const DesktopNotificationPayload& payload) noexcept {
    DesktopNotificationShowResult shown;
    Runtime* runtime = runtime_of(notifications);
    if (!runtime)
        return shown;
    if (!payload.title.bytes || !text_valid(payload.title) || !text_valid(payload.body)) {
        shown.result = {DesktopNotificationError::invalid_input, 0};
        return shown;
    }
    if (payload.title.bytes >= kTitleBytes || payload.body.bytes >= kBodyBytes) {
        shown.result = {DesktopNotificationError::capacity, 0};
        return shown;
    }
    char title[kTitleBytes], body[kBodyBytes], icon[kIconBytes];
    copy_bytes(title, payload.title.data, payload.title.bytes);
    title[payload.title.bytes] = 0;
    if (payload.body.bytes)
        copy_bytes(body, payload.body.data, payload.body.bytes);
    body[payload.body.bytes] = 0;
    const bool has_icon = file_uri_path(payload.avatar_file_uri, icon, kIconBytes);

    Toast* toast = claim(runtime);
    reset_value(toast);
    toast->cookie = ++runtime->next_cookie;
    toast->target = payload.target;
    dbus::NotificationRequest request{};
    request.summary = title;
    request.body = payload.body.bytes ? body : nullptr;
    request.icon = has_icon ? icon : nullptr;
    request.category = "im.received";
    request.desktop_entry = "io.github.maj113.MaiCord";
    request.default_action = true;
    request.timeout_ms = -1;
    // The Windows "suppress popup" files the toast without a banner; a low urgency is the nearest request there is.
    request.urgency = payload.suppress_popup ? dbus::NotificationUrgency::low : dbus::NotificationUrgency::normal;
    request.silent = payload.silent;
    request.cookie = toast->cookie;
    shown.native_show_attempted = true;
    if (!dbus::notification_show(request)) {
        reset_value(toast);
        shown.result = {DesktopNotificationError::transport, 0};
        return shown;
    }
    shown.state = DesktopNotificationShowState::submitted;
    shown.result = {};
    return shown;
}

DesktopNotificationResult desktop_notifications_remove_message(DesktopNotifications* notifications,
                                                               DesktopNotificationActivation target) noexcept {
    Runtime* runtime = runtime_of(notifications);
    if (!runtime)
        return {DesktopNotificationError::closed, 0};
    close_where(runtime, [&](const Toast& toast) noexcept { return same_message(toast.target, target); });
    return {};
}

DesktopNotificationResult desktop_notifications_remove_channel(DesktopNotifications* notifications, u64 profile,
                                                               u64 channel) noexcept {
    Runtime* runtime = runtime_of(notifications);
    if (!runtime)
        return {DesktopNotificationError::closed, 0};
    close_where(runtime, [&](const Toast& toast) noexcept {
        return toast.target.profile_id == profile && toast.target.channel_id == channel;
    });
    return {};
}

DesktopNotificationResult desktop_notifications_history_step(DesktopNotifications* notifications,
                                                             bool* pending) noexcept {
    if (pending)
        *pending = false;
    return runtime_of(notifications) ? DesktopNotificationResult{}
                                     : DesktopNotificationResult{DesktopNotificationError::closed, 0};
}

bool desktop_notifications_history_pending(const DesktopNotifications*) noexcept {
    return false;
}

DesktopNotificationResult desktop_notifications_remove_profile(DesktopNotifications* notifications,
                                                               u64 profile) noexcept {
    Runtime* runtime = runtime_of(notifications);
    if (!runtime)
        return {DesktopNotificationError::closed, 0};
    close_where(runtime, [&](const Toast& toast) noexcept { return toast.target.profile_id == profile; });
    return {};
}

DesktopNotificationResult desktop_notifications_setting(DesktopNotifications* notifications, u32* setting) noexcept {
    if (!runtime_of(notifications) || !setting)
        return {DesktopNotificationError::invalid_input, 0};
    // The NotificationSetting values: 0 enabled; 1 disabled for the application, which is what no server is.
    *setting = dbus::notification_backend() == dbus::NotificationBackend::none ? 1 : 0;
    return {};
}

DesktopNotificationResult desktop_notifications_set_wake(DesktopNotifications* notifications,
                                                         const Waker& wake) noexcept {
    Runtime* runtime = runtime_of(notifications);
    if (!runtime)
        return {DesktopNotificationError::closed, 0};
    runtime->wake = wake;
    return {};
}

bool desktop_notifications_take_activation(DesktopNotifications* notifications,
                                           DesktopNotificationActivation* activation) noexcept {
    Runtime* runtime = runtime_of(notifications);
    if (!runtime || !activation || !runtime->activation_count)
        return false;
    *activation = runtime->activations[0];
    for (u32 i = 1; i < runtime->activation_count; ++i)
        runtime->activations[i - 1] = runtime->activations[i];
    --runtime->activation_count;
    return true;
}

void desktop_notifications_discard_activations(DesktopNotifications* notifications) noexcept {
    if (Runtime* runtime = runtime_of(notifications))
        runtime->activation_count = 0;
}

void desktop_notifications_release(DesktopNotifications* notifications) noexcept {
    if (!runtime_of(notifications))
        return;
    dbus::notifications_set_handler(nullptr, nullptr);
    reset_value(&g_runtime);
    notifications->runtime = nullptr;
}

} // namespace maicord
