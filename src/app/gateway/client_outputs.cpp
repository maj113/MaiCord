#include "client_internal.hpp"
#include "user_payload.hpp"

namespace maicord {
namespace {

// Takes one send from the outbound rate limit; otherwise records when the next one is allowed.
bool acquire_send(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    const auto rate = gateway_rate_limit_acquire(&runtime->outbound.limiter, now_ms);
    if (!rate.acquired)
        runtime->rate_deadline_ms = rate.next_allowed_ms;
    return rate.acquired;
}

OutboundDrive drive_guild_subscription(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none ||
        runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready) {
        return OutboundDrive::idle;
    }
    GatewayGuildSubscription subscription;
    AcquireSRWLockShared(&runtime->subscription_lock);
    const u32 revision = runtime->subscription_revision;
    const bool changed = revision != 0 && revision != runtime->sent_subscription_revision;
    if (changed)
        copy_value(&subscription, runtime->desired_subscription);
    ReleaseSRWLockShared(&runtime->subscription_lock);
    if (!changed) {
        return OutboundDrive::idle;
    }
    GatewayGuildSubscription prepared{};
    if (!gateway_subscription_thread_owner_prepare(runtime->thread_subscription_owner, subscription,
                                                   &prepared))
        return OutboundDrive::failed;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const GatewayCommandWriteResult command = gateway_write_guild_subscriptions_bulk(
        prepared, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_subscription);
    if (result == OutboundDrive::sending) {
        runtime->active_subscription_revision = revision;
        gateway_subscription_thread_owner_admit(&runtime->thread_subscription_owner, prepared);
    }
    return result;
}

// The members key of op 37: the users followed in the guild (see gateway_client_subscribe_guild_members).
OutboundDrive drive_member_subscription(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none ||
        runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready) {
        return OutboundDrive::idle;
    }
    GatewayGuildSubscription subscription;
    AcquireSRWLockShared(&runtime->subscription_lock);
    const u32 revision = runtime->members_revision;
    const bool changed = revision != 0 && revision != runtime->sent_members_revision;
    if (changed)
        copy_value(&subscription, runtime->desired_members);
    ReleaseSRWLockShared(&runtime->subscription_lock);
    if (!changed)
        return OutboundDrive::idle;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const GatewayCommandWriteResult command = gateway_write_guild_subscriptions_bulk(
        subscription, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_members);
    if (result == OutboundDrive::sending)
        runtime->sent_members_revision = revision;
    return result;
}

OutboundDrive drive_voice_output(GatewayClient* client, GatewayClientRuntime* runtime,
                                 u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    gateway_voice_expire(&runtime->voice, now_ms);
    GatewayVoiceStateUpdate desired{};
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const bool requested = runtime->voice_request_pending;
    if (requested) {
        desired = runtime->desired_voice;
        runtime->voice_request_pending = false;
    }
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (requested) {
        gateway_voice_request(&runtime->voice, desired);
        if (!runtime->voice.setup.user_id) {
            const auto bound =
                voice_setup_bind_utf8(&runtime->voice.setup, runtime->self_user_id,
                                      {runtime->resume.session_id, runtime->resume.session_id_bytes});
            if (bound != VoiceSetupResult::updated)
                gateway_voice_cancel(&runtime->voice, GatewayVoiceError::storage);
        }
    }
    publish_voice_snapshot(client, runtime);
    GatewayVoiceStateUpdate state;
    if (!gateway_voice_next(runtime->voice, &state))
        return OutboundDrive::idle;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const auto command = gateway_write_voice_state(
        state, runtime->voice_recording_disallowed ? 0 : kGatewayVoiceStateAllowVoiceRecording,
        {runtime->gateway_payload, static_cast<u32>(sizeof(runtime->gateway_payload))});
    const auto result = send_control_command(runtime, command, SendPurpose::voice_state);
    if (result == OutboundDrive::sending && !gateway_voice_submitted(&runtime->voice, state, now_ms))
        return OutboundDrive::failed;
    publish_voice_snapshot(client, runtime);
    return result;
}

