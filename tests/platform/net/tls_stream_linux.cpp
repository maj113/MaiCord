// tls_stream.hpp over the network layer: the real stack of a Linux client (resolver pool, epoll loop, non-blocking
// sockets, OpenSSL through the TLS client) talking to local `openssl s_server` processes. argv[1] selects a case:
//
//   --get <port> <name> <protocol>      GET / from an s_server status page (-www); the page must report <protocol>
//                                       ("TLSv1.3") and the response ends with the server's close_notify
//   --download <port> <name> <bytes>    GET /payload from a file server (-WWW): <bytes> bytes of a repeating pattern
//                                       over many records, checked byte by byte
//   --reject <port> <name>              the handshake must fail with TlsStreamError::tls (a name the certificate does
//                                       not carry)
//   --refused <port>                    nothing listens there: the stream fails with TlsStreamError::connect
//   --cancel <port> <name>              the stream is closed while it connects and drains with no completion left over
//
// tests/platform/net/tls_stream_smoke.sh starts the servers and runs the cases, with the certificate it made as the
// only trust (SSL_CERT_FILE). Nothing here reaches beyond the loopback.
#include "maicord/core/memory.hpp"
#include "maicord/platform/net/tls_stream.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

constexpr u32 kEncryptedCapacity = 128 * 1024;
constexpr u32 kHandshakeCapacity = 32 * 1024;
constexpr u32 kRecordCapacity = 32 * 1024;
constexpr u32 kPollMilliseconds = 10'000;
constexpr u32 kHeadCapacity = 16 * 1024;

u32 g_failures = 0;

void check(bool condition, const char* name) noexcept {
    test_check(&g_failures, condition, name);
}

