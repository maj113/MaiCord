#include "maicord/platform/tls_stream.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {

enum class SendPurpose : u8 {
    handshake,
    application,
};

bool valid_server_name(const wchar_t* value) noexcept {
    if (!value) {
        return false;
    }
    u32 length = 0;
    bool ipv6 = false;
    while (length <= 253 && value[length]) {
        ipv6 |= value[length++] == L':';
    }
    if (!length || length > 253) {
        return false;
    }
    if (ipv6) {
        IN6_ADDR address{};
        return InetPtonW(AF_INET6, value, &address) == 1;
    }
    u32 bytes = 0;
    u32 label_bytes = 0;
    while (value[bytes] != 0) {
        const wchar_t current = value[bytes];
        const bool alpha = (current >= L'a' && current <= L'z') || (current >= L'A' && current <= L'Z');
        const bool digit = current >= L'0' && current <= L'9';
        if (current == L'.') {
            if (label_bytes == 0 || value[bytes - 1] == L'-') {
                return false;
            }
            label_bytes = 0;
        } else if (alpha || digit || current == L'-') {
            if ((current == L'-' && label_bytes == 0) || ++label_bytes > 63) {
                return false;
            }
        } else {
            return false;
        }
        if (++bytes > 253) {
            return false;
        }
    }
    return bytes != 0 && label_bytes != 0 && value[bytes - 1] != L'-';
}

bool valid_slice(Slice<u8> value) noexcept {
    return value.data && value.count != 0;
}

bool ranges_overlap(Slice<u8> left, Slice<u8> right) noexcept {
    const usize left_begin = reinterpret_cast<usize>(left.data);
    const usize right_begin = reinterpret_cast<usize>(right.data);
    const usize left_end = left_begin + left.count;
    const usize right_end = right_begin + right.count;
    if (left_end < left_begin || right_end < right_begin) {
        return true;
    }
    return left_begin < right_end && right_begin < left_end;
}

TlsStreamEvent event(TlsStreamEventKind kind) noexcept {
    return TlsStreamEvent{0, kind, TlsStreamError::none};
}

TlsStreamEvent invalid_event() noexcept {
    return TlsStreamEvent{ERROR_INVALID_PARAMETER, TlsStreamEventKind::failed,
                          TlsStreamError::invalid_input};
}

TlsStreamEvent failure(TlsStream* stream, TlsStreamError error, u32 native_error = 0,
                       SECURITY_STATUS tls_status = SEC_E_OK) noexcept {
    stream->state = TlsStreamState::failed;
    stream->error = error;
    stream->native_error = native_error;
    stream->tls_status = tls_status;
    return TlsStreamEvent{native_error, TlsStreamEventKind::failed, error};
}

bool release_resolver(TlsStream* stream) noexcept {
    if (!stream->resolver_started) {
        return true;
    }
    if (stream->resolver.pending || !network_resolver_release(&stream->resolver)) {
        return false;
    }
    stream->resolver_started = false;
    stream->next_address = nullptr;
    return true;
}

void release_tls(TlsStream* stream) noexcept {
    if (stream->tls_initialized) {
        tls_client_release(&stream->tls);
        stream->tls_initialized = false;
    }
}

bool start_receive(TlsStream* stream) noexcept {
    const Slice<u8> writable = byte_queue_writable(&stream->encrypted_input);
    if (!writable.data || writable.count == 0 ||
        !network_connection_receive(&stream->connection, writable).ok()) {
        return false;
    }
    ++stream->outstanding_operations;
    return true;
}

bool start_send(TlsStream* stream, ConstSlice<u8> bytes, SendPurpose purpose) noexcept {
    if (!bytes.data || bytes.count == 0 || stream->send_data ||
        !network_connection_send(&stream->connection, bytes).ok()) {
        return false;
    }
    stream->send_data = bytes.data;
    stream->send_bytes = bytes.count;
    stream->send_offset = 0;
    stream->handshake_send = purpose == SendPurpose::handshake;
    ++stream->outstanding_operations;
    return true;
}

bool continue_send(TlsStream* stream) noexcept {
    const ConstSlice<u8> remaining{stream->send_data + stream->send_offset,
                                   stream->send_bytes - stream->send_offset};
    if (!network_connection_send(&stream->connection, remaining).ok()) {
        return false;
    }
    ++stream->outstanding_operations;
    return true;
}