// Go Live commands (ops 19-21). UI watch requests apply here, after the voice
// request, so a join and a watch posted together see the joined channel.
OutboundDrive drive_stream_output(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    auto& control = runtime->stream.control;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const bool requested = runtime->stream_request_pending;
    const GatewayStreamKey desired = runtime->desired_stream;
    runtime->stream_request_pending = false;
    const bool volume = runtime->stream_volume_pending;
    if (volume)
        gateway_stream_session_set_volume(&runtime->stream, runtime->desired_stream_volume);
    runtime->stream_volume_pending = false;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (volume && !requested)
        publish_voice_snapshot(client, runtime);
    if (requested) {
        if (desired.type == GatewayStreamType::none)
            gateway_stream_stop(&control, GatewayStreamEnd::stopped);
        else {
            if (!control.setup.user_id)
                gateway_stream_bind(&control, runtime->self_user_id,
                                    {runtime->resume.session_id, runtime->resume.session_id_bytes});
            gateway_stream_watch(&control, desired, now_ms);
        }
        publish_voice_snapshot(client, runtime);
    }
    GatewayStreamCommand command;
    if (!gateway_stream_next(control, &command))
        return OutboundDrive::idle;
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const auto written =
        gateway_stream_write(command, {runtime->gateway_payload, static_cast<u32>(sizeof(runtime->gateway_payload))});
    if (!written.ok()) {
        gateway_stream_submitted(&control, command); // Unencodable: drop it.
        return OutboundDrive::idle;
    }
    const auto result = send_control_command(runtime, written, SendPurpose::stream);
    if (result == OutboundDrive::sending)
        gateway_stream_submitted(&control, command);
    return result;
}

OutboundDrive drive_self_status(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    if (!owner.authoritative || !gateway_self_status_pending(owner)) {
        runtime->self_status_deadline_ms = 0;
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle;
    }
    runtime->self_status_deadline_ms = gateway_self_status_deadline(&owner, now_ms);
    if (runtime->self_status_deadline_ms) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle; // Presence cap must not delay other traffic.
    }
    if (!acquire_send(runtime, now_ms)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::rate_limited;
    }
    SelfCustomStatus custom{};
    GatewayCustomStatusActivity activity{};
    const ConstSlice<u8> status_group{runtime->self_status_group, runtime->self_status_group_bytes};
    const bool visible = owner.desired.status != GatewayUserStatus::invisible;
    const bool publish = visible && self_status_settings_read_custom(status_group, &custom) &&
                         self_custom_status_active(custom, clock_unix_milliseconds());
    if (publish)
        activity = {{custom.text, custom.text_bytes}, {custom.emoji_name, custom.emoji_name_bytes}, custom.emoji_id,
                    custom.expires_at_ms};
    // The game is shared only while "Share my activity" is on.
    const auto& local = runtime->local_presence;
    bool share = true;
    const bool game = visible && local.game && self_status_settings_read_show_game(status_group, &share) && share;
    const GatewayGameActivity played{{local.game_name, local.game_name_bytes}, local.game_application_id,
                                     local.game_started_ms, local.game_ends_ms,
                                     {local.game_details, local.game_details_bytes},
                                     {local.game_state, local.game_state_bytes}, local.game_type};
    const auto written = gateway_write_presence_update(
        gateway_presence_effective(owner.desired, &local), publish ? &activity : nullptr, game ? &played : nullptr,
        {runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto sent = send_control_command(runtime, written, SendPurpose::self_status);
    if (sent == OutboundDrive::sending)
        gateway_self_status_admit(&owner, owner.revision, now_ms);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return sent;
}
OutboundDrive drive_member_requests(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const GatewayGuildMemberRequest* next = gateway_member_request_next(runtime->member_requests, now_ms);
    runtime->member_deadline_ms = gateway_member_request_deadline(runtime->member_requests, now_ms);
    if (!next) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle;
    }
    if (!acquire_send(runtime, now_ms)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::rate_limited;
    }
    runtime->rate_deadline_ms = 0;
    const auto command =
        gateway_write_guild_member_request(*next, {runtime->gateway_payload, kGatewayUserMaximumCommandBytes});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_members);
    if (result == OutboundDrive::sending)
        gateway_member_request_sent(&runtime->member_requests, next);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return result;
}

