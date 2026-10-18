#include "maicord/app/gateway_stream_session.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>
#include <WS2tcpip.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

JsonString text(Utf8View value) noexcept {
    return {value, 0, false};
}
bool equal(ConstSlice<u8> bytes, Utf8View expected) noexcept {
    return bytes.count == expected.bytes && bytes_equal(bytes.data, expected.data, bytes.count);
}
bool equal(Utf8View left, Utf8View right) noexcept {
    return left.bytes == right.bytes && bytes_equal(left.data, right.data, left.bytes);
}
template <typename Writer> bool written(Writer write, Utf8View expected) noexcept {
    u8 output[256];
    const auto measured = write(Slice<u8>{});
    const auto short_write = write(Slice<u8>{output, expected.bytes - 1});
    const auto result = write(Slice<u8>{output, sizeof(output)});
    return measured.bytes == expected.bytes && measured.error == GatewayCommandError::output_too_small &&
           short_write.error == GatewayCommandError::output_too_small && result.ok() &&
           equal({output, result.bytes}, expected);
}
JsonValue object(Utf8View json) noexcept {
    return {json, 0, JsonValueKind::object};
}
GatewayEnvelope dispatch(const char* name, Utf8View json) noexcept {
    u32 bytes = 0;
    while (name[bytes])
        ++bytes;
    GatewayEnvelope envelope{};
    envelope.data = object(json);
    envelope.event_name = text({name, bytes});
    envelope.has_event_name = true;
    envelope.opcode = 0;
    return envelope;
}

void test_keys(Check check) noexcept {
    const GatewayStreamKey guild = gateway_stream_key_for(1, 20, 300);
    const GatewayStreamKey call = gateway_stream_key_for(0, 20, 300);
    char text_key[kGatewayStreamKeyMaximumBytes];
    u32 bytes = gateway_stream_key_write(guild, {text_key, sizeof(text_key)});
    check(equal({text_key, bytes}, utf8_literal("guild:1:20:300")), "stream key: guild text");
    bytes = gateway_stream_key_write(call, {text_key, sizeof(text_key)});
    check(equal({text_key, bytes}, utf8_literal("call:20:300")), "stream key: call text");
    const GatewayStreamKey widest = gateway_stream_key_for(~u64{0}, ~u64{0}, ~u64{0});
    bytes = gateway_stream_key_write(widest, {text_key, sizeof(text_key)});
    GatewayStreamKey parsed{};
    check(bytes == 6 + 3 * 20 + 2 && gateway_stream_key_parse({text_key, bytes}, &parsed) && parsed == widest,
          "stream key: maximum snowflakes round-trip");
    check(!gateway_stream_key_write(guild, {text_key, 5}), "stream key: short output writes nothing");
    check(gateway_stream_key_parse(utf8_literal("call:20:300"), &parsed) && parsed == call,
          "stream key: call parses");
    const Utf8View invalid[]{utf8_literal("guild:1:20"),      utf8_literal("guild:1:20:300:4"),
                             utf8_literal("call:20:300:4"),   utf8_literal("guild:01:20:300"),
                             utf8_literal("guild:0:20:300"),  utf8_literal("stage:1:20:300"),
                             utf8_literal("guild:1::300"),    utf8_literal("call:20:3x"),
                             utf8_literal("call:20:18446744073709551616"), utf8_literal("guild:1:20:300:")};
    bool rejected = true;
    for (const auto& value : invalid)
        rejected &= !gateway_stream_key_parse(value, &parsed);
    check(rejected && parsed == call, "stream key: malformed keys are rejected without output");
    check(!gateway_stream_key_valid(gateway_stream_key_for(1, 0, 300)) &&
              !gateway_stream_key_valid({0, 20, 300, GatewayStreamType::guild, {}}) &&
              !gateway_stream_key_valid({1, 20, 300, GatewayStreamType::call, {}}),
          "stream key: incomplete identities are invalid");
}

