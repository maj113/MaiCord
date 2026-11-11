#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::dbus {
namespace {

// Linux x86-64 system calls and constants beyond sys/linux/calls.hpp.
constexpr long kCallSocket = 41;
constexpr long kCallConnect = 42;
constexpr long kCallSendmsg = 46;
constexpr long kCallRecvmsg = 47;
constexpr long kCallGeteuid = 107;
constexpr int kAfUnix = 1;
constexpr int kSockStream = 1;
constexpr int kSockNonblock = 0x800;
constexpr int kSockCloexec = 0x80000;
constexpr int kSolSocket = 1;
constexpr int kScmRights = 1;
constexpr int kMsgPeek = 0x2;
constexpr int kMsgDontwait = 0x40;
constexpr int kMsgNosignal = 0x4000;
constexpr int kMsgCmsgCloexec = 0x40000000;
constexpr i16 kPollOut = 4;
constexpr int kErrorInProgress = 115;

struct IoVec {
    void* base;
    usize length;
};

struct MsgHdr {
    void* name;
    u32 name_length;
    IoVec* iov;
    usize iov_length;
    void* control;
    usize control_length;
    int flags;
};

struct CmsgHdr {
    usize length;
    int level;
    int type;
};

constexpr usize kControlBytes = sizeof(CmsgHdr) + ((kMaxFds * 4 + 7) & ~usize{7});

constexpr u32 kMaxPending = 64;
constexpr u32 kMaxSubscriptions = 32;
constexpr u32 kMaxRoutes = 16;
constexpr u32 kMaxReceivedFds = 2 * kMaxFds;
constexpr u32 kMaxOutputBytes = 8u << 20;
constexpr u32 kHandshakeTimeoutMs = 5000;
constexpr u32 kHelloTimeoutMs = 5000;
constexpr u32 kOutputRetryMs = 2;
constexpr const char* kBusName = "org.freedesktop.DBus";
constexpr const char* kBusPath = "/org/freedesktop/DBus";

struct Pending {
    u32 serial;
    ReplyHandler handler;
    void* context;
    u64 deadline;  // monotonic milliseconds; ~0 for none
};

struct Subscription {
    bool active;
    SignalHandler handler;
    void* context;
    char path[128];
    char interface[96];
    char member[64];
    char arg0[96];
    char rule[400];  // the bus's rule, for RemoveMatch
};

struct Route {
    bool used;
    MethodHandler handler;
    void* context;
    char path[160];
};

} // namespace

struct Connection {
    int fd;
    bool alive;
    bool fds_ok;
    char unique_name[64];
    u32 next_serial;
    Waitable waitable;
    u8* input;
    u32 input_used;
    u32 input_capacity;
    int received_fds[kMaxReceivedFds];
    u32 received_fd_count;
    u8* output;
    u32 output_used;
    u32 output_capacity;
    Pending pending[kMaxPending];
    u32 pending_count;
    Subscription subscriptions[kMaxSubscriptions];
    u32 subscription_count;
    MethodHandler method_handler;
    void* method_context;
    Route routes[kMaxRoutes];
};

// Waits for the descriptor until `deadline` (monotonic ms); false on timeout or error.
bool poll_until(int fd, i16 events, u64 deadline) noexcept {
    for (;;) {
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            return false;
        const u64 left = deadline - now;
        TimeSpec timeout{static_cast<i64>(left / 1000), static_cast<i64>(left % 1000) * 1000000};
        PollFd poll{fd, events, 0};
        const long ready = sys_ppoll(&poll, 1, &timeout);
        if (ready > 0)
            return true;
        if (ready < 0 && -ready != kErrorInterrupted)
            return false;
    }
}

namespace {

// One sendmsg of `bytes` carrying `fd_count` descriptors with the first byte; the bytes sent, or a negated errno.
long send_some(int fd, const u8* bytes, u32 count, const int* fds, u32 fd_count) noexcept {
    IoVec vector{const_cast<u8*>(bytes), count};
    alignas(8) u8 control[kControlBytes];
    MsgHdr header{};
    header.iov = &vector;
    header.iov_length = 1;
    if (fd_count) {
        clear_bytes(control, sizeof(control));
        CmsgHdr* cmsg = reinterpret_cast<CmsgHdr*>(control);
        cmsg->length = sizeof(CmsgHdr) + fd_count * 4;
        cmsg->level = kSolSocket;
        cmsg->type = kScmRights;
        copy_bytes(control + sizeof(CmsgHdr), fds, fd_count * 4);
        header.control = control;
        header.control_length = sizeof(CmsgHdr) + ((fd_count * 4 + 7) & ~7u);
    }
    return syscall3(kCallSendmsg, fd, reinterpret_cast<long>(&header), kMsgNosignal | kMsgDontwait);
}

} // namespace

