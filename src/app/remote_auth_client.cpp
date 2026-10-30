#include "maicord/core/win32_process.hpp"
#include "maicord/app/remote_auth_client.hpp"

#include "maicord/app/remote_auth_ticket_exchange.hpp"
#include "maicord/core/byte_queue.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/spsc_queue.hpp"
#include "maicord/platform/network.hpp"
#include "maicord/platform/random.hpp"
#include "maicord/platform/tls_client.hpp"
#include "maicord/protocol/websocket_frame.hpp"
#include "maicord/protocol/websocket_handshake.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {

inline constexpr wchar_t kRemoteAuthHostWide[] = L"remote-auth-gateway.discord.gg";
inline constexpr char kRemoteAuthHost[] = "remote-auth-gateway.discord.gg";
inline constexpr u32 kEncryptedInputBytes = 128 * 1024;
inline constexpr u32 kHandshakeOutputBytes = 64 * 1024;
inline constexpr u32 kRecordOutputBytes = 20 * 1024;
inline constexpr u32 kWebSocketInputBytes = 16 * 1024;
inline constexpr u32 kFrameOutputBytes = 1024;
inline constexpr u32 kStructuralCount = 256;
inline constexpr u32 kEventCapacity = 8;
inline constexpr u32 kPhaseTimeoutMs = 15'000;
inline constexpr u32 kCancelSignalValue = 0x5241434Eu;

enum class WaitState : u8 {
    completion,
    timeout,
    stopped,
    error,
};

enum class PlaintextState : u8 {
    plaintext,
    timeout,
    peer_closed,
    stopped,
    error,
};

struct WaitResult {
    NetworkCompletion completion;
    u32 native_error;
    WaitState state;
};

struct PlaintextResult {
    ConstSlice<u8> plaintext;
    u32 native_error;
    PlaintextState state;
};

struct RemoteAuthClientRuntime {
    RemoteAuthHandshake handshake;
    RemoteAuthTicketExchange ticket_exchange;
    NetworkLoop loop;
    NetworkResolver resolver;
    NetworkConnection connection;
    NetworkSignal cancel_signal;
    TlsClient tls;
    ByteQueue encrypted_input;
    ByteQueue websocket_input;
    SpscQueue<RemoteAuthClientEvent, kEventCapacity> events;
    WebSocketMessageTracker tracker;
    u64 phase_deadline_ms;
    u32 message_bytes;
    volatile LONG loop_ready;
    bool tls_initialized;
    u8 encrypted_storage[kEncryptedInputBytes];
    u8 handshake_output[kHandshakeOutputBytes];
    u8 record_output[kRecordOutputBytes];
    u8 websocket_storage[kWebSocketInputBytes];
    u8 frame_output[kFrameOutputBytes];
    u8 message_storage[kRemoteAuthMaximumPacketBytes];
    u32 structural_scratch[kStructuralCount];
};

template <usize Count> ConstSlice<u8> bytes(const char (&text)[Count]) noexcept {
    static_assert(Count > 0 && Count - 1 <= 0xFFFFFFFFull);
    return ConstSlice<u8>{reinterpret_cast<const u8*>(text), static_cast<u32>(Count - 1)};
}

template <typename T> T load_field(const volatile LONG& field) noexcept {
    return static_cast<T>(read_acquire(&field));
}

u64 load_field64(const volatile LONG64& field) noexcept {
    return static_cast<u64>(read_acquire(&field));
}

RemoteAuthClientPhase load_phase(const RemoteAuthClient* client) noexcept {
    return load_field<RemoteAuthClientPhase>(client->phase);
}

void publish_phase(RemoteAuthClient* client, RemoteAuthClientPhase phase) noexcept {
    InterlockedExchange(&client->phase, static_cast<LONG>(phase));
}

RemoteAuthClientResult client_result(const RemoteAuthClient* client) noexcept {
    RemoteAuthClientResult value{};
    if (!client) {
        value.error = RemoteAuthClientError::invalid_input;
        return value;
    }
    value.next_wake_at_ms = load_field64(client->next_wake_at_ms);
    value.ticket_error_code = load_field64(client->ticket_error_code);
    value.retry_at_ms = load_field64(client->retry_at_ms);
    value.native_error = load_field<u32>(client->native_error);
    value.ticket_response_bytes = load_field<u32>(client->ticket_response_bytes);
    value.ticket_decode_error_offset = load_field<u32>(client->ticket_decode_error_offset);
    value.http_status = load_field<u16>(client->http_status);
    value.handshake_error = load_field<RemoteAuthHandshakeError>(client->handshake_error);
    value.handshake_stage = load_field<RemoteAuthHandshakeStage>(client->handshake_stage);
    value.protocol_error = load_field<RemoteAuthError>(client->protocol_error);
    value.crypto_error = load_field<RemoteAuthCryptoError>(client->crypto_error);
    value.base64_error = load_field<Base64Error>(client->base64_error);
    value.ticket_response_kind = load_field<UserAuthResponseKind>(client->ticket_response_kind);
    value.ticket_decode_error = load_field<UserAuthDecodeError>(client->ticket_decode_error);
    value.ticket_scan_error = load_field<JsonScanError>(client->ticket_scan_error);
    value.ticket_read_error = load_field<JsonReadError>(client->ticket_read_error);
    value.ticket_content_type = load_field<RestContentType>(client->ticket_content_type);
    value.ticket_content_encoding = load_field<RestContentEncoding>(client->ticket_content_encoding);
    value.ticket_body_kind = load_field<RestBodyKind>(client->ticket_body_kind);
    value.captcha_provider = load_field<CaptchaProvider>(client->captcha_provider);
    value.captcha_invisible = load_field<LONG>(client->captcha_invisible) != 0;
    value.error = load_field<RemoteAuthClientError>(client->error);
    value.phase = load_phase(client);
    return value;
}

void publish_event(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime,
                   RemoteAuthClientEventKind kind) noexcept {
    const RemoteAuthClientEvent event{client_result(client), kind};
    if (!spsc_queue_push(&runtime->events, event)) {
        InterlockedCompareExchange(&client->error, static_cast<LONG>(RemoteAuthClientError::event),
                                   static_cast<LONG>(RemoteAuthClientError::none));
        publish_phase(client, RemoteAuthClientPhase::failed);
    }
    SetEvent(client->notification_event);
}