bool start_connect_candidate(TlsStream* stream, NetworkLoop* loop) noexcept {
    while (stream->next_address) {
        ADDRINFOEXW* candidate = stream->next_address;
        stream->next_address = candidate->ai_next;
        if (!candidate->ai_addr || candidate->ai_addrlen == 0 ||
            candidate->ai_addrlen > sizeof(sockaddr_storage) ||
            (candidate->ai_family != AF_INET && candidate->ai_family != AF_INET6)) {
            continue;
        }
        const NetworkResult connected = network_connection_connect(
            loop, &stream->connection, candidate->ai_addr, static_cast<i32>(candidate->ai_addrlen));
        if (connected.ok()) {
            ++stream->outstanding_operations;
            return true;
        }
    }
    return false;
}

bool record_fits(const TlsStream& stream) noexcept {
    const u64 required = static_cast<u64>(stream.tls.stream_sizes.cbHeader) +
                         stream.tls.stream_sizes.cbMaximumMessage + stream.tls.stream_sizes.cbTrailer;
    return required <= stream.buffers.record_output.count;
}

TlsStreamEvent ready(TlsStream* stream) noexcept {
    if (!record_fits(*stream)) {
        return failure(stream, TlsStreamError::capacity);
    }
    stream->state = TlsStreamState::ready;
    return event(TlsStreamEventKind::ready);
}

TlsStreamEvent drive_handshake(TlsStream* stream) noexcept {
    const ConstSlice<u8> incoming = byte_queue_readable(stream->encrypted_input);
    const TlsHandshakeStep step =
        tls_client_handshake(&stream->tls, incoming, stream->buffers.handshake_output);
    if (!step.ok() || !byte_queue_consume(&stream->encrypted_input, step.consumed)) {
        return failure(stream, TlsStreamError::tls, 0, step.native_status);
    }
    if (step.produced != 0) {
        stream->after_tls_send = step.state;
        if (!start_send(stream, ConstSlice<u8>{stream->buffers.handshake_output.data, step.produced},
                        SendPurpose::handshake)) {
            return failure(stream, TlsStreamError::send, static_cast<u32>(WSAGetLastError()));
        }
        return event(TlsStreamEventKind::waiting);
    }
    if (step.state == TlsHandshakeState::complete) {
        return ready(stream);
    }
    if (step.state != TlsHandshakeState::need_receive || !start_receive(stream)) {
        return failure(stream,
                       step.state == TlsHandshakeState::need_receive ? TlsStreamError::receive
                                                                     : TlsStreamError::tls,
                       static_cast<u32>(WSAGetLastError()), step.native_status);
    }
    return event(TlsStreamEventKind::waiting);
}

bool select_write_bytes(TlsStream* stream, ConstSlice<u8> selected[kTlsStreamMaximumWriteSpans],
                        u32* selected_count, u32* selected_bytes) noexcept {
    *selected_count = 0;
    *selected_bytes = 0;
    u32 span_index = stream->write_span_index;
    u32 span_offset = stream->write_span_offset;
    while (span_index < stream->write_span_count) {
        const ConstSlice<u8> span = stream->write_spans[span_index];
        if (span_offset < span.count) {
            const u32 remaining = span.count - span_offset;
            const u32 record_remaining =
                static_cast<u32>(stream->tls.stream_sizes.cbMaximumMessage) - *selected_bytes;
            const u32 count = minimum(remaining, record_remaining);
            selected[*selected_count] = ConstSlice<u8>{span.data + span_offset, count};
            ++*selected_count;
            *selected_bytes += count;
            if (*selected_bytes == static_cast<u32>(stream->tls.stream_sizes.cbMaximumMessage) ||
                *selected_count == kTlsStreamMaximumWriteSpans) {
                return true;
            }
            ++span_index;
            span_offset = 0;
            continue;
        }
        ++span_index;
        span_offset = 0;
    }
    return *selected_count != 0;
}