bool send_all_blocking(int fd, const u8* bytes, u32 count, u64 deadline) noexcept {
    u32 done = 0;
    while (done != count) {
        const long sent = send_some(fd, bytes + done, count - done, nullptr, 0);
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

namespace {

// Reads one line ended by CRLF (without it) into `line`; false on timeout, error or an overlong line. A peek finds the
// line end and a read takes the bytes through it, never past it: two system calls a line, not one a byte.
bool read_line(int fd, char* line, u32 capacity, u64 deadline) noexcept {
    u32 length = 0;
    for (;;) {
        const long got = sys_recv(fd, line + length, capacity - length, kMsgPeek);
        if (got == -kErrorInterrupted)
            continue;
        if (got == -kErrorAgain) {
            if (!poll_until(fd, kPollIn, deadline))
                return false;
            continue;
        }
        if (syscall_failed(got) || got == 0)
            return false;
        u32 take = 0;
        bool ended = false;
        while (take < static_cast<u32>(got) && !ended)
            ended = line[length + take++] == '\n';
        long taken = sys_read(fd, line + length, take);
        while (taken == -kErrorInterrupted)
            taken = sys_read(fd, line + length, take);
        if (taken != static_cast<long>(take))
            return false;
        length += take;
        if (ended) {
            --length;
            if (length && line[length - 1] == '\r')
                --length;
            line[length] = 0;
            return true;
        }
        if (length >= capacity)
            return false;
    }
}

bool starts_with(const char* text, const char* prefix) noexcept {
    while (*prefix && *text == *prefix)
        ++text, ++prefix;
    return *prefix == 0;
}

// The authentication exchange of the specification, with EXTERNAL (the uid as hex of its decimal digits).
bool authenticate(int fd, bool* fds_ok) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + kHandshakeTimeoutMs;
    char decimal[10];
    const u32 digits = decimal_write(decimal, static_cast<u32>(syscall1(kCallGeteuid, 0)));
    char request[64];
    u32 length = 0;
    request[length++] = 0;  // the credentials byte
    const char verb[] = "AUTH EXTERNAL ";
    copy_bytes(request + length, verb, sizeof(verb) - 1);
    length += sizeof(verb) - 1;
    for (u32 i = 0; i < digits; ++i) {
        request[length++] = "0123456789abcdef"[static_cast<u8>(decimal[i]) >> 4];
        request[length++] = "0123456789abcdef"[static_cast<u8>(decimal[i]) & 15];
    }
    request[length++] = '\r';
    request[length++] = '\n';
    if (!send_all_blocking(fd, reinterpret_cast<const u8*>(request), length, deadline))
        return false;
    char line[256];
    if (!read_line(fd, line, sizeof(line), deadline) || !starts_with(line, "OK ")) {
        log_write(LogLevel::warning, "dbus", "the bus refused EXTERNAL authentication");
        return false;
    }
    *fds_ok = false;
    const char negotiate[] = "NEGOTIATE_UNIX_FD\r\n";
    if (!send_all_blocking(fd, reinterpret_cast<const u8*>(negotiate), sizeof(negotiate) - 1, deadline) ||
        !read_line(fd, line, sizeof(line), deadline))
        return false;
    *fds_ok = c_string_equal(line, "AGREE_UNIX_FD");
    const char begin[] = "BEGIN\r\n";
    return send_all_blocking(fd, reinterpret_cast<const u8*>(begin), sizeof(begin) - 1, deadline);
}

void close_descriptors(int* fds, u32* count) noexcept {
    for (u32 i = 0; i < *count; ++i)
        sys_close(fds[i]);
    *count = 0;
}

void disconnect(Connection* c) noexcept {
    if (!c->alive)
        return;
    c->alive = false;
    // The pending calls end with the connection.
    Pending pending[kMaxPending];
    const u32 count = c->pending_count;
    copy_bytes(pending, c->pending, count * static_cast<u32>(sizeof(Pending)));
    c->pending_count = 0;
    for (u32 i = 0; i < count; ++i)
        pending[i].handler(pending[i].context, Status::disconnected, nullptr);
}

// Queues bytes at the end of the output queue.
bool queue_output(Connection* c, const u8* bytes, u32 count) noexcept {
    if (count > kMaxOutputBytes || c->output_used > kMaxOutputBytes - count)
        return false;
    if (!buffer_reserve(&c->output, c->output_used, &c->output_capacity, c->output_used + count, 4096))
        return false;
    copy_bytes(c->output + c->output_used, bytes, count);
    c->output_used += count;
    return true;
}

// Sends from `bytes` until the socket is full; the bytes sent. A failed socket ends the connection.
u32 send_until_full(Connection* c, const u8* bytes, u32 count) noexcept {
    u32 done = 0;
    while (done != count) {
        const long sent = send_some(c->fd, bytes + done, count - done, nullptr, 0);
        if (sent == -kErrorInterrupted)
            continue;
        if (sent == -kErrorAgain)
            break;
        if (syscall_failed(sent) || sent == 0) {
            disconnect(c);
            break;
        }
        done += static_cast<u32>(sent);
    }
    return done;
}

// Sends as much of the queue as the socket takes (on a live connection).
void flush_output(Connection* c) noexcept {
    const u32 sent = send_until_full(c, c->output, c->output_used);
    if (!c->alive)
        return;
    c->output_used -= sent;
    if (sent && c->output_used)
        copy_bytes(c->output, c->output + sent, c->output_used);
}

// Sends one serialised message, with its descriptors when it has any.
bool transmit(Connection* c, const u8* block, u32 bytes, const Builder& body) noexcept {
    if (!c->alive)
        return false;
    if (body.fd_count) {
        if (!c->fds_ok)
            return false;
        // The descriptors travel with the message's first byte, so nothing may be queued in front of it.
        flush_output(c);
        const u64 deadline = clock_monotonic_milliseconds() + 2000;
        while (c->alive && c->output_used) {
            if (!poll_until(c->fd, kPollOut, deadline))
                return false;
            flush_output(c);
        }
        if (!c->alive)
            return false;
        long sent = send_some(c->fd, block, bytes, body.fds, body.fd_count);
        while (sent == -kErrorInterrupted)
            sent = send_some(c->fd, block, bytes, body.fds, body.fd_count);
        if (sent == -kErrorAgain)
            sent = 0;
        if (syscall_failed(sent)) {
            disconnect(c);
            return false;
        }
        if (sent == 0)
            return false;  // nothing went out (not even the descriptors); the caller may retry
        return static_cast<u32>(sent) == bytes || queue_output(c, block + sent, bytes - static_cast<u32>(sent));
    }
    if (c->output_used) {
        if (!queue_output(c, block, bytes))
            return false;
        flush_output(c);
        return c->alive;
    }
    const u32 done = send_until_full(c, block, bytes);
    if (!c->alive)
        return false;
    return done == bytes || queue_output(c, block + done, bytes - done);
}

u32 take_serial(Connection* c) noexcept {
    u32 serial = c->next_serial++;
    if (serial == 0)
        serial = c->next_serial++;
    return serial;
}

bool send_message(Connection* c, MessageHeader header, const Builder* body, u32* serial) noexcept {
    Builder none;
    if (!body) {
        builder_init(&none);
        body = &none;
    }
    header.serial = take_serial(c);
    u8* block = nullptr;
    u32 bytes = 0;
    if (!message_serialize(header, *body, &block, &bytes))
        return false;
    const bool sent = transmit(c, block, bytes, *body);
    secure_clear_bytes(block, bytes);
    heap_free(block);
    if (sent && serial)
        *serial = header.serial;
    return sent;
}

void dispatch_reply(Connection* c, const Message& message) noexcept {
    for (u32 i = 0; i < c->pending_count; ++i) {
        if (c->pending[i].serial != message.reply_serial)
            continue;
        const Pending call = c->pending[i];
        c->pending[i] = c->pending[--c->pending_count];
        call.handler(call.context, message.type == MessageType::error ? Status::error_reply : Status::ok, &message);
        return;
    }
}

void dispatch_signal(Connection* c, const Message& message) noexcept {
    // Handlers may subscribe and unsubscribe (slots are reused, the table does not move), so the count is read afresh and
    // what a slot holds is copied before its handler runs.
    for (u32 i = 0; i < c->subscription_count; ++i) {
        const Subscription& s = c->subscriptions[i];
        if (!s.active)
            continue;
        if (s.path[0] && !c_string_equal(s.path, message.path))
            continue;
        if (s.interface[0] && (!message.interface || !c_string_equal(s.interface, message.interface)))
            continue;
        if (s.member[0] && (!message.member || !c_string_equal(s.member, message.member)))
            continue;
        if (s.arg0[0]) {
            // The first argument, when it is a string, must equal the filter (the bus applies the same rule to its own
            // delivery, but an overlapping subscription's rule lets this signal through).
            Reader reader;
            message_reader(message, &reader);
            const char* first = nullptr;
            u32 first_length = 0;
            const char type = reader_type(reader);
            if ((type != 's' && type != 'o') || !reader_string(&reader, &first, &first_length) ||
                !c_string_equal(s.arg0, first))
                continue;
        }
        const SignalHandler handler = s.handler;
        void* context = s.context;
        handler(context, message);
    }
}

void dispatch_call(Connection* c, const Message& message) noexcept {
    if (message.interface && c_string_equal(message.interface, "org.freedesktop.DBus.Peer") &&
        c_string_equal(message.member, "Ping")) {
        if (!(message.flags & flag_no_reply_expected))
            connection_reply(c, message, nullptr);
        return;
    }
    for (const Route& route : c->routes) {
        if (route.used && c_string_equal(route.path, message.path)) {
            if (route.handler(route.context, c, message))
                return;
            break;
        }
    }
    if (c->method_handler && c->method_handler(c->method_context, c, message))
        return;
    if (!(message.flags & flag_no_reply_expected))
        connection_reply_error(c, message, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
}

void dispatch(Connection* c, const Message& message) noexcept {
    switch (message.type) {
    case MessageType::method_return:
    case MessageType::error:
        dispatch_reply(c, message);
        break;
    case MessageType::signal:
        dispatch_signal(c, message);
        break;
    case MessageType::method_call:
        dispatch_call(c, message);
        break;
    default:
        break;
    }
}

bool grow_input(Connection* c, u32 needed) noexcept {
    return buffer_reserve(&c->input, c->input_used, &c->input_capacity, needed, 8192);
}

// Takes complete messages out of the input buffer and dispatches each.
void extract_messages(Connection* c) noexcept {
    while (c->alive) {
        const u32 size = message_size_at(c->input, c->input_used);
        if (size == 0)
            return;
        if (size > kMaxMessageBytes) {
            log_write(LogLevel::warning, "dbus", "the bus sent an invalid message header");
            disconnect(c);
            return;
        }
        if (size > c->input_used) {
            if (!grow_input(c, size))
                disconnect(c);
            return;
        }
        u8* block = static_cast<u8*>(heap_allocate(size + 1));
        if (!block) {
            disconnect(c);
            return;
        }
        copy_bytes(block, c->input, size);
        c->input_used -= size;
        if (c->input_used)
            copy_bytes(c->input, c->input + size, c->input_used);
        Message message;
        if (!message_parse(&message, block, size)) {
            log_write(LogLevel::warning, "dbus", "the bus sent a malformed message");
            disconnect(c);
            return;
        }
        for (u32 i = 0; i < message.fd_count && c->received_fd_count; ++i) {
            message.fds[i] = c->received_fds[0];
            copy_bytes(c->received_fds, c->received_fds + 1, --c->received_fd_count * static_cast<u32>(sizeof(int)));
        }
        dispatch(c, message);
        message_release(&message);
    }
}

// Reads whatever the socket has, bounded so one busy peer cannot hold the loop.
void read_input(Connection* c) noexcept {
    for (u32 round = 0; round != 64 && c->alive; ++round) {
        if (!grow_input(c, c->input_used + 4096)) {
            disconnect(c);
            return;
        }
        IoVec vector{c->input + c->input_used, c->input_capacity - c->input_used};
        alignas(8) u8 control[kControlBytes];
        MsgHdr header{};
        header.iov = &vector;
        header.iov_length = 1;
        header.control = control;
        header.control_length = sizeof(control);
        const long got = syscall3(kCallRecvmsg, c->fd, reinterpret_cast<long>(&header), kMsgDontwait | kMsgCmsgCloexec);
        if (got == -kErrorInterrupted)
            continue;
        if (got == -kErrorAgain)
            return;
        if (syscall_failed(got) || got == 0) {
            disconnect(c);
            return;
        }
        // Descriptors that came with these bytes join the queue the messages draw from.
        for (usize offset = 0; offset + sizeof(CmsgHdr) <= header.control_length;) {
            const CmsgHdr* cmsg = reinterpret_cast<const CmsgHdr*>(control + offset);
            if (cmsg->length < sizeof(CmsgHdr) || offset + cmsg->length > header.control_length)
                break;
            if (cmsg->level == kSolSocket && cmsg->type == kScmRights) {
                const u32 n = static_cast<u32>((cmsg->length - sizeof(CmsgHdr)) / 4);
                for (u32 i = 0; i < n; ++i) {
                    int fd;
                    copy_bytes(&fd, control + offset + sizeof(CmsgHdr) + i * 4, 4);
                    if (c->received_fd_count < kMaxReceivedFds)
                        c->received_fds[c->received_fd_count++] = fd;
                    else
                        sys_close(fd);
                }
            }
            offset += (cmsg->length + 7) & ~usize{7};
        }
        c->input_used += static_cast<u32>(got);
        extract_messages(c);
    }
}

void expire_calls(Connection* c, u64 now) noexcept {
    for (u32 i = 0; i < c->pending_count;) {
        if (c->pending[i].deadline > now) {
            ++i;
            continue;
        }
        const Pending call = c->pending[i];
        c->pending[i] = c->pending[--c->pending_count];
        call.handler(call.context, Status::timeout, nullptr);
        i = 0;  // the handler may have changed the table
    }
}

// Sync call plumbing ---------------------------------------------------------------------------------------------------

struct SyncCall {
    bool done;
    Status status;
    bool has_message;
    Message message;
};

void sync_handler(void* context, Status status, const Message* reply) noexcept {
    SyncCall* call = static_cast<SyncCall*>(context);
    call->status = status;
    if (reply) {
        if (message_clone(&call->message, *reply))
            call->has_message = true;
        else
            call->status = Status::failed;
    }
    call->done = true;
}

void log_failed_reply(void*, Status status, const Message* reply) noexcept {
    if (status == Status::error_reply && reply && reply->error_name)
        log_write(LogLevel::warning, "dbus", reply->error_name);
}

// Addresses ----------------------------------------------------------------------------------------------------------

// Decodes the %hh escapes of an address value into `out`; the length, or ~0 when it does not fit.
u32 unescape(const char* text, u32 length, char* out, u32 capacity) noexcept {
    u32 written = 0;
    for (u32 i = 0; i < length; ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= length)
                return ~0u;
            const int high = ascii_hex_value(text[i + 1]);
            const int low = ascii_hex_value(text[i + 2]);
            if (high < 0 || low < 0)
                return ~0u;
            c = static_cast<char>(high * 16 + low);
            i += 2;
        }
        if (written + 1 >= capacity)
            return ~0u;
        out[written++] = c;
    }
    out[written] = 0;
    return written;
}

bool open_socket(const SocketAddress& address, int* out) noexcept {
    const long created = syscall3(kCallSocket, kAfUnix, kSockStream | kSockNonblock | kSockCloexec, 0);
    if (syscall_failed(created))
        return false;
    const int fd = static_cast<int>(created);
    long connected = syscall3(kCallConnect, fd, reinterpret_cast<long>(address.data), address.length);
    while (connected == -kErrorInterrupted)
        connected = syscall3(kCallConnect, fd, reinterpret_cast<long>(address.data), address.length);
    if (connected == -kErrorAgain || connected == -kErrorInProgress) {
        // A full listen backlog: wait for the socket to take us.
        if (!poll_until(fd, kPollOut, clock_monotonic_milliseconds() + kHandshakeTimeoutMs)) {
            sys_close(fd);
            return false;
        }
        connected = 0;
    }
    if (syscall_failed(connected)) {
        sys_close(fd);
        return false;
    }
    *out = fd;
    return true;
}

} // namespace

