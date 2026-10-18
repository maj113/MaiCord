#include "maicord/app/gateway_stream_control.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool push(GatewayStreamControl* stream, GatewayStreamCommandKind kind, const GatewayStreamKey& key) noexcept {
    // A queued ping for the same key is redundant with any later command.
    if (kind == GatewayStreamCommandKind::ping)
        for (u32 i = 0; i != stream->queue_count; ++i) {
            const auto& queued = stream->queue[(stream->queue_head + i) % kGatewayStreamCommandQueue];
            if (queued.kind == kind && queued.key == key)
                return true;
        }
    if (stream->queue_count == kGatewayStreamCommandQueue)
        return false;
    stream->queue[(stream->queue_head + stream->queue_count) % kGatewayStreamCommandQueue] = {key, kind};
    ++stream->queue_count;
    return true;
}
// A watch that was never sent needs no delete: drop it from the queue.
bool unsend_watch(GatewayStreamControl* stream, const GatewayStreamKey& key) noexcept {
    for (u32 i = 0; i != stream->queue_count; ++i) {
        const u32 at = (stream->queue_head + i) % kGatewayStreamCommandQueue;
        if (stream->queue[at].kind != GatewayStreamCommandKind::watch || !(stream->queue[at].key == key))
            continue;
        for (u32 k = i; k + 1 < stream->queue_count; ++k)
            stream->queue[(stream->queue_head + k) % kGatewayStreamCommandQueue] =
                stream->queue[(stream->queue_head + k + 1) % kGatewayStreamCommandQueue];
        --stream->queue_count;
        return true;
    }
    return false;
}
void clear_session(GatewayStreamControl* stream, GatewayStreamEnd end) noexcept {
    voice_setup_leave(&stream->setup);
    stream->rtc_server_id = stream->rtc_channel_id = 0;
    stream->create_deadline_ms = 0;
    stream->viewer_count = 0;
    stream->paused = false;
    stream->reconnect_seen = false;
    stream->phase = end == GatewayStreamEnd::none ? GatewayStreamPhase::idle : GatewayStreamPhase::ended;
    stream->end = end;
}
bool matches(const GatewayStreamControl& stream, const GatewayStreamKey& key) noexcept {
    return gateway_stream_active(stream) && stream.key == key;
}
} // namespace

bool gateway_stream_bind(GatewayStreamControl* stream, u64 user_id, Utf8View session) noexcept {
    if (gateway_stream_active(*stream))
        gateway_stream_reset(stream, GatewayStreamEnd::session);
    return voice_setup_bind_utf8(&stream->setup, user_id, session) == VoiceSetupResult::updated;
}

bool gateway_stream_active(const GatewayStreamControl& stream) noexcept {
    return stream.phase != GatewayStreamPhase::idle && stream.phase != GatewayStreamPhase::ended;
}

bool gateway_stream_watch(GatewayStreamControl* stream, const GatewayStreamKey& key, u64 now_ms) noexcept {
    if (!gateway_stream_key_valid(key) || !stream->setup.user_id || !stream->setup.login_session.bytes)
        return false;
    if (matches(*stream, key))
        return true;
    if (gateway_stream_active(*stream)) {
        if (stream->queue_count + 2 > kGatewayStreamCommandQueue)
            return false;
        gateway_stream_stop(stream, GatewayStreamEnd::stopped);
    }
    if (!push(stream, GatewayStreamCommandKind::watch, key))
        return false;
    clear_session(stream, GatewayStreamEnd::none);
    stream->key = key;
    stream->delete_reason = GatewayStreamDeleteReason::none;
    stream->phase = GatewayStreamPhase::requested;
    stream->create_deadline_ms = deadline_after(now_ms, kGatewayStreamCreateTimeoutMs);
    return true;
}

void gateway_stream_stop(GatewayStreamControl* stream, GatewayStreamEnd end) noexcept {
    if (!gateway_stream_active(*stream))
        return;
    // Unsent watch: the server never heard of it. Otherwise tell it we left;
    // a full queue only skips the courtesy delete.
    if (!unsend_watch(stream, stream->key))
        push(stream, GatewayStreamCommandKind::remove, stream->key);
    clear_session(stream, end == GatewayStreamEnd::none ? GatewayStreamEnd::stopped : end);
}

