#include "client_internal.hpp"
#include "maicord/platform/crypto/random.hpp"

namespace maicord {
namespace client_detail {

bool begin_resolution(GatewayClient* client, GatewayClientRuntime* runtime, NetworkLoop* loop,
                      u64 now_ms) noexcept {
    if (runtime->resume.valid && gateway_resume_expired(runtime->last_ack_ms, now_ms)) {
        reset_voice(client, runtime);
        clear_resume(runtime);
    }
    runtime->failure_stage = GatewayClientFailureStage::none;
    runtime->session_error = GatewaySessionError::none;
    runtime->handshake_error = WebSocketHandshakeError::none;
    runtime->websocket_error = WebSocketError::none;
    runtime->gateway_decode_error = GatewayDecodeError::none;
    runtime->event_decode_error = GatewayEventDecodeError::none;
    runtime->compression_error = ZstdError::none;
    runtime->etf_error = EtfError::none;
    runtime->json_scan_error = JsonScanError::none;
    runtime->json_read_error = JsonReadError::none;
    runtime->route_error_offset = 0;
    runtime->route_detail = 0;
    byte_queue_clear(&runtime->encrypted_input, true);
    if (!byte_queue_initialize(&runtime->encrypted_input,
                               Slice<u8>{runtime->encrypted_storage, kEncryptedInputBytes}) ||
        !secure_random_u32(&runtime->heartbeat_jitter).ok()) {
        return false;
    }
    if (runtime->session.input.data) {
        byte_queue_clear(&runtime->session.input, true);
    }
    clear_user_transport(runtime);
    gateway_outbound_reset(&runtime->outbound);
    runtime->qos.has_immediate = false;
    runtime->send_data = nullptr;
    runtime->send_bytes = 0;
    runtime->send_offset = 0;
    runtime->send_purpose = SendPurpose::none;
    runtime->sent_subscription_revision = 0;
    runtime->active_subscription_revision = 0;
    runtime->sent_members_revision = 0;
    gateway_subscription_thread_owner_reset_connection(&runtime->thread_subscription_owner);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_self_status_reset_connection(&runtime->self_status);
    gateway_member_request_reset_connection(&runtime->member_requests, !runtime->resume.valid);
    runtime->member_deadline_ms = 0;
    runtime->channel_info_deadline_ms = 0;
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    runtime->self_status_deadline_ms = 0;
    runtime->rate_deadline_ms = 0;
    runtime->pending_pong_bytes = 0;
    runtime->has_pending_pong = false;
    SecureZeroMemory(runtime->pending_pong, sizeof(runtime->pending_pong));

    if (runtime->resume.valid) {
        if (gateway_endpoint_parse_with_policy(
                Utf8View{runtime->resume.resume_url, runtime->resume.resume_url_bytes},
                GatewayEndpointPolicy::user_v9_etf_zstd_stream, &runtime->endpoint) != GatewayEndpointError::none) {
            return false;
        }
    } else {
        runtime->endpoint = runtime->initial_endpoint;
    }
    runtime->resolver_released = false;
    const NetworkResult resolving = network_resolver_start(
        loop, &runtime->resolver, runtime->endpoint.host_wide, runtime->endpoint.service_wide);
    if (!resolving.ok()) {
        return false;
    }
    runtime->reconnecting = false;
    runtime->reconnect_deadline_ms = 0;
    publish_phase(client, GatewayClientPhase::resolving);
    runtime->deadline_ms = now_ms + kPhaseTimeoutMs;
    return true;
}

} // namespace client_detail
namespace {

// Drops the connection and its TLS state; the worker keeps its loop.
void close_transport(GatewayClientRuntime* runtime) noexcept {
    if (runtime->resolver.pending)
        network_resolver_cancel(&runtime->resolver);
    network_connection_close(&runtime->connection);
    if (runtime->tls_initialized) {
        tls_client_release(&runtime->tls);
        runtime->tls_initialized = false;
    }
    clear_user_transport(runtime);
}

} // namespace
namespace client_detail {

bool schedule_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, GatewayLifecycleAction action,
                        u64 now_ms, u32 delay_ms) noexcept {
    runtime->mailbox_deadline_ms = 0;
    if (action != GatewayLifecycleAction::reconnect_identify &&
        action != GatewayLifecycleAction::reconnect_resume) {
        return false;
    }
    if (action == GatewayLifecycleAction::reconnect_identify) {
        reset_voice(client, runtime);
        clear_resume(runtime);
    }
    runtime->reconnecting = true;
    runtime->reconnect_deadline_ms = deadline_after(now_ms, delay_ms);
    runtime->deadline_ms = 0;
    runtime->rate_deadline_ms = 0;
    gateway_outbound_reset(&runtime->outbound);
    runtime->qos.has_immediate = false;
    close_transport(runtime);
    publish_phase(client, GatewayClientPhase::reconnect_wait);
    GatewayClientEvent reconnecting{};
    reconnecting.timestamp_ms = now_ms;
    reconnecting.kind = GatewayClientEventKind::reconnecting;
    reconnecting.has_sequence = runtime->resume.valid;
    reconnecting.sequence = static_cast<u64>(ReadAcquire64(&runtime->resume_sequence));
    publish_event(runtime, reconnecting);
    return true;
}

} // namespace client_detail
namespace {

bool schedule_backoff_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    u32 random = 0;
    if (!secure_random_u32(&random).ok()) {
        return false;
    }
    const GatewayReconnectDelay delay = gateway_reconnect_next(&runtime->reconnect_backoff, random);
    const GatewayLifecycleAction action = runtime->resume.valid
                                              ? GatewayLifecycleAction::reconnect_resume
                                              : GatewayLifecycleAction::reconnect_identify;
    return schedule_reconnect(client, runtime, action, now_ms, delay.delay_ms);
}

} // namespace
namespace client_detail {

void begin_shutdown(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    if (runtime->mailbox_cancel)
        SetEvent(runtime->mailbox_cancel);
    if (!runtime->stopping) {
        runtime->stopping = true;
        reset_voice(client, runtime);
        if (!runtime->failed) {
            publish_phase(client, GatewayClientPhase::stopping);
        }
        close_transport(runtime);
    }
}

bool shutdown_drained(GatewayClientRuntime* runtime) noexcept {
    if (runtime->resolver.pending || runtime->outstanding_socket_operations != 0 ||
        !voice_connection_drained(runtime->voice_connection) || !gateway_stream_session_drained(runtime->stream)) {
        return false;
    }
    release_resolver(runtime);
    return true;
}

NetworkWorkerStep fail_and_drain(GatewayClient* client, GatewayClientRuntime* runtime,
                                 GatewayClientError error, u32 native_error) noexcept {
    if (error == GatewayClientError::session && runtime->failure_stage == GatewayClientFailureStage::none) {
        runtime->failure_stage = GatewayClientFailureStage::application_output;
    }
    runtime->failed = true;
    GatewayClientEvent event{};
    event.timestamp_ms = GetTickCount64();
    event.native_error = native_error;
    event.kind = GatewayClientEventKind::failed;
    event.error = error;
    event.failure_stage = runtime->failure_stage;
    event.session_error = runtime->session_error;
    event.handshake_error = runtime->handshake_error;
    event.websocket_error = runtime->websocket_error;
    event.gateway_decode_error = runtime->gateway_decode_error;
    event.event_decode_error = runtime->event_decode_error;
    event.compression_error = runtime->compression_error;
    event.etf_error = runtime->etf_error;
    event.json_scan_error = runtime->json_scan_error;
    event.json_read_error = runtime->json_read_error;
    event.route_error_offset = runtime->route_error_offset;
    event.route_detail = runtime->route_detail;
    publish_event(runtime, event);
    // A consumer woken by the event above may run before the terminal status
    // is visible (or the mailbox was full): record the failure, then wake it again.
    publish_failure(client, error, native_error);
    if (client->notification_event)
        SetEvent(client->notification_event);
    begin_shutdown(client, runtime);
    return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
}

NetworkWorkerStep reconnect_or_fail(GatewayClient* client, GatewayClientRuntime* runtime,
                                    GatewayClientError error, u32 native_error, u64 now_ms) noexcept {
    if (runtime->authenticated && schedule_backoff_reconnect(client, runtime, now_ms)) {
        return NetworkWorkerStep{next_worker_wait(client, runtime, now_ms), false};
    }
    return fail_and_drain(client, runtime, error, native_error);
}

bool start_receive(GatewayClientRuntime* runtime) noexcept {
    const Slice<u8> writable = byte_queue_writable(&runtime->encrypted_input);
    if (!writable.data || writable.count == 0 ||
        !network_connection_receive(&runtime->connection, writable).ok()) {
        return false;
    }
    ++runtime->outstanding_socket_operations;
    return true;
}

bool ensure_receive(GatewayClientRuntime* runtime) noexcept {
    return runtime->connection.receive_pending || start_receive(runtime);
}

} // namespace client_detail
namespace {

bool start_send(GatewayClientRuntime* runtime, const ConstSlice<u8>& bytes, SendPurpose purpose) noexcept {
    if (!bytes.data || bytes.count == 0) {
        return false;
    }
    runtime->send_data = bytes.data;
    runtime->send_bytes = bytes.count;
    runtime->send_offset = 0;
    runtime->send_purpose = purpose;
    if (!network_connection_send(&runtime->connection, bytes).ok()) {
        return false;
    }
    ++runtime->outstanding_socket_operations;
    return true;
}

} // namespace
namespace client_detail {

bool continue_send(GatewayClientRuntime* runtime) noexcept {
    const ConstSlice<u8> remaining{runtime->send_data + runtime->send_offset,
                                   runtime->send_bytes - runtime->send_offset};
    if (!network_connection_send(&runtime->connection, remaining).ok()) {
        return false;
    }
    ++runtime->outstanding_socket_operations;
    return true;
}

bool start_connect_candidate(NetworkLoop* loop, GatewayClientRuntime* runtime) noexcept {
    while (runtime->next_address) {
        ADDRINFOEXW* candidate = runtime->next_address;
        runtime->next_address = runtime->next_address->ai_next;
        if (!candidate->ai_addr || candidate->ai_addrlen == 0 ||
            candidate->ai_addrlen > sizeof(sockaddr_storage) ||
            (candidate->ai_family != AF_INET && candidate->ai_family != AF_INET6)) {
            continue;
        }
        const NetworkResult started = network_connection_connect(
            loop, &runtime->connection, candidate->ai_addr, static_cast<i32>(candidate->ai_addrlen));
        if (started.ok()) {
            ++runtime->outstanding_socket_operations;
            return true;
        }
    }
    return false;
}

} // namespace client_detail
namespace {

// The account's cookies for this endpoint: the page reaches the Gateway
// cross-site, so only its SameSite=None cookies apply.
CookieJar* endpoint_cookies(const GatewayClientRuntime* runtime, CookieRequest* request) noexcept {
    const ClientSessionMetadata* metadata = session_metadata(runtime);
    const GatewayEndpoint& endpoint = runtime->endpoint;
    if (!metadata || !metadata->cookies || !endpoint.host_bytes || endpoint.authority[0] == '[')
        return nullptr;
    *request = {{endpoint.authority, endpoint.host_bytes}, {endpoint.target, endpoint.target_bytes}, false};
    return metadata->cookies;
}

bool initialize_session(GatewayClientRuntime* runtime,
                        const u8 client_key[kWebSocketClientKeyBytes]) noexcept {
    // A connection lost while a large payload was spilled leaves its pages behind.
    reserved_bytes_decommit(&runtime->compressed_spill);
    if (!gateway_session_initialize(
            &runtime->session, Slice<u8>{runtime->session_input, kSessionInputBytes},
            Slice<u8>{runtime->handshake_output + kMessageBytes, kMessageBytes},
            main_index_view(runtime), ConstSlice<u8>{client_key, kWebSocketClientKeyBytes},
            kSessionInputBytes - 14, kMessageBytes, runtime->resume.valid, runtime->resume.valid,
            static_cast<u64>(ReadAcquire64(&runtime->resume_sequence)))) {
        return false;
    }
    runtime->session.grow_index = grow_main_index;
    runtime->session.grow_index_context = runtime;
    runtime->session.cookies = endpoint_cookies(runtime, &runtime->session.cookie_request);
    return zstd_stream_initialize(&runtime->user->zstd, zstd_storage_view(runtime), kZstdStorageBytes,
                                  grow_zstd_storage, runtime) &&
           gateway_session_enable_zstd_etf(&runtime->session, &runtime->user->zstd,
                                           {runtime->handshake_output, kMessageBytes}) &&
           gateway_session_enable_output_growth(&runtime->session, decoded_message_view(runtime),
                                                kGatewaySessionMaximumDecodedMessageBytes,
                                                grow_decoded_message, runtime) &&
           (!runtime->compressed_spill.base ||
            gateway_session_enable_compressed_growth(&runtime->session, kGatewaySessionMaximumDecodedMessageBytes,
                                                     grow_compressed_message, runtime));
}

} // namespace
namespace client_detail {

bool begin_websocket_opening(GatewayClientRuntime* runtime) noexcept {
    u8 nonce[kWebSocketNonceBytes]{};
    u8 client_key[kWebSocketClientKeyBytes]{};
    if (!secure_random_fill(Slice<u8>{nonce, kWebSocketNonceBytes}).ok()) {
        return false;
    }
    const WebSocketHandshakeWriteResult encoded = websocket_encode_client_key(
        ConstSlice<u8>{nonce, kWebSocketNonceBytes}, Slice<u8>{client_key, kWebSocketClientKeyBytes});
    SecureZeroMemory(runtime->handshake_output, kMessageBytes);
    if (!encoded.ok() || !initialize_session(runtime, client_key)) {
        return false;
    }
    const ClientSessionMetadata* metadata = session_metadata(runtime);
    CookieRequest cookie_request{};
    CookieJar* cookies = endpoint_cookies(runtime, &cookie_request);
    u8 cookie[kCookieHeaderMaximumBytes];
    const u32 cookie_bytes =
        cookies ? cookie_jar_header(cookies, cookie_request, clock_unix_milliseconds(), {cookie, sizeof(cookie)})
                : 0;
    const WebSocketPageFields page{
        metadata ? ConstSlice<u8>{reinterpret_cast<const u8*>(metadata->accept_language.data),
                                  metadata->accept_language.bytes}
                 : ConstSlice<u8>{},
        {cookie, cookie_bytes}};
    const WebSocketHandshakeWriteResult request = websocket_write_opening_request(
        ConstSlice<u8>{reinterpret_cast<const u8*>(runtime->endpoint.target),
                       runtime->endpoint.target_bytes},
        ConstSlice<u8>{reinterpret_cast<const u8*>(runtime->endpoint.authority),
                       runtime->endpoint.authority_bytes},
        ConstSlice<u8>{client_key, kWebSocketClientKeyBytes},
        Slice<u8>{runtime->plain_output, kPlainOutputBytes}, &page);
    secure_clear_bytes(cookie, cookie_bytes);
    if (!request.ok()) {
        return false;
    }
    u32 encrypted_bytes = 0;
    const bool encrypted =
        tls_client_encrypt(&runtime->tls, ConstSlice<u8>{runtime->plain_output, request.bytes},
                           Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes)
            .ok();
    // The request carries cookies.
    secure_clear_bytes(runtime->plain_output, request.bytes);
    if (!encrypted) {
        return false;
    }
    return start_send(runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes},
                      SendPurpose::websocket_opening);
}

