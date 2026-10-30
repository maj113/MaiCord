#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/thread_creation.hpp"

namespace maicord::app_detail {
// Body bytes borrow the caller's bounded serialization until submit copies
// them. Execution and autocomplete use the same JSON transport contract.
inline RestClientRequest application_command_interaction_request(
    ConstSlice<u8> body, u64 request_id, bool autocomplete) noexcept {
    constexpr auto target = utf8_literal("/api/v10/interactions");
    return {{reinterpret_cast<const u8*>(target.data), target.bytes}, body,
            {0x434d4458, 0, RestMethod::post, RestMajorParameter::none},
            request_id, autocomplete ? 3000u : kRestClientDefaultTimeoutMs, true};
}
// The application command body, in key order: type,
// application_id, guild_id (guild channels only), channel_id, session_id, data,
// nonce, then analytics_location for executions (type 2). Autocomplete (type 4)
// carries no analytics location.
inline bool application_command_interaction_body(bool autocomplete, u64 application, u64 guild, u64 channel,
                                                 Utf8View session_id, u64 nonce, Utf8View data,
                                                 ProtocolWriter* body) noexcept {
    bool ok = protocol_writer_append_literal(body, "{\"type\":") &&
              protocol_writer_append_u64(body, autocomplete ? 4 : 2) &&
              protocol_writer_append_literal(body, ",\"application_id\":\"") &&
              protocol_writer_append_u64(body, application);
    if (guild)
        ok = ok && protocol_writer_append_literal(body, "\",\"guild_id\":\"") &&
             protocol_writer_append_u64(body, guild);
    ok = ok && protocol_writer_append_literal(body, "\",\"channel_id\":\"") &&
         protocol_writer_append_u64(body, channel) &&
         protocol_writer_append_literal(body, "\",\"session_id\":") &&
         protocol_writer_append_json_string(body, session_id) &&
         protocol_writer_append_literal(body, ",\"data\":") &&
         protocol_writer_append_bytes(body, {reinterpret_cast<const u8*>(data.data), data.bytes}) &&
         protocol_writer_append_literal(body, ",\"nonce\":\"") && protocol_writer_append_u64(body, nonce);
    return ok && protocol_writer_append_c_string(body, autocomplete ? "\"}" : "\",\"analytics_location\":\"slash_ui\"}");
}
// The built-in /nick and /thread requests:
// PATCH /guilds/{guild}/members/@me/nick {"nick"} and
// POST /channels/{channel}/threads {"name","type","auto_archive_duration",
// "location"}. The target is written into target_storage; zero target bytes
// mark an invalid request.
inline constexpr u8 kBuiltinCommandNick = 7, kBuiltinCommandThread = 8;
inline constexpr u64 kBuiltinNickRouteId = 0x4E49434B4E414D45ull;
inline RestClientRequest builtin_command_request(u8 operation, u64 guild, u64 channel, ConstSlice<u8> body,
                                                 u64 request_id, Slice<u8> target_storage) noexcept {
    ProtocolWriter path{target_storage, 0};
    const bool nick = operation == kBuiltinCommandNick;
    const u64 major = nick ? guild : channel;
    const bool ok = major && (nick || operation == kBuiltinCommandThread) &&
                    protocol_writer_append_c_string(&path, nick ? "/api/v9/guilds/" : "/api/v9/channels/") &&
                    protocol_writer_append_u64(&path, major) &&
                    protocol_writer_append_c_string(&path, nick ? "/members/@me/nick" : "/threads");
    return {{target_storage.data, ok ? path.used : 0u}, body,
            {nick ? kBuiltinNickRouteId : kThreadCreationRouteId, major, nick ? RestMethod::patch : RestMethod::post,
             nick ? RestMajorParameter::guild : RestMajorParameter::channel},
            request_id, kRestClientDefaultTimeoutMs, true};
}
} // namespace maicord::app_detail
