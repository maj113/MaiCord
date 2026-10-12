#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
enum class VoiceHeartbeatPhase : u8 {
    stopped,
    awaiting_hello,
    running,
    failed
};
enum class VoiceHeartbeatFailure : u8 {
    none,
    hello_timeout,
    ack_timeout,
    nonce_exhausted
};
enum class VoiceHeartbeatAction : u8 {
    none,
    send,
    reconnect,
    stop
};
struct VoiceHeartbeatSend {
    u64 generation, nonce;
    i32 sequence_ack;
    VoiceHeartbeatAction action;
};
// Worker-owned state; contains no credentials and allocates no memory. All times
// are monotonic milliseconds. Generation belongs to a socket attempt; sequence
// may survive a resume, but outstanding heartbeat ACKs never do.
struct VoiceHeartbeat {
    u64 generation, hello_deadline_ms, next_send_ms, ack_deadline_ms;
    u64 last_nonce, first_unacknowledged_nonce, last_send_ms, last_ack_ms, round_trip_ms;
    u32 interval_ms;
    u16 sequence;
    VoiceHeartbeatPhase phase;
    VoiceHeartbeatFailure failure;
    bool has_sequence, queued, awaiting_ack, has_round_trip;
};
u64 voice_heartbeat_start(VoiceHeartbeat* heartbeat, u64 now_ms, bool resume) noexcept;
void voice_heartbeat_stop(VoiceHeartbeat* heartbeat) noexcept;
// Retire a socket while retaining the last sequence for a possible resume.
void voice_heartbeat_disconnect(VoiceHeartbeat* heartbeat) noexcept;
bool voice_heartbeat_hello(VoiceHeartbeat* heartbeat, u64 generation, u32 interval_ms, u64 now_ms) noexcept;
bool voice_heartbeat_sequence(VoiceHeartbeat* heartbeat, u64 generation, u16 sequence) noexcept;
bool voice_heartbeat_request(VoiceHeartbeat* heartbeat, u64 generation) noexcept;
VoiceHeartbeatSend voice_heartbeat_poll(VoiceHeartbeat* heartbeat, u64 now_ms) noexcept;
// Call after the serialized transport accepts the heartbeat send, not when
// merely encoding it. A failed/short encoding leaves the pending action intact.
bool voice_heartbeat_submitted(VoiceHeartbeat* heartbeat, const VoiceHeartbeatSend& send,
                               u64 now_ms) noexcept;
bool voice_heartbeat_ack(VoiceHeartbeat* heartbeat, u64 generation, u64 nonce, u64 now_ms) noexcept;
// Zero means no timed wake. Poll retires due send deadlines even under output
// backpressure; queued sends are retried when the transport becomes writable.
u64 voice_heartbeat_deadline(const VoiceHeartbeat& heartbeat) noexcept;
} // namespace maicord