bool drive_tls_handshake(GatewayClientRuntime* runtime) noexcept {
    const ConstSlice<u8> incoming = byte_queue_readable(runtime->encrypted_input);
    const TlsHandshakeStep step = tls_client_handshake(
        &runtime->tls, incoming, Slice<u8>{runtime->handshake_output, kHandshakeBytes});
    if (!step.ok() || !byte_queue_consume(&runtime->encrypted_input, step.consumed)) {
        return false;
    }
    if (step.produced != 0) {
        runtime->after_tls_send = step.state;
        return start_send(runtime, ConstSlice<u8>{runtime->handshake_output, step.produced},
                          SendPurpose::tls_handshake);
    }
    if (step.state == TlsHandshakeState::complete) {
        return begin_websocket_opening(runtime);
    }
    return step.state == TlsHandshakeState::need_receive && start_receive(runtime);
}

bool queue_pong(GatewayClientRuntime* runtime, ConstSlice<u8> payload) noexcept {
    if (runtime->has_pending_pong || payload.count > sizeof(runtime->pending_pong)) {
        return false;
    }
    copy_bytes(runtime->pending_pong, payload.data, payload.count);
    runtime->pending_pong_bytes = static_cast<u8>(payload.count);
    runtime->has_pending_pong = true;
    return true;
}