bool gateway_stream_next(const GatewayStreamControl& stream, GatewayStreamCommand* output) noexcept {
    if (!stream.queue_count)
        return false;
    *output = stream.queue[stream.queue_head];
    return true;
}

bool gateway_stream_submitted(GatewayStreamControl* stream, const GatewayStreamCommand& command) noexcept {
    GatewayStreamCommand expected{};
    if (!gateway_stream_next(*stream, &expected) || expected.kind != command.kind ||
        !(expected.key == command.key))
        return false;
    stream->queue_head = (stream->queue_head + 1) % kGatewayStreamCommandQueue;
    --stream->queue_count;
    return true;
}

GatewayCommandWriteResult gateway_stream_write(const GatewayStreamCommand& command, Slice<u8> output) noexcept {
    switch (command.kind) {
    case GatewayStreamCommandKind::watch:
        return gateway_write_stream_watch(command.key, output);
    case GatewayStreamCommandKind::remove:
        return gateway_write_stream_delete(command.key, output);
    case GatewayStreamCommandKind::ping:
        return gateway_write_stream_ping(command.key, output);
    default:
        return {0, GatewayCommandError::invalid_input};
    }
}

void gateway_stream_create(GatewayStreamControl* stream, const GatewayStreamCreate& event, u64 now_ms) noexcept {
    if (!matches(*stream, event.key))
        return;
    stream->viewer_count = event.viewer_count;
    stream->paused = event.paused;
    stream->create_deadline_ms = 0;
    if (!event.rtc_server_id)
        return; // No media server yet; a later STREAM_CREATE carries it.
    const u64 channel = event.rtc_channel_id ? event.rtc_channel_id : event.rtc_server_id - 1;
    if (stream->rtc_server_id == event.rtc_server_id && stream->rtc_channel_id == channel)
        return;
    if (!voice_setup_stream_begin(&stream->setup, event.rtc_server_id, channel, now_ms)) {
        gateway_stream_stop(stream, GatewayStreamEnd::invalid_event);
        return;
    }
    stream->rtc_server_id = event.rtc_server_id;
    stream->rtc_channel_id = channel;
    stream->reconnect_seen = false;
    stream->phase = GatewayStreamPhase::awaiting_server;
}

void gateway_stream_server(GatewayStreamControl* stream, const GatewayStreamServer& event) noexcept {
    if (!matches(*stream, event.key) || !stream->rtc_server_id)
        return;
    const auto result = voice_setup_stream_server(&stream->setup, event.token, event.endpoint, event.unavailable);
    if (result == VoiceSetupResult::invalid_input || result == VoiceSetupResult::storage_failed ||
        result == VoiceSetupResult::disconnected) {
        gateway_stream_stop(stream, result == VoiceSetupResult::storage_failed ? GatewayStreamEnd::storage
                                                                                : GatewayStreamEnd::invalid_event);
        return;
    }
    if (result == VoiceSetupResult::credentials_ready) {
        stream->phase = GatewayStreamPhase::connecting;
        stream->reconnect_seen = false;
    } else if (result == VoiceSetupResult::updated)
        stream->phase = GatewayStreamPhase::awaiting_server; // Reallocating.
}

void gateway_stream_update(GatewayStreamControl* stream, const GatewayStreamUpdate& event) noexcept {
    if (!matches(*stream, event.key))
        return;
    if (event.has_viewers)
        stream->viewer_count = event.viewer_count;
    if (event.has_paused)
        stream->paused = event.paused;
}

void gateway_stream_delete(GatewayStreamControl* stream, const GatewayStreamDelete& event) noexcept {
    // The media server may close the stream socket before the Gateway reports
    // the end: a late delete still explains why the stream stopped.
    if (stream->phase == GatewayStreamPhase::ended && stream->end == GatewayStreamEnd::failed &&
        stream->key == event.key) {
        stream->end = GatewayStreamEnd::deleted;
        stream->delete_reason = event.reason;
        return;
    }
    if (!matches(*stream, event.key))
        return;
    unsend_watch(stream, stream->key);
    clear_session(stream, GatewayStreamEnd::deleted);
    stream->delete_reason = event.reason;
}

