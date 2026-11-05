// Loads for the Linux network loop (src/platform/net/linux), measured through the shared NetworkLoop interface so the
// same program times any backend: the gateway socket, a burst of REST round trips, a bulk download, UDP voice at 50
// packets per second, a UDP video burst and the file reads and writes of uploads and staged downloads. The peers run
// on plain blocking sockets in other threads. Each scenario prints "<scenario> <metric> <value>" lines: wall time,
// the CPU time and context switches of the loop's thread (the whole process for files, whose work may run in
// helper threads), and the latency from a peer's stamp to the completion that delivered it.
//
//   MaiCordNetworkBenchmark [ws|rest|cdn|udp|burst|read|write|all] [seconds-or-megabytes]
//
// Syscalls per operation are counted from outside: `strace -f -o log` and the BEGIN/END markers of each scenario.
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../tests/support/test_output.hpp"

namespace maicord {
namespace {

constexpr u32 kLimit = 20000;
constexpr u32 kMaximumSamples = 20000;

u32 g_latency[kMaximumSamples];
u32 g_samples;

u64 now_ns() noexcept {
    TimeSpec time{};
    clock_gettime(kClockMonotonic, &time);
    return static_cast<u64>(time.seconds) * 1'000'000'000ull + static_cast<u64>(time.nanoseconds);
}

struct Usage {
    i64 user_seconds, user_microseconds, system_seconds, system_microseconds;
    i64 other[14];
};

struct Meter {
    const char* scenario;
    bool whole_process;
    u64 start;
    Usage usage;
};

Usage usage_now(bool whole_process) noexcept {
    Usage usage{};
    syscall2(98, whole_process ? 0 : 1, reinterpret_cast<long>(&usage)); // getrusage(RUSAGE_SELF / RUSAGE_THREAD)
    return usage;
}

void metric(const char* scenario, const char* name, u64 value) noexcept {
    test_write_text(scenario);
    test_write_text(" ");
    test_write_metric(name, value);
}

void sort_samples() noexcept {
    for (u32 gap = g_samples / 2; gap > 0; gap /= 2) {
        for (u32 index = gap; index < g_samples; ++index) {
            const u32 value = g_latency[index];
            u32 at = index;
            for (; at >= gap && g_latency[at - gap] > value; at -= gap) {
                g_latency[at] = g_latency[at - gap];
            }
            g_latency[at] = value;
        }
    }
}

void begin(Meter* meter, const char* scenario, bool whole_process) noexcept {
    meter->scenario = scenario;
    meter->whole_process = whole_process;
    g_samples = 0;
    test_write_text("BEGIN ");
    test_write_line(scenario);
    meter->usage = usage_now(whole_process);
    meter->start = now_ns();
}

void finish(const Meter& meter) noexcept {
    const u64 wall = now_ns() - meter.start;
    const Usage usage = usage_now(meter.whole_process);
    test_write_text("END ");
    test_write_line(meter.scenario);
    const char* scenario = meter.scenario;
    metric(scenario, "wall_us ", wall / 1000);
    metric(scenario, "cpu_user_us ",
           static_cast<u64>((usage.user_seconds - meter.usage.user_seconds) * 1'000'000 + usage.user_microseconds -
                            meter.usage.user_microseconds));
    metric(scenario, "cpu_system_us ",
           static_cast<u64>((usage.system_seconds - meter.usage.system_seconds) * 1'000'000 + usage.system_microseconds -
                            meter.usage.system_microseconds));
    metric(scenario, "voluntary_switches ", static_cast<u64>(usage.other[12] - meter.usage.other[12]));
    metric(scenario, "involuntary_switches ", static_cast<u64>(usage.other[13] - meter.usage.other[13]));
    if (g_samples) {
        sort_samples();
        metric(scenario, "latency_samples ", g_samples);
        metric(scenario, "latency_p50_us ", g_latency[g_samples / 2]);
        metric(scenario, "latency_p99_us ", g_latency[g_samples - 1 - g_samples / 100]);
        metric(scenario, "latency_max_us ", g_latency[g_samples - 1]);
    }
}

void sample(u64 stamp) noexcept {
    if (g_samples < kMaximumSamples) {
        const u64 now = now_ns();
        g_latency[g_samples++] = static_cast<u32>(now > stamp ? (now - stamp) / 1000 : 0);
    }
}

u64 read_stamp(const u8* bytes) noexcept {
    u64 stamp = 0;
    copy_bytes(&stamp, bytes, sizeof(stamp));
    return stamp;
}

void sleep_until(u64 deadline) noexcept {
    const u64 now = now_ns();
    if (deadline > now) {
        const u64 wait = deadline - now;
        const TimeSpec request{static_cast<i64>(wait / 1'000'000'000ull), static_cast<i64>(wait % 1'000'000'000ull)};
        sys_nanosleep(&request);
    }
}

u16 swap16(u16 value) noexcept {
    return static_cast<u16>((value >> 8) | (value << 8));
}

SockAddrIn loopback_address(u16 port) noexcept {
    SockAddrIn address{};
    address.family = kAfInet;
    address.port = swap16(port);
    address.address[0] = 127;
    address.address[3] = 1;
    return address;
}

// A blocking listener (stream) or bound datagram socket on an ephemeral loopback port; -1 on failure.
int bound_socket(int type, u16* port) noexcept {
    const long opened = sys_socket(kAfInet, type | kSockCloseOnExec, 0);
    if (syscall_failed(opened)) {
        return -1;
    }
    SockAddrIn address = loopback_address(0);
    u32 bytes = sizeof(address);
    if (syscall_failed(sys_bind(static_cast<int>(opened), &address, sizeof(address))) ||
        syscall_failed(sys_getsockname(static_cast<int>(opened), &address, &bytes)) ||
        (type == kSockStream && syscall_failed(sys_listen(static_cast<int>(opened), 16)))) {
        sys_close(static_cast<int>(opened));
        return -1;
    }
    *port = swap16(address.port);
    return static_cast<int>(opened);
}

NetworkAddress loopback(u16 port) noexcept {
    NetworkAddress address{};
    network_address_parse("127.0.0.1", port, &address);
    return address;
}

bool g_failed;

void require(bool valid, const char* label) noexcept {
    if (!valid && !g_failed) {
        g_failed = true;
        test_write_text("FAILED ");
        test_write_line(label);
    }
}

// The next completion of any kind within the limit.
bool poll(NetworkLoop* loop, u32 timeout_ms, NetworkCompletion* completion) noexcept {
    const NetworkPollResult polled = network_loop_poll(loop, timeout_ms);
    if (polled.state != NetworkPollState::completion) {
        return false;
    }
    *completion = polled.completion;
    return true;
}

bool connect_to(NetworkLoop* loop, NetworkConnection* connection, u16 port) noexcept {
    NetworkCompletion completion{};
    return network_connection_connect(loop, connection, loopback(port)).ok() && poll(loop, kLimit, &completion) &&
           completion.kind == NetworkOperationKind::connect && completion.succeeded;
}

// ---- The gateway: sparse small messages with a heartbeat ---------------------------------------------------------

constexpr u32 kMessageBytes = 300;
constexpr u32 kMessageIntervalMs = 50;
constexpr u32 kHeartbeatIntervalMs = 250;

struct GatewayPeer {
    int listener;
    u32 messages;
};

u32 gateway_peer(void* argument) noexcept {
    auto* peer = static_cast<GatewayPeer*>(argument);
    const long accepted = sys_accept(peer->listener, nullptr, nullptr, kSockCloseOnExec);
    if (syscall_failed(accepted)) {
        return 1;
    }
    const int fd = static_cast<int>(accepted);
    u8 message[kMessageBytes]{};
    u8 scratch[256];
    const u64 start = now_ns();
    for (u32 index = 0; index < peer->messages; ++index) {
        sleep_until(start + u64{index} * kMessageIntervalMs * 1'000'000ull);
        const u64 stamp = now_ns();
        copy_bytes(message, &stamp, sizeof(stamp));
        sys_send(fd, message, sizeof(message), kMessageNoSignal);
        sys_recv(fd, scratch, sizeof(scratch), kMessageDontWait);
    }
    sleep_until(now_ns() + 50'000'000ull);
    sys_close(fd);
    return 0;
}

void run_gateway(u32 seconds) noexcept {
    u16 port = 0;
    GatewayPeer peer{bound_socket(kSockStream, &port), seconds * 1000 / kMessageIntervalMs};
    require(peer.listener >= 0, "listener");
    NetworkLoop loop{};
    require(network_loop_initialize(&loop).ok(), "loop");
    Thread* thread = thread_start(gateway_peer, &peer, 128 * 1024, nullptr);
    NetworkConnection connection{};
    require(connect_to(&loop, &connection, port), "connect");
    static u8 buffer[16384];
    static u8 heartbeat[100];
    Meter meter;
    begin(&meter, "ws", false);
    u32 received = 0;
    u64 next_heartbeat = now_ns() + u64{kHeartbeatIntervalMs} * 1'000'000ull;
    require(network_connection_receive(&connection, {buffer, sizeof(buffer)}).ok(), "receive");
    while (received < peer.messages) {
        const u64 now = now_ns();
        if (now >= next_heartbeat) {
            if (!connection.send_pending) {
                network_connection_send(&connection, {heartbeat, sizeof(heartbeat)});
            }
            next_heartbeat += u64{kHeartbeatIntervalMs} * 1'000'000ull;
        }
        const u64 wait = next_heartbeat > now ? (next_heartbeat - now) / 1'000'000ull + 1 : 0;
        const NetworkPollResult polled = network_loop_poll(&loop, static_cast<u32>(wait));
        if (polled.state == NetworkPollState::error) {
            break;
        }
        if (polled.state != NetworkPollState::completion) {
            continue;
        }
        const NetworkCompletion& completion = polled.completion;
        if (completion.kind == NetworkOperationKind::receive && completion.succeeded && completion.transferred) {
            if (completion.transferred >= sizeof(u64)) {
                sample(read_stamp(buffer));
            }
            received += completion.transferred / kMessageBytes + (completion.transferred % kMessageBytes ? 1 : 0);
            network_connection_receive(&connection, {buffer, sizeof(buffer)});
        } else if (completion.kind == NetworkOperationKind::receive) {
            break;
        }
    }
    finish(meter);
    metric("ws", "messages ", received);
    network_connection_close(&connection);
    if (thread) {
        thread_wait(thread, kLimit);
        thread_close(thread);
    }
    sys_close(peer.listener);
    network_loop_release(&loop);
}

// ---- REST: several keep-alive connections, request and response round trips ---------------------------------------

constexpr u32 kRestConnections = 8;
constexpr u32 kRequestBytes = 600;
constexpr u32 kResponseBytes = 4096;

struct RestPeer {
    int listener;
};

u32 rest_peer(void* argument) noexcept {
    const int fd = static_cast<int>(sys_accept(static_cast<RestPeer*>(argument)->listener, nullptr, nullptr, kSockCloseOnExec));
    if (fd < 0) {
        return 1;
    }
    u8 request[kRequestBytes];
    u8 response[kResponseBytes]{};
    for (;;) {
        u32 got = 0;
        while (got < kRequestBytes) {
            const long read = sys_recv(fd, request + got, kRequestBytes - got, 0);
            if (read <= 0) {
                sys_close(fd);
                return 0;
            }
            got += static_cast<u32>(read);
        }
        u32 sent = 0;
        while (sent < kResponseBytes) {
            const long written = sys_send(fd, response + sent, kResponseBytes - sent, kMessageNoSignal);
            if (written <= 0) {
                sys_close(fd);
                return 0;
            }
            sent += static_cast<u32>(written);
        }
    }
}

struct RestClient {
    NetworkConnection connection;
    u32 remaining_requests;
    u32 response_left;
    u8 buffer[kResponseBytes];
};

void run_rest(u32 rounds) noexcept {
    u16 port = 0;
    RestPeer peer{bound_socket(kSockStream, &port)};
    require(peer.listener >= 0, "listener");
    NetworkLoop loop{};
    require(network_loop_initialize(&loop).ok(), "loop");
    Thread* threads[kRestConnections]{};
    static RestClient clients[kRestConnections];
    static u8 request[kRequestBytes];
    for (u32 index = 0; index < kRestConnections; ++index) {
        threads[index] = thread_start(rest_peer, &peer, 128 * 1024, nullptr);
        require(connect_to(&loop, &clients[index].connection, port), "connect");
    }
    Meter meter;
    begin(&meter, "rest", false);
    u32 active = kRestConnections;
    for (RestClient& client : clients) {
        client.remaining_requests = rounds;
        client.response_left = kResponseBytes;
        const u64 stamp = now_ns();
        copy_bytes(request, &stamp, sizeof(stamp));
        network_connection_send(&client.connection, {request, sizeof(request)});
        network_connection_receive(&client.connection, {client.buffer, sizeof(client.buffer)});
    }
    while (active) {
        NetworkCompletion completion{};
        if (!poll(&loop, kLimit, &completion)) {
            require(false, "rest stalled");
            break;
        }
        if (!completion.connection || completion.stale) {
            continue;
        }
        RestClient* client = reinterpret_cast<RestClient*>(completion.connection);
        if (completion.kind == NetworkOperationKind::receive) {
            if (!completion.succeeded || completion.transferred == 0) {
                require(false, "rest receive");
                break;
            }
            client->response_left -= completion.transferred;
            if (client->response_left == 0) {
                if (--client->remaining_requests == 0) {
                    --active;
                    continue;
                }
                client->response_left = kResponseBytes;
                network_connection_send(&client->connection, {request, sizeof(request)});
            }
            network_connection_receive(&client->connection, {client->buffer, sizeof(client->buffer)});
        }
    }
    finish(meter);
    metric("rest", "requests ", u64{kRestConnections} * rounds);
    for (RestClient& client : clients) {
        network_connection_close(&client.connection);
    }
    for (Thread* thread : threads) {
        if (thread) {
            thread_wait(thread, kLimit);
            thread_close(thread);
        }
    }
    sys_close(peer.listener);
    network_loop_release(&loop);
}

// ---- A bulk download ----------------------------------------------------------------------------------------------

struct BulkPeer {
    int listener;
    u64 bytes;
};

u32 bulk_peer(void* argument) noexcept {
    auto* peer = static_cast<BulkPeer*>(argument);
    const long accepted = sys_accept(peer->listener, nullptr, nullptr, kSockCloseOnExec);
    if (syscall_failed(accepted)) {
        return 1;
    }
    static u8 chunk[64 * 1024];
    for (u64 sent = 0; sent < peer->bytes;) {
        const long written = sys_send(static_cast<int>(accepted), chunk, sizeof(chunk), kMessageNoSignal);
        if (written <= 0) {
            break;
        }
        sent += static_cast<u64>(written);
    }
    sys_close(static_cast<int>(accepted));
    return 0;
}

void run_bulk(u32 megabytes) noexcept {
    u16 port = 0;
    BulkPeer peer{bound_socket(kSockStream, &port), u64{megabytes} << 20};
    require(peer.listener >= 0, "listener");
    NetworkLoop loop{};
    require(network_loop_initialize(&loop).ok(), "loop");
    NetworkConnection connection{};
    Thread* thread = thread_start(bulk_peer, &peer, 128 * 1024, nullptr);
    require(connect_to(&loop, &connection, port), "connect");
    static u8 buffer[128 * 1024];
    Meter meter;
    begin(&meter, "cdn", false);
    u64 total = 0;
    u64 receives = 0;
    while (total < peer.bytes) {
        NetworkCompletion completion{};
        if (!network_connection_receive(&connection, {buffer, sizeof(buffer)}).ok() ||
            !poll(&loop, kLimit, &completion) || completion.kind != NetworkOperationKind::receive ||
            !completion.succeeded || completion.transferred == 0) {
            break;
        }
        total += completion.transferred;
        ++receives;
    }
    finish(meter);
    require(total == peer.bytes, "all bytes arrive");
    metric("cdn", "megabytes ", total >> 20);
    metric("cdn", "receives ", receives);
    network_connection_close(&connection);
    if (thread) {
        thread_wait(thread, kLimit);
        thread_close(thread);
    }
    sys_close(peer.listener);
    network_loop_release(&loop);
}

// ---- UDP: voice (50 packets per second, one read slot) and a video burst (eight slots) ---------------------------

struct DatagramPeer {
    int socket;
    u32 packets;
    u32 size;
    // 0: paced at 50 per second; otherwise a burst of this many back to back, then a millisecond's pause.
    u32 burst;
};

u32 datagram_peer(void* argument) noexcept {
    auto* peer = static_cast<DatagramPeer*>(argument);
    SockAddrStorage from;
    u32 from_bytes = sizeof(from);
    u8 hello[16];
    if (syscall_failed(sys_recv_from(peer->socket, hello, sizeof(hello), 0, &from, &from_bytes))) {
        return 1;
    }
    static u8 packet[2048];
    const u64 start = now_ns();
    for (u32 index = 0; index < peer->packets; ++index) {
        if (peer->burst == 0) {
            sleep_until(start + u64{index} * 20'000'000ull);
        } else if (index && index % peer->burst == 0) {
            sleep_until(now_ns() + 1'000'000ull);
        }
        const u64 stamp = now_ns();
        copy_bytes(packet, &stamp, sizeof(stamp));
        sys_send_to(peer->socket, packet, peer->size, 0, &from, from_bytes);
        sys_recv(peer->socket, hello, sizeof(hello), kMessageDontWait);
    }
    return 0;
}

void run_datagrams(const char* scenario, u32 packets, u32 size, u32 burst, u32 slots) noexcept {
    u16 port = 0;
    DatagramPeer peer{bound_socket(kSockDatagram, &port), packets, size, burst};
    require(peer.socket >= 0, "socket");
    NetworkLoop loop{};
    require(network_loop_initialize(&loop).ok(), "loop");
    NetworkDatagram datagram{};
    require(network_datagram_open(&loop, &datagram, loopback(port)).ok(), "open");
    static u8 buffers[8][2048];
    static const u8 hello[4]{'h', 'i', 0, 0};
    Thread* thread = thread_start(datagram_peer, &peer, 128 * 1024, nullptr);
    if (slots > 1) {
        network_datagram_set_receive_buffer(&datagram, 4 * 1024 * 1024);
    }
    network_datagram_send(&datagram, {hello, sizeof(hello)});
    NetworkCompletion completion{};
    require(poll(&loop, kLimit, &completion) && completion.kind == NetworkOperationKind::datagram_send, "hello");
    Meter meter;
    begin(&meter, scenario, false);
    for (u32 slot = 0; slot < slots; ++slot) {
        network_datagram_receive_slot(&datagram, slot, {buffers[slot], sizeof(buffers[slot])});
    }
    u32 received = 0;
    u32 idle = 0;
    while (received < packets && idle < 3) {
        const NetworkPollResult polled = network_loop_poll(&loop, 400);
        if (polled.state != NetworkPollState::completion) {
            ++idle;
            continue;
        }
        idle = 0;
        const NetworkCompletion& done = polled.completion;
        if (done.kind == NetworkOperationKind::datagram_receive && !done.stale) {
            const u32 slot = done.datagram_slot;
            if (done.succeeded) {
                ++received;
                if (burst == 0) {
                    sample(read_stamp(buffers[slot]));
                    if (!datagram.send_pending) {
                        network_datagram_send(&datagram, {hello, sizeof(hello)});
                    }
                }
            }
            network_datagram_receive_slot(&datagram, slot, {buffers[slot], sizeof(buffers[slot])});
        }
    }
    finish(meter);
    metric(scenario, "received ", received);
    network_datagram_close(&datagram);
    for (u32 round = 0; round < 12; ++round) {
        poll(&loop, 20, &completion);
    }
    if (thread) {
        thread_wait(thread, kLimit);
        thread_close(thread);
    }
    sys_close(peer.socket);
    network_loop_release(&loop);
}

// ---- Files: the chunked reads of an upload and the chunked writes of a staged download ----------------------------

bool scratch(const wchar_t* name, wchar_t (&path)[512]) noexcept {
    const DataPathResult root = data_path(path, 512, L"net-loop-bench", true);
    if (!root.ok()) {
        return false;
    }
    dir_create(path);
    u32 length = root.length;
    return wide_append(path, 512, &length, L"\\") && wide_append(path, 512, &length, name);
}

constexpr u32 kReadChunk = 64 * 1024;
constexpr u32 kWriteChunk = 128 * 1024;

bool write_file(NetworkLoop* loop, NetworkFile* file, u64 bytes) noexcept {
    static u8 chunk[kWriteChunk];
    for (u64 offset = 0; offset < bytes; offset += kWriteChunk) {
        NetworkCompletion completion{};
        if (!network_file_write(file, offset, {chunk, kWriteChunk}).ok() || !poll(loop, kLimit, &completion) ||
            !completion.succeeded || completion.transferred != kWriteChunk) {
            return false;
        }
    }
    return true;
}

void run_files(bool reading, u32 megabytes) noexcept {
    const u64 bytes = u64{megabytes} << 20;
    NetworkLoop loop{};
    require(network_loop_initialize(&loop).ok(), "loop");
    wchar_t path[512];
    require(scratch(reading ? L"read.bin" : L"write.bin", path), "scratch");
    file_remove(path);
    NetworkFile file{};
    require(network_file_create(&loop, &file, path).ok(), "create");
    if (reading) {
        require(write_file(&loop, &file, bytes), "fill");
        network_file_close(&file);
        require(network_file_open(&loop, &file, path).ok(), "open");
    }
    static u8 chunk[kReadChunk];
    Meter meter;
    begin(&meter, reading ? "read" : "write", true);
    u64 operations = 0;
    if (reading) {
        for (u64 offset = 0; offset < bytes; offset += kReadChunk) {
            NetworkCompletion completion{};
            if (!network_file_read(&file, offset, {chunk, kReadChunk}).ok() || !poll(&loop, kLimit, &completion) ||
                !completion.succeeded || completion.transferred != kReadChunk) {
                require(false, "read");
                break;
            }
            ++operations;
        }
    } else {
        require(write_file(&loop, &file, bytes), "write");
        operations = bytes / kWriteChunk;
    }
    finish(meter);
    metric(reading ? "read" : "write", "operations ", operations);
    network_file_close(&file);
    NetworkCompletion closed{};
    poll(&loop, 50, &closed);
    file_remove(path);
    network_loop_release(&loop);
}

bool text_equal(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

bool wants(const char* requested, const char* name) noexcept {
    return text_equal(requested, "all") || text_equal(requested, name);
}

u32 number(const char* text, u32 fallback) noexcept {
    if (!text || !text[0]) {
        return fallback;
    }
    u32 value = 0;
    for (; *text >= '0' && *text <= '9'; ++text) {
        value = value * 10 + static_cast<u32>(*text - '0');
    }
    return value ? value : fallback;
}

} // namespace
} // namespace maicord

extern "C" [[noreturn]] void maicord_network_benchmark_entry() noexcept {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const char* scenario = startup.argc > 1 ? startup.argv[1] : "all";
    const char* amount = startup.argc > 2 ? startup.argv[2] : "";
    if (wants(scenario, "ws")) {
        run_gateway(number(amount, 5));
    }
    if (wants(scenario, "rest")) {
        run_rest(number(amount, 200));
    }
    if (wants(scenario, "cdn")) {
        run_bulk(number(amount, 256));
    }
    if (wants(scenario, "udp")) {
        run_datagrams("udp", number(amount, 5) * 50, 120, 0, 1);
    }
    if (wants(scenario, "burst")) {
        run_datagrams("burst", number(amount, 20000), 1200, 64, 8);
    }
    if (wants(scenario, "read")) {
        run_files(true, number(amount, 64));
    }
    if (wants(scenario, "write")) {
        run_files(false, number(amount, 64));
    }
    test_process_exit(g_failed ? 1 : 0);
}