bool send_pending_pong(GatewayClientRuntime* runtime) noexcept {
    if (!runtime->has_pending_pong || runtime->send_purpose != SendPurpose::none) {
        return false;
    }
    u32 masking_key = 0;
    if (!secure_random_u32(&masking_key).ok()) {
        return false;
    }
    const WebSocketWriteResult framed = websocket_write_client_frame(
        WebSocketOpcode::pong, true, ConstSlice<u8>{runtime->pending_pong, runtime->pending_pong_bytes},
        masking_key, Slice<u8>{runtime->plain_output, kPlainOutputBytes});
    if (!framed.ok()) {
        return false;
    }
    u32 encrypted_bytes = 0;
    if (!tls_client_encrypt(&runtime->tls, ConstSlice<u8>{runtime->plain_output, framed.bytes},
                            Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes)
             .ok()) {
        return false;
    }
    if (!start_send(runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes}, SendPurpose::pong)) {
        return false;
    }
    SecureZeroMemory(runtime->pending_pong, runtime->pending_pong_bytes);
    runtime->pending_pong_bytes = 0;
    runtime->has_pending_pong = false;
    return true;
}

// Frames and sends the command written to gateway_payload (as ETF on the user
// Gateway); the plaintext copies are erased.
OutboundDrive send_control_command(GatewayClientRuntime* runtime, GatewayCommandWriteResult command,
                                   SendPurpose purpose) noexcept {
    if (!command.ok() || command.bytes == 0 || command.bytes > sizeof(runtime->gateway_payload)) {
        return OutboundDrive::failed;
    }
    const EtfResult packed =
        etf_from_json({reinterpret_cast<const char*>(runtime->gateway_payload), command.bytes},
                      {runtime->gateway_etf, kEtfCommandBytes});
    SecureZeroMemory(runtime->gateway_payload, command.bytes);
    if (!packed.ok()) {
        SecureZeroMemory(runtime->gateway_etf, kEtfCommandBytes);
        return OutboundDrive::failed;
    }
    const ConstSlice<u8> payload{runtime->gateway_etf, packed.bytes};
    const WebSocketOpcode opcode = WebSocketOpcode::binary;
    u32 masking_key = 0;
    if (!secure_random_u32(&masking_key).ok()) {
        SecureZeroMemory(const_cast<u8*>(payload.data), payload.count);
        return OutboundDrive::failed;
    }
    const WebSocketWriteResult framed = websocket_write_client_frame(
        opcode, true, payload, masking_key, Slice<u8>{runtime->plain_output, kPlainOutputBytes});
    SecureZeroMemory(runtime->gateway_etf, kEtfCommandBytes);
    if (!framed.ok()) {
        return OutboundDrive::failed;
    }
    u32 encrypted_bytes = 0;
    if (!tls_client_encrypt(&runtime->tls, ConstSlice<u8>{runtime->plain_output, framed.bytes},
                            Slice<u8>{runtime->record_output, kRecordOutputBytes}, &encrypted_bytes)
             .ok()) {
        SecureZeroMemory(runtime->plain_output, framed.bytes);
        return OutboundDrive::failed;
    }
    SecureZeroMemory(runtime->plain_output, framed.bytes);
    if (!start_send(runtime, ConstSlice<u8>{runtime->record_output, encrypted_bytes}, purpose)) {
        SecureZeroMemory(runtime->record_output, encrypted_bytes);
        return OutboundDrive::failed;
    }
    return OutboundDrive::sending;
}

} // namespace client_detail
namespace {

// The user identify into gateway_payload: the launch's first is the
// fast-connect one, later ones carry the connect reasons and QoS token.
GatewayCommandWriteResult write_user_identify_command(GatewayClientRuntime* runtime, Utf8View token) noexcept {
    char heartbeat_id[kGatewayUserLaunchIdBytes];
    char fast_connect_id[kGatewayUserLaunchIdBytes + 1];
    char qos_token[kGatewayQosTokenMaximumBytes];
    auto identify = user_identify(*runtime->user, token, heartbeat_id);
    if (take_fast_connect(fast_connect_id)) {
        identify.fast_connect = true;
        identify.properties.client_launch_id = {fast_connect_id, kGatewayUserLaunchIdBytes};
    } else {
        identify.connect_reasons = utf8_literal(kGatewayConnectReasons);
        const GatewayUserRuntime& user = *runtime->user;
        identify.qos_token = {qos_token, gateway_qos_token_write(qos_active(runtime->qos), user.has_qos_derived,
                                                                 {user.qos_derived, user.qos_derived_bytes},
                                                                 {qos_token, sizeof(qos_token)})};
    }
    AcquireSRWLockShared(&runtime->subscription_lock);
    if (runtime->self_status.authoritative) {
        identify.status = runtime->self_status.desired.status;
        identify.since_ms = runtime->self_status.desired.since_ms;
        identify.has_since = runtime->self_status.desired.has_since;
        identify.afk = runtime->self_status.desired.afk;
    }
    ReleaseSRWLockShared(&runtime->subscription_lock);
    const GatewayCommandWriteResult command =
        gateway_write_user_identify(identify, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    SecureZeroMemory(qos_token, sizeof(qos_token));
    return command;
}

} // namespace
namespace client_detail {

OutboundDrive drive_gateway_outbound(GatewayClient* client, GatewayClientRuntime* runtime,
                                     u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none) {
        return OutboundDrive::sending;
    }
    const GatewayOutboundStep next = gateway_outbound_take(&runtime->outbound, now_ms);
    if (next.state == GatewayOutboundState::idle) {
        runtime->rate_deadline_ms = 0;
        return OutboundDrive::idle;
    }
    if (next.state == GatewayOutboundState::rate_limited) {
        runtime->rate_deadline_ms = next.next_allowed_ms;
        return OutboundDrive::rate_limited;
    }
    if (next.state != GatewayOutboundState::action || !runtime->credential.data ||
        !runtime->credential.locked) {
        return OutboundDrive::failed;
    }