u32 address_parse(const char* address, SocketAddress* out, u32 capacity) noexcept {
    u32 found = 0;
    const char* cursor = address;
    while (*cursor && found < capacity) {
        const char* end = cursor;
        while (*end && *end != ';')
            ++end;
        // One entry: "transport:key=value,key=value".
        if (starts_with(cursor, "unix:")) {
            const char* pair = cursor + 5;
            char path[104] = {};
            u32 length = 0;
            bool abstract_name = false;
            bool have = false;
            bool bad = false;
            while (pair < end) {
                const char* stop = pair;
                while (stop < end && *stop != ',')
                    ++stop;
                const char* equals = pair;
                while (equals < stop && *equals != '=')
                    ++equals;
                const u32 key_length = static_cast<u32>(equals - pair);
                const char* value = equals < stop ? equals + 1 : stop;
                const u32 value_length = static_cast<u32>(stop - value);
                const bool is_path = key_length == 4 && starts_with(pair, "path");
                const bool is_abstract = key_length == 8 && starts_with(pair, "abstract");
                if (is_path || is_abstract) {
                    length = unescape(value, value_length, path, sizeof(path));
                    if (length == ~0u || length == 0)
                        bad = true;
                    abstract_name = is_abstract;
                    have = true;
                }
                pair = stop < end ? stop + 1 : stop;
            }
            // sockaddr_un: family, then the path (NUL terminated) or a leading NUL and the abstract name.
            if (have && !bad && length + 3 <= sizeof(out[found].data)) {
                SocketAddress& entry = out[found];
                clear_bytes(&entry, static_cast<u32>(sizeof(entry)));
                entry.data[0] = kAfUnix;
                if (abstract_name) {
                    copy_bytes(entry.data + 3, path, length);
                    entry.length = 2 + 1 + length;
                } else {
                    copy_bytes(entry.data + 2, path, length);
                    entry.length = 2 + length + 1;
                }
                ++found;
            }
        }
        cursor = *end ? end + 1 : end;
    }
    return found;
}

