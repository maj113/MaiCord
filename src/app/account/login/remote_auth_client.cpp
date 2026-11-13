#include "maicord/core/win32_process.hpp"
#include "maicord/app/account/login/remote_auth_client.hpp"
#include "maicord/core/time.hpp"

#include "maicord/app/account/login/remote_auth_ticket_exchange.hpp"
#include "maicord/app/net/signed_out_session.hpp"
#include "maicord/protocol/auth/user_auth.hpp"
#include "maicord/protocol/http/rest.hpp"
#include "maicord/core/byte_queue.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/spsc_queue.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/platform/net/tls_stream.hpp"
#include "maicord/protocol/http/websocket_handshake.hpp"
#include "maicord/protocol/http/websocket_message.hpp"

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

enum class PumpState : u8 {
    progress,
    timeout,
    stopped,
    failed,
};

struct RemoteAuthClientRuntime {
    RemoteAuthHandshake handshake;
    RemoteAuthTicketExchange ticket_exchange;
    NetworkLoop loop;
    NetworkSignal cancel_signal;
    TlsStream stream;
    TlsStreamEvent stream_event;
    ByteQueue websocket_input;
    SpscQueue<RemoteAuthClientEvent, kEventCapacity> events;
    WebSocketMessageTracker tracker;
    u64 phase_deadline_ms;
    u32 message_bytes;
    volatile LONG loop_ready;
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
    event_set(client->notification_event);
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

// The stream's reader: plaintext queues for the frame reader; the peer's close ends the read.
TlsStreamReadAction receive_plaintext(void* context, ConstSlice<u8> plaintext, bool peer_closed) noexcept {
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(context);
    if (peer_closed)
        return TlsStreamReadAction::complete;
    return byte_queue_append(&runtime->websocket_input, plaintext) ? TlsStreamReadAction::continue_read
                                                                    : TlsStreamReadAction::reject;
}

u32 stream_error(const RemoteAuthClientRuntime& runtime) noexcept {
    return runtime.stream_event.native_error != 0 ? runtime.stream_event.native_error
                                                  : static_cast<u32>(runtime.stream.tls_status);
}

// Waits for one completion of the stream before the deadline and hands it to the stream. Failed covers
// a failed stream and the peer's close; native_error is set for timeout and failed.
PumpState pump(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, u64 deadline,
               u32* native_error) noexcept {
    if (stop_requested(client))
        return PumpState::stopped;
    const u64 now_ms = clock_monotonic_milliseconds();
    const NetworkPollResult polled =
        now_ms < deadline ? network_loop_poll(&runtime->loop, wait_milliseconds_until(now_ms, deadline, INFINITE))
                          : NetworkPollResult{{}, NetworkPollState::timeout};
    if (polled.state == NetworkPollState::timeout) {
        *native_error = WAIT_TIMEOUT;
        return PumpState::timeout;
    }
    if (polled.state == NetworkPollState::error) {
        *native_error = static_cast<u32>(polled.completion.native_error);
        return PumpState::failed;
    }
    if (polled.completion.kind == NetworkOperationKind::signal &&
        polled.completion.signal == &runtime->cancel_signal &&
        polled.completion.signal_value == kCancelSignalValue) {
        return PumpState::stopped;
    }
    runtime->stream_event = tls_stream_handle_completion(&runtime->stream, &runtime->loop, polled.completion);
    if (!runtime->stream_event.ok() || runtime->stream_event.kind == TlsStreamEventKind::read_complete) {
        *native_error = stream_error(*runtime);
        return PumpState::failed;
    }
    return PumpState::progress;
}

// Writes plaintext (borrowed until written); what arrives meanwhile queues for the frame reader.
bool send_plaintext(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, ConstSlice<u8> plaintext,
                    u32* native_error) noexcept {
    runtime->stream_event = tls_stream_start_write(&runtime->stream, ConstSlice<ConstSlice<u8>>{&plaintext, 1});
    while (runtime->stream_event.kind != TlsStreamEventKind::write_complete) {
        if (!runtime->stream_event.ok()) {
            *native_error = stream_error(*runtime);
            return false;
        }
        if (pump(client, runtime, deadline_after(clock_monotonic_milliseconds(), kPhaseTimeoutMs), native_error) !=
            PumpState::progress)
            return false;
    }
    return true;
}

// Resolves, connects and completes TLS, each step within kPhaseTimeoutMs, then starts the stream's reader.
bool connect_stream(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, u32* native_error,
                    RemoteAuthClientError* error) noexcept {
    TlsStream* stream = &runtime->stream;
    TlsStreamEvent& event = runtime->stream_event;
    event = tls_stream_initialize(stream, kRemoteAuthHostWide,
                                  TlsStreamBuffers{Slice<u8>{runtime->encrypted_storage, kEncryptedInputBytes},
                                                   Slice<u8>{runtime->handshake_output, kHandshakeOutputBytes},
                                                   Slice<u8>{runtime->record_output, kRecordOutputBytes}})
                ? tls_stream_begin_connect(stream, &runtime->loop)
                : TlsStreamEvent{ERROR_INVALID_PARAMETER, TlsStreamEventKind::failed, TlsStreamError::invalid_input};
    TlsStreamState shown = TlsStreamState::empty;
    for (;;) {
        if (event.kind == TlsStreamEventKind::ready) {
            event = tls_stream_start_duplex(stream, receive_plaintext, runtime);
            if (event.ok())
                return true;
        }
        if (!event.ok()) {
            *native_error = stream_error(*runtime);
            break;
        }
        if (stream->state != shown) {
            shown = stream->state;
            publish_phase(client, shown == TlsStreamState::resolving    ? RemoteAuthClientPhase::resolving
                                  : shown == TlsStreamState::connecting ? RemoteAuthClientPhase::connecting
                                                                        : RemoteAuthClientPhase::tls_handshake);
        }
        if (pump(client, runtime, deadline_after(clock_monotonic_milliseconds(), kPhaseTimeoutMs), native_error) !=
            PumpState::progress)
            break;
    }
    // A failed stream names its step; a timeout or stop happened in the step the stream is still in.
    const TlsStreamError cause = !event.ok()                                   ? event.error
                                 : stream->state == TlsStreamState::resolving  ? TlsStreamError::resolve
                                 : stream->state == TlsStreamState::connecting ? TlsStreamError::connect
                                                                               : TlsStreamError::tls;
    *error = stop_requested(client)             ? RemoteAuthClientError::cancelled
             : cause == TlsStreamError::resolve ? RemoteAuthClientError::resolve
             : cause == TlsStreamError::connect ? RemoteAuthClientError::connect
                                                : RemoteAuthClientError::tls;
    return false;
}

bool send_frame(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, WebSocketOpcode opcode,
                const ConstSlice<u8>& payload, u32* native_error) noexcept {
    u32 masking_key = 0;
    if (!secure_random_u32(&masking_key).ok()) {
        *native_error = ERROR_GEN_FAILURE;
        return false;
    }
    const WebSocketWriteResult written = websocket_write_client_frame(
        opcode, payload, masking_key, Slice<u8>{runtime->frame_output, kFrameOutputBytes});
    const bool sent =
        written.ok() &&
        send_plaintext(client, runtime, ConstSlice<u8>{runtime->frame_output, written.bytes}, native_error);
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
        written.ok() && send_plaintext(client, runtime, ConstSlice<u8>{request, written.bytes}, native_error);
    SecureZeroMemory(request, sizeof(request));
    if (!sent)
        return false;
    const u64 deadline = deadline_after(clock_monotonic_milliseconds(), kPhaseTimeoutMs);
    for (;;) {
        const WebSocketHandshakeValidation validation = websocket_validate_opening_response(
            byte_queue_readable(runtime->websocket_input), client_key, kWebSocketInputBytes, 64);
        if (validation.state == WebSocketHandshakeState::complete)
            return byte_queue_consume(&runtime->websocket_input, validation.header_bytes);
        if (validation.state == WebSocketHandshakeState::error) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
        if (pump(client, runtime, deadline, native_error) != PumpState::progress)
            return false;
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

// Handles one complete text message; its bytes (fragment storage or consumed input) are erased.
bool handle_message(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime, ConstSlice<u8> message,
                    bool* ticket_ready, u32* native_error, RemoteAuthClientError* client_error,
                    RemoteAuthHandshakeResult* failure) noexcept {
    char command[kRemoteAuthMaximumCommandBytes]{};
    char data[kRemoteAuthMaximumTicketBytes]{};
    RemoteAuthHandshakeEvent event{};
    *failure = remote_auth_handshake_handle(
        &runtime->handshake,
        Utf8View{reinterpret_cast<const char*>(message.data), message.count},
        Slice<u32>{runtime->structural_scratch, kStructuralCount}, 8, clock_monotonic_milliseconds(),
        Slice<char>{command, static_cast<u32>(sizeof(command))},
        Slice<char>{data, static_cast<u32>(sizeof(data))}, &event);
    SecureZeroMemory(const_cast<u8*>(message.data), message.count);
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
        const WebSocketMessageRead read = websocket_read_message(
            &runtime->websocket_input, &runtime->tracker,
            Slice<u8>{runtime->message_storage, kRemoteAuthMaximumPacketBytes}, &runtime->message_bytes,
            kRemoteAuthMaximumPacketBytes, kRemoteAuthMaximumPacketBytes);
        if (read.state == WebSocketReadState::need_input)
            return true;
        if (read.state == WebSocketReadState::error ||
            (read.state == WebSocketReadState::message && read.opcode != WebSocketOpcode::text)) {
            *native_error = ERROR_INVALID_DATA;
            return false;
        }
        if (read.state == WebSocketReadState::message) {
            bool ticket_finished = false;
            if (!handle_message(client, runtime, read.payload, &ticket_finished, native_error, client_error,
                                failure))
                return false;
            if (ticket_finished) {
                *authenticated = true;
                return true;
            }
        } else if (read.opcode == WebSocketOpcode::ping) {
            if (!send_frame(client, runtime, WebSocketOpcode::pong, read.payload, native_error))
                return false;
        } else if (read.opcode == WebSocketOpcode::close) {
            const u16 close_code =
                read.payload.count >= 2
                    ? static_cast<u16>((static_cast<u16>(read.payload[0]) << 8) | read.payload[1])
                    : 0;
            *failure = RemoteAuthHandshakeResult{0,
                                                 close_code == 4003 ? RemoteAuthHandshakeError::session_timeout
                                                                    : RemoteAuthHandshakeError::unexpected_packet,
                                                 RemoteAuthError::none,
                                                 RemoteAuthCryptoError::none,
                                                 Base64Error::none,
                                                 RemoteAuthHandshakeStage::none};
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
    *failure = remote_auth_handshake_tick(&runtime->handshake, clock_monotonic_milliseconds(),
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

// Closes the stream and waits until it and a posted cancel signal drain.
void release_transport(RemoteAuthClientRuntime* runtime) noexcept {
    tls_stream_close(&runtime->stream);
    while (!tls_stream_drained(&runtime->stream) || read_acquire(&runtime->cancel_signal.pending) != 0) {
        const NetworkPollResult polled = network_loop_poll(&runtime->loop, INFINITE);
        if (polled.state == NetworkPollState::completion)
            tls_stream_handle_completion(&runtime->stream, &runtime->loop, polled.completion);
    }
    byte_queue_clear(&runtime->websocket_input, true);
}

bool keep_fingerprint(void* context, const RestClientResponse& response, ConstSlice<u8> body) noexcept {
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(context);
    char fingerprint[kRestUserFingerprintMaximumBytes];
    // The socket's input storage is idle until it connects.
    const Slice<u32> scratch{reinterpret_cast<u32*>(runtime->encrypted_storage), kEncryptedInputBytes / 4};
    const u32 bytes = response.succeeded() ? user_auth_decode_fingerprint(
                                                 {reinterpret_cast<const char*>(body.data), body.count}, scratch,
                                                 {fingerprint, sizeof(fingerprint)})
                                           : 0;
    secure_clear_bytes(runtime->encrypted_storage, kEncryptedInputBytes);
    signed_out_set_fingerprint({fingerprint, bytes});
    secure_clear_bytes(fingerprint, sizeof(fingerprint));
    return true;
}

bool discard_response(void*, const RestClientResponse&, ConstSlice<u8>) noexcept {
    return true;
}

// What the desktop login page requests as it loads, before its QR socket:
// the experiments assignment, which mints the fingerprint every later
// signed-out request carries, then the location metadata. Once per
// signed-out period, whatever the answers (a QR refresh is not a new page).
// Borrows the ticket exchange's transport and stops it again; a failure only
// leaves the login without a fingerprint, as a page whose request failed.
void load_login_page(RemoteAuthClient* client, RemoteAuthClientRuntime* runtime) noexcept {
    RestClient* transport = &runtime->ticket_exchange.transport;
    if (stop_requested(client) || !signed_out_claim_page_load() ||
        !rest_client_start_user_auth(transport, kUserAuthMaximumResponseBytes).ok())
        return;
    constexpr u32 kPageRequestTimeoutMs = 10'000;
    const UserAuthOperation requests[]{UserAuthOperation::experiments, UserAuthOperation::location_metadata};
    for (u32 i = 0; i < 2; ++i) {
        const u64 request_id = i + 1;
        if (!rest_client_submit_user_auth(
                 transport, {nullptr, nullptr, nullptr, nullptr, request_id, kPageRequestTimeoutMs, requests[i]})
                 .ok())
            break;
        HANDLE waits[]{client->cancel_event, rest_client_notification_handle(transport)};
        if (wait_any(waits, 2, kPageRequestTimeoutMs) != WAIT_OBJECT_0 + 1) {
            rest_client_cancel(transport, request_id);
            break;
        }
        RestClientResponse response{};
        if (!rest_client_consume_user_auth_response(transport, &response, i ? discard_response : keep_fingerprint,
                                                    runtime)
                 .ok() ||
            !response.succeeded())
            break;
    }
    RestClientResponse receipt{};
    rest_client_stop_user_auth(transport, 5'000, &receipt);
}

u32 remote_auth_thread(void* parameter) noexcept {
    auto* client = static_cast<RemoteAuthClient*>(parameter);
    auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
    const NetworkResult loop = network_loop_initialize(&runtime->loop);
    if (!loop.ok()) {
        publish_failure(client, runtime, RemoteAuthClientError::network,
                        static_cast<u32>(loop.native_error));
        event_set(client->ready_event);
        return 1;
    }
    InterlockedExchange(&runtime->loop_ready, 1);
    publish_phase(client, RemoteAuthClientPhase::resolving);
    event_set(client->ready_event);

    u32 native_error = 0;
    RemoteAuthClientError error = RemoteAuthClientError::none;
    RemoteAuthHandshakeResult handshake_failure{};
    bool authenticated = false;
    load_login_page(client, runtime);
    // RSA-2048 generation costs tens of milliseconds; doing it here keeps it
    // off the UI thread, which would otherwise delay the first login frame.
    const RemoteAuthHandshakeResult keyed = remote_auth_handshake_initialize(&runtime->handshake);
    if (!keyed.ok()) {
        native_error = static_cast<u32>(keyed.crypto_error);
        error = RemoteAuthClientError::memory;
    }
    bool connected = keyed.ok() && connect_stream(client, runtime, &native_error, &error);
    if (connected && !open_websocket(client, runtime, &native_error)) {
        connected = false;
        error =
            stop_requested(client) ? RemoteAuthClientError::cancelled : RemoteAuthClientError::websocket;
    }
    if (connected) {
        publish_phase(client, RemoteAuthClientPhase::awaiting_qr);
        runtime->phase_deadline_ms = deadline_after(clock_monotonic_milliseconds(), kPhaseTimeoutMs);
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
        const u64 handshake_wake = remote_auth_handshake_next_wake(runtime->handshake);
        const u64 wake = handshake_wake != 0 ? handshake_wake : runtime->phase_deadline_ms;
        InterlockedExchange64(&client->next_wake_at_ms, static_cast<LONG64>(wake));
        u32 pump_error = 0;
        const PumpState pumped = pump(client, runtime, wake, &pump_error);
        if (pumped == PumpState::stopped) {
            break;
        }
        if (pumped == PumpState::failed) {
            connected = false;
            native_error = pump_error;
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
    pages_release(runtime);
    return true;
}

bool initialize_runtime(RemoteAuthClientRuntime* runtime, u32* native_error) noexcept {
    if (!byte_queue_initialize(&runtime->websocket_input,
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
                   const Slice<char>& output, u32* output_bytes) noexcept {
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
    auto* runtime = commit_pages<RemoteAuthClientRuntime>();
    if (!runtime)
        return start_failure(client, RemoteAuthClientError::memory, thread_last_error());
    client->runtime = runtime;
    u32 native_error = 0;
    if (!initialize_runtime(runtime, &native_error)) {
        release_runtime(runtime);
        client->runtime = nullptr;
        return start_failure(client, RemoteAuthClientError::memory, native_error);
    }
    client->ready_event = event_create(true, false);
    client->notification_event = event_create(true, false);
    client->cancel_event = event_create(true, false);
    client->captcha_event = event_create(true, false);
    if (!client->ready_event || !client->notification_event || !client->cancel_event ||
        !client->captcha_event) {
        native_error = thread_last_error();
        if (client->captcha_event)
            waitable_close(client->captcha_event);
        if (client->cancel_event)
            waitable_close(client->cancel_event);
        if (client->notification_event)
            waitable_close(client->notification_event);
        if (client->ready_event)
            waitable_close(client->ready_event);
        release_runtime(runtime);
        reset_value<RemoteAuthClient>(client);
        return start_failure(client, RemoteAuthClientError::event, native_error);
    }
    constexpr usize kStackReserve = 256 * 1024;
    client->thread = thread_start(remote_auth_thread, client, kStackReserve, &native_error);
    if (!client->thread) {
        waitable_close(client->captcha_event);
        waitable_close(client->cancel_event);
        waitable_close(client->notification_event);
        waitable_close(client->ready_event);
        release_runtime(runtime);
        reset_value<RemoteAuthClient>(client);
        return start_failure(client, RemoteAuthClientError::thread, native_error);
    }
    const DWORD ready = wait_one(client->ready_event, 5'000);
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
    event_set(client->cancel_event);
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
    if (!thread_wait(client->thread, 0)) {
        publish_phase(client, RemoteAuthClientPhase::stopping);
        InterlockedExchange(&client->stop_requested, 1);
        event_set(client->cancel_event);
        auto* runtime = static_cast<RemoteAuthClientRuntime*>(client->runtime);
        if (read_acquire(&runtime->loop_ready) != 0) {
            network_signal_post(&runtime->loop, &runtime->cancel_signal, kCancelSignalValue);
        }
    }
    ReleaseSRWLockExclusive(client->snapshot_lock.native());
    if (!thread_wait(client->thread, timeout_ms)) {
        RemoteAuthClientResult value = client_result(client);
        value.error = RemoteAuthClientError::stop;
        value.native_error = WAIT_TIMEOUT;
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
    thread_close(client->thread);
    waitable_close(client->captcha_event);
    waitable_close(client->cancel_event);
    waitable_close(client->notification_event);
    waitable_close(client->ready_event);
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
        event_reset(client->notification_event);
        if (spsc_queue_size(runtime->events) != 0) {
            event_set(client->notification_event);
        }
    }
    return true;
}

bool remote_auth_client_copy_qr_url(const RemoteAuthClient& client, Slice<char> output,
                                    u32* output_bytes) noexcept {
    return copy_snapshot(client, client.qr_url, client.qr_url_bytes, output, output_bytes);
}

bool remote_auth_client_copy_user(const RemoteAuthClient& client, const Slice<char>& output,
                                  u32* output_bytes) noexcept {
    return copy_snapshot(client, client.user, client.user_bytes, output, output_bytes);
}

SecureBufferResult remote_auth_client_take_credential(RemoteAuthClient* client,
                                                      SecureBuffer* output) noexcept {
    auto* runtime =
        client && client->runtime ? static_cast<RemoteAuthClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || !output || !output->data || !output->locked || output->capacity == 0 ||
        load_phase(client) != RemoteAuthClientPhase::authenticated ||
        !thread_wait(client->thread, 0)) {
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
        return thread_waitable(client->thread);
    return client ? client->notification_event : nullptr;
}

RemoteAuthClientResult remote_auth_client_status(const RemoteAuthClient* client) noexcept {
    return client_result(client);
}

} // namespace maicord