    GatewayCommandWriteResult command{};
    const ConstSlice<u8> credential = secure_buffer_view(runtime->credential);
    const Utf8View token{reinterpret_cast<const char*>(credential.data), credential.count};
    if (next.action == GatewayLifecycleAction::send_identify) {
        command = write_user_identify_command(runtime, token);
    } else if (next.action == GatewayLifecycleAction::send_resume && runtime->resume.valid) {
        const GatewayResume resume{token,
                                   Utf8View{runtime->resume.session_id, runtime->resume.session_id_bytes},
                                   static_cast<u64>(ReadAcquire64(&runtime->resume_sequence))};
        command = gateway_write_resume(
            resume, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    } else if (next.action == GatewayLifecycleAction::send_heartbeat) {
        GatewayQosPayload qos{};
        const bool has_qos = gateway_qos_take(&runtime->qos, &qos);
        const GatewayWriteResult heartbeat =
            gateway_write_heartbeat(runtime->session.lifecycle, has_qos ? &qos : nullptr,
                                    Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
        command.bytes = heartbeat.bytes;
        command.error = heartbeat.ok() ? GatewayCommandError::none : GatewayCommandError::output_too_small;
    } else if (next.action == GatewayLifecycleAction::send_time_spent_session) {
        command = gateway_write_time_spent_session(
            runtime->time_spent.created_ms, {runtime->time_spent.uuid, kGatewayUserLaunchIdBytes},
            user_text(runtime->user->client_launch_id),
            Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    } else {
        return OutboundDrive::failed;
    }
    if (send_control_command(runtime, command, SendPurpose::gateway_action) != OutboundDrive::sending)
        return OutboundDrive::failed;
    runtime->rate_deadline_ms = 0;

    GatewayClientEvent event{};
    event.timestamp_ms = now_ms;
    if (next.action == GatewayLifecycleAction::send_identify) {
        event.kind = GatewayClientEventKind::identifying;
        publish_phase(client, GatewayClientPhase::identifying);
    } else if (next.action == GatewayLifecycleAction::send_resume) {
        event.kind = GatewayClientEventKind::resuming;
        publish_phase(client, GatewayClientPhase::resuming);
    } else if (next.action == GatewayLifecycleAction::send_heartbeat) {
        event.kind = GatewayClientEventKind::heartbeat_sent;
        event.has_sequence = runtime->session.lifecycle.has_sequence;
        event.sequence = runtime->session.lifecycle.last_sequence;
    } else {
        return OutboundDrive::sending;
    }
    publish_event(runtime, event);
    return OutboundDrive::sending;
}

} // namespace client_detail
} // namespace maicord
