#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

// The D-Bus connection against a real dbus-daemon and the python mock services (tools/test_dbus.sh): authentication,
// Hello, descriptor passing both ways, typed bodies decoded by libdbus, errors, timeouts, signals with the bus's own
// filtering, an exported object served on a second connection, large messages, and a fake bus that sends garbage.
// Without MAICORD_DBUS_TEST_ADDRESS the test skips (77): it never touches the bus of the user's session.

namespace maicord {
namespace {

using namespace dbus;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool text_equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return a == b;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

constexpr long kCallSocket = 41;
constexpr long kCallBind = 49;
constexpr long kCallListen = 50;
constexpr long kCallAccept4 = 288;

const char* g_address = nullptr;

// The bus's own name for the test service.
const CallTarget kAdd{"org.maicord.Test", "/org/maicord/Test", "org.maicord.Test", "Add"};

CallTarget test_target(const char* member) noexcept {
    return CallTarget{"org.maicord.Test", "/org/maicord/Test", "org.maicord.Test", member};
}

// Waits for the connection to have something and processes it, until `done` or the time passes.
bool pump_until(Connection* c, const volatile bool* done, u32 milliseconds) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    while (!*done) {
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            return false;
        Waitable w = connection_waitable(c);
        u32 wait = connection_wait_ms(c, now);
        const u32 left = static_cast<u32>(deadline - now);
        if (wait > left)
            wait = left;
        wait_many(&w, 1, wait, 0);
        connection_process(c, clock_monotonic_milliseconds());
        if (!connection_alive(c))
            break;
    }
    return *done;
}

// Calls Describe with the body and copies the mock's answer.
bool describe(Connection* c, const Builder& body, char* text, u32 capacity) noexcept {
    Message reply;
    const Status status = connection_call_sync(c, test_target("Describe"), &body, 5000, &reply);
    if (status != Status::ok)
        return false;
    Reader reader;
    message_reader(reply, &reader);
    const char* answer = nullptr;
    u32 length = 0;
    const bool ok = reader_string(&reader, &answer, &length) && length < capacity;
    if (ok)
        copy_bytes(text, answer, length + 1);
    message_release(&reply);
    return ok;
}

bool described_as(Connection* c, const Builder& body, const char* expected, const char* label) noexcept {
    char text[2048];
    const bool ok = describe(c, body, text, sizeof(text)) && text_equal(text, expected);
    if (!ok) {
        test_write_text("  got: ");
        test_write_line(describe(c, body, text, sizeof(text)) ? text : "(no answer)");
        test_write_text("  want: ");
        test_write_line(expected);
    }
    check(ok, label);
    return ok;
}

void test_connect(Connection* c) noexcept {
    check(c && connection_alive(c), "the bus connection opens");
    check(text_equal(connection_unique_name(c), connection_unique_name(c)) && connection_unique_name(c)[0] == ':',
          "Hello gave a unique name");

    Connection* second = connection_open_address(g_address);
    check(second && !text_equal(connection_unique_name(second), connection_unique_name(c)), "a second connection has its own name");
    connection_close(second);

    check(connection_open_address("unix:path=/nonexistent/maicord/bus") == nullptr, "a missing socket gives no connection");
    check(connection_open_address("tcp:host=localhost,port=1") == nullptr, "an unsupported transport gives none");

    Connection* session = connection_open_session();
    check(session && connection_alive(session), "the session bus opens from DBUS_SESSION_BUS_ADDRESS");
    connection_close(session);
}

void test_calls(Connection* c) noexcept {
    Builder body;
    builder_init(&body);
    builder_i32(&body, 3);
    builder_i32(&body, 4);
    Message reply;
    Status status = connection_call_sync(c, kAdd, &body, 3000, &reply);
    Reader reader;
    i32 sum = 0;
    bool ok = false;
    if (status == Status::ok) {
        message_reader(reply, &reader);
        ok = reader_i32(&reader, &sum) && !reader_more(reader);
        check(reply.type == MessageType::method_return && reply.reply_serial != 0 && text_equal(reply.signature, "i") &&
                  reply.sender != nullptr,
              "the reply's header");
        message_release(&reply);
    }
    check(ok && sum == 7, "a method call and its reply");
    builder_release(&body);

    // Errors: from the service, from the bus, from the daemon.
    status = connection_call_sync(c, test_target("Fail"), nullptr, 3000, &reply);
    check(status == Status::error_reply && text_equal(reply.error_name, "org.maicord.Test.Error.Requested"),
          "an error reply from the service");
    if (status == Status::error_reply) {
        message_reader(reply, &reader);
        const char* text = nullptr;
        check(reader_string(&reader, &text, nullptr) && text[0] != 0, "the error carries its text");
        message_release(&reply);
    }
    status = connection_call_sync(c, test_target("NoSuchMethod"), nullptr, 3000, &reply);
    check(status == Status::error_reply && text_equal(reply.error_name, "org.freedesktop.DBus.Error.UnknownMethod"),
          "an unknown method");
    if (status == Status::error_reply)
        message_release(&reply);
    const CallTarget nobody{"org.maicord.Nobody", "/", "org.maicord.Nobody", "Hi"};
    status = connection_call_sync(c, nobody, nullptr, 3000, &reply);
    check(status == Status::error_reply && text_equal(reply.error_name, "org.freedesktop.DBus.Error.ServiceUnknown"),
          "a name nobody owns");
    if (status == Status::error_reply)
        message_release(&reply);

    // A call with no reply expected is accepted by the bus; the connection stays healthy.
    check(connection_call(c, test_target("Add"), nullptr, 0, nullptr, nullptr) && connection_alive(c),
          "a call without a handler is sent");
    check(connection_call_sync(c, kAdd, nullptr, 3000, &reply) == Status::error_reply, "bad arguments are an error reply");
    message_release(&reply);
}

struct Collector {
    volatile bool done;
    Status status;
    i32 value;
    u32 order;
    u32 completions;
};
u32 g_order_counter = 0;

void collect_sum(void* context, Status status, const Message* reply) noexcept {
    Collector* collector = static_cast<Collector*>(context);
    collector->status = status;
    collector->order = ++g_order_counter;
    if (status == Status::ok && reply) {
        Reader reader;
        message_reader(*reply, &reader);
        if (!reader_i32(&reader, &collector->value))
            collector->value = -1;
    }
    ++collector->completions;
    collector->done = true;
}

void collect_text(void* context, Status status, const Message* reply) noexcept {
    Collector* collector = static_cast<Collector*>(context);
    collector->status = status;
    collector->order = ++g_order_counter;
    if (status == Status::ok && reply) {
        Reader reader;
        message_reader(*reply, &reader);
        const char* text = nullptr;
        collector->value = reader_string(&reader, &text, nullptr) && text_equal(text, "late") ? 1 : -1;
    }
    ++collector->completions;
    collector->done = true;
}

void test_async_and_timeouts(Connection* c) noexcept {
    // A slow reply (400 ms) and a fast one: the fast one completes first.
    Collector slow{};
    Collector fast{};
    Builder slow_body;
    builder_init(&slow_body);
    builder_u32(&slow_body, 400);
    check(connection_call(c, test_target("Slow"), &slow_body, 5000, collect_text, &slow), "an async call");
    builder_release(&slow_body);
    Builder add;
    builder_init(&add);
    builder_i32(&add, 20);
    builder_i32(&add, 22);
    check(connection_call(c, kAdd, &add, 5000, collect_sum, &fast), "a second async call");
    builder_release(&add);
    check(connection_wait_ms(c, clock_monotonic_milliseconds()) != kNoTimeout, "a pending call bounds the wait");
    check(pump_until(c, &fast.done, 3000) && fast.status == Status::ok && fast.value == 42, "the fast reply arrives");
    check(!slow.done, "the slow call is still pending");
    check(pump_until(c, &slow.done, 3000) && slow.status == Status::ok && slow.value == 1, "then the slow one");
    check(fast.order < slow.order, "replies complete in the order they arrive");

    // A timeout before the reply: the late reply is ignored.
    Collector timed{};
    Builder late;
    builder_init(&late);
    builder_u32(&late, 600);
    check(connection_call(c, test_target("Slow"), &late, 100, collect_text, &timed), "a call with a short timeout");
    builder_release(&late);
    check(pump_until(c, &timed.done, 2000) && timed.status == Status::timeout && timed.completions == 1,
          "the call times out");
    const u64 until = clock_monotonic_milliseconds() + 800;
    while (clock_monotonic_milliseconds() < until) {
        Waitable w = connection_waitable(c);
        wait_many(&w, 1, 50, 0);
        connection_process(c, clock_monotonic_milliseconds());
    }
    check(timed.completions == 1 && connection_alive(c), "the late reply to a timed-out call is dropped");

    // The synchronous form times out too.
    Builder very_late;
    builder_init(&very_late);
    builder_u32(&very_late, 700);
    Message reply;
    const u64 started = clock_monotonic_milliseconds();
    const Status status = connection_call_sync(c, test_target("Slow"), &very_late, 150, &reply);
    const u64 elapsed = clock_monotonic_milliseconds() - started;
    builder_release(&very_late);
    check(status == Status::timeout && elapsed >= 140 && elapsed < 600, "a synchronous call honours its timeout");
    // Let the late reply pass.
    const u64 settle = clock_monotonic_milliseconds() + 700;
    while (clock_monotonic_milliseconds() < settle) {
        Waitable w = connection_waitable(c);
        wait_many(&w, 1, 50, 0);
        connection_process(c, clock_monotonic_milliseconds());
    }

    // Many in flight at once.
    constexpr u32 kMany = 40;
    static Collector many[kMany];
    clear_bytes(many, sizeof(many));
    for (u32 i = 0; i < kMany; ++i) {
        Builder b;
        builder_init(&b);
        builder_i32(&b, static_cast<i32>(i));
        builder_i32(&b, 1000);
        connection_call(c, kAdd, &b, 5000, collect_sum, &many[i]);
        builder_release(&b);
    }
    bool all = true;
    for (u32 i = 0; i < kMany; ++i)
        all = pump_until(c, &many[i].done, 3000) && many[i].status == Status::ok && many[i].value == static_cast<i32>(i) + 1000 && all;
    check(all, "forty calls in flight each get their own reply");
}

void test_typed_bodies(Connection* c) noexcept {
    {
        Builder b;
        builder_init(&b);
        builder_byte(&b, 200);
        builder_bool(&b, true);
        builder_i16(&b, -5);
        builder_i32(&b, -7);
        builder_u32(&b, 9);
        builder_i64(&b, -9);
        builder_double(&b, 1.5);
        builder_string(&b, "h\xC3\xA9llo");
        builder_object_path(&b, "/a/b");
        builder_signature(&b, "a{sv}");
        described_as(c, b, "ybniuxdsog y:200 b:1 n:-5 i:-7 u:9 x:-9 d:1.5 s:h\xC3\xA9llo o:/a/b g:a{sv}",
                     "every basic type the client writes is decoded by libdbus");
        builder_release(&b);
    }
    {
        Builder b;
        builder_init(&b);
        builder_open_array(&b, "{sv}");
        builder_open_entry(&b);
        builder_string(&b, "name");
        builder_open_variant(&b, "s");
        builder_string(&b, "value");
        builder_close_variant(&b);
        builder_close_entry(&b);
        builder_open_entry(&b);
        builder_string(&b, "list");
        builder_open_variant(&b, "as");
        builder_open_array(&b, "s");
        builder_string(&b, "x");
        builder_string(&b, "y");
        builder_close_array(&b);
        builder_close_variant(&b);
        builder_close_entry(&b);
        builder_open_entry(&b);
        builder_string(&b, "nested");
        builder_open_variant(&b, "v");
        builder_open_variant(&b, "(si)");
        builder_open_struct(&b, "(si)");
        builder_string(&b, "p");
        builder_i32(&b, 3);
        builder_close_struct(&b);
        builder_close_variant(&b);
        builder_close_variant(&b);
        builder_close_entry(&b);
        builder_close_array(&b);
        described_as(c, b,
                     "a{sv} {s:list=v([s:x,s:y]),s:name=v(s:value),s:nested=v(v((s:p,i:3)))}",
                     "a dictionary of variants, arrays and structs");
        builder_release(&b);
    }
    {
        Builder b;
        builder_init(&b);
        builder_open_array(&b, "(oayays)");
        builder_open_struct(&b, "(oayays)");
        builder_object_path(&b, "/s/1");
        const u8 bytes[3] = {1, 2, 255};
        builder_byte_array(&b, bytes, 3);
        builder_byte_array(&b, nullptr, 0);
        builder_string(&b, "text/plain");
        builder_close_struct(&b);
        builder_close_array(&b);
        builder_open_array(&b, "x");
        builder_i64(&b, 1ll << 40);
        builder_close_array(&b);
        builder_open_array(&b, "ai");
        builder_close_array(&b);
        described_as(c, b, "a(oayays)axaai [(o:/s/1,ay:0102ff,ay:,s:text/plain)] [x:1099511627776] []",
                     "arrays of structs, byte arrays, 8-aligned and empty arrays");
        builder_release(&b);
    }
    {
        // The echo returns a variant: read it back with a Reader.
        Builder b;
        builder_init(&b);
        builder_open_variant(&b, "a{sv}");
        builder_open_array(&b, "{sv}");
        builder_open_entry(&b);
        builder_string(&b, "k");
        builder_open_variant(&b, "d");
        builder_double(&b, -0.25);
        builder_close_variant(&b);
        builder_close_entry(&b);
        builder_close_array(&b);
        builder_close_variant(&b);
        Message reply;
        const Status status = connection_call_sync(c, test_target("Echo"), &b, 3000, &reply);
        bool ok = false;
        if (status == Status::ok) {
            Reader r;
            message_reader(reply, &r);
            const char* inner = nullptr;
            const char* key = nullptr;
            double number = 0;
            ok = text_equal(reply.signature, "v") && reader_enter_variant(&r, &inner) && text_equal(inner, "a{sv}") &&
                 reader_enter_array(&r) && reader_enter_struct(&r) && reader_string(&r, &key, nullptr) &&
                 text_equal(key, "k") && reader_enter_variant(&r, &inner) && text_equal(inner, "d") &&
                 reader_double(&r, &number) && number == -0.25;
            message_release(&reply);
        }
        check(ok, "a variant echoed through libdbus reads back");
        builder_release(&b);
    }
    {
        // A megabyte string in each direction: partial writes, queueing and reassembly.
        static char big[(1u << 20) + 1];
        for (u32 i = 0; i < (1u << 20); ++i)
            big[i] = static_cast<char>('a' + i % 26);
        big[1u << 20] = 0;
        Builder b;
        builder_init(&b);
        builder_open_variant(&b, "s");
        builder_string(&b, big, 1u << 20);
        builder_close_variant(&b);
        Message reply;
        const Status status = connection_call_sync(c, test_target("Echo"), &b, 10000, &reply);
        bool ok = false;
        if (status == Status::ok) {
            Reader r;
            message_reader(reply, &r);
            const char* text = nullptr;
            u32 length = 0;
            ok = reader_enter_variant(&r, nullptr) && reader_string(&r, &text, &length) && length == (1u << 20) &&
                 bytes_equal(text, big, length);
            message_release(&reply);
        }
        check(ok && connection_alive(c), "a 1 MiB string round-trips");
        builder_release(&b);
    }
}

// Descriptors ----------------------------------------------------------------------------------------------------------

// A descriptor received (the portal's PipeWire remote is the client's one use): libdbus's service makes a pipe and
// sends its read end.
void test_descriptors(Connection* c) noexcept {
    Builder b;
    Message reply;
    builder_init(&b);
    builder_string(&b, "from-the-mock");
    const Status status = connection_call_sync(c, test_target("MakeFd"), &b, 3000, &reply);
    builder_release(&b);
    bool ok = false;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        int fd = -1;
        ok = text_equal(reply.signature, "h") && reply.fd_count == 1 && reader_unix_fd(&r, &fd) && fd > 2;
        char text[64] = {};
        const long got = ok ? sys_read(fd, text, sizeof(text) - 1) : -1;
        ok = ok && got == 13 && text_equal(text, "from-the-mock");
        message_release(&reply);
    }
    check(ok, "a descriptor received from libdbus's service reads");
}

