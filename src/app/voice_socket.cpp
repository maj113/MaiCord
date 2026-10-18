#include "maicord/app/voice_socket.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/platform/random.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u32 kEncrypted = 64 * 1024, kHandshake = 256 * 1024, kRecord = 64 * 1024;
constexpr u32 kScratch = 256;
bool active(const VoiceSocket& socket) noexcept {
    return socket.phase == VoiceSocketPhase::connecting || socket.phase == VoiceSocketPhase::opening ||
           socket.phase == VoiceSocketPhase::open;
}
void finish(VoiceSocket* socket) noexcept {
    if (socket->phase != VoiceSocketPhase::closing || !tls_stream_drained(&socket->tls))
        return;
    auto heartbeat = socket->session.heartbeat;
    voice_heartbeat_disconnect(&heartbeat);
    voice_session_clear(&socket->session);
    if (socket->session.scratch.data)
        secure_clear_bytes(socket->session.scratch.data, socket->session.scratch.count * sizeof(u32));
    stable_storage_release(&socket->json_storage);
    socket->session = {};
    socket->session.heartbeat = heartbeat;
    if (socket->storage.data) {
        // TLS already erased its regions after drain, and session_clear erased
        // packet input/fragments. Clear the remaining region once, not the
        // entire allocation a second time.
        secure_clear_bytes(socket->output.data, socket->output.count);
        VirtualFree(socket->storage.data, 0, MEM_RELEASE);
        socket->storage = {};
    }
    socket->output = {};
    socket->sending = VoiceSocketOutput::none;
    socket->output_bytes = 0;
    socket->handler = nullptr;
    socket->context = nullptr;
    secure_clear_bytes(socket->control, sizeof(socket->control));
    socket->control_bytes = 0;
    socket->pending_close = socket->pending_pong = false;
    socket->phase = VoiceSocketPhase::closed;
}
Slice<u32> grow_scratch(void* context, u32 required) noexcept {
    auto* socket = static_cast<VoiceSocket*>(context);
    auto& storage = socket->json_storage;
    if (!stable_storage_grow(&storage, required)) {
        socket->error = VoiceSocketError::storage;
        return {};
    }
    return {stable_storage_data<u32>(storage), storage.capacity};
}
void abort(VoiceSocket* socket, VoiceSocketError error) noexcept {
    if (socket->error == VoiceSocketError::none)
        socket->error = error;
    socket->phase = VoiceSocketPhase::closing;
    socket->connect_deadline_ms = socket->close_deadline_ms = 0;
    voice_heartbeat_disconnect(&socket->session.heartbeat);
    tls_stream_close(&socket->tls);
    finish(socket);
}
// Runs inside the TLS read callback: defer shutdown/storage release until the
// TLS API returns. Deliver all events before the next push invalidates views.
bool parse(VoiceSocket* socket) noexcept {
    while (!socket->stop_requested && !socket->pending_close) {
        const auto event = voice_session_next(&socket->session, socket->now_ms);
        if (!event.ok()) {
            if (socket->error == VoiceSocketError::none)
                socket->error = VoiceSocketError::protocol;
            socket->stop_requested = true;
        } else if (event.kind == VoiceSessionEventKind::need_input) {
            return true;
        } else if (event.kind == VoiceSessionEventKind::opened) {
            socket->phase = VoiceSocketPhase::open;
        } else if (event.kind == VoiceSessionEventKind::ping ||
                   event.kind == VoiceSessionEventKind::closed) {
            copy_bytes(socket->control, event.control.data, event.control.count);
            socket->control_bytes = static_cast<u8>(event.control.count);
            if (event.kind == VoiceSessionEventKind::closed) {
                socket->close_code = event.close_code;
                socket->pending_close = true;
                socket->pending_pong = false;
                socket->close_deadline_ms = deadline_after(socket->now_ms, 2000);
            } else {
                // RFC 6455 permits responding to only the most recent ping
                // when several arrive before a pending pong can be sent.
                socket->pending_pong = true;
            }
        }
        if (socket->handler && !socket->handler(socket->context, event))
            socket->stop_requested = true;
    }
    return !socket->stop_requested;
}
TlsStreamReadAction plaintext(void* context, ConstSlice<u8> bytes, bool closed) noexcept {
    auto* socket = static_cast<VoiceSocket*>(context);
    if (closed) {
        socket->peer_closed = true;
        socket->stop_requested = true;
        if (!socket->pending_close)
            socket->error = VoiceSocketError::transport;
        return TlsStreamReadAction::complete;
    }
    while (bytes.count && !socket->pending_close && !socket->stop_requested) {
        const u32 count = minimum(bytes.count, byte_queue_available(socket->session.input));
        if (!count || !voice_session_push(&socket->session, {bytes.data, count})) {
            socket->error = VoiceSocketError::protocol;
            socket->stop_requested = true;
            break;
        }
        bytes.data += count;
        bytes.count -= count;
        if (!parse(socket))
            break;
    }
    // Keep the TLS reader alive until the enclosing API returns. It may post
    // another receive; the outer driver cancels and drains it before freeing.
    return TlsStreamReadAction::continue_read;
}
bool send_output(VoiceSocket* socket, VoiceSocketOutput purpose, u32 bytes) noexcept {
    const ConstSlice<u8> span{socket->output.data, bytes};
    socket->sending = purpose;
    socket->output_bytes = bytes;
    socket->tls_event = tls_stream_start_write(&socket->tls, {&span, 1});
    if (!socket->tls_event.ok()) {
        abort(socket, VoiceSocketError::transport);
        return false;
    }
    return true;
}
bool frame(VoiceSocket* socket, WebSocketOpcode opcode, ConstSlice<u8> payload,
           VoiceSocketOutput purpose) noexcept {
    u32 mask = 0;
    if (!secure_random_u32(&mask).ok()) {
        abort(socket, VoiceSocketError::random);
        return false;
    }
    const auto encoded = websocket_write_client_frame(opcode, true, payload, mask, socket->output);
    if (!encoded.ok()) {
        abort(socket, VoiceSocketError::protocol);
        return false;
    }
    return send_output(socket, purpose, encoded.bytes);
}
void opening(VoiceSocket* socket) noexcept {
    u8 nonce[kWebSocketNonceBytes], key[kWebSocketClientKeyBytes];
    if (!secure_random_fill({nonce, sizeof(nonce)}).ok() ||
        !websocket_encode_client_key({nonce, sizeof(nonce)}, {key, sizeof(key)}).ok()) {
        abort(socket, VoiceSocketError::random);
        return;
    }
    if (!voice_session_initialize(&socket->session,
                                  {socket->session.input.data, socket->session.input.capacity},
                                  socket->session.fragments, socket->session.scratch, {key, sizeof(key)},
                                  socket->session.maximum_frame_bytes,
                                  socket->session.maximum_message_bytes, socket->now_ms, socket->resume)) {
        abort(socket, VoiceSocketError::protocol);
        return;
    }
    const auto& endpoint = socket->endpoint;
    const auto request = websocket_write_opening_request(
        {reinterpret_cast<const u8*>(endpoint.target), endpoint.target_bytes},
        {reinterpret_cast<const u8*>(endpoint.authority), endpoint.authority_bytes}, {key, sizeof(key)},
        socket->output);
    if (!request.ok()) {
        abort(socket, VoiceSocketError::protocol);
        return;
    }
    socket->connect_deadline_ms = 0;
    socket->phase = VoiceSocketPhase::opening;
    socket->tls_event = tls_stream_start_duplex(&socket->tls, plaintext, socket);
    if (!socket->tls_event.ok()) {
        abort(socket, VoiceSocketError::transport);
        return;
    }
    send_output(socket, VoiceSocketOutput::opening, request.bytes);
}
} // namespace