bool text_equal(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

u64 parse_number(const char* text) noexcept {
    u64 value = 0;
    for (; *text; ++text) {
        value = value * 10 + static_cast<u32>(*text - '0');
    }
    return value;
}

const wchar_t* wide_argument(const char* text) noexcept {
    static wchar_t buffer[128];
    u32 length = 0;
    for (; text[length] && length < 127; ++length) {
        buffer[length] = static_cast<wchar_t>(text[length]);
    }
    buffer[length] = 0;
    return buffer;
}

u8 g_encrypted[kEncryptedCapacity];
u8 g_handshake[kHandshakeCapacity];
u8 g_record[kRecordCapacity];

struct Session {
    NetworkLoop loop;
    TlsStream stream;
    // What the read handler saw: the first bytes of the response, the number of bytes in all, and whether the
    // pattern of the body held.
    u8 head[kHeadCapacity];
    u32 head_bytes;
    u64 total;
    u64 body_checked;
    u64 body_start;
    u32 mismatches;
    bool saw_close;
    bool check_pattern;
};

Session g_session;

// The server's payload is "0123456789abcdef\n" repeated.
constexpr char kPattern[] = "0123456789abcdef\n";

// Whether the response head ends with the last four bytes of the bytes kept so far.
bool head_complete(const Session& session) noexcept {
    const u32 count = session.head_bytes;
    return count >= 4 && session.head[count - 4] == '\r' && session.head[count - 3] == '\n' &&
           session.head[count - 2] == '\r' && session.head[count - 1] == '\n';
}

TlsStreamReadAction on_read(void* context, ConstSlice<u8> plaintext, bool peer_closed) noexcept {
    auto* session = static_cast<Session*>(context);
    for (u32 index = 0; index < plaintext.count; ++index) {
        const u8 value = plaintext.data[index];
        // The first bytes are kept for the checks; the pattern starts after the blank line of the head.
        if (session->head_bytes < kHeadCapacity) {
            session->head[session->head_bytes++] = value;
        }
        if (session->body_start == 0) {
            if (head_complete(*session)) {
                session->body_start = session->total + index + 1;
            }
        } else if (session->check_pattern) {
            const u64 offset = session->total + index - session->body_start;
            if (value != static_cast<u8>(kPattern[offset % 17])) {
                ++session->mismatches;
            }
            ++session->body_checked;
        }
    }
    session->total += plaintext.count;
    if (peer_closed) {
        session->saw_close = true;
        return TlsStreamReadAction::complete;
    }
    return TlsStreamReadAction::continue_read;
}

// Polls the loop and feeds each completion to the stream until it reports something other than "waiting".
TlsStreamEvent settle(Session* session, TlsStreamEvent event) noexcept {
    while (event.kind == TlsStreamEventKind::waiting) {
        const NetworkPollResult polled = network_loop_poll(&session->loop, kPollMilliseconds);
        if (polled.state != NetworkPollState::completion) {
            return TlsStreamEvent{0, TlsStreamEventKind::failed, TlsStreamError::receive};
        }
        event = tls_stream_handle_completion(&session->stream, &session->loop, polled.completion);
    }
    return event;
}

bool start(Session* session, const char* port, const char* name) noexcept {
    reset_value(session);
    const TlsStreamBuffers buffers{{g_encrypted, kEncryptedCapacity}, {g_handshake, kHandshakeCapacity},
                                   {g_record, kRecordCapacity}, false};
    return network_loop_initialize(&session->loop).ok() &&
           tls_stream_initialize_endpoint(&session->stream, wide_argument(name), static_cast<u16>(parse_number(port)),
                                          buffers);
}

// Closes the stream and polls until nothing of it is outstanding.
bool drain(Session* session) noexcept {
    tls_stream_close(&session->stream);
    for (u32 round = 0; round < 64 && !tls_stream_drained(&session->stream); ++round) {
        const NetworkPollResult polled = network_loop_poll(&session->loop, kPollMilliseconds);
        if (polled.state != NetworkPollState::completion) {
            return false;
        }
        tls_stream_handle_completion(&session->stream, &session->loop, polled.completion);
    }
    return tls_stream_drained(&session->stream);
}

void finish(Session* session) noexcept {
    check(drain(session), "the stream drains");
    check(network_loop_poll(&session->loop, 0).state == NetworkPollState::timeout, "nothing is left in the loop");
    network_loop_release(&session->loop);
}

// A request, and the response read to the end of the stream.
bool request(Session* session, const char* text, bool pattern) noexcept {
    TlsStreamEvent event = settle(session, tls_stream_begin_connect(&session->stream, &session->loop));
    if (event.kind != TlsStreamEventKind::ready) {
        return false;
    }
    const ConstSlice<u8> bytes{reinterpret_cast<const u8*>(text), c_string_length(text)};
    const ConstSlice<u8> spans[1]{bytes};
    event = settle(session, tls_stream_start_write(&session->stream, {spans, 1}));
    if (event.kind != TlsStreamEventKind::write_complete) {
        return false;
    }
    session->check_pattern = pattern;
    event = settle(session, tls_stream_start_read(&session->stream, on_read, session));
    return event.kind == TlsStreamEventKind::read_complete;
}

bool contains(const Session& session, const char* needle) noexcept {
    const u32 length = c_string_length(needle);
    for (u32 index = 0; index + length <= session.head_bytes; ++index) {
        if (bytes_equal(session.head + index, needle, length)) {
            return true;
        }
    }
    return false;
}

void run_get(const char* port, const char* name, const char* protocol) noexcept {
    check(start(&g_session, port, name), "the stream starts");
    check(request(&g_session, "GET / HTTP/1.0\r\n\r\n", false), "the request is answered");
    check(g_session.head_bytes >= 12 && bytes_equal(g_session.head, "HTTP/1.0 200", 12), "the answer is an HTTP 200");
    check(contains(g_session, protocol), "the page reports the negotiated protocol");
    check(g_session.saw_close, "the response ends with the server's close_notify");
    finish(&g_session);
}

void run_download(const char* port, const char* name, u64 bytes) noexcept {
    check(start(&g_session, port, name), "the stream starts");
    check(request(&g_session, "GET /payload HTTP/1.0\r\n\r\n", true), "the download completes");
    check(g_session.body_start != 0 && g_session.body_checked == bytes && g_session.mismatches == 0,
          "every body byte matches the pattern across the records");
    check(g_session.saw_close, "the download ends with the server's close_notify");
    finish(&g_session);
}

void run_reject(const char* port, const char* name) noexcept {
    check(start(&g_session, port, name), "the stream starts");
    const TlsStreamEvent event = settle(&g_session, tls_stream_begin_connect(&g_session.stream, &g_session.loop));
    check(event.kind == TlsStreamEventKind::failed && event.error == TlsStreamError::tls,
          "a name the certificate does not carry fails the handshake");
    finish(&g_session);
}

void run_refused(const char* port) noexcept {
    check(start(&g_session, port, "127.0.0.1"), "the stream starts");
    const TlsStreamEvent event = settle(&g_session, tls_stream_begin_connect(&g_session.stream, &g_session.loop));
    check(event.kind == TlsStreamEventKind::failed && event.error == TlsStreamError::connect,
          "a port nobody listens on fails the connect");
    finish(&g_session);
}

void run_cancel(const char* port, const char* name) noexcept {
    check(start(&g_session, port, name), "the stream starts");
    check(tls_stream_begin_connect(&g_session.stream, &g_session.loop).kind == TlsStreamEventKind::waiting,
          "begin_connect waits for the lookup");
    // Closing with the lookup pending (and then the connect) leaves one completion per operation.
    finish(&g_session);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_tls_stream_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    auto argument = [&](int index) noexcept { return index < startup.argc ? startup.argv[index] : ""; };
    if (text_equal(mode, "--get")) {
        run_get(argument(2), argument(3), argument(4));
    } else if (text_equal(mode, "--download")) {
        run_download(argument(2), argument(3), parse_number(argument(4)));
    } else if (text_equal(mode, "--reject")) {
        run_reject(argument(2), argument(3));
    } else if (text_equal(mode, "--refused")) {
        run_refused(argument(2));
    } else if (text_equal(mode, "--cancel")) {
        run_cancel(argument(2), argument(3));
    } else {
        check(false, "a known case");
    }
    test_exit(g_failures, "tls_stream");
}