Connection* connection_open_address(const char* address) noexcept {
    SocketAddress entries[4];
    const u32 count = address_parse(address, entries, 4);
    if (count == 0) {
        log_write(LogLevel::trace, "dbus", "no usable unix address for the bus");
        return nullptr;
    }
    for (u32 i = 0; i < count; ++i) {
        int fd = -1;
        if (!open_socket(entries[i], &fd))
            continue;
        bool fds_ok = false;
        if (!authenticate(fd, &fds_ok)) {
            sys_close(fd);
            continue;
        }
        Connection* c = static_cast<Connection*>(heap_allocate(sizeof(Connection), true));
        if (!c) {
            sys_close(fd);
            return nullptr;
        }
        c->fd = fd;
        c->alive = true;
        c->fds_ok = fds_ok;
        c->next_serial = 1;
        // Hello gives the connection its unique name; nothing else may precede it.
        Message reply;
        const CallTarget hello{kBusName, kBusPath, kBusName, "Hello"};
        if (connection_call_sync(c, hello, nullptr, kHelloTimeoutMs, &reply) != Status::ok) {
            log_write(LogLevel::warning, "dbus", "the bus did not answer Hello");
            connection_close(c);
            continue;
        }
        Reader reader;
        message_reader(reply, &reader);
        const char* name = nullptr;
        u32 name_length = 0;
        const bool named = reader_string(&reader, &name, &name_length) && name_length < sizeof(c->unique_name);
        if (named)
            copy_bytes(c->unique_name, name, name_length + 1);
        message_release(&reply);
        if (!named) {
            connection_close(c);
            continue;
        }
        return c;
    }
    log_write(LogLevel::trace, "dbus", "could not connect to the bus");
    return nullptr;
}