bool gateway_stream_dispatch(GatewayStreamControl* stream, const GatewayEnvelope& envelope, Slice<u32> scratch,
                             u32 maximum_depth, u64 now_ms) noexcept {
    const auto& name = envelope.event_name;
    if (json_string_equals_ascii(name, utf8_literal("STREAM_CREATE"))) {
        GatewayStreamCreate event{};
        if (gateway_decode_stream_create(envelope.data, scratch, maximum_depth, &event).ok())
            gateway_stream_create(stream, event, now_ms);
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("STREAM_SERVER_UPDATE"))) {
        GatewayStreamServer event{};
        if (!gateway_stream_active(*stream))
            return true;
        if (gateway_decode_stream_server(envelope.data, scratch, maximum_depth, &event).ok())
            gateway_stream_server(stream, event);
        else if (stream->phase == GatewayStreamPhase::awaiting_server)
            gateway_stream_stop(stream, GatewayStreamEnd::invalid_event);
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("STREAM_UPDATE"))) {
        GatewayStreamUpdate event{};
        if (gateway_decode_stream_update(envelope.data, scratch, maximum_depth, &event).ok())
            gateway_stream_update(stream, event);
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("STREAM_DELETE"))) {
        GatewayStreamDelete event{};
        if (gateway_decode_stream_delete(envelope.data, scratch, maximum_depth, &event).ok())
            gateway_stream_delete(stream, event);
        return true;
    }
    return false;
}

void gateway_stream_observe(GatewayStreamControl* stream, const GatewayStreamConnectionState& state) noexcept {
    if (stream->phase != GatewayStreamPhase::connecting && stream->phase != GatewayStreamPhase::active &&
        stream->phase != GatewayStreamPhase::reconnecting)
        return;
    if (state.failed) {
        gateway_stream_stop(stream, GatewayStreamEnd::failed);
        return;
    }
    if (state.reconnecting) {
        if (!stream->reconnect_seen) {
            // As the official client: a stream RTC loss that will reconnect pings.
            push(stream, GatewayStreamCommandKind::ping, stream->key);
            stream->reconnect_seen = true;
        }
        stream->phase = GatewayStreamPhase::reconnecting;
        return;
    }
    if (state.media_attached) {
        stream->reconnect_seen = false;
        stream->phase = GatewayStreamPhase::active;
    }
}

void gateway_stream_voice(GatewayStreamControl* stream, u64 voice_channel_id, bool in_call) noexcept {
    if (gateway_stream_active(*stream) && (!in_call || voice_channel_id != stream->key.channel_id))
        gateway_stream_stop(stream, GatewayStreamEnd::left_channel);
}

bool gateway_stream_expire(GatewayStreamControl* stream, u64 now_ms) noexcept {
    if (!gateway_stream_active(*stream))
        return false;
    if (!deadline_due(now_ms, stream->create_deadline_ms) && !deadline_due(now_ms, stream->setup.state_deadline_ms))
        return false;
    gateway_stream_stop(stream, GatewayStreamEnd::timeout);
    return true;
}

u64 gateway_stream_deadline(const GatewayStreamControl& stream) noexcept {
    return gateway_stream_active(stream) ? earliest_deadline(stream.create_deadline_ms, stream.setup.state_deadline_ms)
                                         : 0;
}

void gateway_stream_reset(GatewayStreamControl* stream, GatewayStreamEnd end) noexcept {
    stream->queue_head = stream->queue_count = 0;
    if (gateway_stream_active(*stream))
        clear_session(stream, end);
}

void gateway_stream_release(GatewayStreamControl* stream) noexcept {
    gateway_stream_reset(stream, GatewayStreamEnd::session);
    voice_setup_release(&stream->setup);
    stream->phase = GatewayStreamPhase::idle;
    stream->end = GatewayStreamEnd::none;
    stream->key = {};
}
} // namespace maicord
