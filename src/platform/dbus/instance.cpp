#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/instance.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/core/test_hook.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord::dbus {
namespace {

constexpr long kCallSocket = 41;
constexpr long kCallConnect = 42;
constexpr long kCallBind = 49;
constexpr long kCallListen = 50;
constexpr long kCallGetsockopt = 55;
constexpr long kCallAccept4 = 288;
constexpr long kCallGeteuid = 107;
constexpr int kAfUnix = 1;
constexpr int kSockStream = 1;
constexpr int kSockNonblock = 0x800;
constexpr int kSockCloexec = 0x80000;
constexpr int kSolSocket = 1;
constexpr int kSoPeercred = 17;
constexpr int kErrorInProgress = 115;
constexpr i16 kPollOut = 4;
constexpr const char kApplicationInterface[] = "org.freedesktop.Application";
constexpr u32 kSocketMagic = 0x3149434Du;  // "MCI1"
constexpr u32 kSocketHeaderBytes = 12;

struct Instance {
    bool primary;
    bool bus_backend;
    bool hook_added;
    char name[160];
    char path[200];
    InstanceHandler handler;
    void* context;
    bool pending;
    LaunchRequest pending_request;
    int listen_fd;
    Waitable waitable;
    bool listening;
};

Instance g;

// The interface only: object_handle_standard (bus.cpp) adds the node and the standard interfaces.
const char kApplicationIntrospection[] =
    "<interface name=\"org.freedesktop.Application\">"
    "<method name=\"Activate\"><arg type=\"a{sv}\" name=\"platform_data\" direction=\"in\"/></method>"
    "<method name=\"Open\"><arg type=\"as\" name=\"uris\" direction=\"in\"/><arg type=\"a{sv}\" name=\"platform_data\" direction=\"in\"/></method>"
    "<method name=\"ActivateAction\"><arg type=\"s\" name=\"action_name\" direction=\"in\"/><arg type=\"av\" name=\"parameter\" direction=\"in\"/>"
    "<arg type=\"a{sv}\" name=\"platform_data\" direction=\"in\"/></method>"
    "</interface>";

u64 hash_of(const char* a, const char* b) noexcept {
    u64 hash = 0xCBF29CE484222325ull;
    const auto mix = [&](const char* text) {
        for (u32 i = 0; text[i]; ++i)
            hash = (hash ^ static_cast<u8>(text[i])) * 0x100000001B3ull;
        hash = (hash ^ 0xFF) * 0x100000001B3ull;
    };
    mix(a);
    mix(b);
    return hash;
}

void hex16(u64 value, char* out) noexcept {
    for (u32 i = 0; i < 16; ++i)
        out[i] = "0123456789abcdef"[(value >> ((15 - i) * 4)) & 15];
}

// A request, delivered now or kept until a handler is set.
void deliver(const LaunchRequest& request) noexcept {
    if (g.handler) {
        g.handler(g.context, request);
        return;
    }
    g.pending = true;
    g.pending_request = request;
}

// org.freedesktop.Application ---------------------------------------------------------------------------------------------

// The activation token of a platform-data dictionary, at the reader's `a{sv}`.
void read_platform_data(Reader* r, char (&token)[kLaunchTokenCapacity]) noexcept {
    token[0] = 0;
    if (!reader_enter_array(r))
        return;
    while (reader_more(*r)) {
        const char* key = nullptr;
        const char* signature = nullptr;
        if (!reader_enter_struct(r) || !reader_string(r, &key, nullptr) || !reader_enter_variant(r, &signature))
            break;
        if (signature[0] == 's' &&
            (c_string_equal(key, "activation-token") || (c_string_equal(key, "desktop-startup-id") && !token[0]))) {
            const char* value = nullptr;
            if (reader_string(r, &value, nullptr))
                c_string_copy(token, sizeof(token), value);
        }
        if (!reader_leave(r) || !reader_leave(r))  // the variant, then the entry
            break;
    }
}

bool application_call(void*, Connection* c, const Message& call) noexcept {
    const ExportedObject object{nullptr, nullptr, nullptr, kApplicationIntrospection};
    if (object_handle_standard(c, call, object))
        return true;
    if (!call.interface || !c_string_equal(call.interface, kApplicationInterface))
        return false;
    Reader r;
    message_reader(call, &r);
    LaunchRequest request{};
    if (c_string_equal(call.member, "Activate")) {
        read_platform_data(&r, request.activation_token);
    } else if (c_string_equal(call.member, "Open")) {
        if (!reader_enter_array(&r)) {
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "Open(as a{sv})");
            return true;
        }
        const char* uri = nullptr;
        u32 length = 0;
        if (reader_more(r) && reader_string(&r, &uri, &length) && length < kLaunchUrlCapacity) {
            copy_bytes(request.url, uri, length + 1);
            request.url_bytes = length;
            request.url_present = true;
        }
        if (!reader_leave(&r)) {
            connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "Open(as a{sv})");
            return true;
        }
        read_platform_data(&r, request.activation_token);
    } else if (c_string_equal(call.member, "ActivateAction")) {
        // Actions are not defined: the application comes forward.
    } else {
        return false;
    }
    connection_reply(c, call, nullptr);
    deliver(request);
    return true;
}