void publish_failure(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime,
                     RemoteAuthClientError error, u32 native_error = 0,
                     const RemoteAuthHandshakeResult* handshake = nullptr) noexcept {
    if (handshake) {
        InterlockedExchange(&client->handshake_error, static_cast<LONG>(handshake->error));
        InterlockedExchange(&client->protocol_error, static_cast<LONG>(handshake->protocol_error));
        InterlockedExchange(&client->crypto_error, static_cast<LONG>(handshake->crypto_error));
        InterlockedExchange(&client->base64_error, static_cast<LONG>(handshake->base64_error));
        InterlockedExchange(&client->handshake_stage, static_cast<LONG>(handshake->stage));
    }
    if (InterlockedCompareExchange(&client->error, static_cast<LONG>(error),
                                   static_cast<LONG>(RemoteAuthClientError::none)) ==
        static_cast<LONG>(RemoteAuthClientError::none)) {
        InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    }
    publish_phase(client, RemoteAuthClientPhase::failed);
    publish_event(client, runtime, RemoteAuthClientEventKind::failed);
}

bool stop_requested(const RemoteAuthClient* client) noexcept {
    return read_acquire(&client->stop_requested) != 0;
}

bool accept_receive(RemoteAuthClientRuntime* runtime, const NetworkCompletion& completion,
                    u32* native_error) noexcept {
    if (completion.kind != NetworkOperationKind::receive || completion.connection != &runtime->connection ||
        completion.stale || !completion.succeeded || completion.transferred == 0 ||
        !byte_queue_produce(&runtime->encrypted_input, completion.transferred)) {
        *native_error = completion.native_error != 0 ? static_cast<u32>(completion.native_error)
                                                     : ERROR_CONNECTION_ABORTED;
        return false;
    }
    return true;
}

WaitResult wait_for(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, NetworkOperationKind kind,
                    u32 timeout_ms, bool retain_receive) noexcept {
    const u64 deadline = deadline_after(GetTickCount64(), timeout_ms);
    for (;;) {
        if (stop_requested(client)) {
            return WaitResult{{}, 0, WaitState::stopped};
        }
        const u64 now_ms = GetTickCount64();
        if (now_ms >= deadline) {
            return WaitResult{{}, WAIT_TIMEOUT, WaitState::timeout};
        }
        const NetworkPollResult polled =
            network_loop_poll(&runtime->loop, wait_milliseconds_until(now_ms, deadline, INFINITE));
        if (polled.state == NetworkPollState::timeout) {
            return WaitResult{{}, WAIT_TIMEOUT, WaitState::timeout};
        }
        if (polled.state == NetworkPollState::error) {
            return WaitResult{{}, static_cast<u32>(polled.completion.native_error), WaitState::error};
        }
        if (polled.completion.kind == NetworkOperationKind::signal &&
            polled.completion.signal == &runtime->cancel_signal &&
            polled.completion.signal_value == kCancelSignalValue) {
            return WaitResult{{}, 0, WaitState::stopped};
        }
        if (polled.completion.kind == kind) {
            return WaitResult{polled.completion, 0, WaitState::completion};
        }
        if (retain_receive && polled.completion.kind == NetworkOperationKind::receive) {
            u32 native_error = 0;
            if (!accept_receive(runtime, polled.completion, &native_error)) {
                return WaitResult{{}, native_error, WaitState::error};
            }
            continue;
        }
        return WaitResult{{}, ERROR_INVALID_STATE, WaitState::error};
    }
}

bool send_all(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, ConstSlice<u8> value,
              u32* native_error) noexcept {
    u32 offset = 0;
    while (offset < value.count) {
        const NetworkResult started = network_connection_send(
            &runtime->connection, ConstSlice<u8>{value.data + offset, value.count - offset});
        if (!started.ok()) {
            *native_error = static_cast<u32>(started.native_error);
            return false;
        }
        const WaitResult waited =
            wait_for(client, runtime, NetworkOperationKind::send, kPhaseTimeoutMs, true);
        if (waited.state != WaitState::completion || waited.completion.connection != &runtime->connection ||
            waited.completion.stale || !waited.completion.succeeded || waited.completion.transferred == 0) {
            *native_error = waited.native_error != 0 ? waited.native_error
                                                     : static_cast<u32>(waited.completion.native_error);
            return false;
        }
        offset += waited.completion.transferred;
    }
    return true;
}

bool send_tls(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, ConstSlice<u8> plaintext,
              u32* native_error) noexcept {
    u32 offset = 0;
    while (offset < plaintext.count) {
        const u32 maximum = runtime->tls.stream_sizes.cbMaximumMessage;
        const u32 count = minimum(maximum, plaintext.count - offset);
        u32 encrypted_bytes = 0;
        const TlsResult encrypted =
            tls_client_encrypt(&runtime->tls, ConstSlice<u8>{plaintext.data + offset, count},
                               Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes);
        if (!encrypted.ok() ||
            !send_all(client, runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes},
                      native_error)) {
            SecureZeroMemory(runtime->record_output, sizeof(runtime->record_output));
            return false;
        }
        SecureZeroMemory(runtime->record_output, sizeof(runtime->record_output));
        offset += count;
    }
    return true;
}

bool start_receive(RemoteAuthClientRuntime* runtime, u32* native_error) noexcept {
    if (runtime->connection.receive_pending) {
        return true;
    }
    const Slice<u8> writable = byte_queue_writable(&runtime->encrypted_input);
    if (!writable.data || writable.count == 0) {
        *native_error = ERROR_INSUFFICIENT_BUFFER;
        return false;
    }
    const NetworkResult started = network_connection_receive(&runtime->connection, writable);
    if (!started.ok()) {
        *native_error = static_cast<u32>(started.native_error);
        return false;
    }
    return true;
}

PlaintextResult receive_plaintext(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime,
                                  u32 timeout_ms) noexcept {
    const u64 deadline = deadline_after(GetTickCount64(), timeout_ms);
    for (;;) {
        if (byte_queue_size(runtime->encrypted_input) != 0) {
            const ConstSlice<u8> readable = byte_queue_readable(runtime->encrypted_input);
            const TlsDecryptResult decrypted = tls_client_decrypt(
                &runtime->tls, Slice<u8>{const_cast<u8*>(readable.data), readable.count});
            if (!decrypted.ok()) {
                return PlaintextResult{
                    {}, static_cast<u32>(decrypted.native_status), PlaintextState::error};
            }
            if (decrypted.state == TlsDecryptState::peer_closed) {
                return PlaintextResult{{}, 0, PlaintextState::peer_closed};
            }
            if (decrypted.state == TlsDecryptState::plaintext) {
                const ConstSlice<u8> plaintext{decrypted.plaintext.data, decrypted.plaintext.count};
                if (!byte_queue_consume(&runtime->encrypted_input, decrypted.consumed)) {
                    return PlaintextResult{{}, ERROR_INVALID_DATA, PlaintextState::error};
                }
                return PlaintextResult{plaintext, 0, PlaintextState::plaintext};
            }
        }
        u32 native_error = 0;
        if (!start_receive(runtime, &native_error)) {
            return PlaintextResult{{}, native_error, PlaintextState::error};
        }
        const u64 now_ms = GetTickCount64();
        if (now_ms >= deadline) {
            return PlaintextResult{{}, WAIT_TIMEOUT, PlaintextState::timeout};
        }
        const WaitResult waited = wait_for(client, runtime, NetworkOperationKind::receive,
                                           wait_milliseconds_until(now_ms, deadline, INFINITE), false);
        if (waited.state == WaitState::timeout) {
            return PlaintextResult{{}, WAIT_TIMEOUT, PlaintextState::timeout};
        }
        if (waited.state == WaitState::stopped) {
            return PlaintextResult{{}, 0, PlaintextState::stopped};
        }
        if (waited.state != WaitState::completion ||
            !accept_receive(runtime, waited.completion, &native_error)) {
            return PlaintextResult{
                {}, waited.native_error != 0 ? waited.native_error : native_error, PlaintextState::error};
        }
    }
}

