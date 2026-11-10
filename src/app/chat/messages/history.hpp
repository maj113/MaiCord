#pragma once

#include "history_decode.hpp"
#include "maicord/app/net/rest_client.hpp"

namespace maicord::app_detail {

// Shared production catch-up boundary: admit all writable decode storage before
// the REST bridge can change catch-up, timeline, users or members. Empty/error
// responses retain the bridge's existing classification without committing VM;
// `decoded_scratch` then stays as the caller passed it.
inline bool application_message_history_catchup_response(AccountUiRuntime& ui,
    MessageHistoryCatchup& catchup, MessageEventPump& pump,
    const RestClientResponse& response, Utf8View body, u64 now_ms,
    MessageHistoryRestResult* accepted, MessageHistoryDecodeScratch* decoded_scratch) noexcept {
    if (response.transport_ok() && response.status_code == 200 && body.data && body.bytes &&
        body.bytes == response.body_bytes && !application_message_decode_scratch(ui, decoded_scratch, body)) return false;
    *accepted = message_history_catchup_accept_rest(&catchup, &pump, response, body, *decoded_scratch,
        MessageContentAvailability::unknown, now_ms);
    return true;
}

bool pump_message_history() noexcept;
// Actor/lifecycle reset of the reaction-only PRIMARY admission turn.
void application_message_history_reset_reaction_turn() noexcept;
bool scroll_to_resident_message(u64 message_id) noexcept;

} // namespace maicord::app_detail