// Signals --------------------------------------------------------------------------------------------------------------

struct Signals {
    u32 pings;
    u32 others;
    u32 filtered;
    char last[32];
    u32 last_number;
};

void on_ping(void* context, const Message& signal) noexcept {
    Signals* s = static_cast<Signals*>(context);
    Reader r;
    message_reader(signal, &r);
    const char* text = nullptr;
    u32 number = 0;
    if (text_equal(signal.member, "Ping") && reader_string(&r, &text, nullptr) && reader_u32(&r, &number)) {
        ++s->pings;
        s->last_number = number;
        u32 i = 0;
        for (; text[i] && i < sizeof(s->last) - 1; ++i)
            s->last[i] = text[i];
        s->last[i] = 0;
    }
}

void on_other(void* context, const Message& signal) noexcept {
    Signals* s = static_cast<Signals*>(context);
    if (text_equal(signal.member, "Other"))
        ++s->others;
}

void on_filtered(void* context, const Message& signal) noexcept {
    Signals* s = static_cast<Signals*>(context);
    (void)signal;
    ++s->filtered;
}

void fire(Connection* c, const char* text, u32 number) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, text);
    builder_u32(&b, number);
    Message reply;
    if (connection_call_sync(c, test_target("Fire"), &b, 3000, &reply) == Status::ok)
        message_release(&reply);
    builder_release(&b);
}