void test_writers(Check check) noexcept {
    const GatewayStreamKey key = gateway_stream_key_for(1, 20, 300);
    check(written([&](Slice<u8> out) { return gateway_write_stream_watch(key, out); },
                  utf8_literal("{\"op\":20,\"d\":{\"stream_key\":\"guild:1:20:300\"}}")),
          "stream op 20 watch bytes");
    check(written([&](Slice<u8> out) { return gateway_write_stream_delete(key, out); },
                  utf8_literal("{\"op\":19,\"d\":{\"stream_key\":\"guild:1:20:300\"}}")),
          "stream op 19 delete bytes");
    check(written([&](Slice<u8> out) { return gateway_write_stream_ping(gateway_stream_key_for(0, 20, 300), out); },
                  utf8_literal("{\"op\":21,\"d\":{\"stream_key\":\"call:20:300\"}}")),
          "stream op 21 ping bytes");
    u8 output[64];
    check(gateway_write_stream_watch({}, {output, sizeof(output)}).error == GatewayCommandError::invalid_input,
          "stream writers reject invalid keys");
    u8 target[kGatewayStreamPreviewMaximumTargetBytes];
    const u32 bytes = gateway_stream_preview_target(key, 1700000000123, {target, sizeof(target)});
    check(equal({target, bytes}, utf8_literal("/api/v9/streams/guild:1:20:300/preview?version=1700000000123")),
          "stream preview REST target");
    u32 scratch[64];
    char url[256];
    const u32 url_bytes = gateway_stream_preview_url(
        utf8_literal("{\"url\":\"https:\\/\\/media.discordapp.net\\/preview.jpg?x=1\"}"), {scratch, 64}, {url, 256});
    check(equal({url, url_bytes}, utf8_literal("https://media.discordapp.net/preview.jpg?x=1")),
          "stream preview URL decodes escapes");
    check(!gateway_stream_preview_url(utf8_literal("{\"url\":\"http://x.test/a\"}"), {scratch, 64}, {url, 256}) &&
              !gateway_stream_preview_url(utf8_literal("{\"url\":null}"), {scratch, 64}, {url, 256}) &&
              !gateway_stream_preview_url(utf8_literal("{\"url\":\"https://a b\"}"), {scratch, 64}, {url, 256}) &&
              !gateway_stream_preview_url(utf8_literal("{\"url\":\"https://x.test/a\"}"), {scratch, 64}, {url, 12}),
          "stream preview URL rejects non-https, null, spaces and overflow");
}

void test_decoders(Check check) noexcept {
    u32 scratch[256];
    const GatewayStreamKey key = gateway_stream_key_for(1, 20, 300);
    GatewayStreamCreate create{};
    check(gateway_decode_stream_create(
              object(utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"rtc_server_id\":\"5000\","
                                  "\"rtc_channel_id\":null,\"region\":\"rotterdam\",\"viewer_ids\":[\"7\",\"8\"],"
                                  "\"paused\":false}")),
              {scratch, 256}, 16, &create)
                  .ok() &&
              create.key == key && create.rtc_server_id == 5000 && !create.rtc_channel_id && create.viewer_count == 2 &&
              !create.paused && json_string_equals_ascii(create.region, utf8_literal("rotterdam")),
          "STREAM_CREATE decodes media server, viewers and region");
    GatewayStreamServer server{};
    check(gateway_decode_stream_server(object(utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"endpoint\":"
                                                           "\"stream.example.test:443\",\"token\":\"tok\"}")),
                                       {scratch, 256}, 16, &server)
                  .ok() &&
              server.key == key && !server.unavailable && json_string_equals_ascii(server.token, utf8_literal("tok")) &&
              json_string_equals_ascii(server.endpoint, utf8_literal("stream.example.test:443")),
          "STREAM_SERVER_UPDATE decodes credentials");
    check(gateway_decode_stream_server(
              object(utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"endpoint\":null,\"token\":\"tok\"}")),
              {scratch, 256}, 16, &server)
                  .ok() &&
              server.unavailable,
          "STREAM_SERVER_UPDATE null endpoint means reallocation");
    check(!gateway_decode_stream_server(object(utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"endpoint\":\"e\"}")),
                                        {scratch, 256}, 16, &server)
               .ok(),
          "STREAM_SERVER_UPDATE requires a token");
    GatewayStreamUpdate update{};
    check(gateway_decode_stream_update(
              object(utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"viewer_ids\":[],\"paused\":true}")),
              {scratch, 256}, 16, &update)
                  .ok() &&
              update.has_viewers && !update.viewer_count && update.has_paused && update.paused,
          "STREAM_UPDATE decodes viewers and pause");
    GatewayStreamDelete removal{};
    check(gateway_decode_stream_delete(object(utf8_literal("{\"stream_key\":\"call:20:300\",\"unavailable\":false,"
                                                           "\"reason\":\"stream_full\"}")),
                                       {scratch, 256}, 16, &removal)
                  .ok() &&
              removal.key == gateway_stream_key_for(0, 20, 300) &&
              removal.reason == GatewayStreamDeleteReason::stream_full && !removal.unavailable,
          "STREAM_DELETE decodes the reason");
    check(!gateway_decode_stream_delete(object(utf8_literal("{\"stream_key\":\"call:20:300\",\"stream_key\":"
                                                            "\"call:20:300\"}")),
                                        {scratch, 256}, 16, &removal)
               .ok() &&
              !gateway_decode_stream_create(object(utf8_literal("{\"stream_key\":\"bogus\"}")), {scratch, 256}, 16,
                                            &create)
                   .ok() &&
              !gateway_decode_stream_create(
                   object(utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"viewer_ids\":[\"x\"]}")), {scratch, 256},
                   16, &create)
                   .ok(),
          "stream decoders reject duplicate fields, bad keys and bad viewers");
}

