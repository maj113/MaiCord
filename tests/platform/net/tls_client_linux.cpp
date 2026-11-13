// tls_client.hpp over the system OpenSSL (src/platform/net/linux/tls_client.cpp). argv[1] selects a case:
//
//   --bootstrap                          the client without a network: argument checks, the ClientHello and its
//                                        extensions, partial and malformed input
//   --connect <port> <name> <protocol>   a TLS session to 127.0.0.1:<port> verified as <name>; sends a request and
//                                        reads the response until the close_notify; <protocol> is the version the
//                                        server's page must report ("TLSv1.3")
//   --download <port> <name> <bytes>     the same with a request that fills a whole record, answered with <bytes> bytes
//                                        of a repeating pattern that is checked byte by byte across many records
//   --reject <port> <name> <status>      the handshake must fail with this native status (hex); --reject-class
//                                        compares only the bits that are set in <status>
//   --rekey <port> <name>                a TLS 1.3 key update asked for by the server during the session
//   --control <port> <name>              a TLS 1.2 renegotiation request, refused with an alert that waits as control bytes
//
// tests/platform/net/tls_smoke.sh starts `openssl s_server` for the network cases and runs them; no case reaches a
// network beyond the loopback. The bytes cross the loopback in small, varying reads, so records arrive in pieces.
#include "maicord/platform/net/tls_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../../src/platform/crypto/openssl/openssl.hpp"
#include "../../support/test_output.hpp"
#include "../../support/tls_hello.hpp"