void test_signals(Connection* c) noexcept {
    static Signals signals;
    clear_bytes(&signals, sizeof(signals));
    const SignalMatch ping{"org.maicord.Test", "/org/maicord/Test", "org.maicord.Test", "Ping", nullptr};
    const SignalMatch other{nullptr, nullptr, "org.maicord.Test", "Other", nullptr};
    const SignalMatch only_y{"org.maicord.Test", nullptr, "org.maicord.Test", "Ping", "y"};
    check(connection_subscribe(c, ping, on_ping, &signals), "subscribe to a signal");
    check(connection_subscribe(c, other, on_other, &signals), "subscribe by interface and member only");
    check(connection_subscribe(c, only_y, on_filtered, &signals), "subscribe with an arg0 filter");

    fire(c, "x", 5);
    volatile bool never = false;
    for (u32 i = 0; i < 20 && signals.others == 0; ++i)
        pump_until(c, &never, 20);
    check(signals.pings == 1 && signals.others == 1 && signals.last_number == 5 && text_equal(signals.last, "x"),
          "each signal reaches its own handler with its arguments");
    check(signals.filtered == 0, "the bus's arg0 filter kept the other string out");
    fire(c, "y", 6);
    for (u32 i = 0; i < 20 && signals.filtered == 0; ++i)
        pump_until(c, &never, 20);
    check(signals.pings == 2 && signals.filtered == 1 && signals.last_number == 6, "arg0 = y passes the filter");

    // A rule with a quote cannot be expressed.
    const SignalMatch bad{nullptr, "/a'b", nullptr, nullptr, nullptr};
    check(!connection_subscribe(c, bad, on_ping, &signals), "a quote in a match value is refused");
}

