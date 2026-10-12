#include "maicord/app/voice_connection.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
JsonValue object(Utf8View text) noexcept {
    return {text, 0, JsonValueKind::object};
}
VoiceParticipantsResult apply(VoiceParticipants& participants, u8 opcode, Utf8View text,
                              u64 generation = 9) noexcept {
    u32 scratch[1024];
    return voice_participants_event(&participants, generation, opcode, object(text), {scratch, 1024}, 64);
}
void decoding(Check check) noexcept {
    struct Invalid {
        u8 opcode;
        Utf8View json;
    };
    const Invalid invalid[]{
        {5, utf8_literal(R"({"user_id":"2","speaking":1})")},
        {5, utf8_literal(R"({"user_id":"0","speaking":1,"ssrc":1})")},
        {5, utf8_literal(R"({"user_id":2,"speaking":1,"ssrc":1})")},
        {5, utf8_literal(R"({"speaking":-1,"ssrc":1})")},
        {5, utf8_literal(R"({"speaking":1,"ssrc":4294967296})")},
        {5, utf8_literal(R"({"speaking":1,"ssrc":1,"s\u0073rc":2})")},
        {11, utf8_literal(R"({"user_ids":["2",false]})")},
        {11, utf8_literal(R"({"user_ids":["18446744073709551616"]})")},
        {11, utf8_literal(R"({"user_ids":["2"],"user_ids":[]})")},
        {11, utf8_literal(R"({"user_ids":null})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":2,"streams":[null]})")},
        {12,
         utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":2,"streams":[{"ssrc":3,"ssrc":4}]})")},
        {12, utf8_literal(
                 R"({"user_id":"2","audio_ssrc":1,"video_ssrc":2,"streams":[{"ssrc":3,"active":1}]})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":2,"streams":[{"rtx_ssrc":-1}]})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":2,"streams":[{"ssrc":3,"quality":101}]})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":2,"streams":[{"ssrc":3,"quality":"x"}]})")},
        {13, utf8_literal(R"({"user_id":null})")},
        {18, utf8_literal(R"({"user_id":"2","flags":true})")},
        {20, utf8_literal(R"({"user_id":"2","platform":4294967296})")},
        {20, utf8_literal(R"({"user_id":"2","platform":1}garbage)")},
        {7, utf8_literal(R"({})")}};
    u32 scratch[1024];
    for (const auto& fixture : invalid) {
        VoiceParticipantEvent event{};
        event.user_id = 123;
        event.value = 77;
        check(!voice_decode_participant(fixture.opcode, object(fixture.json), {scratch, 1024}, 64, &event)
                      .ok() &&
                  event.user_id == 123 && event.value == 77,
              "participant decoder rejects malformed/duplicate/overflow fields without publishing output");
    }
    // Null values (a member without video, platform or delay) announce the
    // field with its default instead of discarding the event.
    const Invalid nulls[]{
        {5, utf8_literal(R"({"speaking":0,"ssrc":1,"delay":null})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":null,"streams":null})")},
        {12, utf8_literal(R"({"user_id":"2","audio_ssrc":1,"video_ssrc":0,"streams":[{"type":null,"ssrc":null,"rtx_ssrc":null,"active":null}]})")},
        {18, utf8_literal(R"({"user_id":"2","flags":null})")},
        {20, utf8_literal(R"({"user_id":"2","platform":null})")}};
    for (const auto& fixture : nulls) {
        VoiceParticipantEvent decoded{};
        check(voice_decode_participant(fixture.opcode, object(fixture.json), {scratch, 1024}, 64, &decoded).ok() &&
                  !decoded.value && !decoded.video_ssrc,
              "participant decoder treats null optional fields as their defaults");
    }
    VoiceParticipantEvent event{};
    const auto legacy = utf8_literal(R"({"speaking":true,"ssrc":0,"future":{"nested":[1,2]}})");
    check(voice_decode_participant(5, object(legacy), {scratch, 1024}, 64, &event).ok() &&
              event.value == 1 && !event.user_id && !event.audio_ssrc,
          "participant decoder accepts official legacy boolean and identity-free echo");
    event.value = 77;
    check(!voice_decode_participant(5, object(legacy), {scratch, 1}, 64, &event).ok() &&
              event.value == 77 &&
              !voice_decode_participant(5, object(legacy), {scratch, 1024}, 1, &event).ok() &&
              event.value == 77,
          "participant decoder preserves outputs at structural/depth capacity failures");
    const auto input = object(utf8_literal(R"({"user_ids":["18446744073709551615","\u0031"]})"));
    check(voice_decode_participant(11, input, {scratch, 1024}, 64, &event).ok() && event.user_count == 2,
          "participant IDs support escaped digits and exact uint64 maximum");
}
void state(Check check) noexcept {
    VoiceParticipants participants{};
    check(apply(participants, 11, utf8_literal(R"({"user_ids":[]})")).ok() && !participants.users &&
              !participants.sources && participants.generation == 9,
          "empty participant set requires no allocation");
    check(apply(participants, 5, utf8_literal(R"({"ssrc":10,"speaking":true,"user_id":"2"})")).ok() &&
              voice_participants_source(participants, 10).user_id == 2 &&
              !voice_participants_user(participants, 2).connected,
          "SSRC announcement does not imply recognized call membership");
    const auto first = voice_participants_source(participants, 10);
    check(first.revision &&
              apply(participants, 11, utf8_literal(R"({"user_ids":["3","2","1","3"]})")).ok() &&
              participants.user_count == 3 && participants.users[0].user_id == 1 &&
              participants.users[2].user_id == 3 && voice_participants_user(participants, 2).connected &&
              voice_participants_user(participants, 2).speaking == 1 &&
              voice_participants_source(participants, 10).revision == first.revision,
          "batched connects deduplicate/sort while retaining existing source and activity state");
    const auto capacity = participants.user_capacity;
    const auto* allocation = participants.users;
    check(apply(participants, 11, utf8_literal(R"({"user_ids":["1","2","3","2"]})")).ok() &&
              participants.user_count == 3 && participants.user_capacity == capacity &&
              participants.users == allocation,
          "repeat membership does not grow storage or replace the owned allocation");
    check(apply(participants, 5, utf8_literal(R"({"ssrc":10,"speaking":0,"user_id":"2"})")).ok() &&
              voice_participants_source(participants, 10).revision == first.revision &&
              voice_participants_user(participants, 2).speaking == 0,
          "speaking stops preserve source revision for trailing media");
    check(apply(participants, 18, utf8_literal(R"({"user_id":"2","flags":4294967295})")).ok() &&
              apply(participants, 20, utf8_literal(R"({"user_id":"2","platform":4294967295})")).ok() &&
              voice_participants_user(participants, 2).flags == ~u32{0} &&
              voice_participants_user(participants, 2).has_flags &&
              voice_participants_user(participants, 2).has_platform,
          "participant flags/platform retain future numeric values and presence bits");
    const auto video = utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
        {"type":"video","ssrc":20,"rtx_ssrc":21,"active":true,"quality":50},
        {"type":"screen","ssrc":30,"rtx_ssrc":31,"active":false},
        {"type":"audio","ssrc":10,"rtx_ssrc":15},{"type":"test","ssrc":40},
        {"type":"future-type","ssrc":50}]})");
    check(apply(participants, 12, video).ok() && participants.source_count == 8 &&
              voice_participants_source(participants, 10).revision == first.revision &&
              voice_participants_source(participants, 31).primary_ssrc == 30 &&
              voice_participants_source(participants, 31).kind == VoiceSourceKind::rtx &&
              !voice_participants_source(participants, 31).active &&
              voice_participants_source(participants, 30).kind == VoiceSourceKind::screen &&
              voice_participants_source(participants, 40).kind == VoiceSourceKind::test &&
              voice_participants_source(participants, 50).kind == VoiceSourceKind::video,
          "video descriptors own explicit primary/RTX/type/activity mappings and preserve duplicate audio "
          "identity");
    check(voice_participants_source(participants, 20).quality == 50 &&
              voice_participants_source(participants, 21).quality == 50 &&
              voice_participants_source(participants, 30).quality == 100 &&
              voice_participants_source(participants, 10).quality == 100,
          "video descriptors carry simulcast quality to their RTX; absent quality is 100");
    const auto old_video = voice_participants_source(participants, 20);
    const auto previous_revision = participants.source_revision;
    check(apply(participants, 12, video).ok() && participants.source_revision == previous_revision &&
              voice_participants_source(participants, 20).revision == old_video.revision,
          "duplicate video announcement does not reset source identity");
    const auto audio_rtx = voice_participants_source(participants, 15);
    check(apply(participants, 5, utf8_literal(R"({"ssrc":10,"speaking":1,"user_id":"2"})")).ok() &&
              voice_participants_source(participants, 15).revision == audio_rtx.revision &&
              voice_participants_source(participants, 20).revision == old_video.revision,
          "Speaking retains unchanged audio RTX and independent video sources");
    const auto conflict =
        apply(participants, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":0,
        "streams":[{"ssrc":60,"rtx_ssrc":61},{"ssrc":61}]})"));
    check(conflict.error == VoiceParticipantsError::conflicting_sources && participants.source_count == 8 &&
              participants.source_revision == previous_revision &&
              voice_participants_source(participants, 20).revision == old_video.revision &&
              !voice_participants_source(participants, 60).user_id,
          "conflicting primary/RTX announcement preserves all live mappings and revisions");
    check(apply(participants, 11, utf8_literal(R"({"user_ids":["4","0"]})")).error ==
                  VoiceParticipantsError::decode &&
              participants.user_count == 3 && !voice_participants_user(participants, 4).user_id,
          "malformed late member cannot partially publish an earlier new member");
    check(
        apply(participants, 12, utf8_literal(R"({"user_id":"3","audio_ssrc":11,"video_ssrc":20})")).ok() &&
            voice_participants_source(participants, 20).user_id == 3 &&
            voice_participants_source(participants, 21).user_id == 3 &&
            voice_participants_source(participants, 20).revision > old_video.revision &&
            voice_participants_source(participants, 30).user_id == 2,
        "later signaling reassigns an SSRC and its RTX with fresh revisions without removing unrelated "
        "users' streams");
    check(apply(participants, 5, utf8_literal(R"({"user_id":"1","ssrc":20,"speaking":1})")).ok() &&
              voice_participants_source(participants, 20).user_id == 1 &&
              !voice_participants_source(participants, 21).user_id,
          "primary reassignment removes the displaced owner's orphan RTX binding");
    check(apply(participants, 12,
                utf8_literal(R"({"user_id":"2","audio_ssrc":12,"video_ssrc":0,"streams":[]})"))
                  .ok() &&
              !voice_participants_source(participants, 10).user_id &&
              !voice_participants_source(participants, 30).user_id &&
              voice_participants_source(participants, 12).user_id == 2 &&
              voice_participants_source(participants, 20).user_id == 1,
          "video-off replaces only that user's source set and preserves other participants");
    const auto before_ready = participants.source_revision;
    voice_participants_clear_sources(&participants);
    check(participants.source_count == 0 && participants.user_count == 3 &&
              voice_participants_user(participants, 2).connected &&
              voice_participants_user(participants, 2).has_platform &&
              !voice_participants_user(participants, 1).speaking &&
              participants.source_revision == before_ready,
          "transport Ready clears source/activity state but preserves call membership and metadata");
    check(apply(participants, 5, utf8_literal(R"({"user_id":"2","ssrc":10,"speaking":1})")).ok() &&
              voice_participants_source(participants, 10).revision > before_ready,
          "reannounced source cannot share the revision of pre-Ready media jobs");
    check(apply(participants, 13, utf8_literal(R"({"user_id":"2"})")).ok() &&
              !voice_participants_user(participants, 2).user_id &&
              !voice_participants_source(participants, 10).user_id &&
              apply(participants, 13, utf8_literal(R"({"user_id":"2"})")).ok(),
          "disconnect atomically removes member and every source, and duplicates are harmless");
    check(apply(participants, 11, utf8_literal(R"({"user_ids":["7"]})"), 10).error ==
                  VoiceParticipantsError::stale_generation &&
              !voice_participants_user(participants, 7).user_id,
          "stale credential generation cannot populate the participant set");
    participants.source_revision = ~u64{0};
    check(apply(participants, 5, utf8_literal(R"({"user_id":"8","ssrc":100,"speaking":1})")).error ==
                  VoiceParticipantsError::revision_exhausted &&
              !voice_participants_user(participants, 8).user_id && !participants.source_count,
          "source revision exhaustion cannot wrap or partially insert a member");
    voice_participants_release(&participants);
    check(!participants.generation && !participants.users && !participants.sources &&
              !participants.user_capacity,
          "participant release retires all owned state/storage");
}
void controller(Check check) noexcept {
    VoiceConnection connection{};
    u32 scratch[256];
    const auto connected = object(utf8_literal(R"({"user_ids":["2","3"]})"));
    check(!voice_connection_participant(&connection, 11, connected, {scratch, 256}, 64).ok(),
          "controller rejects participant events before authenticated Identify submission");
    connection.socket.phase = VoiceSocketPhase::open;
    connection.socket.setup_generation = 9;
    connection.identify_submitted = true;
    check(voice_connection_participant(&connection, 11, connected, {scratch, 256}, 64).ok(),
          "controller accepts membership even before transport Ready");
    check(voice_connection_participant(&connection, 5,
                                       object(utf8_literal(R"({"user_id":"2","ssrc":10,"speaking":1})")),
                                       {scratch, 256}, 64)
              .ok(),
          "controller binds Speaking to this signaling generation");
    VoiceReady ready{};
    ready.ip = {utf8_literal("127.0.0.1"), 0, false};
    ready.port = 4444;
    ready.ssrc = 12;
    ready.recognized_modes = 1;
    check(voice_connection_ready(&connection, ready) == VoiceMediaSetupError::none &&
              connection.participants.user_count == 2 && !connection.participants.source_count,
          "controller transport Ready preserves member roster while invalidating SSRCs");
    voice_connection_stop(&connection);
    check(!connection.participants.users && !connection.participants.sources &&
              !connection.participants.generation &&
              !voice_connection_participant(&connection, 11, connected, {scratch, 256}, 64).ok(),
          "controller stop releases membership and refuses late events");
}
u32 decimal(char* output, u32 value) noexcept {
    char reverse[10];
    u32 count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    for (u32 i = 0; i != count; ++i)
        output[i] = reverse[count - i - 1];
    return count;
}
bool ignore_socket(void*, const VoiceSessionEvent&) noexcept {
    return true;
}
Slice<u32> no_growth(void*, u32) noexcept {
    return {};
}
void large_session(Check check) noexcept {
    static VoiceSocket socket;
    static NetworkLoop loop;
    static u8 wire[16384];
    auto* json = reinterpret_cast<char*>(wire + 4);
    constexpr auto prefix = utf8_literal(R"({"op":11,"seq":7,"d":{"user_ids":[)");
    copy_bytes(json, prefix.data, prefix.bytes);
    u32 bytes = prefix.bytes;
    // Descending IDs exercise sorted insertion instead of only append order.
    for (u32 i = 1024; i; --i) {
        if (i != 1024)
            json[bytes++] = ',';
        json[bytes++] = '"';
        bytes += decimal(json + bytes, i);
        json[bytes++] = '"';
    }
    copy_bytes(json + bytes, "]}}", 3);
    bytes += 3;
    wire[0] = 0x81;
    wire[1] = 126;
    wire[2] = static_cast<u8>(bytes >> 8);
    wire[3] = static_cast<u8>(bytes);
    GatewayEndpoint endpoint{};
    if (!network_loop_initialize(&loop).ok() ||
        voice_gateway_endpoint(utf8_literal("127.0.0.1:9"), &endpoint) != GatewayEndpointError::none) {
        check(false, "large voice participant loop setup");
        return;
    }
    const auto now = GetTickCount64();
    if (!voice_socket_begin(&socket, &loop, endpoint, 9, sizeof(wire), ignore_socket, nullptr, now)) {
        check(false, "large participant voice socket storage startup");
    } else {
        // Use the real socket owner's storage/growth provider at its decrypted
        // session boundary. This fixture does not claim an authenticated TLS/WS.
        auto& session = socket.session;
        constexpr auto key = utf8_literal("dGhlIHNhbXBsZSBub25jZQ==");
        check(voice_session_initialize(&session, {session.input.data, session.input.capacity},
                                       session.fragments, session.scratch,
                                       {reinterpret_cast<const u8*>(key.data), key.bytes}, sizeof(wire),
                                       sizeof(wire), now, false),
              "large participant decrypted session initialization");
        session.opened = true;
        const auto* original = session.scratch.data;
        const auto committed = socket.json_storage.arena.committed;
        check(voice_session_push(&session, {wire, bytes / 2}) &&
                  voice_session_next(&session, now).kind == VoiceSessionEventKind::need_input &&
                  socket.json_storage.arena.committed == committed,
              "partial participant frame does not grow JSON scratch");
        check(voice_session_push(&session, {wire + bytes / 2, bytes + 4 - bytes / 2}),
              "large participant frame remainder accepted");
        auto event = voice_session_next(&session, now);
        VoiceParticipants participants{};
        check(event.kind == VoiceSessionEventKind::json && event.envelope.opcode == 11 &&
                  session.scratch.data == original && session.scratch.count > 256 &&
                  socket.json_storage.arena.committed > committed &&
                  socket.json_storage.arena.committed < socket.json_storage.arena.reserved &&
                  voice_participants_event(&participants, 9, 11, event.envelope.data, session.scratch, 64)
                      .ok() &&
                  participants.user_count == 1024 && participants.users[0].user_id == 1 &&
                  participants.users[1023].user_id == 1024,
              "1024-member announcement grows committed index in place and retains every member");
        const auto commits = socket.json_storage.arena.commit_count;
        const auto* users = participants.users;
        const auto capacity = participants.user_capacity;
        check(voice_session_push(&session, {wire, bytes + 4}), "repeat large participant frame accepted");
        event = voice_session_next(&session, now);
        check(event.kind == VoiceSessionEventKind::json &&
                  voice_participants_event(&participants, 9, 11, event.envelope.data, session.scratch, 64)
                      .ok() &&
                  socket.json_storage.arena.commit_count == commits && participants.users == users &&
                  participants.user_capacity == capacity && participants.user_count == 1024,
              "repeat large announcement reuses index and member allocation without commits or growth");
        voice_participants_release(&participants);
    }
    voice_socket_stop(&socket);
    const auto deadline = GetTickCount64() + 5000;
    while (socket.phase != VoiceSocketPhase::closed && GetTickCount64() < deadline) {
        const auto next = network_loop_poll(&loop, 100);
        if (next.state == NetworkPollState::completion)
            voice_socket_completion(&socket, &loop, next.completion, GetTickCount64());
    }
    check(socket.phase == VoiceSocketPhase::closed && !socket.storage.data &&
              !socket.json_storage.arena.base,
          "voice socket drain releases packet and grown JSON storage together");
    if (socket.phase == VoiceSocketPhase::closed)
        network_loop_release(&loop);

    VoiceSession bounded{};
    static u8 input[16398], fragments[16384];
    u32 scratch[1];
    constexpr auto key = utf8_literal("dGhlIHNhbXBsZSBub25jZQ==");
    check(voice_session_initialize(&bounded, {input, sizeof(input)}, {fragments, sizeof(fragments)},
                                   {scratch, 1}, {reinterpret_cast<const u8*>(key.data), key.bytes},
                                   sizeof(wire), sizeof(wire), now, false),
          "declined scratch-growth session initializes");
    bounded.opened = true;
    bounded.grow_scratch = no_growth;
    check(voice_session_push(&bounded, {wire, bytes + 4}) &&
              voice_session_next(&bounded, now).error == VoiceSessionError::json && bounded.failed,
          "failed index growth terminates parsing without publishing or retrying forever");
    voice_session_clear(&bounded);
}
} // namespace
void test_voice_participants(Check check) noexcept {
    decoding(check);
    state(check);
    controller(check);
    large_session(check);
}
} // namespace maicord