void export_application(Connection* c) noexcept {
    connection_export_object(c, g.path, application_call, nullptr);
}

// The bus backend: take the name again after a reconnect.
void on_session(Connection* c) noexcept {
    if (!g.primary || !g.bus_backend)
        return;
    if (connection_request_name(c, g.name, 3000))
        export_application(c);
    else
        log_write(LogLevel::warning, "instance", "the application's bus name was lost");
}

// RequestName with DO_NOT_QUEUE: 1 primary owner, 4 already ours, 3 owned by another; 0 on an error.
u32 request_name(Connection* c, const char* name) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, name);
    builder_u32(&body, 4);
    const CallTarget target{"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName"};
    Message reply;
    const Status status = connection_call_sync(c, target, &body, 5000, &reply);
    builder_release(&body);
    u32 result = 0;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        reader_u32(&r, &result);  // stays 0 when the reply holds no code
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return result;
}

InstanceResult forward_over_bus(Connection* c, const LaunchRequest& request, u32 timeout_ms) noexcept {
    Builder body;
    builder_init(&body);
    const bool open = request.url_present;
    if (open) {
        builder_open_array(&body, "s");
        builder_string(&body, request.url, request.url_bytes);
        builder_close_array(&body);
    }
    builder_open_array(&body, "{sv}");
    if (request.activation_token[0])
        builder_dict_string(&body, "activation-token", request.activation_token);
    builder_close_array(&body);
    const CallTarget target{g.name, g.path, kApplicationInterface, open ? "Open" : "Activate"};
    Message reply;
    const Status status = connection_call_sync(c, target, &body, timeout_ms, &reply);
    builder_release(&body);
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return status == Status::ok ? InstanceResult::forwarded : InstanceResult::refused;
}

// The socket backend ------------------------------------------------------------------------------------------------------

SocketAddress socket_address(const char* application_id, const char* profile_key) noexcept {
    SocketAddress address{};
    address.data[0] = kAfUnix;
    const char prefix[] = "maicord.instance.";
    u32 length = 3;  // family, then the abstract name's leading NUL
    copy_bytes(address.data + length, prefix, sizeof(prefix) - 1);
    length += sizeof(prefix) - 1;
    length += decimal_write(reinterpret_cast<char*>(address.data + length), static_cast<u32>(syscall1(kCallGeteuid, 0)));
    address.data[length++] = '.';
    char hex[16];
    hex16(hash_of(application_id, profile_key), hex);
    copy_bytes(address.data + length, hex, 16);
    length += 16;
    address.length = length;
    return address;
}