// An exported object on a second connection ---------------------------------------------------------------------------

struct Server {
    Connection* connection;
    volatile u32 stop;
    volatile u32 calls;
};

bool server_method(void* context, Connection* c, const Message& call) noexcept {
    Server* server = static_cast<Server*>(context);
    __atomic_fetch_add(&server->calls, 1u, __ATOMIC_SEQ_CST);
    if (!text_equal(call.interface, "org.maicord.Server"))
        return false;
    Reader r;
    message_reader(call, &r);
    Builder reply;
    builder_init(&reply);
    bool handled = true;
    if (text_equal(call.member, "Greet")) {
        const char* name = nullptr;
        u32 length = 0;
        if (!reader_string(&r, &name, &length)) {
            handled = connection_reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs", "a string, please");
            builder_release(&reply);
            return handled;
        }
        char text[128];
        const char hello[] = "Hello, ";
        copy_bytes(text, hello, sizeof(hello) - 1);
        copy_bytes(text + sizeof(hello) - 1, name, length + 1);
        builder_string(&reply, text);
        connection_reply(c, call, &reply);
    } else if (text_equal(call.member, "Signal")) {
        builder_string(&reply, "from-the-server");
        connection_emit(c, "/org/maicord/Server", "org.maicord.Server", "Note", &reply);
        builder_release(&reply);
        builder_init(&reply);
        connection_reply(c, call, &reply);
    } else {
        handled = false;
    }
    builder_release(&reply);
    return handled;
}