TlsStreamEvent start_next_record(TlsStream* stream) noexcept {
    ConstSlice<u8> plaintext[kTlsStreamMaximumWriteSpans]{};
    u32 plaintext_spans = 0;
    u32 plaintext_bytes = 0;
    if (!select_write_bytes(stream, plaintext, &plaintext_spans, &plaintext_bytes)) {
        stream->state = stream->duplex ? TlsStreamState::duplex : TlsStreamState::ready;
        stream->commit_handler = nullptr;
        stream->callback_context = nullptr;
        return event(TlsStreamEventKind::write_complete);
    }
    u32 produced = 0;
    const TlsResult encrypted =
        tls_client_encrypt_spans(&stream->tls, ConstSlice<ConstSlice<u8>>{plaintext, plaintext_spans},
                                 stream->buffers.record_output, &produced);
    if (!encrypted.ok()) {
        return failure(stream, TlsStreamError::tls, 0, encrypted.native_status);
    }
    stream->current_plaintext_bytes = plaintext_bytes;
    if (!start_send(stream, ConstSlice<u8>{stream->buffers.record_output.data, produced},
                    SendPurpose::application)) {
        return failure(stream, TlsStreamError::send, static_cast<u32>(WSAGetLastError()));
    }
    return event(TlsStreamEventKind::waiting);
}

bool commit_current_record(TlsStream* stream) noexcept {
    u32 remaining = stream->current_plaintext_bytes;
    while (remaining != 0) {
        if (stream->write_span_index >= stream->write_span_count) {
            return false;
        }
        const ConstSlice<u8> span = stream->write_spans[stream->write_span_index];
        if (stream->write_span_offset >= span.count) {
            ++stream->write_span_index;
            stream->write_span_offset = 0;
            continue;
        }
        const u32 committed_bytes = minimum(remaining, span.count - stream->write_span_offset);
        const ConstSlice<u8> committed{span.data + stream->write_span_offset, committed_bytes};
        if (stream->commit_handler) {
            stream->commit_handler(stream->callback_context, committed);
        }
        stream->write_span_offset += committed_bytes;
        remaining -= committed_bytes;
        if (stream->write_span_offset == span.count) {
            ++stream->write_span_index;
            stream->write_span_offset = 0;
        }
    }
    stream->current_plaintext_bytes = 0;
    return true;
}

TlsStreamEvent complete_read(TlsStream* stream, bool peer_closed) noexcept {
    stream->read_handler = nullptr;
    stream->read_context = nullptr;
    stream->duplex = false;
    stream->paused_peer_closed = false;
    stream->read_completed = false;
    if (peer_closed) {
        tls_stream_close(stream);
    } else {
        stream->state = stream->send_data ? TlsStreamState::writing : TlsStreamState::ready;
    }
    return event(TlsStreamEventKind::read_complete);
}

TlsStreamEvent finish_completed_read(TlsStream* stream, bool peer_closed) noexcept {
    stream->read_completed = true;
    for (;;) {
        const auto readable = byte_queue_readable(stream->encrypted_input);
        if (!readable.count) return complete_read(stream, peer_closed);
        if (peer_closed) return failure(stream, TlsStreamError::unexpected_data);
        const auto decrypted = tls_client_decrypt(&stream->tls,
            {const_cast<u8*>(readable.data), readable.count});
        if (!decrypted.ok() || decrypted.state == TlsDecryptState::renegotiate)
            return failure(stream, TlsStreamError::tls, 0, decrypted.native_status);
        if (decrypted.state == TlsDecryptState::need_receive) {
            // A coalesced prefix may be a fragmented authenticated shutdown.
            // Retain the completed owner state and receive only its remainder.
            if (!start_receive(stream))
                return failure(stream, TlsStreamError::receive, static_cast<u32>(WSAGetLastError()));
            return event(TlsStreamEventKind::waiting);
        }
        if (decrypted.plaintext.count) return failure(stream, TlsStreamError::unexpected_data);
        if (!decrypted.consumed || !byte_queue_consume(&stream->encrypted_input, decrypted.consumed))
            return failure(stream, TlsStreamError::tls, 0, decrypted.native_status);
        peer_closed = decrypted.state == TlsDecryptState::peer_closed;
    }
}