bool connect_socket(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, u32* native_error,
                    RemoteAuthClientError* error) noexcept {
    publish_phase(client, RemoteAuthClientPhase::resolving);
    const NetworkResult resolving =
        network_resolver_start(&runtime->loop, &runtime->resolver, kRemoteAuthHostWide, L"443");
    if (!resolving.ok()) {
        *native_error = static_cast<u32>(resolving.native_error);
        *error = RemoteAuthClientError::resolve;
        return false;
    }
    const WaitResult resolved =
        wait_for(client, runtime, NetworkOperationKind::resolve, kPhaseTimeoutMs, false);
    if (resolved.state != WaitState::completion || resolved.completion.resolver != &runtime->resolver ||
        resolved.completion.stale || !resolved.completion.succeeded || !runtime->resolver.results) {
        *native_error = resolved.native_error != 0 ? resolved.native_error
                                                   : static_cast<u32>(resolved.completion.native_error);
        *error = resolved.state == WaitState::stopped ? RemoteAuthClientError::cancelled
                                                      : RemoteAuthClientError::resolve;
        network_resolver_release(&runtime->resolver);
        return false;
    }

    publish_phase(client, RemoteAuthClientPhase::connecting);
    bool connected = false;
    for (ADDRINFOEXW* address = runtime->resolver.results; address && !connected && !stop_requested(client);
         address = address->ai_next) {
        if (address->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;
        }
        const NetworkResult started = network_connection_connect(
            &runtime->loop, &runtime->connection, address->ai_addr, static_cast<i32>(address->ai_addrlen));
        if (!started.ok()) {
            *native_error = static_cast<u32>(started.native_error);
            network_connection_close(&runtime->connection);
            continue;
        }
        const WaitResult completion =
            wait_for(client, runtime, NetworkOperationKind::connect, kPhaseTimeoutMs, false);
        connected = completion.state == WaitState::completion &&
                    completion.completion.connection == &runtime->connection &&
                    completion.completion.succeeded && !completion.completion.stale &&
                    runtime->connection.state == NetworkConnectionState::connected;
        if (!connected) {
            *native_error = completion.native_error != 0
                                ? completion.native_error
                                : static_cast<u32>(completion.completion.native_error);
            network_connection_close(&runtime->connection);
        }
    }
    network_resolver_release(&runtime->resolver);
    if (!connected) {
        *error = stop_requested(client) ? RemoteAuthClientError::cancelled : RemoteAuthClientError::connect;
        return false;
    }
    return true;
}

bool complete_tls_handshake(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime,
                            u32* native_error) noexcept {
    publish_phase(client, RemoteAuthClientPhase::tls_handshake);
    const TlsResult initialized = tls_client_initialize(&runtime->tls, kRemoteAuthHostWide);
    if (!initialized.ok() || runtime->tls.maximum_token_bytes > kHandshakeOutputBytes) {
        *native_error = static_cast<u32>(initialized.native_status);
        return false;
    }
    runtime->tls_initialized = true;
    TlsHandshakeStep step =
        tls_client_handshake(&runtime->tls, ConstSlice<u8>{nullptr, 0},
                             Slice<u8>{runtime->handshake_output, kHandshakeOutputBytes});
    for (u32 round = 0; round < 64; ++round) {
        if (!step.ok()) {
            *native_error = static_cast<u32>(step.native_status);
            return false;
        }
        if (step.produced != 0 &&
            !send_all(client, runtime, ConstSlice<u8>{runtime->handshake_output, step.produced},
                      native_error)) {
            return false;
        }
        SecureZeroMemory(runtime->handshake_output, sizeof(runtime->handshake_output));
        if (step.state == TlsHandshakeState::complete ||
            step.state == TlsHandshakeState::complete_and_send) {
            return true;
        }
        if (!start_receive(runtime, native_error)) {
            return false;
        }
        const WaitResult waited =
            wait_for(client, runtime, NetworkOperationKind::receive, kPhaseTimeoutMs, false);
        if (waited.state != WaitState::completion ||
            !accept_receive(runtime, waited.completion, native_error)) {
            if (waited.native_error != 0) {
                *native_error = waited.native_error;
            }
            return false;
        }
        const ConstSlice<u8> input = byte_queue_readable(runtime->encrypted_input);
        step = tls_client_handshake(&runtime->tls, input,
                                    Slice<u8>{runtime->handshake_output, kHandshakeOutputBytes});
        if (!byte_queue_consume(&runtime->encrypted_input, step.consumed)) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
    }
    *native_error = WAIT_TIMEOUT;
    return false;
}

bool send_frame(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, WebSocketOpcode opcode,
                ConstSlice<u8> payload, u32* native_error) noexcept {
    u32 masking_key = 0;
    if (!secure_random_u32(&masking_key).ok()) {
        *native_error = ERROR_GEN_FAILURE;
        return false;
    }
    const WebSocketWriteResult written = websocket_write_client_frame(
        opcode, true, payload, masking_key, Slice<u8>{runtime->frame_output, kFrameOutputBytes});
    const bool sent =
        written.ok() &&
        send_tls(client, runtime, ConstSlice<u8>{runtime->frame_output, written.bytes}, native_error);
    SecureZeroMemory(runtime->frame_output, sizeof(runtime->frame_output));
    return sent;
}