u32 server_thread(void* argument) noexcept {
    Server* server = static_cast<Server*>(argument);
    while (!__atomic_load_n(&server->stop, __ATOMIC_ACQUIRE)) {
        Waitable waitable = connection_waitable(server->connection);
        wait_many(&waitable, 1, 20, 0);
        connection_process(server->connection, clock_monotonic_milliseconds());
    }
    return 0;
}

struct NoteCounter {
    volatile u32 notes;
};

void on_note(void* context, const Message& signal) noexcept {
    (void)signal;
    __atomic_fetch_add(&static_cast<NoteCounter*>(context)->notes, 1u, __ATOMIC_SEQ_CST);
}

void test_exported_object(Connection* c) noexcept {
    static Server server;
    server.connection = connection_open_address(g_address);
    check(server.connection != nullptr, "a second connection for the server");
    if (!server.connection)
        return;
    connection_set_method_handler(server.connection, server_method, &server);
    check(connection_request_name(server.connection, "org.maicord.Server", 3000), "RequestName makes it the owner");
    check(!connection_request_name(c, "org.maicord.Server", 3000), "a second claim does not become the owner");
    Thread* thread = thread_start(server_thread, &server, 256 * 1024, nullptr);
    check(thread != nullptr, "the server thread starts");

    const CallTarget greet{"org.maicord.Server", "/org/maicord/Server", "org.maicord.Server", "Greet"};
    Builder b;
    builder_init(&b);
    builder_string(&b, "world");
    Message reply;
    Status status = connection_call_sync(c, greet, &b, 3000, &reply);
    builder_release(&b);
    bool ok = false;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        const char* text = nullptr;
        ok = reader_string(&r, &text, nullptr) && text_equal(text, "Hello, world");
        message_release(&reply);
    }
    check(ok, "a call to an object served by this library");

    // Wrong arguments and unknown members become errors.
    status = connection_call_sync(c, greet, nullptr, 3000, &reply);
    check(status == Status::error_reply && text_equal(reply.error_name, "org.freedesktop.DBus.Error.InvalidArgs"),
          "the handler can answer with an error");
    if (status == Status::error_reply)
        message_release(&reply);
    const CallTarget unknown{"org.maicord.Server", "/org/maicord/Server", "org.maicord.Server", "Nope"};
    status = connection_call_sync(c, unknown, nullptr, 3000, &reply);
    check(status == Status::error_reply && text_equal(reply.error_name, "org.freedesktop.DBus.Error.UnknownMethod"),
          "an unhandled call is an UnknownMethod error");
    if (status == Status::error_reply)
        message_release(&reply);
    const CallTarget ping{"org.maicord.Server", "/org/maicord/Server", "org.freedesktop.DBus.Peer", "Ping"};
    check(connection_call_sync(c, ping, nullptr, 3000, &reply) == Status::ok, "Peer.Ping is answered");
    message_release(&reply);

    // Signals emitted by the server (a broadcast without destination).
    static NoteCounter notes;
    notes.notes = 0;
    const SignalMatch note{"org.maicord.Server", nullptr, "org.maicord.Server", "Note", nullptr};
    check(connection_subscribe(c, note, on_note, &notes), "subscribe to the server's signal");
    const CallTarget trigger{"org.maicord.Server", "/org/maicord/Server", "org.maicord.Server", "Signal"};
    check(connection_call_sync(c, trigger, nullptr, 3000, &reply) == Status::ok, "ask the server to emit");
    message_release(&reply);
    volatile bool never = false;
    for (u32 i = 0; i < 20 && notes.notes == 0; ++i)
        pump_until(c, &never, 20);
    check(notes.notes == 1, "the emitted signal arrives");

    __atomic_store_n(&server.stop, 1u, __ATOMIC_RELEASE);
    check(thread && thread_wait(thread, 3000), "the server thread ends");
    thread_close(thread);
    check(server.calls >= 4, "the server saw the calls");
    connection_close(server.connection);
}

