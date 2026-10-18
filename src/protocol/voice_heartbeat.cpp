#include "maicord/protocol/voice_heartbeat.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool current(const VoiceHeartbeat* heartbeat, u64 generation) noexcept {
    return generation && generation == heartbeat->generation &&
           (heartbeat->phase == VoiceHeartbeatPhase::awaiting_hello ||
            heartbeat->phase == VoiceHeartbeatPhase::running);
}
void halt(VoiceHeartbeat* heartbeat, VoiceHeartbeatPhase phase) noexcept {
    heartbeat->phase = phase;
    heartbeat->queued = heartbeat->awaiting_ack = false;
    heartbeat->hello_deadline_ms = heartbeat->next_send_ms = heartbeat->ack_deadline_ms = 0;
}
void fail(VoiceHeartbeat* heartbeat, VoiceHeartbeatFailure failure) noexcept {
    halt(heartbeat, VoiceHeartbeatPhase::failed);
    heartbeat->failure = failure;
}
} // namespace

void voice_heartbeat_stop(VoiceHeartbeat* heartbeat) noexcept {
    const u64 generation = heartbeat->generation;
    const u64 nonce = heartbeat->last_nonce;
    *heartbeat = {};
    heartbeat->generation = generation;
    heartbeat->last_nonce = nonce;
}