namespace maicord {
namespace {

constexpr u32 kTokenCapacity = 32768;
constexpr u32 kRecordCapacity = 20 * 1024;
constexpr u32 kQueueCapacity = 128 * 1024;
constexpr u32 kResponseCapacity = 256 * 1024;
constexpr long kCallSocket = 41;
constexpr long kCallConnect = 42;
constexpr long kCallSetsockopt = 54;
constexpr int kSocketStream = 1;
constexpr int kSocketAddressInet = 2;
constexpr int kLevelSocket = 1;
constexpr int kOptionReceiveTimeout = 20;

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

u64 parse_number(const char* text, u32 base) noexcept {
    u64 value = 0;
    for (; *text; ++text) {
        const u32 digit = *text <= '9' ? static_cast<u32>(*text - '0') : static_cast<u32>(*text - 'a' + 10);
        value = value * base + digit;
    }
    return value;
}

// An argument as a wide string (the names are ASCII).
const wchar_t* wide_argument(const char* text) noexcept {
    static wchar_t buffer[128];
    u32 length = 0;
    for (; text[length] && length < 127; ++length)
        buffer[length] = static_cast<wchar_t>(text[length]);
    buffer[length] = 0;
    return buffer;
}

u8 g_token[kTokenCapacity + 1];
u8 g_record[kRecordCapacity];
u8 g_queue[kQueueCapacity];
u8 g_response[kResponseCapacity];

struct Connection {
    int fd;
    TlsClient tls;
    u32 queued;
    u32 reads;
    bool ended; // the stream ended (the server closed the connection)
};

Connection g_connection;

// ---------------------------------------------------------------------------------------------------------------
// The loopback socket.

struct SocketAddressInet {
    u16 family;
    u16 port;
    u8 address[4];
    u8 zero[8];
};

int connect_loopback(u16 port) noexcept {
    const long created = syscall3(kCallSocket, kSocketAddressInet, kSocketStream, 0);
    if (syscall_failed(created))
        return -1;
    const int fd = static_cast<int>(created);
    // A server that stops answering fails the read instead of hanging the test.
    const i64 timeout[2]{5, 0};
    syscall5(kCallSetsockopt, fd, kLevelSocket, kOptionReceiveTimeout, reinterpret_cast<long>(timeout), sizeof(timeout));
    SocketAddressInet address{};
    address.family = kSocketAddressInet;
    address.port = static_cast<u16>((port >> 8) | (port << 8));
    address.address[0] = 127;
    address.address[3] = 1;
    if (syscall_failed(syscall3(kCallConnect, fd, reinterpret_cast<long>(&address), sizeof(address)))) {
        sys_close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const u8* data, u32 bytes) noexcept {
    u32 done = 0;
    while (done != bytes) {
        const long wrote = sys_write(fd, data + done, bytes - done);
        if (wrote == -kErrorInterrupted)
            continue;
        if (syscall_failed(wrote) || wrote == 0)
            return false;
        done += static_cast<u32>(wrote);
    }
    return true;
}

// Appends what the socket has (a few hundred bytes to a few KiB: records are split at arbitrary points) to the
// queue; the count, 0 at the end of the stream, or a negative error.
long receive_more(Connection& connection) noexcept {
    const u32 chunk = 97 + (connection.reads++ * 331) % 2900;
    const u32 room = kQueueCapacity - connection.queued;
    const long got = sys_read(connection.fd, g_queue + connection.queued, chunk < room ? chunk : room);
    if (got > 0)
        connection.queued += static_cast<u32>(got);
    else if (got == 0)
        connection.ended = true;
    return got;
}

void drop(Connection& connection, u32 bytes) noexcept {
    connection.queued -= bytes;
    copy_bytes(g_queue, g_queue + bytes, connection.queued);
}

// The handshake exchange of src/app/update/https.cpp: feed the queue, send what the step produced, receive when it
// asks for more.
bool handshake(Connection& connection, const wchar_t* name, TlsError* error, i32* status) noexcept {
    const TlsResult initialized = tls_client_initialize(&connection.tls, name);
    if (!initialized.ok()) {
        *error = initialized.error;
        *status = initialized.native_status;
        return false;
    }
    for (;;) {
        const TlsHandshakeStep step = tls_client_handshake(&connection.tls, {g_queue, connection.queued}, {g_token, kTokenCapacity});
        if (!step.ok() || step.state == TlsHandshakeState::failed) {
            *error = step.error;
            *status = step.native_status;
            return false;
        }
        if (step.produced && !send_all(connection.fd, g_token, step.produced))
            return false;
        if (step.consumed)
            drop(connection, step.consumed);
        if (step.state == TlsHandshakeState::complete || step.state == TlsHandshakeState::complete_and_send)
            return true;
        if (step.native_status == kTlsStatusIncomplete || !connection.queued) {
            if (receive_more(connection) <= 0)
                return false;
        }
    }
}

// Encrypts `request` as the given spans and sends the records.
bool send_request(Connection& connection, ConstSlice<ConstSlice<u8>> spans) noexcept {
    u32 produced = 0;
    const TlsResult encrypted = tls_client_encrypt_spans(&connection.tls, spans, {g_record, kRecordCapacity}, &produced);
    return encrypted.ok() && produced != 0 && send_all(connection.fd, g_record, produced);
}

// Says why a read failed (a failing case is otherwise only a label).
void explain(const char* what, TlsError error, i32 status) noexcept {
    test_write_text("  ");
    test_write_text(what);
    test_write_text(": error ");
    test_write_u64(static_cast<u32>(error));
    test_write_text(", status 0x");
    const char digits[] = "0123456789abcdef";
    char hex[9];
    for (u32 i = 0; i != 8; ++i)
        hex[i] = digits[(static_cast<u32>(status) >> (28 - i * 4)) & 15];
    hex[8] = 0;
    test_write_line(hex);
}

// Reads and decrypts until the server's close_notify; the plaintext goes to g_response. False on any failure,
// including a stream that ends without the authenticated close.
bool read_response(Connection& connection, u32* bytes, bool* closed_cleanly) noexcept {
    *bytes = 0;
    *closed_cleanly = false;
    for (;;) {
        while (connection.queued) {
            const TlsDecryptResult decrypted = tls_client_decrypt(&connection.tls, {g_queue, connection.queued});
            if (!decrypted.ok() || decrypted.state == TlsDecryptState::failed || decrypted.state == TlsDecryptState::renegotiate) {
                explain("decrypt", decrypted.error, decrypted.native_status);
                return false;
            }
            if (decrypted.state == TlsDecryptState::need_receive)
                break;
            if (!decrypted.consumed || decrypted.plaintext.count > kResponseCapacity - *bytes)
                return false;
            copy_bytes(g_response + *bytes, decrypted.plaintext.data, decrypted.plaintext.count);
            *bytes += decrypted.plaintext.count;
            drop(connection, decrypted.consumed);
            if (decrypted.state == TlsDecryptState::peer_closed) {
                *closed_cleanly = true;
                return true;
            }
        }
        const long received = receive_more(connection);
        if (received <= 0) {
            if (received < 0)
                explain("receive", TlsError::none, static_cast<i32>(received));
            return false;
        }
    }
}

bool contains(const u8* data, u32 count, const char* text) noexcept {
    const u32 length = c_string_length(text);
    for (u32 i = 0; length <= count && i <= count - length; ++i)
        if (bytes_equal(data + i, text, length))
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The cases.

void bootstrap_cases() noexcept {
    check(tls_client_initialize(nullptr, L"discord.com").error == TlsError::invalid_argument &&
              tls_client_initialize(&g_connection.tls, nullptr).error == TlsError::invalid_argument &&
              tls_client_initialize(&g_connection.tls, L"").error == TlsError::invalid_argument,
          "tls_client_initialize rejects a null client, a null name and an empty name");

    TlsClient client{};
    const TlsResult initialized = tls_client_initialize(&client, L"discord.com");
    check(initialized.ok() && client.credentials_valid && !client.context_valid && !client.handshake_complete &&
              client.maximum_token_bytes != 0 && client.maximum_token_bytes <= kTokenCapacity,
          "tls_client_initialize prepares a client without a connection");
    if (!initialized.ok())
        return;
    memset(g_token, 0xa5, sizeof(g_token));
    const u32 capacity = client.maximum_token_bytes;
    const TlsHandshakeStep too_small = tls_client_handshake(&client, {}, {g_token, capacity - 1});
    check(too_small.error == TlsError::output_too_small && !client.context_valid,
          "the handshake output bound is checked before any work");
    const TlsHandshakeStep hello = tls_client_handshake(&client, {}, {g_token, capacity});
    check(hello.ok() && hello.state == TlsHandshakeState::need_send && hello.produced != 0 && hello.produced <= capacity &&
              hello.consumed == 0 && g_token[capacity] == 0xa5 && client.context_valid && !client.handshake_complete &&
              g_token[0] == 22 && g_token[1] == 3,
          "the first step makes a ClientHello inside the output bound");
    if (hello.ok()) {
        const u8 partial[]{22, 3, 3, 0, 80};
        for (u32 length = 0; length <= sizeof(partial); ++length) {
            const auto wait = tls_client_handshake(&client, {partial, length}, {g_token, capacity});
            check(wait.ok() && wait.state == TlsHandshakeState::need_receive && wait.native_status == kTlsStatusIncomplete &&
                      wait.produced == 0 && wait.consumed == 0 && g_token[capacity] == 0xa5,
                  "fragmented TLS input is not consumed and publishes nothing");
        }
        const u8 malformed[]{0, 0, 0, 0, 0};
        const auto failed = tls_client_handshake(&client, {malformed, sizeof(malformed)}, {g_token, capacity});
        check(!failed.ok() && failed.state == TlsHandshakeState::failed && failed.produced == 0 && g_token[capacity] == 0xa5,
              "an invalid TLS record fails the handshake");
    }
    u32 encrypted_bytes = 99;
    const u8 plaintext[]{'x'};
    const TlsResult premature = tls_client_encrypt(&client, {plaintext, 1}, {g_record, kRecordCapacity}, &encrypted_bytes);
    check(premature.error == TlsError::invalid_argument && encrypted_bytes == 0, "encrypting needs a finished handshake");
    u8 record[8]{};
    check(tls_client_decrypt(&client, {record, 8}).error == TlsError::invalid_argument,
          "decrypting needs a finished handshake");
    tls_client_release(&client);
    check(!client.credentials_valid && !client.context_valid && !client.handshake_complete && !client.native.ssl &&
              !client.native.read_bio && !client.native.write_bio,
          "tls_client_release retires the connection");
    tls_client_release(&client);
    tls_client_release(nullptr);

    // Every ClientHello names the server and offers ALPN "http/1.1", and is shaped like Chrome's as far as OpenSSL
    // allows (configure_hello): Chrome's suites in its order (OpenSSL before 3.5 adds its renegotiation SCSV), its
    // groups with X25519MLKEM768 first where the library has it, its signature algorithms, status_request and
    // signed_certificate_timestamp, no encrypt_then_mac, the session_ticket extension.
    constexpr u16 kAlpn = 16;
    static const u8 http11[]{0, 9, 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    static const u8 chrome_suites[]{0x13, 0x01, 0x13, 0x02, 0x13, 0x03, 0xc0, 0x2b, 0xc0, 0x2f, 0xc0, 0x2c, 0xc0, 0x30,
                                    0xcc, 0xa9, 0xcc, 0xa8, 0xc0, 0x13, 0xc0, 0x14, 0x00, 0x9c, 0x00, 0x9d, 0x00, 0x2f,
                                    0x00, 0x35};
    static const u8 chrome_signatures[]{0, 16, 0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05, 0x05, 0x01,
                                        0x08, 0x06, 0x06, 0x01};
    static const u8 hybrid_groups[]{0, 8, 0x11, 0xec, 0x00, 0x1d, 0x00, 0x17, 0x00, 0x18};
    static const u8 classic_groups[]{0, 6, 0x00, 0x1d, 0x00, 0x17, 0x00, 0x18};
    TlsClient asking{};
    if (!tls_client_initialize(&asking, L"translate.googleapis.com").ok()) {
        check(false, "tls_alpn_client");
        return;
    }
    const TlsHandshakeStep first = tls_client_handshake(&asking, {}, {g_token, asking.maximum_token_bytes});
    const ConstSlice<u8> offered = first.ok() ? ConstSlice<u8>{g_token, first.produced} : ConstSlice<u8>{};
    const ConstSlice<u8> alpn = hello_extension(offered, kAlpn);
    const ConstSlice<u8> server_name = hello_extension(offered, 0);
    // server_name_list: length, type 0, name length, the name.
    const char host[] = "translate.googleapis.com";
    check(server_name.count == 5 + sizeof(host) - 1 && server_name.data[2] == 0 &&
              bytes_equal(server_name.data + 5, host, sizeof(host) - 1),
          "the ClientHello names the server (SNI)");
    check(alpn.count == sizeof(http11) && bytes_equal(alpn.data, http11, sizeof(http11)),
          "the ClientHello offers ALPN http/1.1");
    const ConstSlice<u8> suites = hello_cipher_suites(offered);
    const bool scsv = suites.count == sizeof(chrome_suites) + 2 && suites.data[sizeof(chrome_suites)] == 0x00 &&
                      suites.data[sizeof(chrome_suites) + 1] == 0xff;
    check((suites.count == sizeof(chrome_suites) || scsv) &&
              bytes_equal(suites.data, chrome_suites, sizeof(chrome_suites)),
          "the ClientHello offers Chrome's cipher suites in Chrome's order");
    const ConstSlice<u8> groups = hello_extension(offered, 10);
    const ConstSlice<u8> shares = hello_extension(offered, 51);
    const bool hybrid =
        groups.count == sizeof(hybrid_groups) && bytes_equal(groups.data, hybrid_groups, sizeof(hybrid_groups));
    // key_share: the list length, then the first share's group; with the hybrid group, X25519's share follows its own.
    check((hybrid || (groups.count == sizeof(classic_groups) &&
                      bytes_equal(groups.data, classic_groups, sizeof(classic_groups)))) &&
              shares.count > 4 && shares.data[2] == groups.data[2] && shares.data[3] == groups.data[3] &&
              (!hybrid || shares.count == 2 + 4 + 1216 + 4 + 32),
          "the ClientHello offers Chrome's groups and shares a key for the first (and X25519 beside the hybrid)");
    const ConstSlice<u8> signatures = hello_extension(offered, 13);
    check(signatures.count == sizeof(chrome_signatures) &&
              bytes_equal(signatures.data, chrome_signatures, sizeof(chrome_signatures)),
          "the ClientHello offers Chrome's signature algorithms");
    check(hello_extension(offered, 5).data && hello_extension(offered, 18).data && hello_extension(offered, 35).data &&
              !hello_extension(offered, 22).data,
          "the ClientHello asks for OCSP and SCTs, offers session tickets and no encrypt_then_mac");
    tls_client_release(&asking);
    // An address literal is not sent as a server name.
    TlsClient literal{};
    if (tls_client_initialize(&literal, L"192.0.2.1").ok()) {
        const auto first = tls_client_handshake(&literal, {}, {g_token, literal.maximum_token_bytes});
        check(first.ok() && hello_extension({g_token, first.produced}, 0).count == 0,
              "the ClientHello of an address literal carries no server name");
    }
    tls_client_release(&literal);
    // A name that is not ASCII, or too long, is refused at the first step.
    TlsClient bad{};
    check(tls_client_initialize(&bad, L"caf\x00e9.example").ok() &&
              tls_client_handshake(&bad, {}, {g_token, bad.maximum_token_bytes}).error == TlsError::invalid_argument,
          "a non-ASCII name is refused");
    tls_client_release(&bad);
}

bool open_session(Connection& connection, u16 port, const wchar_t* name, TlsError* error, i32* status) noexcept {
    reset_value<Connection>(&connection);
    connection.fd = connect_loopback(port);
    check(connection.fd >= 0, "connect to the loopback server");
    return connection.fd >= 0 && handshake(connection, name, error, status);
}

void close_session(Connection& connection) noexcept {
    tls_client_release(&connection.tls);
    if (connection.fd >= 0)
        sys_close(connection.fd);
    connection.fd = -1;
}

void connect_case(u16 port, const wchar_t* name, const char* protocol) noexcept {
    TlsError error = TlsError::none;
    i32 status = 0;
    Connection& connection = g_connection;
    const bool opened = open_session(connection, port, name, &error, &status);
    check(opened, "the handshake with the loopback server succeeds");
    if (!opened) {
        explain("handshake", error, status);
        close_session(connection);
        return;
    }
    check(connection.tls.handshake_complete && connection.tls.stream_sizes.cbMaximumMessage == 16384 &&
              connection.tls.stream_sizes.cbHeader + 16384 + connection.tls.stream_sizes.cbTrailer <= kRecordCapacity,
          "the finished handshake publishes the record sizes");
    // ALPN: the server answered http/1.1. The client never asks (it speaks HTTP/1.1 alone), so the query is not in its
    // table: it comes from the libssl the client loaded, the soname whose SSL_new is the table's.
    using AlpnSelected = void (*)(const void*, const u8**, u32*);
    const openssl::Ssl* library = openssl::ssl();
    AlpnSelected alpn_selected = nullptr;
    const char* const sonames[] = {"libssl.so.4", "libssl.so.3"};
    for (const char* soname : sonames) {
        SharedLibrary ssl{};
        if (library && !alpn_selected && shared_library_open(&ssl, soname) &&
            dlsym(ssl.handle, "SSL_new") == reinterpret_cast<void*>(library->SSL_new))
            alpn_selected = reinterpret_cast<AlpnSelected>(dlsym(ssl.handle, "SSL_get0_alpn_selected"));
        shared_library_close(&ssl);
    }
    const u8* selected = nullptr;
    u32 selected_bytes = 0;
    if (alpn_selected)
        alpn_selected(connection.tls.native.ssl, &selected, &selected_bytes);
    check(selected_bytes == 8 && selected && bytes_equal(selected, "http/1.1", 8), "ALPN http/1.1 is negotiated");

    // Argument checks of a live client.
    u32 produced = 99;
    const u8 byte[]{'x'};
    check(tls_client_encrypt(&connection.tls, {byte, 1}, {g_record, 1 + 13 + 529 - 1}, &produced).error ==
                  TlsError::output_too_small &&
              produced == 0,
          "encrypting into a short output is refused");
    static u8 oversized[16385];
    check(tls_client_encrypt(&connection.tls, {oversized, 16385}, {g_record, kRecordCapacity}, &produced).error ==
                  TlsError::invalid_argument &&
              tls_client_encrypt(&connection.tls, {byte, 0}, {g_record, kRecordCapacity}, &produced).error ==
                  TlsError::invalid_argument,
          "a plaintext of none or of more than a record is refused");

    // The request goes out in two spans and one record.
    const char first[] = "GET / HTTP/1.0\r\nHost: localhost\r\n";
    const char second[] = "Connection: close\r\n\r\n";
    const ConstSlice<u8> spans[]{{reinterpret_cast<const u8*>(first), sizeof(first) - 1},
                                 {reinterpret_cast<const u8*>(second), sizeof(second) - 1}};
    check(send_request(connection, {spans, 2}), "the request is encrypted and sent");
    u32 bytes = 0;
    bool closed = false;
    check(read_response(connection, &bytes, &closed), "the response is decrypted up to the close_notify");
    check(closed, "the server's close_notify is authenticated as a close");
    check(bytes > 100 && contains(g_response, bytes, "HTTP/1.0 200 ok"), "the response is a 200");
    check(contains(g_response, bytes, protocol), "the session runs the expected protocol version");

    // Malformed and partial input to a finished session.
    u8 garbage[16]{0, 0, 0, 0, 0};
    check(tls_client_decrypt(&connection.tls, {garbage, 16}).error == TlsError::decrypt &&
              tls_client_decrypt(&connection.tls, {garbage, 3}).state == TlsDecryptState::need_receive,
          "malformed input fails and a partial header waits");
    close_session(connection);
}

void download_case(u16 port, const wchar_t* name, u32 expected_bytes) noexcept {
    TlsError error = TlsError::none;
    i32 status = 0;
    Connection& connection = g_connection;
    const bool opened = open_session(connection, port, name, &error, &status);
    check(opened, "the handshake for the download succeeds");
    if (!opened) {
        explain("handshake", error, status);
        close_session(connection);
        return;
    }
    // A request of exactly one full record (16384 bytes): the request line, a padding header, the end of the headers.
    static u8 request[16384];
    const char line[] = "GET /payload HTTP/1.0\r\nX-Pad: ";
    const char tail[] = "\r\n\r\n";
    u32 at = 0;
    copy_bytes(request, line, sizeof(line) - 1);
    at += sizeof(line) - 1;
    const u32 padding = sizeof(request) - at - (sizeof(tail) - 1);
    memset(request + at, 'p', padding);
    at += padding;
    copy_bytes(request + at, tail, sizeof(tail) - 1);
    check(at + sizeof(tail) - 1 == sizeof(request), "the request fills one record");
    const ConstSlice<u8> one[]{{request, sizeof(request)}};
    check(send_request(connection, {one, 1}), "a full-record request is sent");
    u32 bytes = 0;
    bool closed = false;
    const bool read = read_response(connection, &bytes, &closed);
    check(read && closed, "the download ends with the server's close_notify");
    // The response is the status line and headers, a blank line, then the pattern "0123456789abcdef\n" repeated.
    u32 body = 0;
    for (u32 i = 0; i + 4 <= bytes; ++i)
        if (bytes_equal(g_response + i, "\r\n\r\n", 4)) {
            body = i + 4;
            break;
        }
    check(body != 0 && bytes - body == expected_bytes && contains(g_response, body, "HTTP/1.0 200 ok"),
          "the download is a 200 with the whole file");
    bool pattern = body != 0;
    const char cycle[] = "0123456789abcdef\n";
    for (u32 i = 0; pattern && body + i < bytes; ++i)
        pattern = g_response[body + i] == static_cast<u8>(cycle[i % 17]);
    check(pattern, "every downloaded byte is in place across the records");
    close_session(connection);
}

// Reads and decrypts until the plaintext holds `text`; false on a failure or a close first.
bool read_until(Connection& connection, const char* text, u32* bytes) noexcept {
    *bytes = 0;
    for (;;) {
        while (connection.queued) {
            const TlsDecryptResult decrypted = tls_client_decrypt(&connection.tls, {g_queue, connection.queued});
            if (!decrypted.ok() || decrypted.state == TlsDecryptState::failed ||
                decrypted.state == TlsDecryptState::peer_closed || decrypted.state == TlsDecryptState::renegotiate) {
                explain("decrypt", decrypted.error, decrypted.native_status);
                return false;
            }
            if (decrypted.state == TlsDecryptState::need_receive)
                break;
            if (!decrypted.consumed || decrypted.plaintext.count > kResponseCapacity - *bytes)
                return false;
            copy_bytes(g_response + *bytes, decrypted.plaintext.data, decrypted.plaintext.count);
            *bytes += decrypted.plaintext.count;
            drop(connection, decrypted.consumed);
            if (contains(g_response, *bytes, text))
                return true;
        }
        const long received = receive_more(connection);
        if (received <= 0) {
            explain(received == 0 ? "end of stream" : "receive", TlsError::none, static_cast<i32>(connection.queued));
            return false;
        }
    }
}

// A TLS 1.3 key update asked for by the server while the session is open. The script (tls_smoke.sh) drives
// `openssl s_server` from its standard input: after the client's first line it sends the key update request and a
// line, then waits for the client's second line, then ends the session.
//   OpenSSL answers a requested update with its own KeyUpdate message at the next write, in front of the data, so
//   the client's second line leaves as that record followed by the 24-byte record of its 2 bytes, and the server
//   can read the line only if the keys changed on both sides as they must.
void rekey_case(u16 port, const wchar_t* name) noexcept {
    TlsError error = TlsError::none;
    i32 status = 0;
    Connection& connection = g_connection;
    const bool opened = open_session(connection, port, name, &error, &status);
    check(opened, "the handshake for the key update case succeeds");
    if (!opened) {
        explain("handshake", error, status);
        close_session(connection);
        return;
    }
    const u8 first[]{'A', '\n'};
    const ConstSlice<u8> first_spans[]{{first, 2}};
    check(send_request(connection, {first_spans, 1}), "the first line is sent");
    u32 bytes = 0;
    check(read_until(connection, "hello\n", &bytes), "the line after the server's key update is received");
    check(connection.tls.native.control_bytes == 0, "nothing was produced while reading the key update");
    const u8 second[]{'B', '\n'};
    u32 produced = 0;
    const ConstSlice<u8> spans[]{{second, 2}};
    const TlsResult sealed = tls_client_encrypt_spans(&connection.tls, {spans, 1}, {g_record, kRecordCapacity}, &produced);
    // The key update answer, then the record of the 2 bytes (5 header + 2 + 1 type + 16 tag = 24).
    check(sealed.ok() && produced > 24, "the record of the second line carries the key update answer");
    check(produced != 0 && send_all(connection.fd, g_record, produced), "the second line and the key update answer are sent");
    bool closed = false;
    // `s_server` ends its session on "Q" by closing the connection without a close_notify: the client must have
    // decrypted everything up to the end of the stream and seen no failure.
    const bool read = read_response(connection, &bytes, &closed);
    check(!read && !closed && connection.ended && connection.queued == 0, "the session ends with the server closing the connection");
    close_session(connection);
}

// A TLS 1.2 server asking for a renegotiation (the script sends `s_server` the command "R"). The client refuses it:
// OpenSSL writes a warning alert (no_renegotiation) while the HelloRequest is being decrypted. That record is output
// that belongs to no encrypt call, so it must wait in the control queue and leave in front of the next record.
// Nothing is sent back: the server abandons the session when it receives the refusal.
void control_case(u16 port, const wchar_t* name) noexcept {
    TlsError error = TlsError::none;
    i32 status = 0;
    Connection& connection = g_connection;
    const bool opened = open_session(connection, port, name, &error, &status);
    check(opened, "the handshake for the control record case succeeds");
    if (!opened) {
        explain("handshake", error, status);
        close_session(connection);
        return;
    }
    const u8 first[]{'A', '\n'};
    const ConstSlice<u8> first_spans[]{{first, 2}};
    check(send_request(connection, {first_spans, 1}), "the first line is sent");
    // The HelloRequest is a record without application data: decrypting it yields an empty plaintext.
    bool controlled = false;
    while (!controlled) {
        if (connection.queued) {
            const TlsDecryptResult decrypted = tls_client_decrypt(&connection.tls, {g_queue, connection.queued});
            if (!decrypted.ok() || decrypted.state != TlsDecryptState::plaintext || !decrypted.consumed) {
                explain("decrypt", decrypted.error, decrypted.native_status);
                break;
            }
            drop(connection, decrypted.consumed);
            controlled = connection.tls.native.control_bytes != 0;
        } else if (receive_more(connection) <= 0) {
            break;
        }
    }
    check(controlled, "the refusal of the renegotiation is queued as control bytes");
    const u32 queued = connection.tls.native.control_bytes;
    const u8 second[]{'B', '\n'};
    const ConstSlice<u8> spans[]{{second, 2}};
    u32 produced = 0;
    const TlsResult sealed = tls_client_encrypt_spans(&connection.tls, {spans, 1}, {g_record, kRecordCapacity}, &produced);
    // The queued alert record first (content type 21), then one application data record (type 23) to the end.
    const u32 record = 5 + ((static_cast<u32>(g_record[queued + 3]) << 8) | g_record[queued + 4]);
    check(sealed.ok() && queued >= 5 && produced > queued && g_record[0] == 21 && g_record[queued] == 23 &&
              produced == queued + record && connection.tls.native.control_bytes == 0,
          "the next record leaves behind the queued control bytes");
    close_session(connection);
}

// The handshake must fail; the bits of its native status that `mask` selects must be those of `expected_status`.
void reject_case(u16 port, const wchar_t* name, u32 expected_status, u32 mask) noexcept {
    TlsError error = TlsError::none;
    i32 status = 0;
    Connection& connection = g_connection;
    const bool opened = open_session(connection, port, name, &error, &status);
    const bool expected = !opened && error == TlsError::handshake && (static_cast<u32>(status) & mask) == expected_status;
    check(expected, "the handshake fails with the expected status");
    if (!expected) {
        test_write_text("  error ");
        test_write_u64(static_cast<u32>(error));
        test_write_text(" status ");
        test_write_u64(static_cast<u32>(status));
        test_write_text("\n");
    }
    check(!connection.tls.handshake_complete, "a failed handshake is not complete");
    close_session(connection);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_tls_client_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "--bootstrap";
    if (text_equal(mode, "--bootstrap")) {
        bootstrap_cases();
        test_exit(g_failures, "maicord_tls_client_bootstrap");
    }
    if (startup.argc < 4) {
        test_write_line("usage: tls_client_linux --connect|--download|--reject|--rekey <port> <name> [<argument>]");
        test_process_exit(2);
    }
    const u16 port = static_cast<u16>(parse_number(startup.argv[2], 10));
    const wchar_t* name = wide_argument(startup.argv[3]);
    const char* argument = startup.argc > 4 ? startup.argv[4] : "";
    if (text_equal(mode, "--connect")) {
        connect_case(port, name, argument);
        test_exit(g_failures, "maicord_tls_client_connect");
    }
    if (text_equal(mode, "--download")) {
        download_case(port, name, static_cast<u32>(parse_number(argument, 10)));
        test_exit(g_failures, "maicord_tls_client_download");
    }
    if (text_equal(mode, "--control")) {
        control_case(port, name);
        test_exit(g_failures, "maicord_tls_client_control");
    }
    if (text_equal(mode, "--rekey")) {
        rekey_case(port, name);
        test_exit(g_failures, "maicord_tls_client_rekey");
    }
    if (text_equal(mode, "--reject") || text_equal(mode, "--reject-class")) {
        // --reject names the whole status, --reject-class only the bits that are set in it.
        const u32 expected = static_cast<u32>(parse_number(argument, 16));
        reject_case(port, name, expected, text_equal(mode, "--reject") ? 0xffffffffu : expected);
        test_exit(g_failures, "maicord_tls_client_reject");
    }
    test_write_line("unknown case");
    test_process_exit(2);
}