// Disconnect -------------------------------------------------------------------------------------------------------------

void test_close_with_pending(Connection*) noexcept {
    Connection* c = connection_open_address(g_address);
    Collector collector{};
    Builder b;
    builder_init(&b);
    builder_u32(&b, 2000);
    check(connection_call(c, test_target("Slow"), &b, 10000, collect_text, &collector), "a call pending on a connection");
    builder_release(&b);
    connection_close(c);
    check(collector.done && collector.status == Status::disconnected && collector.completions == 1,
          "closing a connection completes its pending calls as disconnected");
}

// A fake bus -------------------------------------------------------------------------------------------------------------

struct FakeBus {
    int listener;
    u32 mode;  // 0 bad version, 1 oversized body, 2 close, 3 signal byte by byte, 4 bad field type
    volatile u32 finished;
};

bool read_exact(int fd, u8* bytes, u32 count) noexcept {
    u32 done = 0;
    while (done != count) {
        const long got = sys_read(fd, bytes + done, count - done);
        if (got == -kErrorInterrupted)
            continue;
        if (syscall_failed(got) || got == 0)
            return false;
        done += static_cast<u32>(got);
    }
    return true;
}

bool read_until_crlf(int fd, char* line, u32 capacity) noexcept {
    u32 length = 0;
    for (;;) {
        u8 byte;
        if (!read_exact(fd, &byte, 1) || length + 1 >= capacity)
            return false;
        if (byte == '\n') {
            line[length] = 0;
            return true;
        }
        line[length++] = static_cast<char>(byte);
    }
}