TlsStreamEvent drive_read(TlsStream* stream) noexcept {
    if (stream->read_completed) return finish_completed_read(stream, false);
    for (;;) {
        const ConstSlice<u8> readable = byte_queue_readable(stream->encrypted_input);
        if (readable.count == 0) {
            if (!start_receive(stream)) {
                return failure(stream, TlsStreamError::receive, static_cast<u32>(WSAGetLastError()));
            }
            return event(TlsStreamEventKind::waiting);
        }
        const TlsDecryptResult decrypted =
            tls_client_decrypt(&stream->tls, Slice<u8>{const_cast<u8*>(readable.data), readable.count});
        if (!decrypted.ok() || decrypted.state == TlsDecryptState::renegotiate) {
            return failure(stream, TlsStreamError::tls, 0, decrypted.native_status);
        }
        if (decrypted.state == TlsDecryptState::need_receive) {
            if (!start_receive(stream)) {
                return failure(stream, TlsStreamError::receive, static_cast<u32>(WSAGetLastError()));
            }
            return event(TlsStreamEventKind::waiting);
        }
        if (!decrypted.consumed) return failure(stream, TlsStreamError::tls, 0, decrypted.native_status);
        const bool peer_closed = decrypted.state == TlsDecryptState::peer_closed;
        const TlsStreamReadAction action = stream->read_handler(
            stream->read_context, ConstSlice<u8>{decrypted.plaintext.data, decrypted.plaintext.count},
            peer_closed);
        if (!byte_queue_consume(&stream->encrypted_input, decrypted.consumed)) {
            return failure(stream, TlsStreamError::receive);
        }
        if (action == TlsStreamReadAction::reject) {
            return failure(stream, TlsStreamError::unexpected_data);
        }
        if (action == TlsStreamReadAction::pause) {
            if (stream->duplex) return failure(stream, TlsStreamError::invalid_input);
            stream->paused_peer_closed = peer_closed;
            stream->state = TlsStreamState::read_paused;
            return event(TlsStreamEventKind::read_paused);
        }
        if (action == TlsStreamReadAction::complete) {
            return finish_completed_read(stream, peer_closed);
        }
        if (peer_closed) {
            return failure(stream, TlsStreamError::receive);
        }
    }
}

TlsStreamEvent finish_close(TlsStream* stream) noexcept {
    if (stream->outstanding_operations != 0 || stream->resolver.pending) {
        return event(TlsStreamEventKind::waiting);
    }
    if (!release_resolver(stream)) {
        return failure(stream, TlsStreamError::resolve);
    }
    // A cancelled receive may have written bytes that were never committed to
    // the queue. Erase its full storage, only after the last completion drains.
    if (stream->buffers.encrypted_input.data)
        SecureZeroMemory(stream->buffers.encrypted_input.data, stream->buffers.encrypted_input.count);
    byte_queue_clear(&stream->encrypted_input, false);
    if (stream->buffers.handshake_output.data)
        SecureZeroMemory(stream->buffers.handshake_output.data, stream->buffers.handshake_output.count);
    if (stream->buffers.record_output.data)
        SecureZeroMemory(stream->buffers.record_output.data, stream->buffers.record_output.count);
    stream->state = TlsStreamState::closed;
    return event(TlsStreamEventKind::drained);
}

} // namespace

bool tls_stream_initialize(TlsStream* stream, const wchar_t* server_name,
                           TlsStreamBuffers buffers) noexcept {
    return tls_stream_initialize_endpoint(stream, server_name, 443, buffers);
}

bool tls_stream_initialize_endpoint(TlsStream* stream, const wchar_t* server_name, u16 port,
                                    TlsStreamBuffers buffers) noexcept {
    if (!stream || (stream->state != TlsStreamState::empty && stream->state != TlsStreamState::closed) ||
        stream->outstanding_operations || stream->resolver.pending || stream->tls_initialized ||
        stream->resolver_started || !port || !valid_server_name(server_name) ||
        !valid_slice(buffers.encrypted_input) || !valid_slice(buffers.handshake_output) ||
        !valid_slice(buffers.record_output) ||
        ranges_overlap(buffers.encrypted_input, buffers.handshake_output) ||
        ranges_overlap(buffers.encrypted_input, buffers.record_output) ||
        ranges_overlap(buffers.handshake_output, buffers.record_output)) {
        return false;
    }
    *stream = TlsStream{};
    stream->connection.socket = INVALID_SOCKET;
    stream->server_name = server_name;
    u32 digits = 1;
    for (u32 value = port; value >= 10; value /= 10)
        ++digits;
    for (u32 i = digits, value = port; i; --i, value /= 10)
        stream->service[i - 1] = static_cast<wchar_t>(L'0' + value % 10);
    stream->buffers = buffers;
    return byte_queue_initialize(&stream->encrypted_input, buffers.encrypted_input);
}