// Sends the opening request and validates the response. The caller erases the key.
bool exchange_websocket_opening(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime,
                                ConstSlice<u8> client_key, u32* native_error) noexcept {
    u8 request[1024]{};
    const WebSocketHandshakeWriteResult written = websocket_write_opening_request(
        bytes("/?v=2"), bytes(kRemoteAuthHost), client_key,
        Slice<u8>{request, static_cast<u32>(sizeof(request))});
    const bool sent =
        written.ok() && send_tls(client, runtime, ConstSlice<u8>{request, written.bytes}, native_error);
    SecureZeroMemory(request, sizeof(request));
    if (!sent)
        return false;
    const u64 deadline = deadline_after(GetTickCount64(), kPhaseTimeoutMs);
    for (;;) {
        const WebSocketHandshakeValidation validation = websocket_validate_opening_response(
            byte_queue_readable(runtime->websocket_input), client_key, kWebSocketInputBytes, 64);
        if (validation.state == WebSocketHandshakeState::complete)
            return byte_queue_consume(&runtime->websocket_input, validation.header_bytes);
        if (validation.state == WebSocketHandshakeState::error) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
        const u64 now_ms = GetTickCount64();
        if (now_ms >= deadline) {
            *native_error = WAIT_TIMEOUT;
            return false;
        }
        const PlaintextResult received =
            receive_plaintext(client, runtime, wait_milliseconds_until(now_ms, deadline, INFINITE));
        if (received.state != PlaintextState::plaintext ||
            !byte_queue_append(&runtime->websocket_input, received.plaintext)) {
            *native_error = received.native_error;
            return false;
        }
    }
}

bool open_websocket(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime,
                    u32* native_error) noexcept {
    publish_phase(client, RemoteAuthClientPhase::websocket_opening);
    u8 nonce[kWebSocketNonceBytes]{};
    u8 client_key[kWebSocketClientKeyBytes]{};
    const SecureRandomResult random = secure_random_fill(Slice<u8>{nonce, static_cast<u32>(sizeof(nonce))});
    const WebSocketHandshakeWriteResult encoded =
        random.ok()
            ? websocket_encode_client_key(ConstSlice<u8>{nonce, static_cast<u32>(sizeof(nonce))},
                                          Slice<u8>{client_key, static_cast<u32>(sizeof(client_key))})
            : WebSocketHandshakeWriteResult{};
    SecureZeroMemory(nonce, sizeof(nonce));
    if (!random.ok() || !encoded.ok())
        *native_error = static_cast<u32>(random.platform_status);
    const bool opened = random.ok() && encoded.ok() &&
                        exchange_websocket_opening(client, runtime, ConstSlice<u8>{client_key, encoded.bytes},
                                                   native_error);
    SecureZeroMemory(client_key, sizeof(client_key));
    return opened;
}

void store_qr(RemoteAuthClient* client, Utf8View fingerprint) noexcept {
    char qr[kRemoteAuthQrUrlBytes]{};
    const RemoteAuthWriteResult written =
        remote_auth_write_qr_url(fingerprint, Slice<char>{qr, static_cast<u32>(sizeof(qr))});
    if (!written.ok() || written.bytes != kRemoteAuthQrUrlBytes) {
        SecureZeroMemory(qr, sizeof(qr));
        return;
    }
    AcquireSRWLockExclusive(client->snapshot_lock.native());
    copy_bytes(client->qr_url, qr, written.bytes);
    client->qr_url_bytes = written.bytes;
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
    SecureZeroMemory(qr, sizeof(qr));
}

void store_user(RemoteAuthClient* client, Utf8View user) noexcept {
    AcquireSRWLockExclusive(client->snapshot_lock.native());
    SecureZeroMemory(client->user, sizeof(client->user));
    copy_bytes(client->user, user.data, user.bytes);
    client->user_bytes = user.bytes;
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
}

struct TicketEventContext {
    RemoteAuthClient* client;
    RemoteAuthClientRuntime* runtime;
};

void publish_ticket_status(RemoteAuthClient* client,
                           const RemoteAuthTicketExchangeResult& status) noexcept {
    InterlockedExchange64(&client->ticket_error_code, static_cast<LONG64>(status.error_code));
    InterlockedExchange64(&client->retry_at_ms, static_cast<LONG64>(status.retry_at_ms));
    InterlockedExchange(&client->ticket_response_bytes, static_cast<LONG>(status.response_bytes));
    InterlockedExchange(&client->ticket_decode_error_offset, static_cast<LONG>(status.decode_error_offset));
    InterlockedExchange(&client->http_status, status.http_status);
    InterlockedExchange(&client->ticket_response_kind, static_cast<LONG>(status.response_kind));
    InterlockedExchange(&client->ticket_decode_error, static_cast<LONG>(status.decode_error));
    InterlockedExchange(&client->ticket_scan_error, static_cast<LONG>(status.scan_error));
    InterlockedExchange(&client->ticket_read_error, static_cast<LONG>(status.read_error));
    InterlockedExchange(&client->ticket_content_type, static_cast<LONG>(status.content_type));
    InterlockedExchange(&client->ticket_content_encoding, static_cast<LONG>(status.content_encoding));
    InterlockedExchange(&client->ticket_body_kind, static_cast<LONG>(status.body_kind));
    InterlockedExchange(&client->captcha_provider, static_cast<LONG>(status.captcha_provider));
    InterlockedExchange(&client->captcha_invisible, status.captcha_invisible ? 1 : 0);
}

void handle_ticket_event(void* context, const RemoteAuthTicketExchangeEvent& event) noexcept {
    auto* ticket = static_cast<TicketEventContext*>(context);
    publish_ticket_status(ticket->client, event.status);
    if (event.kind == RemoteAuthTicketExchangeEventKind::retrying) {
        publish_phase(ticket->client, RemoteAuthClientPhase::ticket_exchange);
        return;
    }
    publish_phase(ticket->client, RemoteAuthClientPhase::captcha_required);
    publish_event(ticket->client, ticket->runtime, RemoteAuthClientEventKind::captcha_required);
}

RemoteAuthClientError ticket_error(RemoteAuthTicketExchangeError error) noexcept {
    switch (error) {
    case RemoteAuthTicketExchangeError::none:
        return RemoteAuthClientError::none;
    case RemoteAuthTicketExchangeError::invalid_input:
        return RemoteAuthClientError::invalid_input;
    case RemoteAuthTicketExchangeError::memory:
        return RemoteAuthClientError::memory;
    case RemoteAuthTicketExchangeError::request:
        return RemoteAuthClientError::ticket_request;
    case RemoteAuthTicketExchangeError::transport:
        return RemoteAuthClientError::ticket_transport;
    case RemoteAuthTicketExchangeError::response:
        return RemoteAuthClientError::ticket_response;
    case RemoteAuthTicketExchangeError::captcha:
        return RemoteAuthClientError::captcha;
    case RemoteAuthTicketExchangeError::credential:
        return RemoteAuthClientError::credential;
    case RemoteAuthTicketExchangeError::cancelled:
        return RemoteAuthClientError::cancelled;
    case RemoteAuthTicketExchangeError::stop:
        return RemoteAuthClientError::stop;
    }
    return RemoteAuthClientError::ticket_response;
}