bool poll_until(int fd, i16 events, u64 deadline) noexcept {
    for (;;) {
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            return false;
        const u64 left = deadline - now;
        const TimeSpec timeout{static_cast<i64>(left / 1000), static_cast<i64>(left % 1000) * 1000000};
        PollFd poll{fd, events, 0};
        const long ready = sys_ppoll(&poll, 1, &timeout);
        if (ready > 0)
            return true;
        if (ready < 0 && -ready != kErrorInterrupted)
            return false;
    }
}

bool write_all(int fd, const u8* bytes, u32 count, u64 deadline) noexcept {
    u32 done = 0;
    while (done != count) {
        const long sent = sys_write(fd, bytes + done, count - done);
        if (sent == -kErrorInterrupted)
            continue;
        if (sent == -kErrorAgain) {
            if (!poll_until(fd, kPollOut, deadline))
                return false;
            continue;
        }
        if (syscall_failed(sent) || sent == 0)
            return false;
        done += static_cast<u32>(sent);
    }
    return true;
}

bool read_all(int fd, u8* bytes, u32 count, u64 deadline) noexcept {
    u32 done = 0;
    while (done != count) {
        const long got = sys_read(fd, bytes + done, count - done);
        if (got == -kErrorInterrupted)
            continue;
        if (got == -kErrorAgain) {
            if (!poll_until(fd, kPollIn, deadline))
                return false;
            continue;
        }
        if (syscall_failed(got) || got == 0)
            return false;
        done += static_cast<u32>(got);
    }
    return true;
}

// magic, flags (bit 0 startup, bit 1 url), url bytes, token bytes, then the url and the token.
u32 encode_request(const LaunchRequest& request, u8* out) noexcept {
    const u32 token_bytes = c_string_length(request.activation_token);
    const u32 flags = (request.startup ? 1u : 0u) | (request.url_present ? 2u : 0u);
    const u32 url_bytes = request.url_present ? request.url_bytes : 0;
    copy_bytes(out, &kSocketMagic, 4);
    out[4] = static_cast<u8>(flags);
    out[5] = static_cast<u8>(url_bytes);
    out[6] = static_cast<u8>(url_bytes >> 8);
    out[7] = static_cast<u8>(token_bytes);
    out[8] = out[9] = out[10] = out[11] = 0;
    copy_bytes(out + kSocketHeaderBytes, request.url, url_bytes);
    copy_bytes(out + kSocketHeaderBytes + url_bytes, request.activation_token, token_bytes);
    return kSocketHeaderBytes + url_bytes + token_bytes;
}

InstanceResult forward_over_socket(int fd, const LaunchRequest& request, u32 timeout_ms) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + timeout_ms;
    u8 message[kSocketHeaderBytes + kLaunchUrlCapacity + kLaunchTokenCapacity];
    const u32 bytes = encode_request(request, message);
    u8 answer = 0;
    const bool ok = write_all(fd, message, bytes, deadline) && read_all(fd, &answer, 1, deadline) && answer == 'K';
    return ok ? InstanceResult::forwarded : InstanceResult::refused;
}

bool peer_is_us(int fd) noexcept {
    struct Credentials {
        i32 pid;
        u32 uid;
        u32 gid;
    } credentials{};
    u32 size = sizeof(credentials);
    const long result = syscall5(kCallGetsockopt, fd, kSolSocket, kSoPeercred, reinterpret_cast<long>(&credentials),
                                 reinterpret_cast<long>(&size));
    return !syscall_failed(result) && credentials.uid == static_cast<u32>(syscall1(kCallGeteuid, 0));
}