bool next_is(const GatewayStreamControl& control, GatewayStreamCommandKind kind, const GatewayStreamKey& key) noexcept {
    GatewayStreamCommand command{};
    return gateway_stream_next(control, &command) && command.kind == kind && command.key == key;
}
bool submit(GatewayStreamControl* control) noexcept {
    GatewayStreamCommand command{};
    return gateway_stream_next(*control, &command) && gateway_stream_submitted(control, command);
}

void test_control(Check check) noexcept {
    static GatewayStreamControl control;
    const GatewayStreamKey key = gateway_stream_key_for(1, 20, 300);
    const GatewayStreamKey other = gateway_stream_key_for(1, 20, 301);
    check(!gateway_stream_watch(&control, key, 1000), "stream watch needs a bound session");
    check(gateway_stream_bind(&control, 42, utf8_literal("main-session")), "stream binds the Gateway session");
    check(gateway_stream_watch(&control, key, 1000) && control.phase == GatewayStreamPhase::requested &&
              next_is(control, GatewayStreamCommandKind::watch, key) &&
              gateway_stream_deadline(control) == 1000 + kGatewayStreamCreateTimeoutMs,
          "watch queues STREAM_WATCH and arms the create deadline");
    check(gateway_stream_watch(&control, key, 1001) && control.queue_count == 1, "repeated watch is idempotent");
    check(submit(&control) && !control.queue_count, "watch commits after the send");

    u32 scratch[256];
    gateway_stream_dispatch(&control,
                            dispatch("STREAM_CREATE", utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"rtc_server_id\":"
                                                                   "\"5000\",\"rtc_channel_id\":\"4321\",\"viewer_ids\":"
                                                                   "[\"42\"],\"paused\":false,\"region\":\"x\"}")),
                            {scratch, 256}, 16, 2000);
    check(control.phase == GatewayStreamPhase::awaiting_server && control.rtc_server_id == 5000 &&
              control.rtc_channel_id == 4321 && control.viewer_count == 1 && control.setup.stream &&
              control.setup.server_id == 5000 && !control.create_deadline_ms && control.setup.state_deadline_ms,
          "STREAM_CREATE selects the media server and awaits credentials");
    const u64 created_generation = control.setup.generation;
    gateway_stream_dispatch(&control,
                            dispatch("STREAM_SERVER_UPDATE",
                                     utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"endpoint\":\"stream.example.test:"
                                                  "443\",\"token\":\"stream-token\"}")),
                            {scratch, 256}, 16, 2100);
    VoiceSetupCredentials credentials{};
    check(control.phase == GatewayStreamPhase::connecting && voice_setup_credentials(control.setup, &credentials) &&
              credentials.stream && credentials.server_id == 5000 && credentials.channel_id == 4321 &&
              credentials.user_id == 42 && equal(credentials.session, utf8_literal("main-session")) &&
              equal(credentials.token, utf8_literal("stream-token")) &&
              equal(credentials.endpoint, utf8_literal("stream.example.test:443")) &&
              credentials.generation != created_generation && !control.setup.state_deadline_ms,
          "STREAM_SERVER_UPDATE yields second-connection credentials (rtc server/channel, main session)");
    gateway_stream_dispatch(&control,
                            dispatch("STREAM_UPDATE",
                                     utf8_literal("{\"stream_key\":\"guild:1:20:300\",\"viewer_ids\":[\"42\",\"43\"],"
                                                  "\"paused\":true}")),
                            {scratch, 256}, 16, 2200);
    check(control.viewer_count == 2 && control.paused, "STREAM_UPDATE refreshes viewers and pause");
    gateway_stream_dispatch(&control,
                            dispatch("STREAM_UPDATE", utf8_literal("{\"stream_key\":\"guild:1:20:301\",\"paused\":false}")),
                            {scratch, 256}, 16, 2200);
    check(control.paused, "updates for another stream are ignored");

    gateway_stream_observe(&control, {true, false, false});
    check(control.phase == GatewayStreamPhase::active, "attached media makes the stream active");
    gateway_stream_observe(&control, {false, true, false});
    gateway_stream_observe(&control, {false, true, false});
    check(control.phase == GatewayStreamPhase::reconnecting && control.queue_count == 1 &&
              next_is(control, GatewayStreamCommandKind::ping, key),
          "a reconnecting stream connection sends exactly one STREAM_PING");
    check(submit(&control), "ping commits");
    gateway_stream_observe(&control, {true, false, false});
    gateway_stream_observe(&control, {false, true, false});
    check(control.queue_count == 1 && submit(&control), "a later loss pings again after recovery");
    gateway_stream_observe(&control, {true, false, false});

    // Switching streams deletes the previous one first (one stream at a time).
    check(gateway_stream_watch(&control, other, 3000) && next_is(control, GatewayStreamCommandKind::remove, key) &&
              submit(&control) && next_is(control, GatewayStreamCommandKind::watch, other) && submit(&control) &&
              control.key == other && control.phase == GatewayStreamPhase::requested &&
              control.setup.phase == VoiceSetupPhase::idle && !control.rtc_server_id,
          "watching another stream deletes the old one and resets the connection credentials");
    gateway_stream_dispatch(&control,
                            dispatch("STREAM_CREATE", utf8_literal("{\"stream_key\":\"guild:1:20:301\",\"rtc_server_id\":"
                                                                   "\"6000\",\"rtc_channel_id\":null}")),
                            {scratch, 256}, 16, 3100);
    check(control.setup.channel_id == 5999 && control.rtc_channel_id == 5999,
          "a null rtc_channel_id falls back to rtc_server_id - 1");
    gateway_stream_dispatch(&control,
                            dispatch("STREAM_DELETE", utf8_literal("{\"stream_key\":\"guild:1:20:301\",\"reason\":"
                                                                   "\"unauthorized\",\"unavailable\":false}")),
                            {scratch, 256}, 16, 3200);
    check(control.phase == GatewayStreamPhase::ended && control.end == GatewayStreamEnd::deleted &&
              control.delete_reason == GatewayStreamDeleteReason::unauthorized && !control.queue_count &&
              control.setup.phase == VoiceSetupPhase::idle && control.key == other,
          "STREAM_DELETE tears down without a reply and keeps the key for the UI");

    // Stop before the watch was sent: nothing reaches the server.
    check(gateway_stream_watch(&control, key, 4000), "watch again");
    gateway_stream_stop(&control, GatewayStreamEnd::stopped);
    check(!control.queue_count && control.end == GatewayStreamEnd::stopped, "stopping an unsent watch sends nothing");
    // Leaving the voice channel ends a sent watch with STREAM_DELETE.
    check(gateway_stream_watch(&control, key, 5000) && submit(&control), "watch sent");
    gateway_stream_voice(&control, 20, true);
    check(gateway_stream_active(control), "staying in the channel keeps the stream");
    gateway_stream_voice(&control, 21, true);
    check(control.end == GatewayStreamEnd::left_channel && next_is(control, GatewayStreamCommandKind::remove, key) &&
              submit(&control),
          "moving to another channel deletes the stream");
    check(gateway_stream_watch(&control, key, 6000) && submit(&control), "watch sent again");
    check(!gateway_stream_expire(&control, 6000 + kGatewayStreamCreateTimeoutMs - 1) &&
              gateway_stream_expire(&control, 6000 + kGatewayStreamCreateTimeoutMs) &&
              control.end == GatewayStreamEnd::timeout && next_is(control, GatewayStreamCommandKind::remove, key),
          "a missing STREAM_CREATE times out and deletes the watch");
    gateway_stream_reset(&control, GatewayStreamEnd::session);
    check(!control.queue_count, "reset drops queued commands");
    check(gateway_stream_watch(&control, key, 7000) && submit(&control), "watch for failure");
    gateway_stream_create(&control, {key, 5000, 4321, {}, 1, false}, 7100);
    gateway_stream_server(&control, {key, text(utf8_literal("t")), text(utf8_literal("e.test:443")), false});
    gateway_stream_observe(&control, {false, false, true});
    check(control.end == GatewayStreamEnd::failed && next_is(control, GatewayStreamCommandKind::remove, key),
          "a permanently failed stream connection ends the watch");
    gateway_stream_delete(&control, {key, GatewayStreamDeleteReason::none, false});
    check(control.end == GatewayStreamEnd::deleted && control.phase == GatewayStreamPhase::ended,
          "a STREAM_DELETE after the media socket closed reports the stream end");
    gateway_stream_release(&control);
    check(control.phase == GatewayStreamPhase::idle && !control.setup.user_id && !control.queue_count,
          "release erases the session binding");
}