Connection* connection_open_session() noexcept {
    const char* address = environment_get("DBUS_SESSION_BUS_ADDRESS");
    if (address && *address)
        return connection_open_address(address);
    // The default location: the runtime directory's `bus` socket.
    char fallback[160];
    const char* runtime = environment_get("XDG_RUNTIME_DIR");
    const char prefix[] = "unix:path=";
    u32 length = sizeof(prefix) - 1;
    copy_bytes(fallback, prefix, length);
    const u32 runtime_length = runtime ? c_string_length(runtime) : 0;
    if (runtime_length && runtime_length < 120) {
        copy_bytes(fallback + length, runtime, runtime_length);
        length += runtime_length;
    } else {
        // /run/user/<uid>
        const char run[] = "/run/user/";
        copy_bytes(fallback + length, run, sizeof(run) - 1);
        length += sizeof(run) - 1;
        length += decimal_write(fallback + length, static_cast<u32>(syscall1(kCallGeteuid, 0)));
    }
    const char tail[] = "/bus";
    copy_bytes(fallback + length, tail, sizeof(tail));
    return connection_open_address(fallback);
}

void connection_close(Connection* c) noexcept {
    if (!c)
        return;
    disconnect(c);
    if (c->waitable)
        waitable_close(c->waitable);
    if (c->fd >= 0)
        sys_close(c->fd);
    close_descriptors(c->received_fds, &c->received_fd_count);
    if (c->input) {
        secure_clear_bytes(c->input, c->input_capacity);
        heap_free(c->input);
    }
    if (c->output) {
        secure_clear_bytes(c->output, c->output_capacity);
        heap_free(c->output);
    }
    heap_free(c);
}