void write_all(int fd, const void* bytes, u32 count) noexcept {
    const u8* p = static_cast<const u8*>(bytes);
    u32 done = 0;
    while (done != count) {
        const long sent = sys_write(fd, p + done, count - done);
        if (sent == -kErrorInterrupted)
            continue;
        if (syscall_failed(sent) || sent == 0)
            return;
        done += static_cast<u32>(sent);
    }
}

u32 fake_bus_thread(void* argument) noexcept {
    FakeBus* bus = static_cast<FakeBus*>(argument);
    const long accepted = syscall4(kCallAccept4, bus->listener, 0, 0, 0);
    if (syscall_failed(accepted)) {
        __atomic_store_n(&bus->finished, 1u, __ATOMIC_RELEASE);
        return 1;
    }
    const int fd = static_cast<int>(accepted);
    u8 credentials = 0;
    char line[256];
    bool ok = read_exact(fd, &credentials, 1) && read_until_crlf(fd, line, sizeof(line));  // AUTH EXTERNAL <hex>
    ok = ok && line[0] == 'A';
    if (ok)
        write_all(fd, "OK 00112233445566778899aabbccddeeff\r\n", 37);
    ok = ok && read_until_crlf(fd, line, sizeof(line));  // NEGOTIATE_UNIX_FD
    if (ok)
        write_all(fd, "AGREE_UNIX_FD\r\n", 15);
    ok = ok && read_until_crlf(fd, line, sizeof(line));  // BEGIN
    // Hello.
    u8 header[16];
    ok = ok && read_exact(fd, header, 16);
    u32 hello_serial = 0;
    if (ok) {
        const u32 total = message_size_at(header, 16);
        u8* block = static_cast<u8*>(heap_allocate(total));
        copy_bytes(block, header, 16);
        ok = read_exact(fd, block + 16, total - 16);
        Message hello;
        if (ok && message_parse(&hello, block, total)) {
            hello_serial = hello.serial;
            message_release(&hello);
        } else {
            ok = false;
        }
    }
    if (ok) {
        Builder body;
        builder_init(&body);
        builder_string(&body, ":1.77");
        MessageHeader reply{};
        reply.type = MessageType::method_return;
        reply.serial = 1;
        reply.reply_serial = hello_serial;
        u8* block = nullptr;
        u32 bytes = 0;
        if (message_serialize(reply, body, &block, &bytes)) {
            write_all(fd, block, bytes);
            heap_free(block);
        }
        builder_release(&body);

        if (bus->mode == 0) {
            const u8 garbage[16] = {'l', 4, 0, 7, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0};  // version 7
            write_all(fd, garbage, 16);
        } else if (bus->mode == 1) {
            const u8 garbage[16] = {'l', 4, 0, 1, 0xFF, 0xFF, 0xFF, 0x7F, 5, 0, 0, 0, 0, 0, 0, 0};  // 2 GiB body
            write_all(fd, garbage, 16);
        } else if (bus->mode == 4) {
            // A well-formed header whose one field has a type no field has.
            const u8 garbage[24] = {'l', 4, 0, 1, 0, 0, 0, 0, 9, 0, 0, 0, 8, 0, 0, 0, 1, 1, 'x', 0, 0, 0, 0, 0};
            write_all(fd, garbage, 24);
        } else if (bus->mode == 3) {
            Builder signal_body;
            builder_init(&signal_body);
            builder_string(&signal_body, "byte-by-byte");
            MessageHeader signal{};
            signal.type = MessageType::signal;
            signal.serial = 2;
            signal.path = "/fake";
            signal.interface = "org.fake";
            signal.member = "Slowly";
            u8* wire = nullptr;
            u32 wire_bytes = 0;
            if (message_serialize(signal, signal_body, &wire, &wire_bytes)) {
                for (u32 i = 0; i < wire_bytes; ++i) {
                    write_all(fd, wire + i, 1);
                    const TimeSpec pause{0, 2 * 1000000};
                    syscall2(35 /* nanosleep */, reinterpret_cast<long>(&pause), 0);
                }
                heap_free(wire);
            }
            builder_release(&signal_body);
            const TimeSpec wait{0, 300 * 1000000};
            syscall2(35, reinterpret_cast<long>(&wait), 0);
        }
    }
    sys_close(fd);
    __atomic_store_n(&bus->finished, 1u, __ATOMIC_RELEASE);
    return 0;
}

