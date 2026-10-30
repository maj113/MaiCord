#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/writer.hpp"

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
// The official client's application command body, in its key order: type,
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
} // namespace maicord::app_detail
