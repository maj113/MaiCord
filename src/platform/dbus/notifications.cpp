#include "maicord/core/decimal.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/dbus/notifications.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::dbus {
namespace {

constexpr const char* kDaemonName = "org.freedesktop.Notifications";
constexpr const char* kDaemonPath = "/org/freedesktop/Notifications";
constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kPortalInterface = "org.freedesktop.portal.Notification";
constexpr u32 kCallTimeoutMs = 10000;
constexpr u32 kMaxEntries = 64;
constexpr u32 kTokenCapacity = 128;
constexpr u32 kCapabilitiesTimeoutMs = 500;
// The portal takes an icon as bytes (a file path means nothing to a host that cannot see the sandbox): the avatar PNGs
// the application writes are a few kilobytes, never near this.
constexpr u32 kPortalIconMaximumBytes = 512 * 1024;
constexpr u32 kEscapedCapacity = 20 * 1024;

struct Entry {
    bool used;
    u32 id;       // the server's id (direct) or the local counter (portal); 0 until the server answered
    u64 cookie;
    char token[kTokenCapacity];
};

Entry g_entries[kMaxEntries];
u32 g_next_eviction;
u32 g_local_id;
NotificationHandler g_handler;
void* g_handler_context;
bool g_hook_registered;
NotificationBackend g_backend;  // none until chosen
u32 g_markup;                   // 0 not asked, 1 the daemon reads markup in the body, 2 it does not
char g_escaped[kEscapedCapacity];

void emit(NotificationEventKind kind, u64 cookie, u32 id, const char* key, const char* token, u32 reason) noexcept {
    if (!g_handler)
        return;
    NotificationEvent event{};
    event.kind = kind;
    event.cookie = cookie;
    event.id = id;
    event.action_key = key;
    event.activation_token = token;
    event.reason = reason;
    g_handler(g_handler_context, event);
}

Entry* find_by_id(u32 id) noexcept {
    if (id == 0)
        return nullptr;
    for (Entry& entry : g_entries)
        if (entry.used && entry.id == id)
            return &entry;
    return nullptr;
}

// A slot for a new notification; the oldest is reused when all are taken.
Entry* take_entry(u64 cookie) noexcept {
    Entry* slot = nullptr;
    for (Entry& entry : g_entries) {
        if (!entry.used) {
            slot = &entry;
            break;
        }
    }
    if (!slot) {
        slot = &g_entries[g_next_eviction++ % kMaxEntries];
    }
    *slot = Entry{};
    slot->used = true;
    slot->cookie = cookie;
    return slot;
}

NotificationBackend choose_backend() noexcept {
    const char* forced = environment_get("MAICORD_NOTIFICATION_BACKEND");
    if (forced && c_string_equal(forced, "portal"))
        return NotificationBackend::portal;
    if (forced && c_string_equal(forced, "direct"))
        return NotificationBackend::direct;
    return portal_sandboxed() ? NotificationBackend::portal : NotificationBackend::direct;
}

// Direct backend signals ---------------------------------------------------------------------------------------------

void on_action_invoked(void*, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    u32 id = 0;
    const char* key = nullptr;
    if (!reader_u32(&r, &id) || !reader_string(&r, &key, nullptr))
        return;
    Entry* entry = find_by_id(id);
    if (!entry)
        return;
    char token[kTokenCapacity];
    copy_bytes(token, entry->token, kTokenCapacity);
    entry->token[0] = 0;
    emit(NotificationEventKind::action, entry->cookie, id, key, token[0] ? token : nullptr, 0);
}

void on_closed(void*, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    u32 id = 0;
    u32 reason = 0;
    if (!reader_u32(&r, &id) || !reader_u32(&r, &reason))
        return;
    Entry* entry = find_by_id(id);
    if (!entry)
        return;
    const u64 cookie = entry->cookie;
    *entry = Entry{};
    emit(NotificationEventKind::closed, cookie, id, nullptr, nullptr, reason);
}

void on_activation_token(void*, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    u32 id = 0;
    const char* token = nullptr;
    u32 length = 0;
    if (!reader_u32(&r, &id) || !reader_string(&r, &token, &length) || length >= kTokenCapacity)
        return;
    if (Entry* entry = find_by_id(id))
        copy_bytes(entry->token, token, length + 1);
}

// Portal backend signal: ActionInvoked(s id, s action, av parameter)
void on_portal_action(void*, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    const char* identifier = nullptr;
    const char* action = nullptr;
    if (!reader_string(&r, &identifier, nullptr) || !reader_string(&r, &action, nullptr))
        return;
    u32 id = 0;
    for (const char* p = identifier; *p >= '0' && *p <= '9'; ++p)
        id = id * 10 + static_cast<u32>(*p - '0');
    Entry* entry = find_by_id(id);
    if (entry)
        emit(NotificationEventKind::action, entry->cookie, id, action, nullptr, 0);
}

void subscribe(Connection* c) noexcept {
    if (g_backend == NotificationBackend::direct) {
        connection_subscribe(c, SignalMatch{kDaemonName, kDaemonPath, kDaemonName, "ActionInvoked", nullptr},
                             on_action_invoked, nullptr);
        connection_subscribe(c, SignalMatch{kDaemonName, kDaemonPath, kDaemonName, "NotificationClosed", nullptr},
                             on_closed, nullptr);
        connection_subscribe(c, SignalMatch{kDaemonName, kDaemonPath, kDaemonName, "ActivationToken", nullptr},
                             on_activation_token, nullptr);
    } else if (g_backend == NotificationBackend::portal) {
        connection_subscribe(c, SignalMatch{kPortalName, kPortalPath, kPortalInterface, "ActionInvoked", nullptr},
                             on_portal_action, nullptr);
    }
}

// The bus's session changed: the old entries are no longer known to any server.
void on_session(Connection* c) noexcept {
    for (Entry& entry : g_entries)
        entry = Entry{};
    g_markup = 0;
    subscribe(c);
}

bool ensure_backend() noexcept {
    if (g_backend == NotificationBackend::none) {
        if (!session_bus_open())
            return false;
        g_backend = choose_backend();
    }
    if (!g_hook_registered) {
        g_hook_registered = true;
        session_bus_add_hook(on_session);
    }
    return session_bus() != nullptr;
}

// Replies -------------------------------------------------------------------------------------------------------------

// The call failed: the entry is dropped, the reason logged and the owner told.
void fail_entry(Entry* entry, Status status, const Message* reply, const char* fallback) noexcept {
    const u64 cookie = entry->cookie;
    *entry = Entry{};
    log_write(LogLevel::info, "notifications", reply_error_name(status, reply, fallback));
    emit(NotificationEventKind::failed, cookie, 0, nullptr, nullptr, 0);
}

void on_notify_reply(void* context, Status status, const Message* reply) noexcept {
    Entry* entry = &g_entries[reinterpret_cast<usize>(context) - 1];
    if (!entry->used)
        return;  // dropped while waiting (a new session cleared the table)
    u32 id = 0;
    if (status == Status::ok) {
        Reader r;
        message_reader(*reply, &r);
        reader_u32(&r, &id);  // stays 0 when the reply holds no id
    }
    if (id == 0) {
        fail_entry(entry, status, reply, "Notify failed");
        return;
    }
    // A replaced notification keeps one entry.
    if (Entry* previous = find_by_id(id); previous && previous != entry)
        *previous = Entry{};
    entry->id = id;
    emit(NotificationEventKind::shown, entry->cookie, id, nullptr, nullptr, 0);
}

void on_portal_reply(void* context, Status status, const Message* reply) noexcept {
    Entry* entry = &g_entries[reinterpret_cast<usize>(context) - 1];
    if (!entry->used)
        return;
    if (status != Status::ok) {
        fail_entry(entry, status, reply, "AddNotification failed");
        return;
    }
    emit(NotificationEventKind::shown, entry->cookie, entry->id, nullptr, nullptr, 0);
}

// Whether the daemon reads markup in the body (the capability body-markup of the specification), asked once per session.
// A daemon that does not answer is taken to: message text then stays text on one that reads markup, and the rare entity
// shows on one that does not.
bool daemon_reads_markup(Connection* c) noexcept {
    if (g_markup != 0)
        return g_markup == 1;
    g_markup = 1;
    const CallTarget target{kDaemonName, kDaemonPath, kDaemonName, "GetCapabilities"};
    Message reply;
    const Status status = connection_call_sync(c, target, nullptr, kCapabilitiesTimeoutMs, &reply);
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        bool markup = false;
        if (reader_enter_array(&r)) {
            while (reader_more(r)) {
                const char* name = nullptr;
                if (!reader_string(&r, &name, nullptr))
                    break;
                markup = markup || c_string_equal(name, "body-markup");
            }
            reader_leave(&r);
        }
        g_markup = markup ? 1 : 2;
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return g_markup == 1;
}

// `text` as markup: &, < and > as entities, so that a message with a link or an image tag in it is shown as typed (the
// Windows toast escapes the same way). Into a buffer of this file; text that does not fit is cut at a character.
const char* escape_markup(const char* text) noexcept {
    u32 used = 0;
    for (const char* p = text; *p; ++p) {
        const char* entity = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : nullptr;
        u32 length = 1;
        while (entity && entity[length])
            ++length;
        if (used + (entity ? length : 1) + 1 > kEscapedCapacity) {
            // Not the rest of a character whose start is already in.
            if ((static_cast<u8>(*p) & 0xC0) == 0x80)
                while (used && (static_cast<u8>(g_escaped[used - 1]) & 0xC0) == 0x80)
                    --used;
            if (used && (static_cast<u8>(*p) & 0xC0) == 0x80)
                --used;
            break;
        }
        if (entity) {
            copy_bytes(g_escaped + used, entity, length);
            used += length;
        } else {
            g_escaped[used++] = *p;
        }
    }
    g_escaped[used] = 0;
    return g_escaped;
}

bool show_direct(Connection* c, const NotificationRequest& request, Entry* entry) noexcept {
    // The body goes in as plain text unless the daemon reads markup (asked before the call is built: it may wait).
    const char* body_text = !request.body                      ? ""
                            : daemon_reads_markup(c)           ? escape_markup(request.body)
                                                               : request.body;
    Builder body;
    builder_init(&body);
    builder_string(&body, "MaiCord");
    builder_u32(&body, request.replaces_id);
    builder_string(&body, request.icon ? request.icon : "");
    builder_string(&body, request.summary);
    builder_string(&body, body_text);
    builder_open_array(&body, "s");
    if (request.default_action) {
        builder_string(&body, "default");
        builder_string(&body, "Open");
    }
    for (u32 i = 0; i < request.action_count; ++i) {
        builder_string(&body, request.actions[i].key);
        builder_string(&body, request.actions[i].label);
    }
    builder_close_array(&body);
    builder_open_array(&body, "{sv}");
    if (request.desktop_entry)
        builder_dict_string(&body, "desktop-entry", request.desktop_entry);
    if (request.category)
        builder_dict_string(&body, "category", request.category);
    builder_open_dict_entry(&body, "urgency", "y");
    builder_byte(&body, request.urgency == NotificationUrgency::low ? 0 : request.urgency == NotificationUrgency::critical ? 2 : 1);
    builder_close_dict_entry(&body);
    if (request.silent)
        builder_dict_bool(&body, "suppress-sound", true);
    builder_close_array(&body);
    builder_i32(&body, request.timeout_ms);
    const CallTarget target{kDaemonName, kDaemonPath, kDaemonName, "Notify"};
    const bool sent = connection_call(c, target, &body, kCallTimeoutMs, on_notify_reply,
                                      reinterpret_cast<void*>(static_cast<usize>(entry - g_entries) + 1));
    builder_release(&body);
    return sent;
}

// The content of the image file `path` (an absolute UTF-8 path) on the heap, for the portal; null when it is missing,
// empty or larger than the maximum. The caller frees it with heap_free.
u8* read_icon_file(const char* path, u32* count) noexcept {
    wchar_t wide[1024];
    if (!path_from_utf8(path, wide, 1024))
        return nullptr;
    const FileHandle file = file_open(wide, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareWrite, 0});
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    u64 size = 0;
    u8* bytes = nullptr;
    u32 read = 0;
    bool ok = file_size(file, &size) && size != 0 && size <= kPortalIconMaximumBytes;
    if (ok) {
        bytes = static_cast<u8*>(heap_allocate(static_cast<usize>(size)));
        ok = bytes && file_read(file, bytes, static_cast<u32>(size), &read) && read == size;
    }
    file_close(file);
    if (!ok) {
        if (bytes)
            heap_free(bytes);
        return nullptr;
    }
    *count = read;
    return bytes;
}

