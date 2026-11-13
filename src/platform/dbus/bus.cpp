#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/platform/dbus/bus.hpp"

namespace maicord::dbus {
namespace {

constexpr const char* kBusName = "org.freedesktop.DBus";
constexpr const char* kBusPath = "/org/freedesktop/DBus";
constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr u32 kMaxWatches = 8;

// Name watching -------------------------------------------------------------------------------------------------------

struct Watch {
    bool used;
    Connection* connection;
    u32 subscription;
    u32 serial;  // tells a reply for a dropped watch from one for the slot's next user
    NameWatchHandler handler;
    void* context;
    char name[128];
};

Watch g_watches[kMaxWatches];
u32 g_watch_serial;

Watch* watch_of(usize token) noexcept {
    const u32 slot = static_cast<u32>(token & 0xFF);
    const u32 serial = static_cast<u32>(token >> 8);
    if (slot == 0 || slot > kMaxWatches)
        return nullptr;
    Watch& watch = g_watches[slot - 1];
    return watch.used && watch.serial == serial ? &watch : nullptr;
}

void on_owner_changed(void* context, const Message& signal) noexcept {
    Watch* watch = watch_of(reinterpret_cast<usize>(context));
    if (!watch)
        return;
    Reader r;
    message_reader(signal, &r);
    const char* name = nullptr;
    const char* old_owner = nullptr;
    const char* new_owner = nullptr;
    if (!reader_string(&r, &name, nullptr) || !reader_string(&r, &old_owner, nullptr) ||
        !reader_string(&r, &new_owner, nullptr))
        return;
    watch->handler(watch->context, watch->name, new_owner[0] != 0);
}

void on_owner_reply(void* context, Status status, const Message*) noexcept {
    Watch* watch = watch_of(reinterpret_cast<usize>(context));
    if (!watch)
        return;
    if (status != Status::ok && status != Status::error_reply)
        return;  // a timeout or a lost connection says nothing about the name
    watch->handler(watch->context, watch->name, status == Status::ok);
}

// Properties ------------------------------------------------------------------------------------------------------------

// The position of the property `name` of `interface` in the object's list, or ~0u when it has none such.
u32 property_index(const ExportedObject& object, const char* interface, const char* name) noexcept {
    if (!object.properties || !c_string_equal(object.interface, interface))
        return ~0u;
    u32 index = 0;
    for (const char* property = object.properties; *property; property += c_string_length(property) + 1, ++index)
        if (c_string_equal(property, name))
            return index;
    return ~0u;
}

// Portal requests ---------------------------------------------------------------------------------------------------------

u32 g_request_counter;

void on_response(void* context, const Message& signal) noexcept {
    PortalRequest* request = static_cast<PortalRequest*>(context);
    if (!request->active)
        return;
    Reader r;
    message_reader(signal, &r);
    u32 code = 2;  // left as is when the response has no code
    reader_u32(&r, &code);
    const PortalResponseHandler handler = request->handler;
    void* handler_context = request->context;
    portal_request_end(request);
    handler(handler_context, code, &r);
}

bool listen(PortalRequest* request) noexcept {
    const SignalMatch match{kPortalName, request->path, "org.freedesktop.portal.Request", "Response", nullptr};
    request->subscription = connection_subscribe(request->connection, match, on_response, request);
    return request->subscription != 0;
}

} // namespace

bool connection_release_name(Connection* c, const char* name, u32 timeout_ms) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, name);
    const CallTarget target{kBusName, kBusPath, kBusName, "ReleaseName"};
    Message reply;
    const Status status = connection_call_sync(c, target, &body, timeout_ms, &reply);
    builder_release(&body);
    bool released = false;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        u32 result = 0;
        released = reader_u32(&r, &result) && result == 1;
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return released;
}

u32 connection_watch_name(Connection* c, const char* name, NameWatchHandler handler, void* context) noexcept {
    const u32 length = c_string_length(name);
    if (length >= sizeof(Watch::name))
        return 0;
    for (u32 i = 0; i < kMaxWatches; ++i) {
        Watch& watch = g_watches[i];
        if (watch.used)
            continue;
        watch = Watch{};
        watch.used = true;
        watch.connection = c;
        watch.serial = ++g_watch_serial & 0xFFFFFF;
        watch.handler = handler;
        watch.context = context;
        copy_bytes(watch.name, name, length + 1);
        const usize token = (static_cast<usize>(watch.serial) << 8) | (i + 1);
        const SignalMatch match{kBusName, kBusPath, kBusName, "NameOwnerChanged", watch.name};
        watch.subscription = connection_subscribe(c, match, on_owner_changed, reinterpret_cast<void*>(token));
        if (!watch.subscription) {
            watch.used = false;
            return 0;
        }
        Builder body;
        builder_init(&body);
        builder_string(&body, watch.name);
        const CallTarget target{kBusName, kBusPath, kBusName, "GetNameOwner"};
        connection_call(c, target, &body, 10000, on_owner_reply, reinterpret_cast<void*>(token));
        builder_release(&body);
        return static_cast<u32>(token);
    }
    return 0;
}

void connection_unwatch_name(Connection* c, u32 id) noexcept {
    Watch* watch = watch_of(id);
    if (!watch)
        return;
    connection_unsubscribe(c, watch->subscription);
    watch->used = false;
}