TlsStreamEvent tls_stream_begin_connect(TlsStream* stream, NetworkLoop* loop) noexcept {
    if (!stream || !loop || !loop->completion_port ||
        (stream->state != TlsStreamState::empty && stream->state != TlsStreamState::closed) ||
        stream->outstanding_operations != 0 || stream->resolver.pending) {
        return invalid_event();
    }
    stream->resolver = NetworkResolver{};
    stream->connection = NetworkConnection{};
    stream->connection.socket = INVALID_SOCKET;
    stream->tls = TlsClient{};
    stream->error = TlsStreamError::none;
    stream->native_error = 0;
    stream->tls_status = SEC_E_OK;
    stream->next_address = nullptr;
    stream->send_data = nullptr;
    stream->commit_handler = nullptr;
    stream->read_handler = nullptr;
    stream->callback_context = nullptr;
    stream->read_context = nullptr;
    stream->duplex = false;
    stream->send_bytes = 0;
    stream->paused_peer_closed = false;
    stream->read_completed = false;
    stream->send_offset = 0;
    stream->write_span_count = 0;
    stream->write_span_index = 0;
    stream->write_span_offset = 0;
    stream->current_plaintext_bytes = 0;
    stream->outstanding_operations = 0;
    stream->after_tls_send = TlsHandshakeState{};
    stream->resolver_started = false;
    stream->tls_initialized = false;
    stream->handshake_send = false;
    for (u32 index = 0; index < kTlsStreamMaximumWriteSpans; ++index) {
        stream->write_spans[index] = {};
    }
    byte_queue_clear(&stream->encrypted_input, true);
    if (!byte_queue_initialize(&stream->encrypted_input, stream->buffers.encrypted_input)) {
        return failure(stream, TlsStreamError::capacity);
    }
    const TlsResult initialized = tls_client_initialize(&stream->tls, stream->server_name);
    if (!initialized.ok() || stream->tls.maximum_token_bytes > stream->buffers.handshake_output.count) {
        if (initialized.ok()) {
            tls_client_release(&stream->tls);
        }
        return failure(stream, initialized.ok() ? TlsStreamError::capacity : TlsStreamError::tls, 0,
                       initialized.native_status);
    }
    stream->tls_initialized = true;
    const NetworkResult resolving =
        network_resolver_start(loop, &stream->resolver, stream->server_name, stream->service);
    if (!resolving.ok()) {
        release_tls(stream);
        return failure(stream, TlsStreamError::resolve, static_cast<u32>(resolving.native_error));
    }
    stream->resolver_started = true;
    stream->state = TlsStreamState::resolving;
    return event(TlsStreamEventKind::waiting);
}

TlsStreamEvent tls_stream_start_write(TlsStream* stream, ConstSlice<ConstSlice<u8>> spans,
                                      TlsStreamCommitHandler commit_handler,
                                      void* callback_context) noexcept {
    if (!stream || (stream->state != TlsStreamState::ready && stream->state != TlsStreamState::duplex) ||
        !spans.data || spans.count == 0 || spans.count > kTlsStreamMaximumWriteSpans ||
        stream->connection.send_pending || (!stream->duplex && stream->connection.receive_pending) ||
        stream->send_data) {
        return invalid_event();
    }
    u32 nonempty = 0;
    for (u32 index = 0; index < spans.count; ++index) {
        if (!spans[index].data && spans[index].count != 0) {
            return invalid_event();
        }
        nonempty += spans[index].count != 0 ? 1u : 0u;
    }
    if (nonempty == 0) {
        return invalid_event();
    }
    for (u32 index = 0; index < spans.count; ++index) {
        stream->write_spans[index] = spans[index];
    }
    stream->write_span_count = spans.count;
    stream->write_span_index = 0;
    stream->write_span_offset = 0;
    stream->commit_handler = commit_handler;
    stream->callback_context = callback_context;
    stream->state = stream->duplex ? TlsStreamState::duplex : TlsStreamState::writing;
    return start_next_record(stream);
}