bool connection_alive(const Connection* c) noexcept {
    return c && c->alive;
}

const char* connection_unique_name(const Connection* c) noexcept {
    return c->unique_name;
}

bool connection_unix_fds(const Connection* c) noexcept {
    return c->fds_ok;
}

int connection_fd(const Connection* c) noexcept {
    return c->fd;
}

Waitable connection_waitable(Connection* c) noexcept {
    if (!c->waitable)
        c->waitable = waitable_borrow_fd(c->fd);
    return c->waitable;
}

u32 connection_wait_ms(const Connection* c, u64 now_ms) noexcept {
    if (!c->alive)
        return kNoTimeout;
    if (c->output_used)
        return kOutputRetryMs;
    u64 earliest = ~0ull;
    for (u32 i = 0; i < c->pending_count; ++i)
        if (c->pending[i].deadline < earliest)
            earliest = c->pending[i].deadline;
    if (earliest == ~0ull)
        return kNoTimeout;
    if (earliest <= now_ms)
        return 0;
    const u64 left = earliest - now_ms;
    return left >= kNoTimeout ? kNoTimeout - 1 : static_cast<u32>(left);
}

void connection_process(Connection* c, u64 now_ms, bool readable) noexcept {
    if (!c->alive)
        return;
    if (c->output_used)
        flush_output(c);
    if (readable)
        read_input(c);
    if (c->alive)
        expire_calls(c, now_ms);
}

