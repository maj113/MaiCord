#include "probe_transport.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {

NetworkPollResult wait_for(NetworkLoop* loop, NetworkOperationKind kind) noexcept {
    for (u32 attempt = 0; attempt < 30; ++attempt) {
        const NetworkPollResult result = network_loop_poll(loop, 500);
        if (result.state != NetworkPollState::timeout &&
            (result.state != NetworkPollState::completion || result.completion.kind == kind)) {
            return result;
        }
    }
    NetworkPollResult timeout{};
    timeout.state = NetworkPollState::timeout;
    return timeout;
}

bool send_all(NetworkLoop* loop, NetworkConnection* connection, ConstSlice<u8> bytes) noexcept {
    u32 offset = 0;
    while (offset < bytes.count) {
        const NetworkResult started =
            network_connection_send(connection, ConstSlice<u8>{bytes.data + offset, bytes.count - offset});
        if (!started.ok()) {
            return false;
        }
        const NetworkPollResult completion = wait_for(loop, NetworkOperationKind::send);
        if (completion.state != NetworkPollState::completion || !completion.completion.succeeded ||
            completion.completion.stale || completion.completion.transferred == 0) {
            return false;
        }
        offset += completion.completion.transferred;
    }
    return true;
}

bool receive_more(ProbeTransport* transport) noexcept {
    const Slice<u8> writable = byte_queue_writable(&transport->incoming);
    if (!writable.data || writable.count == 0 ||
        !network_connection_receive(&transport->connection, writable).ok()) {
        return false;
    }
    const NetworkPollResult completion = wait_for(&transport->loop, NetworkOperationKind::receive);
    return completion.state == NetworkPollState::completion && completion.completion.succeeded &&
           !completion.completion.stale && completion.completion.transferred != 0 &&
           byte_queue_produce(&transport->incoming, completion.completion.transferred);
}

bool connect_socket(ProbeTransport* transport, const wchar_t* hostname) noexcept {
    NetworkResolver resolver{};
    if (!network_resolver_start(&transport->loop, &resolver, hostname, L"443").ok()) {
        return false;
    }

    const NetworkPollResult resolved = wait_for(&transport->loop, NetworkOperationKind::resolve);
    if (resolved.state != NetworkPollState::completion || !resolved.completion.succeeded ||
        resolved.completion.stale || resolved.completion.resolver != &resolver || !resolver.results) {
        network_resolver_release(&resolver);
        return false;
    }

    bool connected = false;
    for (ADDRINFOEXW* current = resolver.results; current && !connected; current = current->ai_next) {
        if (current->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;
        }
        const NetworkResult started =
            network_connection_connect(&transport->loop, &transport->connection, current->ai_addr,
                                       static_cast<i32>(current->ai_addrlen));
        if (!started.ok()) {
            network_connection_close(&transport->connection);
            continue;
        }
        const NetworkPollResult completion = wait_for(&transport->loop, NetworkOperationKind::connect);
        connected = completion.state == NetworkPollState::completion && completion.completion.succeeded &&
                    !completion.completion.stale &&
                    transport->connection.state == NetworkConnectionState::connected;
        if (!connected) {
            network_connection_close(&transport->connection);
        }
    }
    network_resolver_release(&resolver);
    return connected;
}

bool complete_handshake(ProbeTransport* transport) noexcept {
    TlsHandshakeStep step =
        tls_client_handshake(&transport->tls, ConstSlice<u8>{nullptr, 0},
                             Slice<u8>{transport->handshake_output, kProbeHandshakeBufferBytes});
    for (u32 round = 0; round < 64; ++round) {
        if (!step.ok()) {
            return false;
        }
        if (step.produced != 0 && !send_all(&transport->loop, &transport->connection,
                                            ConstSlice<u8>{transport->handshake_output, step.produced})) {
            return false;
        }
        if (step.state == TlsHandshakeState::complete ||
            step.state == TlsHandshakeState::complete_and_send) {
            return true;
        }
        if (!receive_more(transport)) {
            return false;
        }
        const ConstSlice<u8> readable = byte_queue_readable(transport->incoming);
        step = tls_client_handshake(&transport->tls, readable,
                                    Slice<u8>{transport->handshake_output, kProbeHandshakeBufferBytes});
        if (!byte_queue_consume(&transport->incoming, step.consumed)) {
            return false;
        }
    }
    return false;
}

} // namespace

bool probe_transport_connect(ProbeTransport* transport, const wchar_t* hostname) noexcept {
    if (!transport || !hostname || hostname[0] == 0) {
        return false;
    }
    if (!byte_queue_initialize(&transport->incoming,
                               Slice<u8>{transport->incoming_storage, kProbeHandshakeBufferBytes}) ||
        !network_loop_initialize(&transport->loop).ok() || !connect_socket(transport, hostname) ||
        !tls_client_initialize(&transport->tls, hostname).ok() ||
        transport->tls.maximum_token_bytes > kProbeHandshakeBufferBytes || !complete_handshake(transport)) {
        probe_transport_release(transport);
        return false;
    }
    return true;
}

bool probe_transport_send(ProbeTransport* transport, ConstSlice<u8> plaintext) noexcept {
    if (!transport || (!plaintext.data && plaintext.count != 0)) {
        return false;
    }
    u32 offset = 0;
    while (offset < plaintext.count) {
        const u32 bytes = plaintext.count - offset > transport->tls.stream_sizes.cbMaximumMessage
                              ? transport->tls.stream_sizes.cbMaximumMessage
                              : plaintext.count - offset;
        u32 encrypted_bytes = 0;
        const TlsResult encrypted = tls_client_encrypt(
            &transport->tls, ConstSlice<u8>{plaintext.data + offset, bytes},
            Slice<u8>{transport->record_output, kProbeRecordBufferBytes}, &encrypted_bytes);
        if (!encrypted.ok() || !send_all(&transport->loop, &transport->connection,
                                         ConstSlice<u8>{transport->record_output, encrypted_bytes})) {
            return false;
        }
        offset += bytes;
    }
    return true;
}

ProbeReceiveResult probe_transport_receive(ProbeTransport* transport) noexcept {
    if (!transport) {
        return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::error};
    }
    for (u32 round = 0; round < 64; ++round) {
        if (byte_queue_size(transport->incoming) == 0 && !receive_more(transport)) {
            return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::error};
        }
        const ConstSlice<u8> readable = byte_queue_readable(transport->incoming);
        const TlsDecryptResult decrypted =
            tls_client_decrypt(&transport->tls, Slice<u8>{const_cast<u8*>(readable.data), readable.count});
        if (!decrypted.ok()) {
            return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::error};
        }
        if (decrypted.state == TlsDecryptState::need_receive) {
            if (!receive_more(transport)) {
                return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::error};
            }
            continue;
        }
        if (decrypted.state == TlsDecryptState::peer_closed) {
            return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::peer_closed};
        }
        if (!byte_queue_consume(&transport->incoming, decrypted.consumed)) {
            return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::error};
        }
        if (decrypted.state == TlsDecryptState::plaintext && decrypted.plaintext.count != 0) {
            return ProbeReceiveResult{decrypted.plaintext, ProbeReceiveState::plaintext};
        }
    }
    return ProbeReceiveResult{{nullptr, 0}, ProbeReceiveState::error};
}

void probe_transport_release(ProbeTransport* transport) noexcept {
    if (!transport) {
        return;
    }
    tls_client_release(&transport->tls);
    network_connection_close(&transport->connection);
    network_loop_release(&transport->loop);
    byte_queue_clear(&transport->incoming, true);
}

} // namespace maicord