bool show_portal(Connection* c, const NotificationRequest& request, Entry* entry) noexcept {
    // The portal names a notification with a string: the decimal local id.
    entry->id = ++g_local_id;
    char identifier[12];
    identifier[decimal_write(identifier, entry->id)] = 0;

    Builder body;
    builder_init(&body);
    builder_string(&body, identifier);
    builder_open_array(&body, "{sv}");
    builder_dict_string(&body, "title", request.summary);
    if (request.body)
        builder_dict_string(&body, "body", request.body);
    // The icon is a themed name, or the bytes of an image file (a path means nothing to the portal's host).
    u32 image_bytes = 0;
    u8* image = request.icon && request.icon[0] == '/' ? read_icon_file(request.icon, &image_bytes) : nullptr;
    if (request.icon && (request.icon[0] != '/' || image)) {
        // ("themed", ["name"]) or ("bytes", the image's bytes).
        builder_open_dict_entry(&body, "icon", "(sv)");
        builder_open_struct(&body, "(sv)");
        if (image) {
            builder_string(&body, "bytes");
            builder_open_variant(&body, "ay");
            builder_byte_array(&body, image, image_bytes);
            builder_close_variant(&body);
        } else {
            builder_string(&body, "themed");
            builder_open_variant(&body, "as");
            builder_open_array(&body, "s");
            builder_string(&body, request.icon);
            builder_close_array(&body);
            builder_close_variant(&body);
        }
        builder_close_struct(&body);
        builder_close_dict_entry(&body);
    }
    builder_dict_string(&body, "priority",
                        request.urgency == NotificationUrgency::critical ? "urgent"
                        : request.urgency == NotificationUrgency::low    ? "low"
                                                                         : "normal");
    if (request.default_action)
        builder_dict_string(&body, "default-action", "default");
    if (request.action_count) {
        builder_open_dict_entry(&body, "buttons", "aa{sv}");
        builder_open_array(&body, "a{sv}");
        for (u32 i = 0; i < request.action_count; ++i) {
            builder_open_array(&body, "{sv}");
            builder_dict_string(&body, "label", request.actions[i].label);
            builder_dict_string(&body, "action", request.actions[i].key);
            builder_close_array(&body);
        }
        builder_close_array(&body);
        builder_close_dict_entry(&body);
    }
    builder_close_array(&body);
    const CallTarget target{kPortalName, kPortalPath, kPortalInterface, "AddNotification"};
    const bool sent = connection_call(c, target, &body, kCallTimeoutMs, on_portal_reply,
                                      reinterpret_cast<void*>(static_cast<usize>(entry - g_entries) + 1));
    builder_release(&body);
    if (image)
        heap_free(image);
    return sent;
}

} // namespace