bool connection_call(Connection* c, const CallTarget& target, const Builder* body, u32 timeout_ms, ReplyHandler handler,
                     void* context, u8 flags) noexcept {
    if (!c->alive || (handler && c->pending_count == kMaxPending))
        return false;
    MessageHeader header{};
    header.type = MessageType::method_call;
    header.flags = static_cast<u8>((handler ? 0 : flag_no_reply_expected) | flags);
    header.destination = target.destination;
    header.path = target.path;
    header.interface = target.interface;
    header.member = target.member;
    u32 serial = 0;
    if (!send_message(c, header, body, &serial))
        return false;
    if (handler) {
        const u64 now = clock_monotonic_milliseconds();
        c->pending[c->pending_count++] =
            Pending{serial, handler, context, timeout_ms == kNoTimeout ? ~0ull : now + timeout_ms};
    }
    return true;
}

Status connection_call_sync(Connection* c, const CallTarget& target, const Builder* body, u32 timeout_ms,
                            Message* reply) noexcept {
    SyncCall call{};
    if (!connection_call(c, target, body, timeout_ms, sync_handler, &call))
        return Status::failed;
    while (!call.done) {
        const u64 now = clock_monotonic_milliseconds();
        connection_process(c, now);
        if (call.done)
            break;
        if (!c->alive) {
            call.status = Status::disconnected;
            break;
        }
        const u32 wait = connection_wait_ms(c, clock_monotonic_milliseconds());
        TimeSpec timeout{static_cast<i64>(wait / 1000), static_cast<i64>(wait % 1000) * 1000000};
        PollFd poll{c->fd, static_cast<i16>(kPollIn | (c->output_used ? kPollOut : 0)), 0};
        const long ready = sys_ppoll(&poll, 1, wait == kNoTimeout ? nullptr : &timeout);
        if (ready < 0 && -ready != kErrorInterrupted) {
            disconnect(c);
        }
    }
    if (call.has_message)
        *reply = call.message;
    else
        clear_bytes(reply, static_cast<u32>(sizeof(*reply)));
    return call.status;
}

__declspec(noinline) const char* reply_error_name(Status status, const Message* reply, const char* fallback) noexcept {
    return status == Status::error_reply && reply && reply->error_name ? reply->error_name : fallback;
}