struct FrameCapture {
    u32 ssrc, ended, calls;
    bool intact;
};
bool capture_frame(void* context, u32 ssrc, ConstSlice<u8>, u32, bool, bool intact) noexcept {
    auto& capture = *static_cast<FrameCapture*>(context);
    capture.ssrc = ssrc;
    capture.intact = intact;
    ++capture.calls;
    return true;
}
void capture_end(void* context, u32 ssrc) noexcept {
    static_cast<FrameCapture*>(context)->ended = ssrc;
}
bool pending(const VoiceVideoSink& sink, u32 ssrc) noexcept {
    for (const auto value : sink.pending_keyframes)
        if (static_cast<u32>(value) == ssrc)
            return true;
    return false;
}

void test_session(Check check) noexcept {
    static GatewayStreamSession session;
    static VoiceVideoSink call;
    FrameCapture capture{};
    gateway_stream_session_set_target(&session, {&capture, capture_frame, capture_end});
    const auto frames = gateway_stream_session_frames(&session);
    const u8 frame[4]{0, 0, 0, 1};
    check(frames.push_frame(frames.context, 0x1234, {frame, 4}, 90000, true, true) && capture.calls == 1 &&
              capture.ssrc == (0x1234u | kGatewayStreamVideoSsrcTag) && capture.intact,
          "stream video reaches the call decoders under a tagged SSRC");
    check(frames.push_frame(frames.context, 0x1234, {frame, 4}, 93000, false, false) && capture.calls == 2 &&
              !capture.intact,
          "stream frames keep their not-intact mark on the way to the decoders");
    check(!frames.push_frame(frames.context, 0x8000'0001u, {frame, 4}, 0, true, true) && capture.calls == 2 &&
              session.frames_untagged == 1,
          "stream SSRCs that collide with the tag are dropped");
    gateway_stream_session_want_video(&session, 77, false);
    check(session.receive.video.unwanted_count == 1 && session.receive.video.unwanted[0] == 77,
          "a hidden stream tile asks the stream connection for quality 0");
    gateway_stream_session_want_video(&session, 77, true);
    check(!session.receive.video.unwanted_count, "a visible stream tile restores the stream's wants");
    check(gateway_stream_video_user(77) == (77ull | kGatewayStreamVideoUserTag) &&
              gateway_stream_video_user_tagged(gateway_stream_video_user(77)) && !gateway_stream_video_user_tagged(77) &&
              gateway_stream_video_owner(gateway_stream_video_user(77)) == 77 && !gateway_stream_video_user(0),
          "stream tile IDs tag the owner without colliding with snowflakes");
    frames.end_stream(frames.context, 0x1234);
    check(capture.ended == (0x1234u | kGatewayStreamVideoSsrcTag), "stream end reaches the decoder by tagged SSRC");
    gateway_stream_session_keyframe(&call, &session, 0x1234u | kGatewayStreamVideoSsrcTag);
    gateway_stream_session_keyframe(&call, &session, 0x55);
    check(pending(session.receive.video, 0x1234) && !pending(call, 0x1234) && pending(call, 0x55) &&
              !pending(session.receive.video, 0x55),
          "decoder keyframe requests route by SSRC tag");

    VoiceAudioSettings output = voice_audio_settings_defaults();
    output.output_gain_percent = 150;
    check(gateway_stream_bus_settings(output, 100).output_gain_percent == 150 &&
              gateway_stream_bus_settings(output, 50).output_gain_percent == 75 &&
              gateway_stream_bus_settings(output, 200).output_gain_percent == kGatewayStreamVolumeMaximum &&
              gateway_stream_bus_settings(output, 0).output_gain_percent == 0,
          "stream bus gain multiplies the output volume and clamps");
    gateway_stream_session_set_volume(&session, 500);
    auto snapshot = gateway_stream_session_snapshot(session, 42);
    check(snapshot.volume_percent == kGatewayStreamVolumeMaximum && snapshot.phase == GatewayStreamPhase::idle &&
              !snapshot.video_ssrc,
          "stream snapshot reports the clamped volume while idle");
    gateway_stream_session_release(&session);
}

// The stream connection identifies with the stream credentials (screen stream
// parameters) and keys its DAVE group by the stream's rtc channel. Loopback only.
void test_stream_connection(Check check) noexcept {
    static VoiceConnection connection;
    static GatewayStreamControl control;
    static NetworkLoop loop;
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "stream connection loop startup");
        return;
    }
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    const bool listening = listener != INVALID_SOCKET &&
                           bind(listener, reinterpret_cast<const sockaddr*>(&address), length) == 0 &&
                           getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
                           listen(listener, 1) == 0;
    check(listening, "stream connection loopback listener");
    if (listening) {
        char endpoint[32] = "127.0.0.1:", reverse[6]{};
        u32 digits = 0, port = ntohs(address.sin_port);
        do {
            reverse[digits++] = static_cast<char>('0' + port % 10);
            port /= 10;
        } while (port);
        for (u32 i = 0; i < digits; ++i)
            endpoint[10 + i] = reverse[digits - 1 - i];
        const GatewayStreamKey key = gateway_stream_key_for(1, 20, 300);
        gateway_stream_bind(&control, 42, utf8_literal("main-session"));
        gateway_stream_watch(&control, key, GetTickCount64());
        gateway_stream_create(&control, {key, 5000, 4321, {}, 1, false}, GetTickCount64());
        gateway_stream_server(&control, {key, text(utf8_literal("stream-token")), text({endpoint, 10 + digits}), false});
        voice_connection_drive(&connection, control.setup, &loop, GetTickCount64());
        constexpr auto identify = utf8_literal(
            "{\"op\":0,\"d\":{\"server_id\":\"5000\",\"channel_id\":\"4321\",\"session_id\":\"main-session\","
            "\"token\":\"stream-token\",\"user_id\":\"42\",\"max_dave_protocol_version\":1,\"video\":true,"
            "\"streams\":[{\"type\":\"screen\",\"rid\":\"100\",\"quality\":100}]}}");
        check(connection.socket.phase == VoiceSocketPhase::connecting &&
                  equal(secure_buffer_view(connection.command), identify),
              "stream connection Identify uses rtc server/channel, main session and a screen stream");
        check(connection.dave_group_id == 4321 && connection.dave_user_id == 42,
              "stream connection DAVE group is the stream's rtc channel");
        gateway_stream_stop(&control, GatewayStreamEnd::stopped);
        voice_connection_drive(&connection, control.setup, &loop, GetTickCount64());
        const u64 deadline = GetTickCount64() + 3000;
        while (!voice_connection_drained(connection) && GetTickCount64() < deadline) {
            const auto next = network_loop_poll(&loop, 50);
            if (next.state == NetworkPollState::completion)
                voice_connection_completion(&connection, control.setup, &loop, next.completion, GetTickCount64());
        }
        check(voice_connection_drained(connection) && !connection.command.data,
              "stopping the stream retires its connection");
        gateway_stream_release(&control);
    }
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    network_loop_release(&loop);
}
void test_live_badges(Check check) noexcept {
    VoiceState records[8]{};
    VoiceStateStore store{};
    voice_state_store_initialize_sparse(&store, {records, 8});
    voice_state_store_apply(&store, 1, 20, 300, voice_self_stream | voice_self_mute);
    voice_state_store_apply(&store, 1, 20, 301, voice_self_video);
    check(voice_state_live(*voice_state_store_find(store, 1, 300)) &&
              !voice_state_live(*voice_state_store_find(store, 1, 301)),
          "LIVE badges: self_stream marks the streaming user");
    check(gateway_stream_key_for(1, 20, 300) == GatewayStreamKey{1, 20, 300, GatewayStreamType::guild, {}},
          "a live voice row maps to its stream key");
}
} // namespace

void test_gateway_stream(Check check) noexcept {
    test_live_badges(check);
    test_keys(check);
    test_writers(check);
    test_decoders(check);
    test_control(check);
    test_session(check);
    test_stream_connection(check);
}
} // namespace maicord
