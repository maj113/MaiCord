#include "maicord/protocol/voice_session.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr auto key = utf8_literal("dGhlIHNhbXBsZSBub25jZQ==");
constexpr auto opening =
    utf8_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                 "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
u32 frame(u8* out, u8 opcode, bool final, ConstSlice<u8> bytes) noexcept {
    out[0] = static_cast<u8>(opcode | (final ? 0x80 : 0));
    out[1] = static_cast<u8>(bytes.count);
    copy_bytes(out + 2, bytes.data, bytes.count);
    return bytes.count + 2;
}
ConstSlice<u8> bytes(Utf8View text) noexcept {
    return {reinterpret_cast<const u8*>(text.data), text.bytes};
}
} // namespace

void test_voice_session(void (*check)(bool, const char*) noexcept) noexcept {
    u8 wire[1024]{};
    copy_bytes(wire, opening.data, opening.bytes);
    u32 count = opening.bytes;
    count += frame(wire + count, 1, true,
                   bytes(utf8_literal(R"({"op":8,"d":{"heartbeat_interval":1000,"v":9}})")));
    const u8 binary[] = {0xFF, 0xFF, 25, 7, 8, 9};
    count += frame(wire + count, 2, false, {binary, 2});
    count += frame(wire + count, 9, true, bytes(utf8_literal("x")));
    count += frame(wire + count, 0, true, {binary + 2, 4});
    count += frame(wire + count, 1, true, bytes(utf8_literal(R"({"op":11,"seq":0,"d":{"user_ids":[]}})")));
    count += frame(wire + count, 1, true, bytes(utf8_literal(R"({"op":3,"d":null})")));
    count += frame(wire + count, 1, true, bytes(utf8_literal(R"({"op":6,"d":{"t":1}})")));
    const u8 close[] = {0x0F, 0xA0};
    count += frame(wire + count, 8, true, {close, 2});
    // Every possible split exercises HTTP/frame boundaries and coalesced input.
    for (u32 split = 0; split <= count + 1; ++split) {
        VoiceSession session{};
        u8 input[2048]{}, fragments[1024]{};
        u32 scratch[256]{};
        check(voice_session_initialize(&session, {input, sizeof(input)}, {fragments, sizeof(fragments)},
                                       {scratch, 256}, bytes(key), 1024, 1024, 0, false),
              "voice session initializes caller-owned disjoint storage");
        u32 received = 0;
        const VoiceSessionEventKind expected[] = {VoiceSessionEventKind::opened,
                                                  VoiceSessionEventKind::hello,
                                                  VoiceSessionEventKind::ping,
                                                  VoiceSessionEventKind::binary,
                                                  VoiceSessionEventKind::json,
                                                  VoiceSessionEventKind::heartbeat_requested,
                                                  VoiceSessionEventKind::heartbeat_acknowledged,
                                                  VoiceSessionEventKind::closed};
        const u32 parts = split > count ? count : 2;
        for (u32 half = 0; half < parts; ++half) {
            const u32 begin = parts == 2 ? (half ? split : 0) : half;
            const u32 length = parts == 2 ? (half ? count - split : split) : 1;
            if (length)
                check(voice_session_push(&session, {wire + begin, length}),
                      "voice input accepts a partial or coalesced network read");
            for (;;) {
                const auto event = voice_session_next(&session, 10);
                if (event.kind == VoiceSessionEventKind::need_input)
                    break;
                check(event.ok() && received < 8 && event.kind == expected[received],
                      "voice session emits the same ordered events at every network split");
                if (!event.ok() || received >= 8)
                    break;
                ++received;
                if (event.kind == VoiceSessionEventKind::binary)
                    check(event.binary.sequence == 65535 && event.binary.opcode == 25 &&
                              event.binary.data.data == fragments + 3 && event.binary.data.count == 3 &&
                              event.binary.data[2] == 9,
                          "fragmented MLS framing survives an interleaved ping");
                if (event.kind == VoiceSessionEventKind::json)
                    check(event.envelope.sequence == 0 && event.envelope.has_sequence &&
                              event.envelope.data.source.data >= reinterpret_cast<const char*>(input) &&
                              event.envelope.data.source.data <
                                  reinterpret_cast<const char*>(input + sizeof(input)),
                          "unfragmented voice JSON borrows input instead of copying to fragment storage");
                if (event.kind == VoiceSessionEventKind::heartbeat_requested) {
                    const auto send = voice_heartbeat_poll(&session.heartbeat, 10);
                    check(send.action == VoiceHeartbeatAction::send && send.sequence_ack == 0 &&
                              voice_heartbeat_submitted(&session.heartbeat, send, 10),
                          "server request drives the shared heartbeat owner using the received sequence");
                }
                if (event.kind == VoiceSessionEventKind::closed)
                    check(event.close_code == 4000 && !voice_heartbeat_deadline(session.heartbeat),
                          "close frame retires socket timers");
            }
        }
        check(received == 8 && session.closed && session.heartbeat.has_sequence &&
                  !session.heartbeat.sequence,
              "close retains wrapped sequence for an explicit resume decision");
        const auto old_generation = session.heartbeat.generation;
        check(voice_session_initialize(&session, {input, sizeof(input)}, {fragments, sizeof(fragments)},
                                       {scratch, 256}, bytes(key), 1024, 1024, 100, true) &&
                  session.heartbeat.has_sequence && session.heartbeat.generation != old_generation,
              "reinitialization resumes sequence with a new socket attempt");
        voice_session_clear(&session);
        bool erased = true;
        for (u8 b : input)
            erased &= b == 0;
        for (u8 b : fragments)
            erased &= b == 0;
        check(erased && !session.heartbeat.has_sequence && !voice_heartbeat_deadline(session.heartbeat),
              "terminal clear erases packet storage and ends resume state");
    }
    VoiceSession session{};
    u8 input[2048]{}, fragments[1024]{};
    u32 scratch[256]{};
    auto initialize = [&]() noexcept {
        return voice_session_initialize(&session, {input, sizeof(input)}, {fragments, sizeof(fragments)},
                                        {scratch, 256}, bytes(key), 1024, 1024, 0, false);
    };
    check(!voice_session_initialize(&session, {input, sizeof(input)}, {input, 1024}, {scratch, 256},
                                    bytes(key), 1024, 1024, 0, false) &&
              !session.input.data,
          "voice session rejects overlapping packet buffers without changing ownership");
    initialize();
    u8 bad_opening[256]{};
    copy_bytes(bad_opening, opening.data, opening.bytes);
    bad_opening[opening.bytes - 6] = 'A';
    voice_session_push(&session, {bad_opening, opening.bytes});
    check(voice_session_next(&session, 1).error == VoiceSessionError::handshake &&
              !voice_session_push(&session, bytes(opening)) && !voice_heartbeat_deadline(session.heartbeat),
          "invalid HTTP upgrade is terminal and retires its Hello deadline");
    const u8 malformed[][6] = {{0x81, 0x80, 0, 0, 0, 0}, {0x80, 0, 0, 0, 0, 0}};
    const u32 lengths[] = {6, 2};
    for (u32 i = 0; i < 2; ++i) {
        initialize();
        voice_session_push(&session, bytes(opening));
        voice_session_next(&session, 1);
        voice_session_push(&session, {malformed[i], lengths[i]});
        const auto failed = voice_session_next(&session, 2);
        check(failed.error == VoiceSessionError::websocket && session.failed,
              "masked server frames and orphan continuations fail through shared frame validation");
    }
    initialize();
    voice_session_push(&session, bytes(opening));
    voice_session_next(&session, 1);
    const u8 truncated_binary[] = {0x82, 2, 0, 0};
    voice_session_push(&session, {truncated_binary, sizeof(truncated_binary)});
    check(voice_session_next(&session, 2).error == VoiceSessionError::binary,
          "complete WebSocket message with truncated voice binary prefix is rejected");
    voice_session_clear(&session);
}
} // namespace maicord
