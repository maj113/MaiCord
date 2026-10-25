#include "maicord/protocol/gateway_voice.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_gateway_voice(void (*check)(bool, const char*) noexcept) noexcept {
    u32 scratch[256]{};
    GatewayVoiceState state{};
    auto state_read = [&](Utf8View text) noexcept {
        return gateway_decode_voice_state({text, 0, JsonValueKind::object}, {scratch, 256}, 16, &state);
    };
    const auto full = utf8_literal(
        R"({"guild_id":"10","channel_id":"20","user_id":"30","session_id":"session-test","self_mute":true,"self_deaf":false,"mute":false,"deaf":true,"self_video":true,"self_stream":true,"suppress":false,"member":{"roles":["40"]}})");
    check(state_read(full).ok() && state.guild_id == 10 && state.channel_id == 20 && state.user_id == 30 &&
              state.present_flags == 127 &&
              state.flags == (gateway_voice_self_mute | gateway_voice_deaf | gateway_voice_self_video |
                              gateway_voice_self_stream),
          "voice state decodes guild membership and explicit flag presence");
    check(state.session_id.encoded.data >= full.data &&
              state.session_id.encoded.data < full.data + full.bytes,
          "voice session borrows its source document without retained allocation");
    check(state_read(utf8_literal(R"({"channel_id":null,"user_id":"30","session_id":"test"})")).ok() &&
              !state.channel_id && !state.guild_id && !state.present_flags,
          "voice leave and contextual/private state decode");
    const Utf8View invalid_states[] = {
        utf8_literal(R"({"channel_id":"20","user_id":"30","session_id":"x","self_mute":0})"),
        utf8_literal(R"({"channel_id":null,"user_id":"0","session_id":"x"})"),
        utf8_literal(R"({"channel_id":null,"user_id":30,"session_id":"x"})"),
        utf8_literal(R"({"channel_id":null,"user_id":"30","session_id":""})"),
        utf8_literal(R"({"channel_id":null,"user_id":"30"})"),
        utf8_literal(R"({"channel_id":null,"user_id":"30","session_id":"x","user\u005fid":"31"})"),
        utf8_literal(
            R"({"channel_id":null,"user_id":"30","session_id":"x","self_video":true,"self_video":false})"),
        utf8_literal(R"({"channel_id":null,"user_id":"18446744073709551616","session_id":"x"})"),
        utf8_literal(R"({"channel_id":null,"user_id":"30","session_id":"x","ignored":[})")};
    for (const auto input : invalid_states) {
        state.user_id = 999;
        check(!state_read(input).ok() && state.user_id == 999,
              "invalid voice state leaves prior output unchanged");
    }
    GatewayVoiceServer server{};
    auto server_read = [&](Utf8View text) noexcept {
        return gateway_decode_voice_server({text, 0, JsonValueKind::object}, {scratch, 256}, 16, &server);
    };
    check(server_read(
              utf8_literal(R"({"guild_id":"10","token":"fixture-only","endpoint":"example.invalid:443"})"))
                  .ok() &&
              server.guild_id == 10 && !server.channel_id && !server.unavailable,
          "guild voice server routing decodes");
    check(server_read(
              utf8_literal(R"({"guild_id":null,"channel_id":"20","token":"fixture-only","endpoint":null})"))
                  .ok() &&
              !server.guild_id && server.channel_id == 20 && server.unavailable &&
              !server.endpoint.encoded.bytes,
          "private call null endpoint clears the returned endpoint and marks reallocation");
    const Utf8View invalid_servers[] = {
        utf8_literal(R"({"guild_id":null,"token":"fixture-only","endpoint":null})"),
        utf8_literal(R"({"guild_id":"10","token":"","endpoint":null})"),
        utf8_literal(R"({"guild_id":"10","token":"fixture-only","endpoint":""})"),
        utf8_literal(R"({"guild_id":"10","token":"fixture-only"})"),
        utf8_literal(
            R"({"guild_id":"10","token":"fixture-only","endpoint":null,"token":"duplicate-fixture"})")};
    for (const auto input : invalid_servers) {
        server.guild_id = 999;
        check(!server_read(input).ok() && server.guild_id == 999,
              "invalid voice server cannot partially replace credentials");
    }
    const auto escaped = utf8_literal(R"({"guild_id":"10","token":"fixture\u002donly","endpoint":null})");
    check(server_read(escaped).ok() && server.token.has_escapes &&
              json_string_equals_ascii(server.token, utf8_literal("fixture-only")),
          "voice tokens preserve valid escaped JSON strings");
    check(
        gateway_decode_voice_server({escaped, 0, JsonValueKind::object}, {scratch, 1}, 16, &server).error ==
            GatewayEventDecodeError::scan_failed,
        "voice decoder reports structural scratch exhaustion");
    u8 output[256];
    const GatewayVoiceStateUpdate updates[] = {{10, 20, false, true, false},
                                               {0, 20, true, false, true},
                                               {0, 0, false, false, false},
                                               {~0ull, ~0ull, true, true, true}};
    const Utf8View expected[] = {
        utf8_literal(
            R"({"op":4,"d":{"guild_id":"10","channel_id":"20","self_mute":false,"self_deaf":true,"self_video":false,"flags":2}})"),
        utf8_literal(
            R"({"op":4,"d":{"guild_id":null,"channel_id":"20","self_mute":true,"self_deaf":false,"self_video":true,"flags":0}})"),
        utf8_literal(
            R"({"op":4,"d":{"guild_id":null,"channel_id":null,"self_mute":false,"self_deaf":false,"self_video":false,"flags":2}})"),
        utf8_literal(
            R"({"op":4,"d":{"guild_id":"18446744073709551615","channel_id":"18446744073709551615","self_mute":true,"self_deaf":true,"self_video":true,"flags":4294967295}})")};
    const u32 flags[] = {kGatewayVoiceStateAllowVoiceRecording, 0, kGatewayVoiceStateAllowVoiceRecording, ~0u};
    u32 fixture = 0;
    for (const auto& update : updates) {
        auto result = gateway_write_voice_state(update, flags[fixture], {output, 256});
        check(result.ok() && result.bytes < 256, "voice command bounds cover full-width snowflakes");
        const u32 required = result.bytes;
        for (u32 capacity = 0; capacity < required; ++capacity) {
            for (auto& byte : output)
                byte = 0xA5;
            result = gateway_write_voice_state(update, flags[fixture], {output, capacity});
            bool intact = true;
            for (auto byte : output)
                intact &= byte == 0xA5;
            check(result.error == GatewayCommandError::output_too_small && !result.bytes && intact,
                  "voice command is atomic at every insufficient capacity");
        }
        result = gateway_write_voice_state(update, flags[fixture], {output, required});
        check(result.ok() && result.bytes == required && output[required] == 0xA5,
              "voice command writes exactly its measured bytes");
        bool matches = result.bytes == expected[fixture].bytes;
        for (u32 i = 0; matches && i < result.bytes; ++i)
            matches = output[i] == static_cast<u8>(expected[fixture].data[i]);
        check(matches, "voice command matches guild, private-call and leave wire fixtures");
        ++fixture;
        GatewayEnvelope envelope{};
        check(gateway_decode_envelope({reinterpret_cast<char*>(output), result.bytes}, {scratch, 256}, 16,
                                      &envelope)
                      .ok() &&
                  envelope.opcode == 4,
              "voice update is a complete opcode 4 Gateway envelope");
    }
}
} // namespace maicord