TlsStreamEvent tls_stream_start_read(TlsStream* stream, TlsStreamReadHandler read_handler,
                                     void* callback_context) noexcept {
    if (!stream || stream->state != TlsStreamState::ready || !read_handler ||
        stream->connection.send_pending || stream->connection.receive_pending) {
        return invalid_event();
    }
    stream->read_handler = read_handler;
    stream->read_context = callback_context;
    stream->state = TlsStreamState::reading;
    stream->read_completed = false;
    return drive_read(stream);
}

TlsStreamEvent tls_stream_start_duplex(TlsStream* stream, TlsStreamReadHandler read_handler,
                                       void* callback_context) noexcept {
    if (!stream || stream->state != TlsStreamState::ready || !read_handler ||
        stream->connection.send_pending || stream->connection.receive_pending)
        return invalid_event();
    stream->read_handler = read_handler;
    stream->read_context = callback_context;
    stream->duplex = true;
    stream->read_completed = false;
    stream->state = TlsStreamState::duplex;
    return drive_read(stream);
}

TlsStreamEvent tls_stream_resume_read(TlsStream* stream) noexcept {
    if (!stream || stream->state != TlsStreamState::read_paused || stream->duplex ||
        !stream->read_handler || stream->connection.receive_pending)
        return invalid_event();
    stream->state = TlsStreamState::reading;
    // Give the owner a chance to finish after acknowledging downstream output,
    // without waiting for another record or an HTTP keep-alive peer to close.
    const auto action = stream->read_handler(stream->read_context, {}, stream->paused_peer_closed);
    if (action == TlsStreamReadAction::complete) {
        return finish_completed_read(stream, stream->paused_peer_closed);
    }
    if (action == TlsStreamReadAction::pause) {
        stream->state = TlsStreamState::read_paused;
        return event(TlsStreamEventKind::read_paused);
    }
    if (action == TlsStreamReadAction::reject || stream->paused_peer_closed)
        return failure(stream, TlsStreamError::receive);
    return drive_read(stream);
}