// Request Channel Info (opcode 43), once per guild per session.
OutboundDrive drive_channel_info(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->session.lifecycle.phase != GatewayLifecyclePhase::ready)
        return OutboundDrive::idle;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const u64 guild = gateway_channel_info_next(runtime->channel_info, now_ms);
    runtime->channel_info_deadline_ms = gateway_channel_info_deadline(runtime->channel_info, now_ms);
    if (!guild) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::idle;
    }
    if (!acquire_send(runtime, now_ms)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return OutboundDrive::rate_limited;
    }
    const auto command =
        gateway_write_channel_info_request(guild, {runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::channel_info);
    if (result == OutboundDrive::sending)
        gateway_channel_info_sent(&runtime->channel_info, guild);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return result;
}

} // namespace
namespace client_detail {

OutboundDrive drive_application_output(GatewayClient* client, GatewayClientRuntime* runtime,
                                       u64 now_ms) noexcept {
    if (runtime->send_purpose != SendPurpose::none) {
        return OutboundDrive::sending;
    }
    if (runtime->has_pending_pong) {
        return send_pending_pong(runtime) ? OutboundDrive::sending : OutboundDrive::failed;
    }
    const OutboundDrive lifecycle = drive_gateway_outbound(client, runtime, now_ms);
    if (lifecycle != OutboundDrive::idle)
        return lifecycle;
    const auto voice = drive_voice_output(client, runtime, now_ms);
    if (voice != OutboundDrive::idle)
        return voice;
    const auto stream = drive_stream_output(client, runtime, now_ms);
    if (stream != OutboundDrive::idle)
        return stream;
    const auto presence = drive_self_status(runtime, now_ms);
    if (presence != OutboundDrive::idle)
        return presence;
    // Finite targeted batches precede last-write-wins viewport subscriptions;
    // continuous sidebar scrolling cannot starve visible-author metadata.
    const auto members = drive_member_requests(runtime, now_ms);
    if (members != OutboundDrive::idle)
        return members;
    const auto subscription = drive_guild_subscription(runtime, now_ms);
    if (subscription != OutboundDrive::idle)
        return subscription;
    const auto followed = drive_member_subscription(runtime, now_ms);
    return followed == OutboundDrive::idle ? drive_channel_info(runtime, now_ms) : followed;
}

PlaintextDrive fail_plaintext(GatewayClientRuntime* runtime, GatewayClientFailureStage stage) noexcept {
    runtime->failure_stage = stage;
    return PlaintextDrive::failed;
}

PlaintextDrive fail_session_event(GatewayClientRuntime* runtime,
                                  const GatewaySessionEvent& event) noexcept {
    runtime->failure_stage = GatewayClientFailureStage::session_event;
    runtime->session_error = event.error;
    runtime->handshake_error = event.handshake_error;
    runtime->websocket_error = event.websocket_error;
    runtime->gateway_decode_error = event.gateway_error;
    runtime->json_scan_error = event.scan_error;
    runtime->event_decode_error = event.event_error;
    runtime->compression_error = event.zstd_error;
    runtime->etf_error = event.etf_error;
    return PlaintextDrive::failed;
}

} // namespace client_detail
namespace {

bool flush_ready_users(ReadyStatePublishContext* context) noexcept {
    if (!context->user_count)
        return true;
    GatewayClientEvent event = gateway_dispatch_event(GatewayClientDispatchKind::user_snapshot,
                                                      context->sequence, context->now_ms);
    event.state.record_count = context->user_count;
    event.dispatch_payload_bytes = context->user_bytes;
    if (!publish_event_payload(
            context->client, context->runtime, event,
            {reinterpret_cast<const u8*>(context->payload_scratch.data), context->user_bytes})) {
        context->publish_failed = true;
        return false;
    }
    context->user_count = context->user_bytes = 0;
    return true;
}

bool publish_ready_state_record(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ReadyStatePublishContext*>(opaque);
    if (context->kind == ReadyRecordKind::user) {
        // Every maximum-size record, including serialized appearance, fits in
        // the existing batch buffer. Append directly; do not stage a second
        // copy in a smaller stack buffer.
        static_assert(64 * (sizeof(GatewayClientUserMutation) + 256 + 34 + 1024) <=
                      kHandshakeBytes - kMessageBytes);
        if (context->user_count == 64 && !flush_ready_users(context))
            return false;
        GatewayPayloadWriter batch{context->payload_scratch, context->user_bytes};
        context->route = gateway_append_user(value, context->object_scratch, 64, &batch);
        if (!context->route.ok())
            return false;
        context->user_bytes = batch.used;
        ++context->user_count;
        return true;
    }
    GatewayEnvelope envelope{};
    envelope.data = value;
    envelope.event_name =
        JsonString{context->kind == ReadyRecordKind::private_channel ? utf8_literal("CHANNEL_CREATE")
                                                                     : utf8_literal("READY_GUILD"),
                   0, false};
    envelope.sequence = context->sequence;
    envelope.has_sequence = true;
    envelope.has_event_name = true;

    GatewayClientEvent record{};
    ConstSlice<u8> payload{};
    context->route = gateway_client_route_dispatch(envelope, context->object_scratch, 64,
                                                   context->payload_scratch, Slice<u64>{nullptr, 0},
                                                   context->runtime->self_user_id, &record, &payload);
    const bool expected = context->kind == ReadyRecordKind::private_channel
                              ? record.dispatch_kind == GatewayClientDispatchKind::channel_create &&
                                    record.state.guild_id == 0 &&
                                    (record.state.type == 1 || record.state.type == 3)
                              : record.dispatch_kind == GatewayClientDispatchKind::guild_create;
    if (!context->route.ok() || !expected) {
        if (context->route.ok()) {
            context->route.error = GatewayClientDispatchRouteError::decode;
            context->route.event_error = GatewayEventDecodeError::invalid_data;
        }
        return false;
    }
    record.timestamp_ms = context->now_ms;
    if (!publish_event_payload(context->client, context->runtime, record, payload)) {
        context->publish_failed = true;
        return false;
    }
    return true;
}

} // namespace
namespace client_detail {

ReadyStatePublishContext publish_ready_state(GatewayClient* client, GatewayClientRuntime* runtime,
                                             JsonValue records, ReadyRecordKind kind, u64 sequence,
                                             u64 now_ms) noexcept {
    ReadyStatePublishContext context{};
    context.client = client;
    context.runtime = runtime;
    context.sequence = sequence;
    context.now_ms = now_ms;
    context.kind = kind;
    context.payload_scratch =
        Slice<char>{reinterpret_cast<char*>(runtime->handshake_output + kMessageBytes),
                    kHandshakeBytes - kMessageBytes};

    const JsonScanResult scan = json_scan_structurals_avx1(records.source, main_index_view(runtime));
    if (!scan.ok()) {
        context.route.error = GatewayClientDispatchRouteError::decode;
        context.route.event_error = GatewayEventDecodeError::scan_failed;
        context.route.scan_error = scan.error;
        context.route.error_offset = scan.error_offset;
        return context;
    }

    // The outer READY array still owns runtime->structural_scratch while its
    // visitor runs. The plaintext staging buffer is idle during inbound
    // dispatch, so reuse it as distinct per-record structural scratch instead
    // of reserving 128 KiB on the network-worker stack.
    context.object_scratch = object_scratch(runtime);
    if (kind == ReadyRecordKind::user && records.kind == JsonValueKind::object) {
        if (publish_ready_state_record(&context, 0, records))
            flush_ready_users(&context);
        return context;
    }
    const JsonReadResult read =
        json_visit_value_array(records, ConstSlice<u32>{main_index_view(runtime).data, scan.count},
                              64, publish_ready_state_record, &context);
    if (!read.ok() && context.route.ok() && !context.publish_failed) {
        context.route.error = GatewayClientDispatchRouteError::decode;
        context.route.event_error = GatewayEventDecodeError::invalid_json;
        context.route.read_error = read.error;
        context.route.error_offset = read.error_offset;
    }
    if (context.kind == ReadyRecordKind::user && context.route.ok() && !context.publish_failed)
        flush_ready_users(&context);
    return context;
}

PlaintextDrive drive_queued_actions(GatewayClient* client, GatewayClientRuntime* runtime,
                                    u64 now_ms) noexcept {
    const OutboundDrive outbound = drive_application_output(client, runtime, now_ms);
    return outbound == OutboundDrive::failed
               ? PlaintextDrive::failed
               : (outbound == OutboundDrive::sending ? PlaintextDrive::sending : PlaintextDrive::receive);
}

// A dispatch that cannot be handed to the consumer ends the connection; the session resumes.
PlaintextDrive resume_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    return schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_resume, now_ms, 0)
               ? PlaintextDrive::reconnecting
               : PlaintextDrive::failed;
}

// After a handled dispatch, so a RESUME continues behind it.
void checkpoint_resume_sequence(GatewayClientRuntime* runtime) noexcept {
    if (runtime->resume.valid && runtime->session.lifecycle.has_sequence)
        publish_resume_sequence(runtime, runtime->session.lifecycle.last_sequence);
}

// A decode failure as a route failure.
void set_decode_failure(GatewayClientDispatchRouteResult* routed, const GatewayEventDecodeResult& decoded) noexcept {
    routed->error = GatewayClientDispatchRouteError::decode;
    routed->event_error = decoded.error;
    routed->scan_error = decoded.scan_error;
    routed->read_error = decoded.read_error;
    routed->error_offset = decoded.error_offset;
}

} // namespace client_detail
} // namespace maicord