u64 voice_heartbeat_start(VoiceHeartbeat* heartbeat, u64 now_ms, bool resume) noexcept {
    const u16 sequence = heartbeat->sequence;
    const bool has_sequence = resume && heartbeat->has_sequence;
    voice_heartbeat_stop(heartbeat);
    if (heartbeat->generation == ~u64{0})
        return 0;
    ++heartbeat->generation;
    heartbeat->sequence = has_sequence ? sequence : 0;
    heartbeat->has_sequence = has_sequence;
    heartbeat->phase = VoiceHeartbeatPhase::awaiting_hello;
    heartbeat->hello_deadline_ms = deadline_after(now_ms, 20'000);
    return heartbeat->generation;
}

void voice_heartbeat_disconnect(VoiceHeartbeat* heartbeat) noexcept {
    halt(heartbeat, VoiceHeartbeatPhase::stopped);
}

bool voice_heartbeat_hello(VoiceHeartbeat* heartbeat, u64 generation, u32 interval_ms,
                           u64 now_ms) noexcept {
    if (!current(heartbeat, generation) || heartbeat->phase != VoiceHeartbeatPhase::awaiting_hello ||
        !interval_ms)
        return false;
    if (deadline_due(now_ms, heartbeat->hello_deadline_ms)) {
        fail(heartbeat, VoiceHeartbeatFailure::hello_timeout);
        return false;
    }
    heartbeat->hello_deadline_ms = 0;
    heartbeat->interval_ms = minimum(interval_ms, 5'000u);
    heartbeat->next_send_ms = deadline_after(now_ms, heartbeat->interval_ms);
    heartbeat->phase = VoiceHeartbeatPhase::running;
    return true;
}

bool voice_heartbeat_sequence(VoiceHeartbeat* heartbeat, u64 generation, u16 sequence) noexcept {
    if (!current(heartbeat, generation))
        return false;
    // WebSocket delivery is ordered. Taking a maximum would lose wraparound.
    heartbeat->sequence = sequence;
    heartbeat->has_sequence = true;
    return true;
}

bool voice_heartbeat_request(VoiceHeartbeat* heartbeat, u64 generation) noexcept {
    if (!current(heartbeat, generation) || heartbeat->phase != VoiceHeartbeatPhase::running)
        return false;
    heartbeat->queued = true;
    return true;
}

VoiceHeartbeatSend voice_heartbeat_poll(VoiceHeartbeat* heartbeat, u64 now_ms) noexcept {
    if (heartbeat->phase == VoiceHeartbeatPhase::awaiting_hello &&
        deadline_due(now_ms, heartbeat->hello_deadline_ms))
        fail(heartbeat, VoiceHeartbeatFailure::hello_timeout);
    if (heartbeat->phase == VoiceHeartbeatPhase::running) {
        if (heartbeat->awaiting_ack && deadline_due(now_ms, heartbeat->ack_deadline_ms))
            fail(heartbeat, VoiceHeartbeatFailure::ack_timeout);
        else if (deadline_due(now_ms, heartbeat->next_send_ms)) {
            heartbeat->next_send_ms = 0;
            heartbeat->queued = true;
        }
    }
    if (heartbeat->phase == VoiceHeartbeatPhase::failed)
        return {heartbeat->generation, 0, -1,
                heartbeat->failure == VoiceHeartbeatFailure::nonce_exhausted
                    ? VoiceHeartbeatAction::stop
                    : VoiceHeartbeatAction::reconnect};
    if (heartbeat->phase != VoiceHeartbeatPhase::running || !heartbeat->queued)
        return {};
    if (heartbeat->last_nonce == ~u64{0}) {
        fail(heartbeat, VoiceHeartbeatFailure::nonce_exhausted);
        return {heartbeat->generation, 0, -1, VoiceHeartbeatAction::stop};
    }
    return {heartbeat->generation, heartbeat->last_nonce + 1,
            heartbeat->has_sequence ? static_cast<i32>(heartbeat->sequence) : -1,
            VoiceHeartbeatAction::send};
}

bool voice_heartbeat_submitted(VoiceHeartbeat* heartbeat, const VoiceHeartbeatSend& send,
                               u64 now_ms) noexcept {
    if (!current(heartbeat, send.generation) || heartbeat->phase != VoiceHeartbeatPhase::running ||
        !heartbeat->queued || send.action != VoiceHeartbeatAction::send ||
        heartbeat->last_nonce == ~u64{0} || send.nonce != heartbeat->last_nonce + 1)
        return false;
    // Do not revive a send prepared before its outstanding ACK deadline expired.
    if (heartbeat->awaiting_ack && deadline_due(now_ms, heartbeat->ack_deadline_ms)) {
        fail(heartbeat, VoiceHeartbeatFailure::ack_timeout);
        return false;
    }
    heartbeat->last_nonce = send.nonce;
    heartbeat->last_send_ms = now_ms;
    heartbeat->queued = false;
    if (!heartbeat->awaiting_ack) {
        heartbeat->awaiting_ack = true;
        heartbeat->first_unacknowledged_nonce = send.nonce;
        heartbeat->ack_deadline_ms = deadline_after(now_ms, heartbeat->interval_ms);
    }
    // Urgent server requests keep the regular cadence. Delayed periodic sends
    // start a new interval at submission rather than replaying missed ticks.
    if (!heartbeat->next_send_ms || heartbeat->next_send_ms <= now_ms)
        heartbeat->next_send_ms = deadline_after(now_ms, heartbeat->interval_ms);
    return true;
}

bool voice_heartbeat_ack(VoiceHeartbeat* heartbeat, u64 generation, u64 nonce, u64 now_ms) noexcept {
    if (!current(heartbeat, generation) || !heartbeat->awaiting_ack ||
        nonce < heartbeat->first_unacknowledged_nonce || nonce > heartbeat->last_nonce ||
        now_ms < heartbeat->last_send_ms)
        return false;
    if (deadline_due(now_ms, heartbeat->ack_deadline_ms)) {
        fail(heartbeat, VoiceHeartbeatFailure::ack_timeout);
        return false;
    }
    // Nonces are consecutive, incremented only for accepted sends, so any nonce
    // in this interval was actually sent and needs no cache. A matching ACK
    // proves liveness. Only the newest send has a stored timestamp for an RTT
    // sample.
    if (nonce == heartbeat->last_nonce) {
        heartbeat->round_trip_ms = now_ms - heartbeat->last_send_ms;
        heartbeat->has_round_trip = true;
    }
    heartbeat->awaiting_ack = false;
    heartbeat->ack_deadline_ms = 0;
    return true;
}

u64 voice_heartbeat_deadline(const VoiceHeartbeat& heartbeat) noexcept {
    return earliest_deadline(heartbeat.hello_deadline_ms,
                             earliest_deadline(heartbeat.next_send_ms, heartbeat.ack_deadline_ms));
}
} // namespace maicord
