#include "client_internal.hpp"
#include "user_payload.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {

// Takes one send from the outbound rate limit; otherwise records when the next one is allowed.
bool acquire_send(GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    const auto rate = gateway_rate_limit_acquire(&runtime->outbound.limiter, now_ms);
    if (!rate.acquired)
        runtime->rate_deadline_ms = rate.next_allowed_ms;
    return rate.acquired;
}

u64 mix_ranges(u64 hash, const GatewayMemberSubscriptionRange* ranges, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        hash = hash64_mix(hash64_mix(hash, ranges[i].first), ranges[i].last);
    return hash;
}

// The member list ranges of every channel the frame names.
u64 ranges_digest(const GatewayGuildSubscription& subscription) noexcept {
    const GatewayMemberSubscriptionRange single{subscription.range_first, subscription.range_last};
    u64 hash = mix_ranges(hash64_mix(kHash64Seed, subscription.channel_id),
                          subscription.range_count ? subscription.ranges : &single,
                          subscription.range_count ? subscription.range_count : 1);
    for (u32 c = 0; c < subscription.channel_count; ++c)
        hash = mix_ranges(hash64_mix(hash, subscription.channels[c].channel_id), subscription.channels[c].ranges,
                          subscription.channels[c].range_count);
    return hash;
}

// As the desktop client's subscription store: a guild's typing, activities and threads go once a session, its
// member list ranges again only when they changed, and thread member lists only to set or clear one. Returns the
// guild's sent record (null before its first frame) and the frame's ranges digest (zero without ranges); false when
// the server already holds everything the frame says.
bool shape_guild_subscription(GatewayClientRuntime* runtime, GatewayGuildSubscription* frame,
                              GatewayClientRuntime::SentGuildSubscription** sent, u64* ranges) noexcept {
    *sent = nullptr;
    for (u32 i = 0; i < runtime->sent_guild_count && !*sent; ++i)
        if (runtime->sent_guilds[i].guild_id == frame->guild_id)
            *sent = &runtime->sent_guilds[i];
    if (*sent)
        frame->omit_features = true;
    *ranges = frame->omit_channel_ranges ? 0 : ranges_digest(*frame);
    if (*sent && *ranges && (*sent)->ranges_digest == *ranges) {
        frame->omit_channel_ranges = true;
        frame->channel_id = 0;
        frame->range_count = frame->channel_count = 0;
    }
    if (frame->thread_member_lists_present && !frame->thread_id && !frame->clear_thread_guild_id &&
        runtime->thread_subscription_owner.sent_guild_id != frame->guild_id)
        frame->thread_member_lists_present = false;
    return !frame->omit_features || !frame->omit_channel_ranges || frame->members_present ||
           frame->thread_member_lists_present;
}

// Records a guild frame being written.
void note_guild_subscription(GatewayClientRuntime* runtime, GatewayClientRuntime::SentGuildSubscription* sent,
                             u64 guild_id, u64 ranges) noexcept {
    if (!sent && runtime->sent_guild_count < 16)
        sent = &runtime->sent_guilds[runtime->sent_guild_count++];
    if (!sent)
        return;
    sent->guild_id = guild_id;
    if (ranges)
        sent->ranges_digest = ranges;
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
    GatewayClientRuntime::SentGuildSubscription* sent = nullptr;
    u64 ranges = 0;
    if (!shape_guild_subscription(runtime, &prepared, &sent, &ranges)) {
        runtime->sent_subscription_revision = revision; // The server already holds all of it.
        return OutboundDrive::idle;
    }
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const GatewayCommandWriteResult command = gateway_write_guild_subscriptions_bulk(
        prepared, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_subscription);
    if (result == OutboundDrive::sending) {
        runtime->active_subscription_revision = revision;
        gateway_subscription_thread_owner_admit(&runtime->thread_subscription_owner, prepared);
        note_guild_subscription(runtime, sent, prepared.guild_id, ranges);
    }
    return result;
}

// The desktop client releases a followed user a minute after the view stops showing them, so a user who drops out
// and comes back (an author list refilling after a reload) sends nothing.
inline constexpr u64 kGatewayMemberReleaseDelayMs = 60'000;

