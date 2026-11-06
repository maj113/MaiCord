#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/platform/net/socket.hpp"
#include "maicord/platform/net/worker.hpp"
#include "../../support/test_output.hpp"
#if !defined(_WIN32)
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#endif

// The network layer (maicord/platform/net/network.hpp) against sockets of this machine, the same on Windows (the
// completion port) and Linux (io_uring): streams, connected datagrams, name lookup, signals from other threads, file
// operations and the worker. The model is checked, not the timing: every operation finishes as exactly one
// completion from network_loop_poll, and closing an owner delivers one for each operation it still had pending.

namespace maicord {
namespace {

constexpr u32 kLimit = 5000;
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

NetworkAddress loopback(u16 port) noexcept {
    NetworkAddress address{};
    network_address_parse("127.0.0.1", port, &address);
    return address;
}

// The next completion of `loop`, which must be of `kind`.
bool next(NetworkLoop* loop, NetworkOperationKind kind, NetworkCompletion* completion) noexcept {
    const NetworkPollResult polled = network_loop_poll(loop, kLimit);
    if (polled.state != NetworkPollState::completion || polled.completion.kind != kind) {
        return false;
    }
    *completion = polled.completion;
    return true;
}

bool idle(NetworkLoop* loop) noexcept {
    return network_loop_poll(loop, 0).state == NetworkPollState::timeout;
}

void test_addresses() noexcept {
    NetworkAddress address{};
    check(network_address_parse("192.0.2.17", 443, &address) && address.family == 4 && address.port == 443 &&
              address.ip[0] == 192 && address.ip[1] == 0 && address.ip[2] == 2 && address.ip[3] == 17,
          "an IPv4 literal");
    check(network_address_parse("2001:db8::1", 80, &address) && address.family == 6 && address.ip[0] == 0x20 &&
              address.ip[1] == 0x01 && address.ip[2] == 0x0d && address.ip[3] == 0xb8 && address.ip[15] == 1 &&
              address.ip[14] == 0 && address.port == 80,
          "an IPv6 literal");
    check(network_address_parse("::1", 1, &address) && address.family == 6 && address.ip[15] == 1, "the IPv6 loopback");
    check(network_address_parse("::", 1, &address) && address.family == 6 && test_filled(address.ip, 16, 0),
          "the IPv6 unspecified address");
    check(network_address_parse("fe80:0:0:0:1:2:3:4", 1, &address) && address.ip[0] == 0xfe && address.ip[15] == 4,
          "eight IPv6 groups");
    check(network_address_parse("::ffff:1.2.3.4", 1, &address) && address.ip[10] == 0xff && address.ip[11] == 0xff &&
              address.ip[12] == 1 && address.ip[15] == 4,
          "an IPv4-mapped IPv6 literal");
    const char* const invalid[]{"",        "example.com", "1.2.3",   "1.2.3.4.5", "256.1.1.1", "1.2.3.-4",
                                "[::1]",   "1::2::3",     ":1",      "1:",        "1:2:3:4:5:6:7:8:9",
                                "12345::", "::g",         "1.2.3.4 ", " 1.2.3.4"};
    bool all_refused = true;
    for (const char* text : invalid) {
        all_refused = all_refused && !network_address_parse(text, 1, &address) && address.family == 0;
    }
    check(all_refused, "malformed literals are refused");
    check(network_address_parse(nullptr, 1, &address) == false, "no text");
    check(network_address_is_ipv6_literal(L"2001:db8::1") && network_address_is_ipv6_literal(L"::1") &&
              !network_address_is_ipv6_literal(L"1.2.3.4") && !network_address_is_ipv6_literal(L"example.com") &&
              !network_address_is_ipv6_literal(L"") && !network_address_is_ipv6_literal(nullptr),
          "IPv6 literal detection");
}

struct Pair {
    Socket listener;
    Socket server;
    SocketWatch listener_watch;
    SocketWatch server_watch;
    NetworkConnection client;
    u16 port;
};

// A listener, a client connected to it through the loop, and the accepted server end.
bool open_pair(NetworkLoop* loop, Pair* pair) noexcept {
    *pair = {};
    const SocketListenResult listening = socket_listen_loopback(&pair->listener, 0, 0, 4);
    if (!listening.ok() || !socket_watch_open(&pair->listener_watch) ||
        !socket_watch_attach(&pair->listener_watch, &pair->listener, kSocketAccept)) {
        return false;
    }
    pair->port = listening.port;
    if (!network_connection_connect(loop, &pair->client, loopback(pair->port)).ok()) {
        return false;
    }
    NetworkCompletion connected{};
    if (!next(loop, NetworkOperationKind::connect, &connected) || !connected.succeeded || connected.stale ||
        pair->client.state != NetworkConnectionState::connected) {
        return false;
    }
    for (u32 attempt = 0; attempt < 50; ++attempt) {
        bool from_loopback = false;
        i32 error = 0;
        if (socket_accept(&pair->listener, &pair->server, &from_loopback, &error)) {
            return from_loopback && socket_watch_open(&pair->server_watch) &&
                   socket_watch_attach(&pair->server_watch, &pair->server, kSocketRead | kSocketWrite | kSocketClose);
        }
        if (error != WSAEWOULDBLOCK || wait_one(socket_watch_waitable(pair->listener_watch), kLimit) != WAIT_OBJECT_0) {
            return false;
        }
        u32 events = 0;
        socket_watch_poll(&pair->listener_watch, pair->listener, &events);
    }
    return false;
}

void close_pair(Pair* pair) noexcept {
    network_connection_close(&pair->client);
    socket_close(&pair->server);
    socket_close(&pair->listener);
    socket_watch_close(&pair->server_watch);
    socket_watch_close(&pair->listener_watch);
}

// Reads exactly `bytes` from the accepted end, waiting for what has not arrived; -1 on a failure or a close.
bool server_read(Pair* pair, void* data, u32 bytes) noexcept {
    u32 done = 0;
    for (u32 attempt = 0; attempt < 200 && done < bytes; ++attempt) {
        const i32 received = socket_receive(pair->server, static_cast<u8*>(data) + done, bytes - done);
        if (received > 0) {
            done += static_cast<u32>(received);
            continue;
        }
        if (received == 0 || network_last_error() != WSAEWOULDBLOCK ||
            wait_one(socket_watch_waitable(pair->server_watch), kLimit) != WAIT_OBJECT_0) {
            return false;
        }
        u32 events = 0;
        socket_watch_poll(&pair->server_watch, pair->server, &events);
    }
    return done == bytes;
}

void test_stream(NetworkLoop* loop) noexcept {
    Pair pair;
    check(open_pair(loop, &pair), "a connection through the loop is accepted");
    check(idle(loop), "a connect leaves no other completion");

    u8 reply[64]{};
    check(network_connection_receive(&pair.client, {reply, sizeof(reply)}).ok() && pair.client.receive_pending,
          "post a receive");
    check(!network_connection_receive(&pair.client, {reply, sizeof(reply)}).ok(), "one receive at a time");
    check(idle(loop), "a receive with no data is still pending");

    const u8 request[]{'h', 'e', 'l', 'l', 'o'};
    check(network_connection_send(&pair.client, {request, 5}).ok() && pair.client.send_pending, "post a send");
    NetworkCompletion sent{};
    check(next(loop, NetworkOperationKind::send, &sent) && sent.succeeded && !sent.stale && sent.transferred == 5 &&
              sent.connection == &pair.client && !pair.client.send_pending,
          "the send completes with its length");
    u8 received[8]{};
    check(server_read(&pair, received, 5) && bytes_equal(received, "hello", 5), "the server reads the request");

    check(socket_send(pair.server, "world", 5) == 5, "the server answers");
    NetworkCompletion answer{};
    check(next(loop, NetworkOperationKind::receive, &answer) && answer.succeeded && !answer.stale &&
              answer.transferred == 5 && bytes_equal(reply, "world", 5) && !pair.client.receive_pending &&
              pair.client.state == NetworkConnectionState::connected,
          "the receive completes with the answer");

    // Data that arrives before a receive is posted is delivered by the next poll after it.
    check(socket_send(pair.server, "later", 5) == 5, "the server sends ahead of a receive");
    check(network_connection_receive(&pair.client, {reply, sizeof(reply)}).ok(), "post the late receive");
    check(next(loop, NetworkOperationKind::receive, &answer) && answer.transferred == 5 &&
              bytes_equal(reply, "later", 5),
          "the receive finds the waiting data");

    // A buffer smaller than what arrived takes it in pieces.
    check(socket_send(pair.server, "0123456789", 10) == 10, "ten bytes");
    u8 piece[4]{};
    u32 total = 0;
    for (u32 round = 0; round < 3 && total < 10; ++round) {
        check(network_connection_receive(&pair.client, {piece, sizeof(piece)}).ok(), "post a short receive");
        check(next(loop, NetworkOperationKind::receive, &answer) && answer.transferred > 0 && answer.transferred <= 4,
              "a short receive completes");
        total += answer.transferred;
    }
    check(total == 10, "the pieces add up");

    // The peer closing is a zero-length receive and the connection's peer_closed state.
    socket_close(&pair.server);
    check(network_connection_receive(&pair.client, {reply, sizeof(reply)}).ok(), "post a receive for the close");
    check(next(loop, NetworkOperationKind::receive, &answer) && answer.succeeded && answer.transferred == 0 &&
              pair.client.state == NetworkConnectionState::peer_closed,
          "a close is a zero-length receive");
    close_pair(&pair);
    check(idle(loop), "nothing is left after the close");
}

void test_cancelled_on_close(NetworkLoop* loop) noexcept {
    Pair pair;
    check(open_pair(loop, &pair), "a connection for the cancel test");
    u8 reply[16]{};
    check(network_connection_receive(&pair.client, {reply, sizeof(reply)}).ok(), "post a receive that stays pending");
    network_connection_close(&pair.client);
    NetworkCompletion cancelled{};
    check(next(loop, NetworkOperationKind::receive, &cancelled) && !cancelled.succeeded && cancelled.stale &&
              cancelled.connection == &pair.client,
          "closing completes the pending receive once, as stale");
    check(idle(loop), "and only once");
    check(pair.client.state == NetworkConnectionState::closed && !network_connection_has_socket(pair.client),
          "the connection is closed");
    close_pair(&pair);

    // A send that cannot finish (nobody reads) is cancelled the same way.
    static u8 large[32u * 1024 * 1024];
    check(open_pair(loop, &pair), "a connection for the send cancel test");
    check(network_connection_send(&pair.client, {large, sizeof(large)}).ok() && pair.client.send_pending,
          "post a send too large to finish");
    network_connection_close(&pair.client);
    NetworkCompletion send_done{};
    check(next(loop, NetworkOperationKind::send, &send_done) && send_done.stale && send_done.connection == &pair.client,
          "closing completes the pending send once, as stale");
    check(idle(loop), "and only once for the send");
    close_pair(&pair);

    // Closing a connection with nothing pending delivers nothing.
    check(open_pair(loop, &pair), "a connection with nothing pending");
    network_connection_close(&pair.client);
    check(idle(loop), "an idle connection closes silently");
    close_pair(&pair);
}

void test_refused(NetworkLoop* loop) noexcept {
    Socket listener;
    const SocketListenResult listening = socket_listen_loopback(&listener, 0, 0, 1);
    check(listening.ok(), "pick a port");
    socket_close(&listener);
    NetworkConnection connection{};
    NetworkCompletion completion{};
    const NetworkResult started = network_connection_connect(loop, &connection, loopback(listening.port));
    // A refusal is the connect's completion; it is never returned by the call that starts it.
    check(started.ok() && next(loop, NetworkOperationKind::connect, &completion) && !completion.succeeded &&
              !completion.stale && completion.native_error != 0 &&
              connection.state == NetworkConnectionState::failed,
          "a refused connect completes as a failure");
    network_connection_close(&connection);
    check(idle(loop), "a refused connect leaves nothing");
    check(!network_connection_connect(loop, &connection, NetworkAddress{}).ok(), "no address");
}

// The peer of a connected datagram: a bound UDP socket with a watch, and the address a datagram came from.
bool receive_datagram(Socket* peer, SocketWatch* watch, void* data, u32 bytes, i32* count, NetworkAddress* from) noexcept {
    for (u32 attempt = 0; attempt < 50; ++attempt) {
        *count = socket_receive_from(*peer, data, bytes, from);
        if (*count >= 0) {
            return true;
        }
        if (network_last_error() != WSAEWOULDBLOCK || wait_one(socket_watch_waitable(*watch), kLimit) != WAIT_OBJECT_0) {
            return false;
        }
        u32 events = 0;
        socket_watch_poll(watch, *peer, &events);
    }
    return false;
}

void test_datagrams(NetworkLoop* loop) noexcept {
    Socket peer;
    SocketWatch watch;
    u16 port = 0;
    i32 error = 0;
    check(socket_bind_datagram_loopback(&peer, &port, &error) && socket_watch_open(&watch) &&
              socket_watch_attach(&watch, &peer, kSocketRead),
          "a UDP peer");
    NetworkDatagram datagram{};
    check(!network_datagram_open(loop, &datagram, NetworkAddress{}).ok(), "no peer address");
    check(!network_datagram_open(loop, &datagram, loopback(0)).ok(), "no peer port");
    check(network_datagram_open(loop, &datagram, loopback(port)).ok() && datagram.open && datagram.generation != 0,
          "open a connected datagram");
    check(!network_datagram_open(loop, &datagram, loopback(port)).ok(), "it opens once");

    u8 first[16]{};
    u8 second[16]{};
    u8 third[16]{};
    check(network_datagram_receive_slot(&datagram, 0, {first, sizeof(first)}).ok() &&
              network_datagram_receive_slot(&datagram, 1, {second, sizeof(second)}).ok() &&
              network_datagram_receive_slot(&datagram, 2, {third, sizeof(third)}).ok() &&
              !network_datagram_receive_slot(&datagram, 1, {second, sizeof(second)}).ok() &&
              !network_datagram_receive_slot(&datagram, kNetworkDatagramReceiveSlots, {second, sizeof(second)}).ok(),
          "post three receive slots");

    const u8 ping[]{'p', 'i', 'n', 'g'};
    check(network_datagram_send(&datagram, {ping, 4}).ok() && datagram.send_pending, "send a datagram");
    NetworkCompletion done{};
    check(next(loop, NetworkOperationKind::datagram_send, &done) && done.succeeded && !done.stale &&
              done.transferred == 4 && done.datagram == &datagram && !datagram.send_pending,
          "the send completes");
    u8 heard[16]{};
    i32 count = 0;
    NetworkAddress from{};
    check(receive_datagram(&peer, &watch, heard, sizeof(heard), &count, &from) && count == 4 &&
              bytes_equal(heard, "ping", 4) && from.family == 4 && from.ip[0] == 127,
          "the peer hears the datagram and its sender");

    // The reads are served in the order they were posted, an empty datagram being a datagram too.
    check(socket_send_to(peer, "one", 3, from) == 3 && socket_send_to(peer, "two!", 4, from) == 4 &&
              socket_send_to(peer, "", 0, from) == 0,
          "the peer answers three times");
    NetworkCompletion reads[3]{};
    for (u32 index = 0; index < 3; ++index) {
        check(next(loop, NetworkOperationKind::datagram_receive, &reads[index]) && reads[index].succeeded &&
                  !reads[index].stale,
              "a read completes");
    }
    check(reads[0].datagram_slot == 0 && reads[0].transferred == 3 && bytes_equal(first, "one", 3) &&
              reads[1].datagram_slot == 1 && reads[1].transferred == 4 && bytes_equal(second, "two!", 4) &&
              reads[2].datagram_slot == 2 && reads[2].transferred == 0,
          "in order, the empty one included");
    check(!datagram.receive_pending && !datagram.extra_receive_pending, "no read is pending");

    // A datagram larger than the buffer fails the read with the bytes that fit; the voice code drops it.
    u8 small[4]{};
    check(network_datagram_receive(&datagram, {small, sizeof(small)}).ok(), "post a small read");
    check(socket_send_to(peer, "too long for it", 15, from) == 15, "send a long datagram");
    check(next(loop, NetworkOperationKind::datagram_receive, &done) && !done.succeeded && !done.stale &&
              (done.native_error == ERROR_MORE_DATA || done.native_error == WSAEMSGSIZE) && done.transferred <= 4,
          "a truncated read fails with the truncation error");

    // Zero-length sends are legal.
    check(network_datagram_send(&datagram, {}).ok() &&
              next(loop, NetworkOperationKind::datagram_send, &done) && done.succeeded && done.transferred == 0,
          "a zero-length send");
    check(receive_datagram(&peer, &watch, heard, sizeof(heard), &count, nullptr) && count == 0, "the peer hears it");

    // Closing delivers one cancelled completion per pending read, flagged stale, and the socket stays closed.
    check(network_datagram_receive(&datagram, {first, sizeof(first)}).ok() &&
              network_datagram_receive_slot(&datagram, 3, {third, sizeof(third)}).ok(),
          "post two reads and close");
    network_datagram_close(&datagram);
    u32 cancelled = 0;
    bool slots_seen[2]{};
    for (u32 index = 0; index < 2; ++index) {
        if (next(loop, NetworkOperationKind::datagram_receive, &done) && done.stale && !done.succeeded) {
            ++cancelled;
            slots_seen[done.datagram_slot == 0 ? 0 : 1] = true;
        }
    }
    check(cancelled == 2 && slots_seen[0] && slots_seen[1] && idle(loop), "one cancelled completion per read");
    check(!datagram.open && network_datagram_idle(datagram), "the datagram is idle and closed");
    check(network_datagram_open(loop, &datagram, loopback(port)).ok() && datagram.generation > 1,
          "it opens again with a new generation");
    check(network_datagram_set_receive_buffer(&datagram, 256 * 1024), "a receive buffer");
    network_datagram_close(&datagram);
    socket_close(&peer);
    socket_watch_close(&watch);
}

struct Posting {
    NetworkLoop* loop;
    NetworkSignal* signal;
    u32 value;
    volatile LONG result;
};

u32 post_signal(void* argument) noexcept {
    auto* posting = static_cast<Posting*>(argument);
    posting->result = network_signal_post(posting->loop, posting->signal, posting->value).ok() ? 1 : -1;
    return 0;
}

void test_signals(NetworkLoop* loop) noexcept {
    check(idle(loop), "an idle loop times out");
    NetworkSignal signal{};
    Posting posting{loop, &signal, 0x1234, 0};
    Thread* thread = thread_start(post_signal, &posting, 128 * 1024, nullptr);
    check(thread != nullptr, "start a thread that posts");
    NetworkCompletion done{};
    check(next(loop, NetworkOperationKind::signal, &done) && done.signal == &signal && done.signal_value == 0x1234 &&
              done.succeeded && !done.stale,
          "a signal posted from another thread wakes the poll");
    if (thread) {
        check(thread_wait(thread, kLimit), "the thread ended");
        thread_close(thread);
    }
    check(posting.result == 1, "the post succeeded");
    check(network_signal_post(loop, &signal, 1).ok() && !network_signal_post(loop, &signal, 2).ok(),
          "a signal has one post outstanding");
    check(next(loop, NetworkOperationKind::signal, &done) && done.signal_value == 1, "the first post arrives");
    check(network_signal_post(loop, &signal, 3).ok() && next(loop, NetworkOperationKind::signal, &done) &&
              done.signal_value == 3,
          "and the signal can be posted again");
    NetworkLoop uninitialized{};
    check(!network_signal_post(&uninitialized, &signal, 1).ok(), "no post to a loop that is not ready");
    check(network_loop_poll(&uninitialized, 0).state == NetworkPollState::error, "no poll of one either");
}

void test_resolver(NetworkLoop* loop) noexcept {
    NetworkResolver resolver{};
    check(!network_resolver_start(loop, &resolver, L"", L"80").ok(), "an empty host");
    check(network_resolver_start(loop, &resolver, L"127.0.0.1", L"8080").ok() && resolver.pending &&
              resolver.state == NetworkResolverState::resolving,
          "start a lookup of an address literal");
    check(!network_resolver_start(loop, &resolver, L"127.0.0.1", L"8080").ok(), "one lookup at a time");
    check(!network_resolver_release(&resolver), "a pending resolver is not released");
    NetworkCompletion done{};
    check(next(loop, NetworkOperationKind::resolve, &done) && done.succeeded && !done.stale &&
              done.resolver == &resolver && !resolver.pending && resolver.state == NetworkResolverState::resolved,
          "the lookup completes");
    check(resolver.address_count >= 1 && resolver.addresses[0].family == 4 && resolver.addresses[0].port == 8080 &&
              resolver.addresses[0].ip[0] == 127 && resolver.addresses[0].ip[3] == 1,
          "with the address and the port");
    check(network_resolver_release(&resolver) && resolver.address_count == 0, "release");

    check(network_resolver_start(loop, &resolver, L"localhost", L"443").ok() &&
              next(loop, NetworkOperationKind::resolve, &done) && done.succeeded && resolver.address_count >= 1 &&
              resolver.address_count <= kNetworkResolverMaximumAddresses && resolver.addresses[0].port == 443,
          "localhost resolves");
    bool all_loopback = true;
    for (u32 index = 0; index < resolver.address_count; ++index) {
        const NetworkAddress& address = resolver.addresses[index];
        all_loopback = all_loopback && ((address.family == 4 && address.ip[0] == 127) ||
                                        (address.family == 6 && address.ip[15] == 1 && address.ip[0] == 0));
    }
    check(all_loopback, "to loopback addresses");
    network_resolver_release(&resolver);

    // Cancelling right away yields exactly one completion: the cancel, or the answer if it was first.
    for (u32 round = 0; round < 20; ++round) {
        check(network_resolver_start(loop, &resolver, L"localhost", L"80").ok(), "start a lookup to cancel");
        network_resolver_cancel(&resolver);
        const bool one = next(loop, NetworkOperationKind::resolve, &done) && done.resolver == &resolver && !resolver.pending;
        check(one && (resolver.state == NetworkResolverState::cancelled ||
                      resolver.state == NetworkResolverState::resolved),
              "one completion for a cancelled lookup");
        check(idle(loop) && network_resolver_release(&resolver), "and no other");
    }
}

// The scratch directory of the file test, below the private data directory.
bool scratch_path(const wchar_t* name, wchar_t (&path)[512]) noexcept {
    const DataPathResult root = data_path(path, 512, L"net-loop-test", true);
    if (!root.ok()) {
        return false;
    }
    dir_create(path);
    u32 length = root.length;
    return wide_append(path, 512, &length, L"\\") && wide_append(path, 512, &length, name);
}

void test_files(NetworkLoop* loop) noexcept {
    wchar_t path[512];
    check(scratch_path(L"staged.bin", path), "a scratch path");
    file_remove(path);
    NetworkFile writer{};
    check(network_file_create(loop, &writer, path).ok() && writer.handle != nullptr, "create a staging file");
    NetworkFile again{};
    check(!network_file_create(loop, &again, path).ok(), "an existing file is not replaced");

    static u8 content[96 * 1024];
    for (u32 index = 0; index < sizeof(content); ++index) {
        content[index] = static_cast<u8>(index * 7 + (index >> 8));
    }
    NetworkCompletion done{};
    check(network_file_write(&writer, 0, {content, 40000}).ok() && writer.pending &&
              !network_file_write(&writer, 40000, {content + 40000, 10}).ok(),
          "start a write; one operation at a time");
    check(next(loop, NetworkOperationKind::file_write, &done) && done.succeeded && !done.stale &&
              done.transferred == 40000 && done.file == &writer && !writer.pending,
          "the write completes");
    check(network_file_write(&writer, 40000, {content + 40000, sizeof(content) - 40000}).ok() &&
              next(loop, NetworkOperationKind::file_write, &done) && done.succeeded &&
              done.transferred == sizeof(content) - 40000,
          "the second write completes");
    network_file_close(&writer);
    check(writer.handle == nullptr && idle(loop), "close the staging file");

    NetworkFile reader{};
    check(!network_file_open(loop, &reader, nullptr).ok(), "no path");
    wchar_t missing[512];
    check(scratch_path(L"missing.bin", missing), "a missing path");
    check(!network_file_open(loop, &reader, missing).ok(), "a missing file is not opened");
    check(network_file_open(loop, &reader, path).ok() && reader.bytes == sizeof(content), "open for reading");
    FileSnapshot snapshot{};
    check(file_snapshot_read(reader.handle, &snapshot, nullptr) && file_identity_equal(snapshot.identity, reader.identity),
          "the identity captured at open");

    static u8 read_back[sizeof(content)];
    check(!network_file_read(&reader, sizeof(content), {read_back, 1}).ok() &&
              !network_file_read(&reader, 0, {read_back, sizeof(content) + 1}).ok() &&
              !network_file_read(&reader, 0, {}).ok(),
          "reads outside the file are refused");
    check(network_file_read(&reader, 100, {read_back, 4096}).ok() && reader.pending &&
              next(loop, NetworkOperationKind::file_read, &done) && done.succeeded && done.transferred == 4096 &&
              bytes_equal(read_back, content + 100, 4096) && !reader.pending,
          "read in the middle");
    check(network_file_read(&reader, 0, {read_back, sizeof(read_back)}).ok() &&
              next(loop, NetworkOperationKind::file_read, &done) && done.succeeded &&
              done.transferred == sizeof(content) && bytes_equal(read_back, content, sizeof(content)),
          "read it all");

    // Closing with a read pending still completes it once, and the descriptor is not reused under it.
    check(network_file_read(&reader, 0, {read_back, sizeof(read_back)}).ok(), "start a read to cancel");
    network_file_close(&reader);
    check(next(loop, NetworkOperationKind::file_read, &done) && done.file == &reader && !reader.pending &&
              reader.handle == nullptr,
          "the closed file's read completes once");
    check(idle(loop), "and only once");
    file_remove(path);
}

// A send larger than the socket buffers moves in pieces: it completes once, with every byte, in order.
void test_large_send(NetworkLoop* loop) noexcept {
    Pair pair;
    check(open_pair(loop, &pair), "a connection for the large send");
    constexpr u32 kBytes = 3u * 1024 * 1024;
    static u8 payload[kBytes];
    static u8 sink[64 * 1024];
    for (u32 index = 0; index < kBytes; ++index) {
        payload[index] = static_cast<u8>(index * 31 + (index >> 10));
    }
    check(network_connection_send(&pair.client, {payload, kBytes}).ok(), "post a send of three megabytes");
    u32 received = 0;
    bool in_order = true;
    bool finished = false;
    bool whole = false;
    for (u32 round = 0; round < 5000 && !(finished && received == kBytes); ++round) {
        for (;;) {
            const i32 count = socket_receive(pair.server, sink, sizeof(sink));
            if (count <= 0) {
                break;
            }
            in_order = in_order && bytes_equal(sink, payload + received, static_cast<u32>(count));
            received += static_cast<u32>(count);
        }
        const NetworkPollResult polled = network_loop_poll(loop, 2);
        if (polled.state == NetworkPollState::completion && polled.completion.kind == NetworkOperationKind::send) {
            finished = true;
            whole = polled.completion.succeeded && !polled.completion.stale && polled.completion.transferred == kBytes;
        }
    }
    check(finished && whole, "the send completes once with its full length");
    check(received == kBytes && in_order, "the peer reads every byte in order");
    check(idle(loop), "and nothing else completes");
    close_pair(&pair);
}

// More requests than the ring has entries are queued before the loop is polled once; each still finishes once.
void test_many_operations(NetworkLoop* loop) noexcept {
    constexpr u32 kCount = 140;
    static NetworkConnection clients[kCount];
    static Socket servers[kCount];
    static u8 inbound[kCount][8];
    static const u8 outbound[8] = {'o', 'u', 't', 'b', 'o', 'u', 'n', 'd'};
    Socket listener;
    const SocketListenResult listening = socket_listen_loopback(&listener, 0, 0, 256);
    check(listening.ok(), "a listener for many connections");
    u32 connected = 0;
    u32 accepted = 0;
    for (u32 index = 0; index < kCount; ++index) {
        NetworkCompletion done{};
        connected += network_connection_connect(loop, &clients[index], loopback(listening.port)).ok() &&
                     next(loop, NetworkOperationKind::connect, &done) && done.succeeded;
    }
    for (u32 index = 0; index < kCount; ++index) {
        for (u32 attempt = 0; attempt < 200; ++attempt) {
            bool from_loopback = false;
            i32 error = 0;
            if (socket_accept(&listener, &servers[accepted], &from_loopback, &error)) {
                ++accepted;
                break;
            }
            thread_sleep(1);
        }
    }
    check(connected == kCount && accepted == kCount, "many connections are made and accepted");
    u32 posted = 0;
    for (u32 index = 0; index < kCount; ++index) {
        posted += network_connection_receive(&clients[index], {inbound[index], 8}).ok();
        posted += network_connection_send(&clients[index], {outbound, 8}).ok();
    }
    check(posted == 2 * kCount, "two hundred and eighty requests are queued at once");
    u32 sent = 0;
    while (sent < kCount) {
        NetworkCompletion done{};
        if (!next(loop, NetworkOperationKind::send, &done) || !done.succeeded || done.transferred != 8) {
            break;
        }
        ++sent;
    }
    check(sent == kCount, "every send completes");
    u32 answered = 0;
    for (u32 index = 0; index < kCount; ++index) {
        answered += socket_send(servers[index], "answer!!", 8) == 8;
    }
    u32 received = 0;
    u32 intact = 0;
    while (received < answered) {
        NetworkCompletion done{};
        if (!next(loop, NetworkOperationKind::receive, &done) || !done.succeeded) {
            break;
        }
        ++received;
        intact += done.transferred == 8 && bytes_equal(inbound[done.connection - clients], "answer!!", 8);
    }
    check(answered == kCount && received == kCount && intact == kCount, "every receive completes with its answer");
    for (u32 index = 0; index < kCount; ++index) {
        network_connection_close(&clients[index]);
        socket_close(&servers[index]);
    }
    socket_close(&listener);
    check(idle(loop), "nothing is left");
}

#if !defined(_WIN32)
bool contains(const char* text, const char* part) noexcept {
    for (; *text; ++text) {
        const char* at = text;
        const char* want = part;
        while (*want && *at == *want) {
            ++at;
            ++want;
        }
        if (!*want) {
            return true;
        }
    }
    return false;
}

struct FilterInstruction {
    u16 code;
    u8 jump_true;
    u8 jump_false;
    u32 operand;
};
struct FilterProgram {
    u16 length;
    const FilterInstruction* instructions;
};

// The network loop is io_uring and nothing else: a system that refuses io_uring_setup (a seccomp profile with EPERM, as
// Docker's and Podman's defaults answer, or ENOSYS, as a runtime that does not know the call answers) leaves the client
// no loop, and network_unavailable_reason says why instead of the loop failing mysteriously.
void test_ring_refused(int error, const char* expected) noexcept {
    check(network_unavailable_reason() == nullptr, "io_uring is available before the filter");
    constexpr u32 kRingSetupNumber = 425;
    const FilterInstruction instructions[] = {
        {0x20, 0, 0, 0},                                          // load the system call number
        {0x15, 0, 1, kRingSetupNumber},                           // io_uring_setup: refuse it
        {0x06, 0, 0, 0x00050000u | static_cast<u32>(error)},      // return errno
        {0x06, 0, 0, 0x7fff0000u},                                // allow
    };
    const FilterProgram program{4, instructions};
    check(!syscall_failed(syscall5(kSysPrctl, 38, 1, 0, 0, 0)) && // PR_SET_NO_NEW_PRIVS
              !syscall_failed(syscall3(317, 1, 0, reinterpret_cast<long>(&program))), // seccomp(SET_MODE_FILTER)
          "install the filter");
    const char* reason = network_unavailable_reason();
    check(reason != nullptr && contains(reason, expected), "the reason names the cause");
    NetworkLoop loop{};
    const NetworkResult result = network_loop_initialize(&loop);
    check(!result.ok() && result.error == NetworkError::completion_port_create && result.native_error != 0 &&
              !network_loop_ready(loop),
          "no loop is made");
    NetworkSignal signal{};
    check(!network_signal_post(&loop, &signal, 1).ok() && network_loop_poll(&loop, 0).state == NetworkPollState::error,
          "a loop that was not made refuses work");
    network_loop_release(&loop);
}
#endif

struct WorkerContext {
    volatile LONG started;
    volatile LONG stopped;
    volatile LONG other;
};

NetworkWorkerStep worker_handler(void* context, NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    auto* state = static_cast<WorkerContext*>(context);
    switch (event.kind) {
    case NetworkWorkerEventKind::started:
        InterlockedExchange(&state->started, network_loop_ready(*loop) ? 1 : -1);
        return {INFINITE, false};
    case NetworkWorkerEventKind::stop_requested:
        InterlockedExchange(&state->stopped, 1);
        return {0, true};
    default:
        InterlockedIncrement(&state->other);
        return {INFINITE, false};
    }
}

void test_worker() noexcept {
    WorkerContext context{};
    NetworkWorker worker{};
    check(network_worker_start(&worker, worker_handler, &context).ok(), "start a worker");
    check(context.started == 1, "its loop is ready when it starts");
    check(!network_worker_start(&worker, worker_handler, &context).ok(), "a running worker is not started again");
    check(network_worker_request_stop(&worker).ok(), "ask it to stop");
    check(network_worker_join(&worker, kLimit).ok(), "join it");
    check(context.stopped == 1 && context.other == 0, "it saw the stop and nothing else");
    check(!network_worker_join(&worker, 0).ok(), "a joined worker cannot be joined again");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_network_loop_entry) {
    using namespace maicord;
#if !defined(_WIN32)
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    if (contains(mode, "ring-refused") || contains(mode, "ring-hidden")) {
        const bool hidden = contains(mode, "ring-hidden");
        test_ring_refused(hidden ? 38 : 1, hidden ? "Linux 6.1" : "seccomp");
        test_exit(g_failures, "network_loop_ring_refused");
    }
#endif
    const i32 started = socket_startup();
    check(started == 0, "sockets start");
    test_addresses();
    NetworkLoop loop{};
    check(network_loop_initialize(&loop).ok() && network_loop_ready(loop), "initialize the loop");
    test_signals(&loop);
    test_stream(&loop);
    test_cancelled_on_close(&loop);
    test_refused(&loop);
    test_datagrams(&loop);
    test_resolver(&loop);
    test_files(&loop);
    test_large_send(&loop);
    test_many_operations(&loop);
    network_loop_release(&loop);
    check(!network_loop_ready(loop), "release the loop");
    test_worker();
    socket_cleanup();
    test_exit(g_failures, "network_loop");
}
