#include "server_internal.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::atspi {

Server g_server;

namespace {

using namespace dbus;

constexpr const char kStatusName[] = "org.a11y.Bus";
constexpr const char kStatusPath[] = "/org/a11y/bus";
constexpr const char kStatusInterface[] = "org.a11y.Status";
constexpr const char kRegistryName[] = "org.a11y.atspi.Registry";
constexpr u32 kCallTimeoutMs = 10000;

bool equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return false;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

void disconnect_bus() noexcept {
    Server& s = g_server;
    if (s.bus) {
        connection_close(s.bus);
        log_write(LogLevel::trace, "atspi", "left the accessibility bus");
    }
    s.bus = nullptr;
    s.embedded = false;
    s.endpoint.bus_name[0] = 0;
    s.endpoint.parent_name[0] = 0;
    s.endpoint.parent_path[0] = 0;
    s.endpoint.registry_sender[0] = 0;
    s.endpoint.queried = false;
    events_reset();
}

// Calls arriving on the accessibility bus.
bool on_call(void*, Connection* connection, const Message& call) noexcept {
    Server& s = g_server;
    Builder body;
    builder_init(&body);
    Failure failure{};
    const Outcome outcome = answer_call(&s.endpoint, call, &body, &failure);
    bool handled = true;
    if (outcome == Outcome::replied)
        connection_reply(connection, call, &body);
    else if (outcome == Outcome::failed)
        connection_reply_error(connection, call, failure.name, failure.text);
    else
        handled = false;
    builder_release(&body);
    return handled;
}

void on_embed(void*, Status status, const Message* reply) noexcept {
    Server& s = g_server;
    if (status != Status::ok || !reply || !s.bus) {
        log_write(LogLevel::warning, "atspi", "the registry refused Socket.Embed");
        return;
    }
    Reader reader;
    message_reader(*reply, &reader);
    const char* name = nullptr;
    const char* path = nullptr;
    if (reader_enter_struct(&reader) && reader_string(&reader, &name, nullptr) && reader_string(&reader, &path, nullptr)) {
        c_string_copy(s.endpoint.parent_name, sizeof(s.endpoint.parent_name), name);
        c_string_copy(s.endpoint.parent_path, sizeof(s.endpoint.parent_path), path);
    }
    c_string_copy(s.endpoint.registry_sender, sizeof(s.endpoint.registry_sender), reply->sender);
    s.embedded = true;
    log_write(LogLevel::trace, "atspi", "registered with the accessibility registry");
}

// The registry is (again) on the bus: hand it the application's root.
void on_registry(void*, const char*, bool owned) noexcept {
    Server& s = g_server;
    if (!s.bus)
        return;
    if (!owned) {
        s.embedded = false;
        return;
    }
    Builder body;
    builder_init(&body);
    append_reference(&body, s.endpoint.bus_name, kRootPath);
    const CallTarget target{kRegistryName, kRootPath, "org.a11y.atspi.Socket", "Embed"};
    connection_call(s.bus, target, &body, kCallTimeoutMs, on_embed, nullptr);
    builder_release(&body);
}

void connect_bus(const char* address) noexcept {
    Server& s = g_server;
    if (s.bus)
        return;
    Connection* bus = connection_open_address(address);
    if (!bus) {
        log_write(LogLevel::warning, "atspi", "the accessibility bus cannot be reached");
        return;
    }
    s.bus = bus;
    c_string_copy(s.endpoint.bus_name, sizeof(s.endpoint.bus_name), connection_unique_name(bus));
    connection_set_method_handler(bus, on_call, nullptr);
    connection_watch_name(bus, kRegistryName, on_registry, nullptr);
    log_write(LogLevel::trace, "atspi", "connected to the accessibility bus");
}

void on_address(void*, Status status, const Message* reply) noexcept {
    Server& s = g_server;
    s.address_pending = false;
    if (status != Status::ok || !reply || !s.started)
        return;
    Reader reader;
    message_reader(*reply, &reader);
    const char* address = nullptr;
    if (reader_string(&reader, &address, nullptr) && address && *address)
        connect_bus(address);
}

void request_address() noexcept {
    Server& s = g_server;
    if (!s.session || s.bus || s.address_pending)
        return;
    const CallTarget target{kStatusName, kStatusPath, kStatusName, "GetAddress"};
    s.address_pending = connection_call(s.session, target, nullptr, kCallTimeoutMs, on_address, nullptr);
}

// IsEnabled (the desktop's accessibility switch) or ScreenReaderEnabled (a screen reader runs) asks for the server.
void on_status(void*, Status status, const Message* reply) noexcept {
    if (status != Status::ok || !reply)
        return;
    Reader reader;
    message_reader(*reply, &reader);
    bool wanted = false;
    if (reader_enter_array(&reader)) {
        while (reader_more(reader)) {
            const char* key = nullptr;
            const char* signature = nullptr;
            bool value = false;
            if (!reader_enter_struct(&reader) || !reader_string(&reader, &key, nullptr) ||
                !reader_enter_variant(&reader, &signature))
                break;
            if (signature && equal(signature, "b") && reader_bool(&reader, &value) &&
                (equal(key, "IsEnabled") || equal(key, "ScreenReaderEnabled")))
                wanted = wanted || value;
            if (!reader_leave(&reader) || !reader_leave(&reader))
                break;
        }
    }
    if (wanted)
        request_address();
    else
        disconnect_bus();
}

// Without starting the service: when it is not running nobody wants accessibility, and its appearance is announced by the
// name watch.
void request_status() noexcept {
    Server& s = g_server;
    if (!s.session)
        return;
    Builder body;
    builder_init(&body);
    builder_string(&body, kStatusInterface);
    const CallTarget target{kStatusName, kStatusPath, "org.freedesktop.DBus.Properties", "GetAll"};
    connection_call(s.session, target, &body, kCallTimeoutMs, on_status, nullptr, flag_no_auto_start);
    builder_release(&body);
}

void on_status_changed(void*, const Message&) noexcept {
    request_status();
}

void on_bus_name(void*, const char*, bool owned) noexcept {
    if (owned)
        request_status();
    else
        disconnect_bus();
}

void on_session(Connection* session) noexcept {
    Server& s = g_server;
    if (!s.started)
        return;
    s.session = session;
    s.address_pending = false;
    s.name_watch = connection_watch_name(session, kStatusName, on_bus_name, nullptr);
    const SignalMatch match{kStatusName, kStatusPath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            kStatusInterface};
    s.status_subscription = connection_subscribe(session, match, on_status_changed, nullptr);
}

} // namespace