bool submit_ticket(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, Utf8View ticket,
                   u32* native_error, RemoteAuthClientError* client_error,
                   RemoteAuthHandshakeResult* handshake_failure) noexcept {
    publish_phase(client, RemoteAuthClientPhase::ticket_exchange);
    TicketEventContext event_context{client, runtime};
    const RemoteAuthTicketExchangeResult exchanged = remote_auth_ticket_exchange_run(
        &runtime->ticket_exchange, &runtime->handshake, ticket, client->captcha_event, client->cancel_event,
        handle_ticket_event, &event_context);
    publish_ticket_status(client, exchanged);
    *native_error = exchanged.native_error;
    *client_error = ticket_error(exchanged.error);
    *handshake_failure = exchanged.handshake;
    return exchanged.ok();
}

u64 handshake_wake(const RemoteAuthHandshake& handshake, u64 fallback) noexcept {
    u64 value = handshake.expires_at_ms;
    if (value == 0 || (handshake.next_heartbeat_at_ms != 0 && handshake.next_heartbeat_at_ms < value)) {
        value = handshake.next_heartbeat_at_ms;
    }
    return value != 0 ? value : fallback;
}

bool handle_message(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, bool* ticket_ready,
                    u32* native_error, RemoteAuthClientError* client_error,
                    RemoteAuthHandshakeResult* failure) noexcept {
    char command[kRemoteAuthMaximumCommandBytes]{};
    char data[kRemoteAuthMaximumTicketBytes]{};
    RemoteAuthHandshakeEvent event{};
    *failure = remote_auth_handshake_handle(
        &runtime->handshake,
        Utf8View{reinterpret_cast<const char*>(runtime->message_storage), runtime->message_bytes},
        Slice<u32>{runtime->structural_scratch, kStructuralCount}, 8, GetTickCount64(),
        Slice<char>{command, static_cast<u32>(sizeof(command))},
        Slice<char>{data, static_cast<u32>(sizeof(data))}, &event);
    SecureZeroMemory(runtime->message_storage, sizeof(runtime->message_storage));
    runtime->message_bytes = 0;
    if (!failure->ok()) {
        SecureZeroMemory(command, sizeof(command));
        SecureZeroMemory(data, sizeof(data));
        return false;
    }
    InterlockedExchange64(&client->next_wake_at_ms, static_cast<LONG64>(event.next_wake_at_ms));
    if (event.kind == RemoteAuthHandshakeEventKind::send_command) {
        const bool sent = send_frame(
            client, runtime, WebSocketOpcode::text,
            ConstSlice<u8>{reinterpret_cast<const u8*>(command), event.command_bytes}, native_error);
        SecureZeroMemory(command, sizeof(command));
        SecureZeroMemory(data, sizeof(data));
        return sent;
    }
    SecureZeroMemory(command, sizeof(command));
    if (event.kind == RemoteAuthHandshakeEventKind::qr_ready) {
        store_qr(client, Utf8View{data, event.data_bytes});
        publish_phase(client, RemoteAuthClientPhase::qr_ready);
        publish_event(client, runtime, RemoteAuthClientEventKind::qr_ready);
    } else if (event.kind == RemoteAuthHandshakeEventKind::user_pending) {
        store_user(client, Utf8View{data, event.data_bytes});
        publish_phase(client, RemoteAuthClientPhase::mobile_pending);
        publish_event(client, runtime, RemoteAuthClientEventKind::mobile_pending);
    } else if (event.kind == RemoteAuthHandshakeEventKind::ticket_ready) {
        *ticket_ready = submit_ticket(client, runtime, Utf8View{data, event.data_bytes}, native_error,
                                      client_error, failure);
        SecureZeroMemory(data, sizeof(data));
        return *ticket_ready;
    } else if (event.kind == RemoteAuthHandshakeEventKind::cancelled) {
        *client_error = RemoteAuthClientError::cancelled;
        SecureZeroMemory(data, sizeof(data));
        return false;
    }
    SecureZeroMemory(data, sizeof(data));
    return true;
}