TlsStreamEvent tls_stream_handle_completion(TlsStream* stream, NetworkLoop* loop,
                                            const NetworkCompletion& completion) noexcept {
    if (!stream || !loop) {
        return invalid_event();
    }
    // Shared loops may deliver another stream's event. Never close this stream
    // or consume its operation count for an unrelated completion.
    const bool owned_socket = (completion.kind == NetworkOperationKind::connect ||
                               completion.kind == NetworkOperationKind::send ||
                               completion.kind == NetworkOperationKind::receive) &&
                              completion.connection == &stream->connection;
    const bool owned_resolver =
        completion.kind == NetworkOperationKind::resolve && completion.resolver == &stream->resolver;
    if (!owned_socket && !owned_resolver)
        return event(TlsStreamEventKind::waiting);
    if (completion.kind == NetworkOperationKind::connect || completion.kind == NetworkOperationKind::send ||
        completion.kind == NetworkOperationKind::receive) {
        if (completion.connection == &stream->connection && stream->outstanding_operations != 0) {
            --stream->outstanding_operations;
        }
    }
    if (stream->state == TlsStreamState::closing) {
        return finish_close(stream);
    }
    if (completion.kind == NetworkOperationKind::resolve) {
        if (stream->state != TlsStreamState::resolving || completion.resolver != &stream->resolver ||
            completion.stale || !completion.succeeded || !stream->resolver.results) {
            return failure(stream, TlsStreamError::resolve, static_cast<u32>(completion.native_error));
        }
        stream->next_address = stream->resolver.results;
        stream->state = TlsStreamState::connecting;
        if (!start_connect_candidate(stream, loop)) {
            return failure(stream, TlsStreamError::connect, WSAEHOSTUNREACH);
        }
        return event(TlsStreamEventKind::waiting);
    }
    if (completion.kind == NetworkOperationKind::connect) {
        if (stream->state != TlsStreamState::connecting || completion.connection != &stream->connection ||
            completion.stale || !completion.succeeded ||
            stream->connection.state != NetworkConnectionState::connected) {
            network_connection_close(&stream->connection);
            if (start_connect_candidate(stream, loop)) {
                return event(TlsStreamEventKind::waiting);
            }
            return failure(stream, TlsStreamError::connect, static_cast<u32>(completion.native_error));
        }
        if (!release_resolver(stream)) {
            return failure(stream, TlsStreamError::resolve);
        }
        stream->state = TlsStreamState::handshake;
        return drive_handshake(stream);
    }
    if (completion.kind == NetworkOperationKind::send) {
        if (completion.connection != &stream->connection || completion.stale || !completion.succeeded ||
            completion.transferred == 0 || !stream->send_data) {
            return failure(stream, TlsStreamError::send, static_cast<u32>(completion.native_error));
        }
        stream->send_offset += completion.transferred;
        if (stream->send_offset < stream->send_bytes) {
            if (!continue_send(stream)) {
                return failure(stream, TlsStreamError::send, static_cast<u32>(WSAGetLastError()));
            }
            return event(TlsStreamEventKind::waiting);
        }
        const bool handshake_send = stream->handshake_send;
        const u32 sent_bytes = stream->send_bytes;
        stream->send_data = nullptr;
        stream->send_bytes = 0;
        stream->send_offset = 0;
        stream->handshake_send = false;
        if (handshake_send) {
            SecureZeroMemory(stream->buffers.handshake_output.data, sent_bytes);
            return stream->after_tls_send == TlsHandshakeState::complete_and_send ? ready(stream)
                                                                                  : drive_handshake(stream);
        }
        if ((stream->state != TlsStreamState::writing && stream->state != TlsStreamState::duplex) ||
            !commit_current_record(stream)) {
            return failure(stream, TlsStreamError::send);
        }
        SecureZeroMemory(stream->buffers.record_output.data, sent_bytes);
        return start_next_record(stream);
    }
    if (completion.kind == NetworkOperationKind::receive) {
        if (completion.connection != &stream->connection || completion.stale || !completion.succeeded) {
            return failure(stream, TlsStreamError::receive, static_cast<u32>(completion.native_error));
        }
        if (completion.transferred == 0) {
            if ((stream->state != TlsStreamState::reading && stream->state != TlsStreamState::duplex) ||
                !stream->read_handler) {
                return failure(stream, TlsStreamError::receive);
            }
            if (stream->read_completed) {
                if (byte_queue_size(stream->encrypted_input)) return failure(stream, TlsStreamError::tls);
                return complete_read(stream, true);
            }
            const TlsStreamReadAction action = stream->read_handler(stream->read_context, {}, true);
            if (action == TlsStreamReadAction::pause && !stream->duplex) {
                stream->paused_peer_closed = true;
                stream->state = TlsStreamState::read_paused;
                return event(TlsStreamEventKind::read_paused);
            }
            return action == TlsStreamReadAction::complete ? complete_read(stream, true)
                                                           : failure(stream, TlsStreamError::receive);
        }
        if (!byte_queue_produce(&stream->encrypted_input, completion.transferred)) {
            return failure(stream, TlsStreamError::capacity);
        }
        if (stream->state == TlsStreamState::handshake) {
            return drive_handshake(stream);
        }
        if ((stream->state == TlsStreamState::reading || stream->state == TlsStreamState::duplex) &&
            stream->read_handler) {
            return drive_read(stream);
        }
        return failure(stream, TlsStreamError::receive);
    }
    return failure(stream, TlsStreamError::invalid_input);
}

void tls_stream_close(TlsStream* stream) noexcept {
    if (!stream || stream->state == TlsStreamState::closed) {
        return;
    }
    if (stream->state == TlsStreamState::empty) {
        stream->state = TlsStreamState::closed;
        return;
    }
    stream->state = TlsStreamState::closing;
    if (stream->resolver.pending) {
        network_resolver_cancel(&stream->resolver);
    }
    network_connection_close(&stream->connection);
    release_tls(stream);
    stream->send_data = nullptr;
    stream->read_handler = nullptr;
    stream->commit_handler = nullptr;
    stream->callback_context = nullptr;
    stream->read_context = nullptr;
    stream->duplex = false;
    stream->paused_peer_closed = false;
    stream->read_completed = false;
    finish_close(stream);
}

bool tls_stream_drained(TlsStream* stream) noexcept {
    if (!stream || (stream->state != TlsStreamState::closing && stream->state != TlsStreamState::closed) ||
        stream->outstanding_operations != 0 || stream->resolver.pending) {
        return false;
    }
    return stream->state == TlsStreamState::closed ||
           finish_close(stream).kind == TlsStreamEventKind::drained;
}

} // namespace maicord