bool server_start(const HostCallbacks& host) noexcept {
    Server& s = g_server;
    if (s.started)
        return false;
    s.endpoint.host = host;
    s.started = true;
    if (!session_bus_add_hook(on_session)) {
        s.started = false;
        return false;
    }
    session_bus_open();
    return true;
}

void server_stop() noexcept {
    Server& s = g_server;
    if (!s.started)
        return;
    s.started = false;
    disconnect_bus();
    if (s.session && connection_alive(s.session)) {
        connection_unwatch_name(s.session, s.name_watch);
        connection_unsubscribe(s.session, s.status_subscription);
    }
    s.session = nullptr;
    s.name_watch = s.status_subscription = 0;
}

bool server_connected() noexcept {
    return g_server.bus && g_server.embedded;
}

bool server_queried() noexcept {
    return g_server.bus && g_server.endpoint.queried;
}

Waitable server_waitable() noexcept {
    return g_server.bus ? connection_waitable(g_server.bus) : nullptr;
}

u32 server_wait_ms(u64 now_ms) noexcept {
    return g_server.bus ? connection_wait_ms(g_server.bus, now_ms) : kNoTimeout;
}

void server_process(u64 now_ms) noexcept {
    Server& s = g_server;
    if (!s.bus)
        return;
    connection_process(s.bus, now_ms);
    if (!connection_alive(s.bus)) {
        log_write(LogLevel::warning, "atspi", "the accessibility bus connection ended");
        disconnect_bus();
    }
}

} // namespace maicord::atspi