void on_slowly(void* context, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    const char* text = nullptr;
    if (reader_string(&r, &text, nullptr) && text_equal(text, "byte-by-byte"))
        __atomic_fetch_add(static_cast<volatile u32*>(context), 1u, __ATOMIC_SEQ_CST);
}

void test_fake_bus(u32 mode, const char* label) noexcept {
    // A listening socket on an abstract name.
    const long created = syscall3(kCallSocket, 1, 1 | 0x80000, 0);
    check(!syscall_failed(created), "a listening socket");
    if (syscall_failed(created))
        return;
    const int listener = static_cast<int>(created);
    u8 address[40] = {};
    address[0] = 1;
    const char prefix[] = "maicord-fake-bus-";
    u32 length = 3;
    copy_bytes(address + length, prefix, sizeof(prefix) - 1);
    length += sizeof(prefix) - 1;
    char digits[12];
    u32 count = 0;
    for (u32 n = process_id() * 10 + mode; n || count == 0; n /= 10)
        digits[count++] = static_cast<char>('0' + n % 10);
    char name[48] = {};
    copy_bytes(name, prefix, sizeof(prefix) - 1);
    u32 name_length = sizeof(prefix) - 1;
    while (count) {
        address[length++] = static_cast<u8>(digits[--count]);
        name[name_length++] = address[length - 1];
    }
    syscall3(kCallBind, listener, reinterpret_cast<long>(address), length);
    syscall2(kCallListen, listener, 1);

    static FakeBus bus;
    bus.listener = listener;
    bus.mode = mode;
    bus.finished = 0;
    Thread* thread = thread_start(fake_bus_thread, &bus, 256 * 1024, nullptr);

    char target[96] = "unix:abstract=";
    copy_bytes(target + 14, name, name_length + 1);
    Connection* c = connection_open_address(target);
    check(c != nullptr && text_equal(connection_unique_name(c), ":1.77"), label);
    if (c) {
        static volatile u32 slowly;
        slowly = 0;
        const SignalMatch match{nullptr, "/fake", "org.fake", "Slowly", nullptr};
        // The AddMatch call goes to a bus that does not answer; the local handler still takes the signal.
        connection_subscribe(c, match, on_slowly, const_cast<u32*>(&slowly));
        volatile bool never = false;
        const u64 until = clock_monotonic_milliseconds() + 1500;
        while (connection_alive(c) && clock_monotonic_milliseconds() < until && (mode != 3 || slowly == 0))
            pump_until(c, &never, 10);
        if (mode == 3) {
            check(slowly == 1, "a message that arrives one byte at a time is reassembled");
        } else {
            check(!connection_alive(c), "garbage from the bus ends the connection without a crash");
        }
        connection_close(c);
    }
    if (thread) {
        thread_wait(thread, 3000);
        thread_close(thread);
    }
    sys_close(listener);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_dbus_bus_entry) {
    using namespace maicord;
    g_address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    if (!g_address || !*g_address || !session || !text_equal(g_address, session)) {
        test_write_line("SKIP dbus_bus (MAICORD_DBUS_TEST_ADDRESS and DBUS_SESSION_BUS_ADDRESS name the private bus: tools/test_dbus.sh)");
        test_process_exit(77);
    }
    Connection* c = connection_open_address(g_address);
    test_connect(c);
    if (c) {
        test_calls(c);
        test_async_and_timeouts(c);
        test_typed_bodies(c);
        test_descriptors(c);
        test_signals(c);
        test_exported_object(c);
        test_close_with_pending(c);
        connection_close(c);
    }
    test_fake_bus(0, "the fake bus's Hello is answered");
    test_fake_bus(1, "the fake bus's Hello is answered (oversized)");
    test_fake_bus(4, "the fake bus's Hello is answered (bad field)");
    test_fake_bus(3, "the fake bus's Hello is answered (slow signal)");
    test_exit(g_failures, "dbus_bus");
}