bool object_handle_standard(Connection* c, const Message& call, const ExportedObject& object) noexcept {
    if (!call.interface)
        return false;
    if (c_string_equal(call.interface, "org.freedesktop.DBus.Introspectable") && c_string_equal(call.member, "Introspect")) {
        if (!object.introspection) {
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.UnknownMethod", "no introspection data");
            return true;
        }
        // The object's own interfaces, in a node with the standard ones this file answers: Properties where the object
        // has any, and Introspectable.
        char text[3072];
        u32 used = 0;
        const char* const parts[] = {"<node>", object.introspection,
                                     object.properties ? "<interface name=\"org.freedesktop.DBus.Properties\"/>" : "",
                                     "<interface name=\"org.freedesktop.DBus.Introspectable\"/></node>"};
        for (const char* part : parts) {
            const u32 length = c_string_length(part);
            if (length > sizeof(text) - used) {
                connection_reply_error(c, call, "org.freedesktop.DBus.Error.Failed", "introspection too long");
                return true;
            }
            copy_bytes(text + used, part, length);
            used += length;
        }
        Builder body;
        builder_init(&body);
        builder_string(&body, text, used);
        connection_reply(c, call, &body);
        builder_release(&body);
        return true;
    }
    if (!c_string_equal(call.interface, "org.freedesktop.DBus.Properties"))
        return false;
    Reader r;
    message_reader(call, &r);
    const char* interface = nullptr;
    const char* name = nullptr;
    if (c_string_equal(call.member, "Get")) {
        if (!reader_string(&r, &interface, nullptr) || !reader_string(&r, &name, nullptr)) {
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "Get(ss)");
            return true;
        }
        Builder body;
        builder_init(&body);
        const u32 index = property_index(object, interface, name);
        if (index != ~0u)
            object.write(index, &body);
        if (index == ~0u || !builder_ok(body)) {
            builder_release(&body);
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
            return true;
        }
        connection_reply(c, call, &body);
        builder_release(&body);
        return true;
    }
    if (c_string_equal(call.member, "GetAll")) {
        if (!reader_string(&r, &interface, nullptr)) {
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "GetAll(s)");
            return true;
        }
        Builder body;
        builder_init(&body);
        builder_open_array(&body, "{sv}");
        if (object.properties && c_string_equal(object.interface, interface)) {
            u32 index = 0;
            for (const char* property = object.properties; *property; property += c_string_length(property) + 1, ++index) {
                builder_open_entry(&body);
                builder_string(&body, property);
                object.write(index, &body);
                builder_close_entry(&body);
            }
        }
        builder_close_array(&body);
        if (!builder_ok(body)) {
            builder_release(&body);
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.Failed", "a property could not be read");
            return true;
        }
        connection_reply(c, call, &body);
        builder_release(&body);
        return true;
    }
    if (c_string_equal(call.member, "Set")) {
        connection_reply_error(c, call, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only property");
        return true;
    }
    return false;
}

bool portal_request_begin(PortalRequest* request, Connection* c, PortalResponseHandler handler, void* context) noexcept {
    *request = PortalRequest{};
    request->connection = c;
    request->handler = handler;
    request->context = context;
    // The token: maicord_<n>. The path: .../request/<sender, ':' dropped and '.' as '_'>/<token>.
    const char prefix[] = "maicord_";
    copy_bytes(request->token, prefix, sizeof(prefix) - 1);
    u32 length = sizeof(prefix) - 1;
    length += decimal_write(request->token + length, ++g_request_counter);
    request->token[length] = 0;
    const char base[] = "/org/freedesktop/portal/desktop/request/";
    copy_bytes(request->path, base, sizeof(base) - 1);
    length = sizeof(base) - 1;
    const char* sender = connection_unique_name(c);
    for (const char* p = sender; *p && length < sizeof(request->path) - 48; ++p) {
        if (*p == ':')
            continue;
        request->path[length++] = *p == '.' ? '_' : *p;
    }
    request->path[length++] = '/';
    copy_bytes(request->path + length, request->token, c_string_length(request->token) + 1);
    if (!listen(request))
        return false;
    request->active = true;
    return true;
}

void portal_request_end(PortalRequest* request) noexcept {
    if (request->active || request->subscription)
        connection_unsubscribe(request->connection, request->subscription);
    request->subscription = 0;
    request->active = false;
}

void portal_request_adopt_reply(PortalRequest* request, const Message& reply) noexcept {
    Reader r;
    message_reader(reply, &r);
    const char* path = nullptr;
    if (!reader_string(&r, &path, nullptr) || !request->active || c_string_equal(path, request->path) ||
        c_string_length(path) >= sizeof(request->path))
        return;
    connection_unsubscribe(request->connection, request->subscription);
    copy_bytes(request->path, path, c_string_length(path) + 1);
    if (!listen(request))
        request->active = false;
}

void portal_request_append_token(Builder* options, const PortalRequest& request) noexcept {
    builder_dict_string(options, "handle_token", request.token);
}

void portal_request_close(PortalRequest* request) noexcept {
    if (!request->active)
        return;
    const CallTarget target{kPortalName, request->path, "org.freedesktop.portal.Request", "Close"};
    connection_call(request->connection, target, nullptr, 0, nullptr, nullptr);
}

} // namespace maicord::dbus