bool members_within(const GatewayGuildSubscription& desired, const u64* sent, u32 sent_count) noexcept {
    for (u32 i = 0; i < desired.member_count; ++i) {
        bool found = false;
        for (u32 j = 0; j < sent_count && !found; ++j)
            found = sent[j] == desired.members[i];
        if (!found)
            return false;
    }
    return true;
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
    const bool same_guild = runtime->sent_members_guild == subscription.guild_id;
    if (same_guild && members_within(subscription, runtime->sent_member_ids, runtime->sent_member_count)) {
        if (subscription.member_count == runtime->sent_member_count) {
            runtime->sent_members_revision = revision; // The server already follows exactly these users.
            runtime->members_release_ms = 0;
            return OutboundDrive::idle;
        }
        if (!runtime->members_release_ms)
            runtime->members_release_ms = deadline_after(now_ms, kGatewayMemberReleaseDelayMs);
        if (now_ms < runtime->members_release_ms)
            return OutboundDrive::idle; // Only releases: they wait out the delay.
    }
    if (!acquire_send(runtime, now_ms))
        return OutboundDrive::rate_limited;
    const GatewayCommandWriteResult command = gateway_write_guild_subscriptions_bulk(
        subscription, Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    const auto result = send_control_command(runtime, command, SendPurpose::guild_members);
    if (result == OutboundDrive::sending) {
        runtime->sent_members_revision = revision;
        runtime->members_release_ms = 0;
        runtime->sent_members_guild = subscription.guild_id;
        runtime->sent_member_count = subscription.member_count;
        copy_bytes(runtime->sent_member_ids, subscription.members, subscription.member_count * sizeof(u64));
    }
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

// Opcode 3 for `presence` into gateway_payload; with activities, the custom status and the shared game the session
// publishes. Caller holds subscription_lock.
GatewayCommandWriteResult write_self_presence(GatewayClientRuntime* runtime, const GatewayPresenceUpdate& presence,
                                              bool activities) noexcept {
    SelfCustomStatus custom{};
    GatewayCustomStatusActivity activity{};
    const ConstSlice<u8> status_group{runtime->self_status_group, runtime->self_status_group_bytes};
    const bool visible = activities && presence.status != GatewayUserStatus::invisible;
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
    return gateway_write_presence_update(presence, publish ? &activity : nullptr, game ? &played : nullptr,
                                         {runtime->gateway_payload, sizeof(runtime->gateway_payload)});
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
    const auto written =
        write_self_presence(runtime, gateway_presence_effective(owner.desired, &runtime->local_presence), true);
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

GatewayPresenceUpdate ready_presence(const GatewayClientRuntime* runtime) noexcept {
    const auto& owner = runtime->self_status;
    return gateway_presence_effective(
        owner.authoritative ? owner.desired : GatewayPresenceUpdate{0, GatewayUserStatus::online, false, false},
        &runtime->local_presence);
}

// As the desktop client's connection store does on CONNECTION_OPEN: it commits the presence its identify carried
// unless that is plain Online (its starting state), then the voice state, then the presence the session settles on
// once it differs from the first. The server then holds the owner's status: drive_self_status adds nothing.
void queue_ready_outputs(GatewayClientRuntime* runtime) noexcept {
    static constexpr char plain[] = R"({"op":3,"d":{"status":"online","since":0,"activities":[],"afk":false}})";
    const u8* payload = runtime->gateway_payload;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    const GatewayPresenceUpdate ready = ready_presence(runtime);
    const auto initial = write_self_presence(runtime, runtime->identify_presence, false);
    const u64 initial_digest = hash64_bytes(kHash64Seed, payload, initial.bytes);
    const bool initial_plain = initial.bytes == sizeof(plain) - 1 && bytes_equal(payload, plain, initial.bytes);
    const auto settled = write_self_presence(runtime, ready, true);
    const bool changed = settled.ok() && hash64_bytes(kHash64Seed, payload, settled.bytes) != initial_digest;
    if (initial.ok() && !initial_plain)
        gateway_outbound_enqueue(&runtime->outbound, GatewayLifecycleAction::send_identify_presence);
    gateway_outbound_enqueue(&runtime->outbound, GatewayLifecycleAction::send_idle_voice_state);
    if (changed)
        gateway_outbound_enqueue(&runtime->outbound, GatewayLifecycleAction::send_ready_presence);
    else if (owner.authoritative)
        owner.transmitted_revision = owner.revision;
    note_launch_presence(ready.status);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}

// One of the post-READY frames; the outbound queue already took its send from the rate limit.
OutboundDrive send_ready_output(GatewayClientRuntime* runtime, GatewayLifecycleAction action, u64 now_ms) noexcept {
    if (action == GatewayLifecycleAction::send_idle_voice_state) {
        AcquireSRWLockShared(&runtime->voice_lock);
        const GatewayVoiceStateUpdate idle{0, 0, runtime->desired_voice.self_mute, runtime->desired_voice.self_deaf,
                                           false, false};
        ReleaseSRWLockShared(&runtime->voice_lock);
        return send_control_command(
            runtime,
            gateway_write_voice_state(idle,
                                      runtime->voice_recording_disallowed ? 0 : kGatewayVoiceStateAllowVoiceRecording,
                                      {runtime->gateway_payload, static_cast<u32>(sizeof(runtime->gateway_payload))}),
            SendPurpose::gateway_action);
    }
    const bool settled = action == GatewayLifecycleAction::send_ready_presence;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    const auto written = write_self_presence(runtime, settled ? ready_presence(runtime) : runtime->identify_presence,
                                             settled);
    const auto sent = send_control_command(runtime, written, settled ? SendPurpose::self_status
                                                                     : SendPurpose::gateway_action);
    if (sent == OutboundDrive::sending && settled && owner.authoritative) {
        gateway_self_status_deadline(&owner, now_ms);
        gateway_self_status_admit(&owner, owner.revision, now_ms);
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return sent;
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

// A dispatch that cannot be handed to the consumer ends the connection as the page's socket reset does (a close
// frame without a code); the session resumes.
PlaintextDrive resume_reconnect(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    return schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_resume, now_ms, 0, kCloseWithoutCode)
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
