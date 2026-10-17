#include "maicord/protocol/voice_heartbeat.hpp"
#include "maicord/protocol/voice_gateway.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_events.hpp"

namespace maicord {
void test_voice_heartbeat(void (*check)(bool, const char*) noexcept) noexcept {
    VoiceHeartbeat heartbeat{};
    check(!voice_heartbeat_deadline(heartbeat) &&
              voice_heartbeat_poll(&heartbeat, 0).action == VoiceHeartbeatAction::none,
          "idle heartbeat has no timers or work");
    u64 generation = voice_heartbeat_start(&heartbeat, 100, false);
    check(generation && voice_heartbeat_deadline(heartbeat) == 20100 &&
              !voice_heartbeat_request(&heartbeat, generation),
          "socket attempt waits for Hello before heartbeating");
    u32 scratch[128]{};
    VoiceGatewayEnvelope envelope{};
    GatewayHello hello{};
    check(voice_gateway_decode_json(utf8_literal(R"({"op":8,"d":{"heartbeat_interval":41250,"v":9}})"),
                                    {scratch, 128}, 16, &envelope)
                  .ok() &&
              gateway_decode_hello(envelope.data, {scratch, 128}, 16, &hello).ok() &&
              voice_heartbeat_hello(&heartbeat, generation, hello.heartbeat_interval_ms, 200) &&
              heartbeat.interval_ms == 5000 && voice_heartbeat_deadline(heartbeat) == 5200,
          "v9 Hello drives the normal-user five-second heartbeat cap");
    check(!voice_heartbeat_hello(&heartbeat, generation, 1000, 300),
          "duplicate Hello cannot reset active ACK tracking");
    check(voice_heartbeat_poll(&heartbeat, 5199).action == VoiceHeartbeatAction::none,
          "first heartbeat waits for its scheduled interval");
    auto send = voice_heartbeat_poll(&heartbeat, 5200);
    check(send.action == VoiceHeartbeatAction::send && send.sequence_ack == -1 && send.nonce == 1 &&
              !heartbeat.awaiting_ack && !voice_heartbeat_deadline(heartbeat),
          "due send removes its timer but does not await an ACK before transport accepts it");
    u8 small[1]{0xCC};
    check(!voice_gateway_write_heartbeat(send.nonce, send.sequence_ack, {small, 1}).ok() &&
              small[0] == 0xCC && voice_heartbeat_poll(&heartbeat, 9000).nonce == send.nonce &&
              !heartbeat.last_nonce,
          "short encoding and output backpressure neither consume a nonce nor spin an expired timer");
    check(voice_heartbeat_submitted(&heartbeat, send, 9000) && heartbeat.next_send_ms == 14000 &&
              heartbeat.ack_deadline_ms == 14000 && !voice_heartbeat_submitted(&heartbeat, send, 9001),
          "accepted delayed send starts one new interval and rejects duplicate submission");
    check(!voice_heartbeat_ack(&heartbeat, generation, 0, 9050) &&
              !voice_heartbeat_ack(&heartbeat, generation, 2, 9050) && heartbeat.awaiting_ack,
          "unsent nonces cannot satisfy liveness");
    u64 nonce = 99;
    check(voice_gateway_decode_json(utf8_literal(R"({"op":6,"d":{"t":1}})"), {scratch, 128}, 16, &envelope)
                  .ok() &&
              voice_gateway_decode_heartbeat_ack(envelope.data, {scratch, 128}, 16, &nonce).ok() &&
              voice_heartbeat_ack(&heartbeat, generation, nonce, 9100) && heartbeat.round_trip_ms == 100 &&
              heartbeat.has_round_trip && !heartbeat.ack_deadline_ms && heartbeat.next_send_ms == 14000,
          "typed ACK feeds monotonic RTT and clears the independent ACK deadline");
    check(!voice_heartbeat_ack(&heartbeat, generation, nonce, 9200), "duplicate ACK is ignored");
    voice_heartbeat_sequence(&heartbeat, generation, 65535);
    voice_heartbeat_sequence(&heartbeat, generation, 0);
    send = voice_heartbeat_poll(&heartbeat, 14000);
    check(send.sequence_ack == 0 && voice_heartbeat_submitted(&heartbeat, send, 14000),
          "heartbeat includes wrapped sequence zero");
    // Many urgent server requests need no ring/cache of nonces and cannot
    // keep extending the original ACK deadline when the peer is silent.
    for (u32 i = 0; i < 32; ++i) {
        voice_heartbeat_request(&heartbeat, generation);
        const auto urgent = voice_heartbeat_poll(&heartbeat, 14001 + i);
        check(urgent.action == VoiceHeartbeatAction::send &&
                  voice_heartbeat_submitted(&heartbeat, urgent, 14001 + i) &&
                  heartbeat.ack_deadline_ms == 19000 && heartbeat.next_send_ms == 19000,
              "urgent heartbeats retain the original timeout and regular cadence");
    }
    check(voice_heartbeat_ack(&heartbeat, generation, send.nonce, 15000) && !heartbeat.awaiting_ack,
          "ACK for any actually sent outstanding heartbeat proves liveness");
    send = voice_heartbeat_poll(&heartbeat, 19000);
    check(voice_heartbeat_submitted(&heartbeat, send, 19000) &&
              !voice_heartbeat_ack(&heartbeat, generation, send.nonce - 1, 19100),
          "an older acknowledged range cannot satisfy the next heartbeat");
    check(voice_heartbeat_poll(&heartbeat, 24000).action == VoiceHeartbeatAction::reconnect &&
              heartbeat.failure == VoiceHeartbeatFailure::ack_timeout &&
              !voice_heartbeat_deadline(heartbeat) &&
              !voice_heartbeat_ack(&heartbeat, generation, send.nonce, 24001),
          "missing ACK retires all deadlines and rejects late callbacks");
    const u64 old_generation = generation;
    generation = voice_heartbeat_start(&heartbeat, 25000, true);
    check(generation != old_generation && heartbeat.has_sequence && !heartbeat.sequence &&
              !heartbeat.awaiting_ack && !voice_heartbeat_submitted(&heartbeat, send, 25001) &&
              !voice_heartbeat_sequence(&heartbeat, old_generation, 5),
          "resume retains sequence but rejects callbacks from the previous socket attempt");
    check(!voice_heartbeat_hello(&heartbeat, generation, 0, 25001) &&
              voice_heartbeat_hello(&heartbeat, generation, 1000, 25001) && heartbeat.interval_ms == 1000,
          "small server intervals are honored and zero intervals rejected");
    voice_heartbeat_request(&heartbeat, generation);
    send = voice_heartbeat_poll(&heartbeat, 25002);
    check(send.nonce > 1 && voice_heartbeat_submitted(&heartbeat, send, 25002),
          "nonces remain unique across socket attempts");
    voice_heartbeat_request(&heartbeat, generation);
    auto prepared = voice_heartbeat_poll(&heartbeat, 25003);
    check(!voice_heartbeat_submitted(&heartbeat, prepared, 26002) &&
              heartbeat.failure == VoiceHeartbeatFailure::ack_timeout &&
              !voice_heartbeat_deadline(heartbeat),
          "a prepared send cannot revive an expired connection after transport stalls");
    generation = voice_heartbeat_start(&heartbeat, 30000, false);
    check(!heartbeat.has_sequence &&
              voice_heartbeat_poll(&heartbeat, 49999).action == VoiceHeartbeatAction::none &&
              voice_heartbeat_poll(&heartbeat, 50000).action == VoiceHeartbeatAction::reconnect &&
              heartbeat.failure == VoiceHeartbeatFailure::hello_timeout &&
              !voice_heartbeat_deadline(heartbeat),
          "fresh attempts clear sequence and Hello timeout removes its wakeup");
    generation = voice_heartbeat_start(&heartbeat, 60000, false);
    check(!voice_heartbeat_hello(&heartbeat, generation, 5000, 80000) &&
              !voice_heartbeat_deadline(heartbeat),
          "late Hello is rejected even before a timer callback runs");
    generation = voice_heartbeat_start(&heartbeat, 90000, false);
    voice_heartbeat_hello(&heartbeat, generation, 5000, 90001);
    send = voice_heartbeat_poll(&heartbeat, 1'000'000);
    check(send.action == VoiceHeartbeatAction::send &&
              voice_heartbeat_submitted(&heartbeat, send, 1'000'000) &&
              heartbeat.next_send_ms == 1'005'000 &&
              voice_heartbeat_poll(&heartbeat, 1'000'001).action == VoiceHeartbeatAction::none,
          "long scheduling pauses do not replay missed heartbeat intervals");
    voice_heartbeat_stop(&heartbeat);
    check(!voice_heartbeat_deadline(heartbeat) && !heartbeat.has_sequence &&
              !voice_heartbeat_request(&heartbeat, generation),
          "terminal stop disables timers and callbacks");
    generation = voice_heartbeat_start(&heartbeat, ~u64{0} - 1, false);
    check(voice_heartbeat_deadline(heartbeat) == ~u64{0} &&
              voice_heartbeat_poll(&heartbeat, ~u64{0}).action == VoiceHeartbeatAction::reconnect,
          "Hello deadline arithmetic saturates instead of wrapping into a busy loop");
    generation = voice_heartbeat_start(&heartbeat, 1, false);
    voice_heartbeat_hello(&heartbeat, generation, 1, 1);
    heartbeat.last_nonce = ~u64{0};
    check(voice_heartbeat_poll(&heartbeat, 2).action == VoiceHeartbeatAction::stop &&
              heartbeat.failure == VoiceHeartbeatFailure::nonce_exhausted &&
              !voice_heartbeat_deadline(heartbeat),
          "nonce exhaustion stops rather than reusing an old nonce or reconnecting forever");

    const Utf8View invalid[] = {utf8_literal(R"({})"),
                                utf8_literal(R"({"t":1,"\u0074":2})"),
                                utf8_literal(R"({"t":"1"})"),
                                utf8_literal(R"({"t":-1})"),
                                utf8_literal(R"({"t":18446744073709551616})"),
                                utf8_literal(R"({"t":1,"x":[1,]})")};
    for (const auto json : invalid) {
        nonce = 99;
        check(!voice_gateway_decode_heartbeat_ack({json, 0, JsonValueKind::object}, {scratch, 128}, 16,
                                                  &nonce)
                      .ok() &&
                  nonce == 99,
              "malformed ACK payload never partially publishes a nonce");
    }
}
} // namespace maicord