bool process_frames(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, bool* authenticated,
                    u32* native_error, RemoteAuthClientError* client_error,
                    RemoteAuthHandshakeResult* failure) noexcept {
    for (;;) {
        const WebSocketParseResult parsed =
            websocket_parse_frame(byte_queue_readable(runtime->websocket_input),
                                  kRemoteAuthMaximumPacketBytes, WebSocketDirection::server_to_client);
        if (parsed.state == WebSocketParseState::need_more) {
            return true;
        }
        if (!parsed.ok()) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
        const WebSocketFrame frame = parsed.frame;
        const WebSocketMessageStep tracked =
            websocket_track_frame(&runtime->tracker, frame, kRemoteAuthMaximumPacketBytes);
        if (!tracked.ok()) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
        if (tracked.state == WebSocketMessageState::control) {
            if (frame.opcode == WebSocketOpcode::ping &&
                !send_frame(client, runtime, WebSocketOpcode::pong, frame.payload, native_error)) {
                return false;
            }
            if (frame.opcode == WebSocketOpcode::close) {
                const u16 close_code =
                    frame.payload.count >= 2
                        ? static_cast<u16>((static_cast<u16>(frame.payload[0]) << 8) | frame.payload[1])
                        : 0;
                *failure = RemoteAuthHandshakeResult{0,
                                                     close_code == 4003
                                                         ? RemoteAuthHandshakeError::session_timeout
                                                         : RemoteAuthHandshakeError::unexpected_packet,
                                                     RemoteAuthError::none,
                                                     RemoteAuthCryptoError::none,
                                                     Base64Error::none,
                                                     RemoteAuthHandshakeStage::none};
                return false;
            }
        } else {
            if (runtime->message_bytes + frame.payload.count > sizeof(runtime->message_storage)) {
                *native_error = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            copy_bytes(runtime->message_storage + runtime->message_bytes, frame.payload.data,
                       frame.payload.count);
            runtime->message_bytes += frame.payload.count;
            if (tracked.state == WebSocketMessageState::message_complete) {
                if (tracked.message_opcode != WebSocketOpcode::text) {
                    *native_error = ERROR_INVALID_DATA;
                    return false;
                }
                bool ticket_finished = false;
                if (!handle_message(client, runtime, &ticket_finished, native_error, client_error,
                                    failure)) {
                    return false;
                }
                if (ticket_finished) {
                    *authenticated = true;
                    return true;
                }
            }
        }
        if (!byte_queue_consume(&runtime->websocket_input, frame.frame_bytes)) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
    }
}

bool tick_handshake(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, u32* native_error,
                    RemoteAuthHandshakeResult* failure) noexcept {
    if (runtime->handshake.phase < RemoteAuthHandshakePhase::awaiting_nonce ||
        runtime->handshake.phase > RemoteAuthHandshakePhase::ticket_ready) {
        return true;
    }
    char command[kRemoteAuthMaximumCommandBytes]{};
    RemoteAuthHandshakeEvent event{};
    *failure = remote_auth_handshake_tick(&runtime->handshake, GetTickCount64(),
                                          Slice<char>{command, static_cast<u32>(sizeof(command))}, &event);
    if (!failure->ok()) {
        SecureZeroMemory(command, sizeof(command));
        return false;
    }
    InterlockedExchange64(&client->next_wake_at_ms, static_cast<LONG64>(event.next_wake_at_ms));
    const bool sent =
        event.kind != RemoteAuthHandshakeEventKind::send_command ||
        send_frame(client, runtime, WebSocketOpcode::text,
                   ConstSlice<u8>{reinterpret_cast<const u8*>(command), event.command_bytes}, native_error);
    SecureZeroMemory(command, sizeof(command));
    return sent;
}

void drain_transport(RemoteAuthClientRuntime* runtime) noexcept {
    u32 remaining_socket = (runtime->connection.connect_pending ? 1u : 0u) +
                           (runtime->connection.receive_pending ? 1u : 0u) +
                           (runtime->connection.send_pending ? 1u : 0u);
    if (runtime->resolver.pending) {
        network_resolver_cancel(&runtime->resolver);
    }
    if (remaining_socket != 0) {
        network_connection_close(&runtime->connection);
    }
    while (remaining_socket != 0 || runtime->resolver.pending ||
           read_acquire(&runtime->cancel_signal.pending) != 0) {
        const NetworkPollResult polled = network_loop_poll(&runtime->loop, INFINITE);
        if (polled.state != NetworkPollState::completion) {
            continue;
        }
        if (polled.completion.connection == &runtime->connection && remaining_socket != 0) {
            --remaining_socket;
        }
    }
    network_resolver_release(&runtime->resolver);
}

void release_transport(RemoteAuthClientRuntime* runtime) noexcept {
    if (runtime->tls_initialized) {
        tls_client_release(&runtime->tls);
        runtime->tls_initialized = false;
    }
    drain_transport(runtime);
    network_connection_close(&runtime->connection);
    byte_queue_clear(&runtime->encrypted_input, true);
    byte_queue_clear(&runtime->websocket_input, true);
}

DWORD WINAPI remote_auth_thread(void* parameter) noexcept {
    auto* client = static_cast<RemoteAuthClient*>(parameter);
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
    const NetworkResult loop = network_loop_initialize(&runtime->loop);
    if (!loop.ok()) {
        publish_failure(client, runtime, RemoteAuthClientError::network,
                        static_cast<u32>(loop.native_error));
        SetEvent(client->ready_event);
        return 1;
    }
    InterlockedExchange(&runtime->loop_ready, 1);
    publish_phase(client, RemoteAuthClientPhase::resolving);
    SetEvent(client->ready_event);

    u32 native_error = 0;
    RemoteAuthClientError error = RemoteAuthClientError::none;
    RemoteAuthHandshakeResult handshake_failure{};
    bool authenticated = false;
    // RSA-2048 generation costs tens of milliseconds; doing it here keeps it
    // off the UI thread, which would otherwise delay the first login frame.
    const RemoteAuthHandshakeResult keyed = remote_auth_handshake_initialize(&runtime->handshake);
    if (!keyed.ok()) {
        native_error = static_cast<u32>(keyed.crypto_error);
        error = RemoteAuthClientError::memory;
    }
    bool connected = keyed.ok() && connect_socket(client, runtime, &native_error, &error);
    if (connected && !complete_tls_handshake(client, runtime, &native_error)) {
        connected = false;
        error = stop_requested(client) ? RemoteAuthClientError::cancelled : RemoteAuthClientError::tls;
    }
    if (connected && !open_websocket(client, runtime, &native_error)) {
        connected = false;
        error =
            stop_requested(client) ? RemoteAuthClientError::cancelled : RemoteAuthClientError::websocket;
    }
    if (connected) {
        publish_phase(client, RemoteAuthClientPhase::awaiting_qr);
        runtime->phase_deadline_ms = deadline_after(GetTickCount64(), kPhaseTimeoutMs);
    }

    while (connected && !authenticated && !stop_requested(client)) {
        if (!process_frames(client, runtime, &authenticated, &native_error, &error, &handshake_failure)) {
            connected = false;
            if (error == RemoteAuthClientError::none) {
                error = RemoteAuthClientError::handshake;
            }
            break;
        }
        if (authenticated) {
            break;
        }
        if (!tick_handshake(client, runtime, &native_error, &handshake_failure)) {
            connected = false;
            error = RemoteAuthClientError::handshake;
            break;
        }
        const u64 now_ms = GetTickCount64();
        const u64 wake = handshake_wake(runtime->handshake, runtime->phase_deadline_ms);
        InterlockedExchange64(&client->next_wake_at_ms, static_cast<LONG64>(wake));
        const PlaintextResult received =
            receive_plaintext(client, runtime, wait_milliseconds_until(now_ms, wake, INFINITE));
        if (received.state == PlaintextState::timeout) {
            continue;
        }
        if (received.state == PlaintextState::stopped) {
            break;
        }
        if (received.state != PlaintextState::plaintext ||
            !byte_queue_append(&runtime->websocket_input, received.plaintext)) {
            connected = false;
            native_error = received.native_error;
            error = RemoteAuthClientError::network;
            break;
        }
    }

    release_transport(runtime);
    InterlockedExchange(&runtime->loop_ready, 0);
    network_loop_release(&runtime->loop);

    // Serialize the final adoption with cancel/stop admission. Otherwise a
    // cancel can publish "cancelling" between the flag check and the final
    // authenticated publication, allowing the worker to overwrite it.
    AcquireSRWLockExclusive(client->snapshot_lock.native());
    if (authenticated && !stop_requested(client)) {
        remote_auth_handshake_release(&runtime->handshake);
        publish_phase(client, RemoteAuthClientPhase::authenticated);
        publish_event(client, runtime, RemoteAuthClientEventKind::authenticated);
        ReleaseSRWLockExclusive(client->snapshot_lock.native());
        return 0;
    }
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
    if (stop_requested(client) || error == RemoteAuthClientError::cancelled) {
        secure_buffer_clear(&runtime->ticket_exchange.credential);
        remote_auth_handshake_release(&runtime->handshake);
        const RemoteAuthClientPhase phase = load_phase(client);
        const bool stopping = phase == RemoteAuthClientPhase::stopping;
        InterlockedExchange(&client->error, static_cast<LONG>(stopping ? RemoteAuthClientError::none
                                                                       : RemoteAuthClientError::cancelled));
        publish_phase(client, stopping ? RemoteAuthClientPhase::stopped : RemoteAuthClientPhase::cancelled);
        publish_event(client, runtime,
                      stopping ? RemoteAuthClientEventKind::stopped : RemoteAuthClientEventKind::cancelled);
        return 0;
    }
    remote_auth_handshake_release(&runtime->handshake);
    publish_failure(
        client, runtime, error == RemoteAuthClientError::none ? RemoteAuthClientError::network : error,
        native_error,
        handshake_failure.error != RemoteAuthHandshakeError::none ? &handshake_failure : nullptr);
    return 1;
}

bool release_runtime(RemoteAuthClientRuntime* runtime) noexcept {
    if (!runtime) {
        return true;
    }
    remote_auth_ticket_exchange_release(&runtime->ticket_exchange);
    if (runtime->ticket_exchange.transport.worker.thread || runtime->ticket_exchange.transport.runtime) {
        return false;
    }
    remote_auth_handshake_release(&runtime->handshake);
    SecureZeroMemory(runtime, sizeof(*runtime));
    VirtualFree(runtime, 0, MEM_RELEASE);
    return true;
}

bool initialize_runtime(RemoteAuthClientRuntime* runtime, u32* native_error) noexcept {
    if (!byte_queue_initialize(&runtime->encrypted_input,
                               Slice<u8>{runtime->encrypted_storage, kEncryptedInputBytes}) ||
        !byte_queue_initialize(&runtime->websocket_input,
                               Slice<u8>{runtime->websocket_storage, kWebSocketInputBytes})) {
        *native_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    const RemoteAuthTicketExchangeResult exchange =
        remote_auth_ticket_exchange_initialize(&runtime->ticket_exchange);
    if (!exchange.ok()) {
        *native_error = exchange.native_error;
        return false;
    }
    return true;
}

// source_bytes names the field guarded by the snapshot lock, so it is read under that lock.
bool copy_snapshot(const RemoteAuthClient& client, const char* source, const u32& source_bytes,
                   Slice<char> output, u32* output_bytes) noexcept {
    if (!output_bytes || (!output.data && output.count != 0))
        return false;
    AcquireSRWLockShared(client.snapshot_lock.native());
    const u32 count = source_bytes;
    const bool fits = count != 0 && count <= output.count && output.data;
    if (fits)
        copy_bytes(output.data, source, count);
    *output_bytes = count;
    ReleaseSRWLockShared(client.snapshot_lock.native());
    return fits;
}

RemoteAuthClientResult start_failure(RemoteAuthClient* client, RemoteAuthClientError error,
                                     u32 native_error) noexcept {
    InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    InterlockedExchange(&client->error, static_cast<LONG>(error));
    publish_phase(client, RemoteAuthClientPhase::failed);
    return client_result(client);
}

} // namespace

RemoteAuthClientResult remote_auth_client_start(RemoteAuthClient* client) noexcept {
    if (!client || client->runtime || client->thread ||
        load_phase(client) != RemoteAuthClientPhase::empty)
        return client_result(nullptr);
    reset_value<RemoteAuthClient>(client);
    publish_phase(client, RemoteAuthClientPhase::starting);
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(
        VirtualAlloc(nullptr, sizeof(RemoteAuthClientRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime)
        return start_failure(client, RemoteAuthClientError::memory, thread_last_error());
    client->runtime = runtime;
    u32 native_error = 0;
    if (!initialize_runtime(runtime, &native_error)) {
        release_runtime(runtime);
        client->runtime = nullptr;
        return start_failure(client, RemoteAuthClientError::memory, native_error);
    }
    client->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->notification_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->captcha_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!client->ready_event || !client->notification_event || !client->cancel_event ||
        !client->captcha_event) {
        native_error = thread_last_error();
        if (client->captcha_event)
            CloseHandle(client->captcha_event);
        if (client->cancel_event)
            CloseHandle(client->cancel_event);
        if (client->notification_event)
            CloseHandle(client->notification_event);
        if (client->ready_event)
            CloseHandle(client->ready_event);
        release_runtime(runtime);
        reset_value<RemoteAuthClient>(client);
        return start_failure(client, RemoteAuthClientError::event, native_error);
    }
    constexpr usize kStackReserve = 256 * 1024;
    client->thread = CreateThread(nullptr, kStackReserve, remote_auth_thread, client,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!client->thread) {
        native_error = thread_last_error();
        CloseHandle(client->captcha_event);
        CloseHandle(client->cancel_event);
        CloseHandle(client->notification_event);
        CloseHandle(client->ready_event);
        release_runtime(runtime);
        reset_value<RemoteAuthClient>(client);
        return start_failure(client, RemoteAuthClientError::thread, native_error);
    }
    const DWORD ready = WaitForSingleObject(client->ready_event, 5'000);
    if (ready != WAIT_OBJECT_0) {
        remote_auth_client_stop(client, kPhaseTimeoutMs);
        RemoteAuthClientResult value{};
        value.native_error = ready == WAIT_FAILED ? thread_last_error() : WAIT_TIMEOUT;
        value.error = RemoteAuthClientError::thread;
        value.phase = RemoteAuthClientPhase::failed;
        return value;
    }
    return client_result(client);
}

RemoteAuthClientResult remote_auth_client_cancel(RemoteAuthClient* client) noexcept {
    if (!client || !client->runtime || !client->thread)
        return client_result(nullptr);
    AcquireSRWLockExclusive(client->snapshot_lock.native());
    const RemoteAuthClientPhase phase = load_phase(client);
    if (phase == RemoteAuthClientPhase::authenticated || phase == RemoteAuthClientPhase::cancelled ||
        phase == RemoteAuthClientPhase::stopped || phase == RemoteAuthClientPhase::failed) {
        ReleaseSRWLockExclusive(client->snapshot_lock.native());
        return client_result(client);
    }
    publish_phase(client, RemoteAuthClientPhase::cancelling);
    InterlockedExchange(&client->stop_requested, 1);
    SetEvent(client->cancel_event);
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
    if (read_acquire(&runtime->loop_ready) != 0) {
        network_signal_post(&runtime->loop, &runtime->cancel_signal, kCancelSignalValue);
    }
    return client_result(client);
}

RemoteAuthClientResult remote_auth_client_stop(RemoteAuthClient* client, u32 timeout_ms) noexcept {
    if (!client || !client->runtime || !client->thread || timeout_ms == 0)
        return client_result(nullptr);
    AcquireSRWLockExclusive(client->snapshot_lock.native());
    if (load_phase(client) == RemoteAuthClientPhase::authenticated) {
        // Authentication publication proves the worker finished writing this
        // credential. Stop can erase it even while the thread is still exiting.
        auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
        secure_buffer_clear(&runtime->ticket_exchange.credential);
    }
    if (WaitForSingleObject(client->thread, 0) == WAIT_TIMEOUT) {
        publish_phase(client, RemoteAuthClientPhase::stopping);
        InterlockedExchange(&client->stop_requested, 1);
        SetEvent(client->cancel_event);
        auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
        if (read_acquire(&runtime->loop_ready) != 0) {
            network_signal_post(&runtime->loop, &runtime->cancel_signal, kCancelSignalValue);
        }
    }
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
    const DWORD waited = WaitForSingleObject(client->thread, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        RemoteAuthClientResult value = client_result(client);
        value.error = RemoteAuthClientError::stop;
        value.native_error = waited == WAIT_FAILED ? thread_last_error() : WAIT_TIMEOUT;
        return value;
    }
    // Joining the outer QR thread does not prove its nested REST worker has
    // joined: a ticket exchange may have timed out while draining. Keep the
    // complete client owner, handles, and runtime available for a stop retry.
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
    const auto ticket_stopped = remote_auth_ticket_exchange_stop(&runtime->ticket_exchange, timeout_ms);
    if (ticket_stopped.http_status == 429) {
        // A nested worker may publish after the outer thread's cancelled exit.
        // Merge only the terminal receipt, without replacing prior diagnostics.
        AcquireSRWLockExclusive(client->snapshot_lock.native());
        InterlockedExchange(&client->http_status, 429);
        const u64 previous = static_cast<u64>(read_acquire(&client->retry_at_ms));
        InterlockedExchange64(&client->retry_at_ms,
                              static_cast<LONG64>(maximum(previous, ticket_stopped.retry_at_ms)));
        ReleaseSRWLockExclusive(client->snapshot_lock.native());
    }
    if (runtime->ticket_exchange.transport.worker.thread || runtime->ticket_exchange.transport.runtime) {
        publish_phase(client, RemoteAuthClientPhase::stopping);
        RemoteAuthClientResult value = client_result(client);
        value.error = RemoteAuthClientError::stop;
        value.native_error = ticket_stopped.native_error;
        return value;
    }
    // The worker may publish a 429 while stop is joining it, after the host's
    // last poll. Keep only its non-secret rate-limit receipt across teardown.
    const auto terminal = client_result(client);
    if (!release_runtime(runtime)) {
        publish_phase(client, RemoteAuthClientPhase::stopping);
        RemoteAuthClientResult value = client_result(client);
        value.error = RemoteAuthClientError::stop;
        value.native_error = ERROR_INVALID_STATE;
        return value;
    }
    CloseHandle(client->thread);
    CloseHandle(client->captcha_event);
    CloseHandle(client->cancel_event);
    CloseHandle(client->notification_event);
    CloseHandle(client->ready_event);
    reset_value<RemoteAuthClient>(client);
    RemoteAuthClientResult value{};
    value.phase = RemoteAuthClientPhase::stopped;
    if (terminal.http_status == 429) {
        value.http_status = terminal.http_status;
        value.retry_at_ms = terminal.retry_at_ms;
    }
    return value;
}

bool remote_auth_client_pop_event(RemoteAuthClient* client, RemoteAuthClientEvent* event) noexcept {
    auto* runtime =
        client && client->runtime ? static_cast<RemoteAuthClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || !event || !spsc_queue_pop(&runtime->events, event)) {
        return false;
    }
    if (spsc_queue_size(runtime->events) == 0) {
        ResetEvent(client->notification_event);
        if (spsc_queue_size(runtime->events) != 0) {
            SetEvent(client->notification_event);
        }
    }
    return true;
}

bool remote_auth_client_copy_qr_url(const RemoteAuthClient& client, Slice<char> output,
                                    u32* output_bytes) noexcept {
    return copy_snapshot(client, client.qr_url, client.qr_url_bytes, output, output_bytes);
}

bool remote_auth_client_copy_user(const RemoteAuthClient& client, Slice<char> output,
                                  u32* output_bytes) noexcept {
    return copy_snapshot(client, client.user, client.user_bytes, output, output_bytes);
}

SecureBufferResult remote_auth_client_take_credential(RemoteAuthClient* client,
                                                      SecureBuffer* output) noexcept {
    auto* runtime =
        client && client->runtime ? static_cast<RemoteAuthClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || !output || !output->data || !output->locked || output->capacity == 0 ||
        load_phase(client) != RemoteAuthClientPhase::authenticated ||
        WaitForSingleObject(client->thread, 0) != WAIT_OBJECT_0) {
        return SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    }
    if (InterlockedCompareExchange(&client->token_taken, 1, 0) != 0) {
        return SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    }
    const SecureBufferResult assigned =
        remote_auth_ticket_exchange_take_credential(&runtime->ticket_exchange, output);
    if (!assigned.ok()) {
        InterlockedExchange(&client->token_taken, 0);
    }
    return assigned;
}

SecureBufferResult remote_auth_client_copy_captcha_secret(const RemoteAuthClient* client,
                                                          CaptchaChallengeSecretKind kind,
                                                          SecureBuffer* output) noexcept {
    const auto* runtime =
        client && client->runtime ? static_cast<const RemoteAuthClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || load_phase(client) != RemoteAuthClientPhase::captcha_required) {
        return SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    }
    return remote_auth_ticket_exchange_copy_captcha_secret(&runtime->ticket_exchange, kind, output);
}