u32 connection_subscribe(Connection* c, const SignalMatch& match, SignalHandler handler, void* context) noexcept {
    if (!c->alive)
        return 0;
    u32 slot = c->subscription_count;
    for (u32 i = 0; i < c->subscription_count; ++i) {
        if (!c->subscriptions[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxSubscriptions)
        return 0;
    // Built in its slot, which stays inactive until the bus has taken the rule.
    Subscription& s = c->subscriptions[slot];
    s = Subscription{};
    s.handler = handler;
    s.context = context;
    const auto store = [](char* destination, u32 capacity, const char* value) {
        if (!value)
            return true;
        const u32 length = c_string_length(value);
        if (length >= capacity)
            return false;
        copy_bytes(destination, value, length + 1);
        return true;
    };
    if (!store(s.path, sizeof(s.path), match.path) || !store(s.interface, sizeof(s.interface), match.interface) ||
        !store(s.member, sizeof(s.member), match.member) || !store(s.arg0, sizeof(s.arg0), match.arg0))
        return 0;
    // The bus's rule: single-quoted values (a quote inside one cannot be expressed).
    char* rule = s.rule;
    u32 length = 0;
    bool ok = true;
    const auto append = [&](const char* text) {
        const u32 n = c_string_length(text);
        if (length + n + 1 > sizeof(s.rule)) {
            ok = false;
            return;
        }
        copy_bytes(rule + length, text, n);
        length += n;
    };
    const auto clause = [&](const char* key, const char* value) {
        if (!value)
            return;
        for (u32 i = 0; value[i]; ++i)
            if (value[i] == '\'')
                ok = false;
        append(length ? ",": "");
        append(key);
        append("='");
        append(value);
        append("'");
    };
    clause("type", "signal");
    clause("sender", match.sender);
    clause("path", match.path);
    clause("interface", match.interface);
    clause("member", match.member);
    clause("arg0", match.arg0);
    if (!ok)
        return 0;
    rule[length] = 0;
    Builder body;
    builder_init(&body);
    builder_string(&body, rule);
    const CallTarget target{kBusName, kBusPath, kBusName, "AddMatch"};
    const bool sent = connection_call(c, target, &body, 10000, log_failed_reply, nullptr);
    builder_release(&body);
    if (!sent)
        return 0;
    s.active = true;
    if (slot == c->subscription_count)
        ++c->subscription_count;
    return slot + 1;
}

void connection_unsubscribe(Connection* c, u32 id) noexcept {
    if (id == 0 || id > c->subscription_count || !c->subscriptions[id - 1].active)
        return;
    Subscription& s = c->subscriptions[id - 1];
    s.active = false;
    if (!c->alive)
        return;
    Builder body;
    builder_init(&body);
    builder_string(&body, s.rule);
    const CallTarget target{kBusName, kBusPath, kBusName, "RemoveMatch"};
    connection_call(c, target, &body, 10000, log_failed_reply, nullptr);
    builder_release(&body);
}

bool connection_export_object(Connection* c, const char* path, MethodHandler handler, void* context) noexcept {
    const u32 length = c_string_length(path);
    if (length >= sizeof(Route::path))
        return false;
    Route* free_route = nullptr;
    for (Route& route : c->routes) {
        if (route.used && c_string_equal(route.path, path)) {
            route.handler = handler;
            route.context = context;
            return true;
        }
        if (!route.used && !free_route)
            free_route = &route;
    }
    if (!free_route)
        return false;
    free_route->used = true;
    free_route->handler = handler;
    free_route->context = context;
    copy_bytes(free_route->path, path, length + 1);
    return true;
}

void connection_unexport_object(Connection* c, const char* path) noexcept {
    for (Route& route : c->routes)
        if (route.used && c_string_equal(route.path, path))
            route.used = false;
}

void connection_set_method_handler(Connection* c, MethodHandler handler, void* context) noexcept {
    c->method_handler = handler;
    c->method_context = context;
}

bool connection_reply(Connection* c, const Message& call, const Builder* body) noexcept {
    MessageHeader header{};
    header.type = MessageType::method_return;
    header.flags = flag_no_reply_expected;
    header.reply_serial = call.serial;
    header.destination = call.sender;
    return send_message(c, header, body, nullptr);
}

bool connection_reply_error(Connection* c, const Message& call, const char* name, const char* text) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, text);
    MessageHeader header{};
    header.type = MessageType::error;
    header.flags = flag_no_reply_expected;
    header.reply_serial = call.serial;
    header.error_name = name;
    header.destination = call.sender;
    const bool sent = send_message(c, header, &body, nullptr);
    builder_release(&body);
    return sent;
}

bool connection_emit(Connection* c, const char* path, const char* interface, const char* member,
                     const Builder* body) noexcept {
    MessageHeader header{};
    header.type = MessageType::signal;
    header.flags = flag_no_reply_expected;
    header.path = path;
    header.interface = interface;
    header.member = member;
    return send_message(c, header, body, nullptr);
}

bool connection_request_name(Connection* c, const char* name, u32 timeout_ms) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, name);
    builder_u32(&body, 4);  // DBUS_NAME_FLAG_DO_NOT_QUEUE
    const CallTarget target{kBusName, kBusPath, kBusName, "RequestName"};
    Message reply;
    const Status status = connection_call_sync(c, target, &body, timeout_ms, &reply);
    builder_release(&body);
    bool primary = false;
    if (status == Status::ok) {
        Reader reader;
        message_reader(reply, &reader);
        u32 result = 0;
        primary = reader_u32(&reader, &result) && result == 1;
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return primary;
}

} // namespace maicord::dbus