void serve_connection(int fd) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + 300;
    u8 header[kSocketHeaderBytes];
    if (!peer_is_us(fd) || !read_all(fd, header, kSocketHeaderBytes, deadline))
        return;
    u32 magic = 0;
    copy_bytes(&magic, header, 4);
    const u32 url_bytes = header[5] | (static_cast<u32>(header[6]) << 8);
    const u32 token_bytes = header[7];
    if (magic != kSocketMagic || url_bytes >= kLaunchUrlCapacity || token_bytes >= kLaunchTokenCapacity)
        return;
    u8 payload[kLaunchUrlCapacity + kLaunchTokenCapacity];
    if (!read_all(fd, payload, url_bytes + token_bytes, deadline))
        return;
    LaunchRequest request{};
    request.startup = (header[4] & 1) != 0;
    request.url_present = (header[4] & 2) != 0;
    if (request.url_present) {
        copy_bytes(request.url, payload, url_bytes);
        request.url_bytes = url_bytes;
    }
    copy_bytes(request.activation_token, payload + url_bytes, token_bytes);
    const u8 ok = 'K';
    write_all(fd, &ok, 1, deadline);
    deliver(request);
}

// Binds the abstract name: true when this process now owns it; false with *in_use when another process does.
bool listen_socket(const SocketAddress& address, bool* in_use) noexcept {
    *in_use = false;
    const long created = syscall3(kCallSocket, kAfUnix, kSockStream | kSockNonblock | kSockCloexec, 0);
    if (syscall_failed(created))
        return false;
    const int fd = static_cast<int>(created);
    const long bound = syscall3(kCallBind, fd, reinterpret_cast<long>(address.data), address.length);
    if (syscall_failed(bound)) {
        *in_use = -bound == kErrorAddressInUse;
        sys_close(fd);
        return false;
    }
    if (syscall_failed(syscall2(kCallListen, fd, 8))) {
        sys_close(fd);
        return false;
    }
    g.listen_fd = fd;
    g.listening = true;
    return true;
}

InstanceResult forward_to_socket_owner(const SocketAddress& address, const LaunchRequest& request, u32 timeout_ms) noexcept {
    const long created = syscall3(kCallSocket, kAfUnix, kSockStream | kSockNonblock | kSockCloexec, 0);
    if (syscall_failed(created))
        return InstanceResult::refused;
    const int fd = static_cast<int>(created);
    long connected = syscall3(kCallConnect, fd, reinterpret_cast<long>(address.data), address.length);
    if (connected == -kErrorAgain || connected == -kErrorInProgress) {
        connected = poll_until(fd, kPollOut, clock_monotonic_milliseconds() + timeout_ms) ? 0 : -kErrorTimedOut;
    }
    InstanceResult result = InstanceResult::refused;
    if (!syscall_failed(connected))
        result = forward_over_socket(fd, request, timeout_ms);
    sys_close(fd);
    return result;
}

} // namespace

void instance_socket_address(const char* application_id, const char* profile_key, SocketAddress* address) noexcept {
    *address = socket_address(application_id, profile_key ? profile_key : "");
}