RemoteAuthClientResult remote_auth_client_submit_captcha(RemoteAuthClient* client,
                                                         const SecureBuffer* solution) noexcept {
    auto* runtime =
        client && client->runtime ? static_cast<RemoteAuthClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || load_phase(client) != RemoteAuthClientPhase::captcha_required) {
        RemoteAuthClientResult result = client_result(client);
        result.error = RemoteAuthClientError::invalid_input;
        return result;
    }
    const SecureBufferResult assigned = remote_auth_ticket_exchange_submit_captcha(
        &runtime->ticket_exchange, solution, client->captcha_event);
    if (!assigned.ok()) {
        RemoteAuthClientResult result = client_result(client);
        result.native_error = assigned.native_error;
        result.error = RemoteAuthClientError::captcha;
        return result;
    }
    return client_result(client);
}

HANDLE remote_auth_client_notification_handle(const RemoteAuthClient* client) noexcept {
    if (client && client->thread && load_phase(client) == RemoteAuthClientPhase::authenticated &&
        read_acquire(&client->token_taken) == 0)
        return client->thread;
    return client ? client->notification_event : nullptr;
}

RemoteAuthClientResult remote_auth_client_status(const RemoteAuthClient* client) noexcept {
    return client_result(client);
}

u32 remote_auth_client_runtime_bytes() noexcept {
    const u64 total = static_cast<u64>(sizeof(RemoteAuthClientRuntime)) - sizeof(RemoteAuthTicketExchange) +
                      remote_auth_ticket_exchange_runtime_bytes();
    return total <= 0xFFFFFFFFull ? static_cast<u32>(total) : 0;
}

} // namespace maicord