void notifications_set_handler(NotificationHandler handler, void* context) noexcept {
    g_handler = handler;
    g_handler_context = context;
}

NotificationBackend notification_backend() noexcept {
    if (!ensure_backend())
        return NotificationBackend::none;
    return g_backend;
}

bool notification_show(const NotificationRequest& request) noexcept {
    if (!request.summary || (request.action_count && !request.actions))
        return false;
    for (u32 i = 0; i < request.action_count; ++i)
        if (!request.actions[i].key || !request.actions[i].label || c_string_equal(request.actions[i].key, "default"))
            return false;
    if (!ensure_backend())
        return false;
    Connection* c = session_bus();
    Entry* entry = nullptr;
    if (g_backend == NotificationBackend::direct && request.replaces_id)
        entry = find_by_id(request.replaces_id);
    if (entry) {
        entry->cookie = request.cookie;
    } else {
        entry = take_entry(request.cookie);
    }
    const bool sent =
        g_backend == NotificationBackend::portal ? show_portal(c, request, entry) : show_direct(c, request, entry);
    if (!sent)
        *entry = Entry{};
    return sent;
}

bool notification_close(u32 id) noexcept {
    if (!ensure_backend())
        return false;
    Connection* c = session_bus();
    Builder body;
    builder_init(&body);
    bool sent = false;
    if (g_backend == NotificationBackend::portal) {
        char identifier[12];
        identifier[decimal_write(identifier, id)] = 0;
        builder_string(&body, identifier);
        const CallTarget target{kPortalName, kPortalPath, kPortalInterface, "RemoveNotification"};
        sent = connection_call(c, target, &body, 0, nullptr, nullptr);
        if (Entry* entry = find_by_id(id)) {
            const u64 cookie = entry->cookie;
            *entry = Entry{};
            emit(NotificationEventKind::closed, cookie, id, nullptr, nullptr, 3);
        }
    } else {
        builder_u32(&body, id);
        const CallTarget target{kDaemonName, kDaemonPath, kDaemonName, "CloseNotification"};
        sent = connection_call(c, target, &body, 0, nullptr, nullptr);
    }
    builder_release(&body);
    return sent;
}

} // namespace maicord::dbus