bool instance_bus_name(const char* application_id, const char* profile_key, char* name, u32 name_capacity, char* path,
                       u32 path_capacity) noexcept {
    if (!application_id)
        return false;
    const u32 length = c_string_length(application_id);
    u32 elements = 1;
    bool element_start = true;
    for (u32 i = 0; i < length; ++i) {
        const char c = application_id[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        const bool digit = c >= '0' && c <= '9';
        if (c == '.') {
            if (element_start)
                return false;
            element_start = true;
            ++elements;
            continue;
        }
        if (!alpha && !digit)
            return false;
        if (element_start && digit)
            return false;
        element_start = false;
    }
    if (element_start || elements < 2 || length > 100)
        return false;
    const bool profile = profile_key && *profile_key;
    char suffix[20];
    u32 suffix_length = 0;
    if (profile) {
        suffix[suffix_length++] = '.';
        suffix[suffix_length++] = 'p';
        hex16(hash_of(application_id, profile_key), suffix + suffix_length);
        suffix_length += 16;
    }
    if (length + suffix_length + 1 > name_capacity || length + suffix_length + 2 > path_capacity)
        return false;
    copy_bytes(name, application_id, length);
    copy_bytes(name + length, suffix, suffix_length);
    name[length + suffix_length] = 0;
    // The object path: '/' and the name's elements, '.' as '/', '-' as '_' (a path element has no dash).
    path[0] = '/';
    for (u32 i = 0; i < length + suffix_length; ++i)
        path[i + 1] = name[i] == '.' ? '/' : name[i] == '-' ? '_' : name[i];
    path[length + suffix_length + 1] = 0;
    return true;
}

InstanceResult instance_acquire(const InstanceConfig& config, const LaunchRequest& request, u32 timeout_ms) noexcept {
    if (g.primary)
        return InstanceResult::primary;
    if (!instance_bus_name(config.application_id, config.profile_key, g.name, sizeof(g.name), g.path, sizeof(g.path))) {
        log_write(LogLevel::warning, "instance", "the application id is not a valid bus name");
        return InstanceResult::unguarded;
    }
    // A test forces one backend ("socket": no bus; "bus": no socket); in a shipped client the bus is tried first and
    // the socket is the answer when there is none.
    const char* forced = test_hook("MAICORD_INSTANCE_BACKEND");
    const bool bus_allowed = !(forced && c_string_equal(forced, "socket"));
    const bool socket_allowed = !(forced && c_string_equal(forced, "bus"));

    if (bus_allowed && session_bus_open()) {
        Connection* c = session_bus();
        const u32 owner = c ? request_name(c, g.name) : 0;
        if (owner == 1 || owner == 4) {
            g.primary = true;
            g.bus_backend = true;
            export_application(c);
            if (!g.hook_added) {
                g.hook_added = true;
                session_bus_add_hook(on_session);
            }
            return InstanceResult::primary;
        }
        if (owner == 3)
            return forward_over_bus(c, request, timeout_ms);
        log_write(LogLevel::info, "instance", "the bus did not grant the application's name");
    }
    if (!socket_allowed)
        return InstanceResult::unguarded;

    const SocketAddress address = socket_address(config.application_id, config.profile_key ? config.profile_key : "");
    bool in_use = false;
    if (listen_socket(address, &in_use)) {
        g.primary = true;
        g.waitable = waitable_borrow_fd(g.listen_fd);
        return InstanceResult::primary;
    }
    if (in_use)
        return forward_to_socket_owner(address, request, timeout_ms);
    log_write(LogLevel::warning, "instance", "no single-instance channel could be opened");
    return InstanceResult::unguarded;
}

void instance_set_handler(InstanceHandler handler, void* context) noexcept {
    g.handler = handler;
    g.context = context;
    if (handler && g.pending) {
        g.pending = false;
        const LaunchRequest request = g.pending_request;
        handler(context, request);
    }
}

Waitable instance_waitable() noexcept {
    return g.listening ? g.waitable : nullptr;
}

void instance_process() noexcept {
    if (!g.listening)
        return;
    for (u32 i = 0; i < 4; ++i) {
        const long accepted = syscall4(kCallAccept4, g.listen_fd, 0, 0, kSockNonblock | kSockCloexec);
        if (syscall_failed(accepted))
            return;
        const int fd = static_cast<int>(accepted);
        serve_connection(fd);
        sys_close(fd);
    }
}

void instance_release() noexcept {
    if (g.listening) {
        sys_close(g.listen_fd);
        if (g.waitable)
            waitable_close(g.waitable);
        g.listening = false;
        g.waitable = nullptr;
    }
    if (g.primary && g.bus_backend) {
        if (Connection* c = session_bus()) {
            connection_unexport_object(c, g.path);
            connection_release_name(c, g.name, 1000);
        }
    }
    g.primary = false;
    g.bus_backend = false;
}

} // namespace maicord::dbus