bool voice_socket_begin(VoiceSocket* socket, NetworkLoop* loop, const GatewayEndpoint& endpoint,
                        u64 setup_generation, u32 maximum_message_bytes, VoiceSocketHandler handler,
                        void* context, u64 now_ms, bool resume) noexcept {
    if (!socket || !loop || !loop->completion_port || !setup_generation || !handler ||
        (socket->phase != VoiceSocketPhase::empty && socket->phase != VoiceSocketPhase::closed) ||
        socket->storage.data || socket->json_storage.arena.base || !gateway_endpoint_validate(endpoint) ||
        endpoint.policy != GatewayEndpointPolicy::voice_v9_json || maximum_message_bytes < 4096)
        return false;
    // The input holds a maximum frame including its header. Larger TLS chunks
    // are incrementally consumed by plaintext(), not copied into another queue.
    const u64 packet_bytes = static_cast<u64>(maximum_message_bytes) + 14;
    const u64 total = kEncrypted + kHandshake + kRecord + packet_bytes * 2 + maximum_message_bytes;
    if (total > ~u32{0})
        return false;
    socket->error = VoiceSocketError::none;
    socket->tls_event = {};
    socket->stop_requested = socket->pending_pong = socket->pending_close = false;
    socket->peer_closed = false;
    socket->resume = resume;
    socket->setup_generation = setup_generation;
    socket->endpoint = endpoint;
    socket->handler = handler;
    socket->context = context;
    socket->now_ms = now_ms;
    socket->close_code = 0;
    socket->sending = VoiceSocketOutput::none;
    socket->storage.data = static_cast<u8*>(
        VirtualAlloc(nullptr, static_cast<usize>(total), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!socket->storage.data) {
        socket->error = VoiceSocketError::storage;
        socket->phase = VoiceSocketPhase::closed;
        socket->handler = nullptr;
        socket->context = nullptr;
        return false;
    }
    socket->storage.count = static_cast<u32>(total);
    u8* data = socket->storage.data;
    const TlsStreamBuffers tls_buffers{
        {data, kEncrypted}, {data + kEncrypted, kHandshake}, {data + kEncrypted + kHandshake, kRecord}};
    data += kEncrypted + kHandshake + kRecord;
    // At most one structural index per input byte. Reserve that address range,
    // but commit pages only as an actual JSON envelope exceeds the small index.
    if (!stable_storage_initialize(&socket->json_storage, maximum_message_bytes, kScratch, sizeof(u32),
                                   alignof(u32))) {
        abort(socket, VoiceSocketError::storage);
        return false;
    }
    socket->session.scratch = {stable_storage_data<u32>(socket->json_storage), kScratch};
    socket->session.grow_scratch = grow_scratch;
    socket->session.scratch_context = socket;
    byte_queue_initialize(&socket->session.input, {data, static_cast<u32>(packet_bytes)});
    data += packet_bytes;
    socket->session.fragments = {data, maximum_message_bytes};
    data += maximum_message_bytes;
    socket->output = {data, static_cast<u32>(packet_bytes)};
    socket->session.maximum_frame_bytes = socket->session.maximum_message_bytes = maximum_message_bytes;
    socket->phase = VoiceSocketPhase::connecting;
    socket->connect_deadline_ms = deadline_after(now_ms, 20'000);
    if (!tls_stream_initialize_endpoint(&socket->tls, socket->endpoint.host_wide, endpoint.port,
                                        tls_buffers)) {
        abort(socket, VoiceSocketError::transport);
        return false;
    }
    socket->tls_event = tls_stream_begin_connect(&socket->tls, loop);
    if (!socket->tls_event.ok()) {
        abort(socket, VoiceSocketError::transport);
        return false;
    }
    return true;
}

void voice_socket_completion(VoiceSocket* socket, NetworkLoop* loop, const NetworkCompletion& completion,
                             u64 now_ms) noexcept {
    if (!socket || socket->phase == VoiceSocketPhase::empty || socket->phase == VoiceSocketPhase::closed)
        return;
    socket->now_ms = now_ms;
    socket->tls_event = tls_stream_handle_completion(&socket->tls, loop, completion);
    if (socket->phase == VoiceSocketPhase::closing) {
        finish(socket);
        return;
    }
    if (!socket->tls_event.ok()) {
        abort(socket, VoiceSocketError::transport);
        return;
    }
    if (socket->tls_event.kind == TlsStreamEventKind::ready) {
        opening(socket);
    } else if (socket->tls_event.kind == TlsStreamEventKind::write_complete) {
        const auto purpose = socket->sending;
        secure_clear_bytes(socket->output.data, socket->output_bytes);
        socket->output_bytes = 0;
        socket->sending = VoiceSocketOutput::none;
        if (purpose == VoiceSocketOutput::close) {
            abort(socket, VoiceSocketError::none);
            return;
        }
    }
    voice_socket_tick(socket, now_ms);
}

void voice_socket_tick(VoiceSocket* socket, u64 now_ms) noexcept {
    if (!socket)
        return;
    socket->now_ms = now_ms;
    if (!active(*socket)) {
        finish(socket);
        return;
    }
    if (socket->stop_requested) {
        abort(socket, socket->error);
        return;
    }
    if (deadline_due(now_ms, socket->connect_deadline_ms) ||
        deadline_due(now_ms, socket->close_deadline_ms)) {
        abort(socket, VoiceSocketError::timeout);
        return;
    }
    const auto heartbeat = voice_heartbeat_poll(&socket->session.heartbeat, now_ms);
    if (heartbeat.action == VoiceHeartbeatAction::reconnect ||
        heartbeat.action == VoiceHeartbeatAction::stop) {
        abort(socket, VoiceSocketError::heartbeat);
        return;
    }
    if (socket->phase != VoiceSocketPhase::open || socket->sending != VoiceSocketOutput::none)
        return;
    if (socket->pending_close || socket->pending_pong) {
        const bool close = socket->pending_close;
        if (frame(socket, close ? WebSocketOpcode::close : WebSocketOpcode::pong,
                  {socket->control, socket->control_bytes},
                  close ? VoiceSocketOutput::close : VoiceSocketOutput::pong)) {
            socket->pending_pong = false;
            secure_clear_bytes(socket->control, sizeof(socket->control));
            socket->control_bytes = 0;
        }
        return;
    }
    if (heartbeat.action == VoiceHeartbeatAction::send) {
        u8 payload[128];
        const auto encoded = voice_gateway_write_heartbeat(heartbeat.nonce, heartbeat.sequence_ack,
                                                           {payload, sizeof(payload)});
        if (!encoded.ok()) {
            abort(socket, VoiceSocketError::protocol);
            return;
        }
        if (frame(socket, WebSocketOpcode::text, {payload, encoded.bytes}, VoiceSocketOutput::heartbeat) &&
            !voice_heartbeat_submitted(&socket->session.heartbeat, heartbeat, now_ms))
            abort(socket, VoiceSocketError::heartbeat);
    }
}

u64 voice_socket_deadline(const VoiceSocket& socket) noexcept {
    if (!active(socket))
        return 0;
    return earliest_deadline(
        socket.connect_deadline_ms,
        earliest_deadline(socket.close_deadline_ms, voice_heartbeat_deadline(socket.session.heartbeat)));
}
VoiceSocketSend voice_socket_send(VoiceSocket* socket, WebSocketOpcode opcode, ConstSlice<u8> payload,
                                  u64 now_ms) noexcept {
    if (!socket || socket->phase != VoiceSocketPhase::open || socket->pending_close ||
        socket->stop_requested || !payload.data || !payload.count ||
        (opcode != WebSocketOpcode::text && opcode != WebSocketOpcode::binary) ||
        payload.count > socket->session.maximum_message_bytes)
        return VoiceSocketSend::invalid;
    // Encoding writes its header before the payload. Reject aliases, including
    // a caller trying to reuse the currently submitted output buffer.
    const usize source = reinterpret_cast<usize>(payload.data);
    const usize destination = reinterpret_cast<usize>(socket->output.data);
    if (source <= destination ? destination - source < payload.count
                              : source - destination < socket->output.count)
        return VoiceSocketSend::invalid;
    voice_socket_tick(socket, now_ms);
    if (!active(*socket))
        return VoiceSocketSend::failed;
    if (socket->sending != VoiceSocketOutput::none)
        return VoiceSocketSend::busy;
    return frame(socket, opcode, payload, VoiceSocketOutput::application) ? VoiceSocketSend::accepted
                                                                          : VoiceSocketSend::failed;
}
bool voice_socket_close(VoiceSocket* socket, u16 code, u64 now_ms) noexcept {
    if (!socket || socket->phase != VoiceSocketPhase::open || socket->pending_close || socket->stop_requested)
        return false;
    socket->control[0] = static_cast<u8>(code >> 8);
    socket->control[1] = static_cast<u8>(code);
    socket->control_bytes = 2;
    socket->pending_close = true;
    socket->pending_pong = false;
    socket->close_deadline_ms = deadline_after(now_ms, 2000);
    voice_socket_tick(socket, now_ms);
    return true;
}
void voice_socket_stop(VoiceSocket* socket) noexcept {
    if (!socket)
        return;
    voice_heartbeat_stop(&socket->session.heartbeat);
    if (socket->phase == VoiceSocketPhase::empty || socket->phase == VoiceSocketPhase::closed)
        return;
    abort(socket, VoiceSocketError::none);
}
} // namespace maicord
